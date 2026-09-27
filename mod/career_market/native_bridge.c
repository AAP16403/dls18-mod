/*
 * Runtime bridge for the DLS18 career transfer market.
 *
 * This file intentionally has no libc dependency. Game functions are reached
 * through the position-independent base passed by the code-cave bootstrap.
 * Addresses and layouts are for DLS18 v5.064 / armeabi-v7a and are documented
 * in CAREER_TRANSFER_MARKET_PLAN.md.
 */
#include <stdint.h>
#include "career_market_api.h"

#define SAVE_VERSION 0xAF
#define BUY_ATTEMPT_SAVE_VERSION 0xB0
#define FINANCE_LEDGER_SAVE_VERSION 0xB1
#define CONTRACT_EXTENSION_SAVE_VERSION 0xB2
#define MARKET_MAGIC 0x4D4B5432u
#define MARKET_FORMAT 2
#define USER_TEAM_ID 0x102
#define MARKET_TRANSFER_SCREEN_UI 1   /* 1 = v35 market inside the stock transfer screen, 0 = hub box on entry */
#define MAX_CLUBS 256
#define MAX_PLAYERS 8192
#define HISTORY_CAPACITY 128
#define CONTRACT_CAPACITY 512
#define CONTRACT_STORAGE_CAPACITY MAX_PLAYERS
#define CONTRACT_EXTENSION_CAPACITY (CONTRACT_STORAGE_CAPACITY - CONTRACT_CAPACITY)
#define AI_CONTRACT_RENEWAL_LIMIT 40   /* clubs renew everyone they still want; fringe players run down */
#define AI_CONTRACT_RENEWAL_YEARS 3
#define OFFER_CAPACITY 32
#define USER_LISTING_CAPACITY 32
#define FINANCE_ENTRIES_PER_CLUB 16
#define PLAYER_INFO_SIZE 0xB0
#define LINK_TEAM_ID 0x00
#define LINK_PLAYER_COUNT 0x04
#define LINK_TEAM_DATA 0x08
#define LINK_PLAYER_IDS 0x88
#define DLOPEN_PLT 0x1D90FC  /* PLT stubs are ARM code: even address, no Thumb bit */

typedef struct {
    int32_t team_id;
    int32_t cash;
    int32_t transfer_budget;
    int32_t wage_budget;
    int32_t payroll;
    int32_t squad_value;
    int32_t strength;
    uint32_t window_flags;
} ClubAccount;

typedef struct {
    int32_t season;
    int32_t turn;
    int32_t player_id;
    int32_t seller_id;
    int32_t buyer_id;
    int32_t fee;
    int32_t annual_wage;
    int32_t contract_years;
} TransferRecord;

typedef struct {
    int32_t player_id;
    int32_t club_id;
    int32_t annual_wage;
    int32_t expiry_season;
} PlayerContract;

typedef struct {
    int32_t offer_id;
    int32_t season;
    int32_t turn;
    int32_t window_id;
    int32_t player_id;
    int32_t seller_id;
    int32_t buyer_id;
    int32_t fee;
    int32_t counter_fee;
    int32_t annual_wage;
    int32_t counter_wage;
    int32_t contract_years;
    int32_t rounds;
    int32_t status;
} MarketOffer;

typedef struct {
    int32_t season;
    int32_t turn;
    int32_t category;
    int32_t cash_delta;
    int32_t transfer_budget_delta;
    int32_t payroll_delta;
    int32_t player_id;
    int32_t reserved;
} FinanceLedgerEntry;

typedef struct {
    uint32_t head;
    uint32_t count;
} FinanceLedgerMeta;

typedef struct {
    int32_t magic;
    int32_t format;
    int32_t club_count;
    int32_t history_head;
    int32_t history_count;
    int32_t contract_next;
    int32_t last_turn_key;
    int32_t season;
    int32_t window_id;
    int32_t current_turn;
    int32_t turns_per_season;
    int32_t user_team_id;
    int32_t offer_next;
    int32_t next_offer_id;
    int32_t reserved[2]; /* reserved[0] stores contract epoch season + 1 */
    ClubAccount clubs[MAX_CLUBS];
    TransferRecord history[HISTORY_CAPACITY];
    PlayerContract contracts[CONTRACT_CAPACITY];
    MarketOffer offers[OFFER_CAPACITY];
} CareerMarketState;

typedef struct {
    uint8_t bytes[PLAYER_INFO_SIZE];
} PlayerInfo;

typedef struct {
    int32_t player_id;
    int32_t owner_id;
    int32_t owner_index;
    int32_t position;
    int32_t rating;
    int32_t value;
    int32_t wage;
    int32_t listed_for_sale;
} MarketPlayer;

typedef int (*GetLinkCountFn)(void);
typedef void *(*GetTeamLinkByIndexFn)(int32_t);
typedef int (*GetTeamValueTotalFn)(int32_t);
typedef int (*IsValidSearchTeamFn)(int32_t);
typedef int (*GetPlayerInfoSimpleFn)(PlayerInfo *, int32_t, int32_t, int32_t);
typedef int (*GetPlayerRatingFn)(PlayerInfo *);
typedef int (*GetPlayerValueFn)(PlayerInfo *, int32_t, int32_t, int32_t, int32_t);
typedef int (*GetCurrentTurnFn)(void *);
typedef int (*GetEndTurnFn)(void *);
typedef int (*GetSeasonCountFn)(void *);
typedef void *(*GetTeamSpecificDataFn)(int32_t, int32_t);
typedef void *(*DlopenFn)(const char *, int32_t);
typedef void (*AddPlayerToLinkFn)(int32_t, int32_t, const PlayerInfo *, const void *, int32_t, int32_t);
typedef void (*RemovePlayerFromLinkFn)(int32_t, int32_t, int32_t, int32_t);
typedef void (*SignPlayerFn)(const PlayerInfo *, int32_t, const void *, int32_t, int32_t, int32_t);
typedef void (*SellPlayerFn)(PlayerInfo *, int32_t, const void *, int32_t);
typedef void (*PlayerDevelopmentAddFn)(int32_t, int32_t);
typedef void (*CalculateLinksFn)(int32_t, int32_t, int32_t, int32_t);
typedef void (*SerializeU64Fn)(void *, uint64_t *, int32_t);

static CareerMarketState g_market __attribute__((aligned(8)));

/*
 * v6 extension block (save version 0xB4): data the original state has no room for. It is cleared
 * with the state, so an older save loads with empty extension data.
 */
#define MARKET_EXT_SAVE_VERSION 0xB4
#define TALK_CAPACITY 32
#define SHORTLIST_CAPACITY 16
#define ECON_CATEGORIES 16
#define ECON_MATCH_ROWS 8
typedef struct {
    int32_t rival_team;          /* runner-up in the auction, -1 if uncontested */
    int32_t rival_fee;           /* the runner-up's valuation */
} HistoryExt;
typedef struct {
    int32_t rival_team;          /* AI club competing with the user for the player, -1 if none */
    int32_t rival_fee;           /* its current bid */
    int32_t rival_max;           /* its valuation (hidden) */
    int32_t reservation;         /* seller's reservation when talks opened (hidden) */
} OfferExt;
typedef struct {
    int32_t player_id;
    int32_t window_id;
    int32_t strikes;             /* v36: patience used, in thousandths of a strike; 3000 ends the talks */
    int32_t best_bid;
} TalkMemory;
typedef struct {
    int32_t season;                          /* season the running tallies belong to */
    int32_t this_season[ECON_CATEGORIES];    /* coins by EconCategory, signed */
    int32_t last_season[ECON_CATEGORIES];
    int32_t last_match[ECON_MATCH_ROWS];     /* breakdown of the last match payout */
    int32_t last_match_turn;
    int32_t season_end_paid;                 /* season id + 1 of the last season-end payout */
    int32_t division;                        /* last known league tree (0 Elite .. 5 Academy) */
    int32_t reserved[13];
} UserEconomy;
typedef struct {
    HistoryExt history[HISTORY_CAPACITY];
    OfferExt offers[OFFER_CAPACITY];
    TalkMemory talks[TALK_CAPACITY];
    int32_t shortlist[SHORTLIST_CAPACITY];
    UserEconomy econ;
    int32_t market_index[4][10];             /* smoothed price index per position and rating band, % */
    int32_t signing_player[32];              /* the user's signings, locked until next season */
    int32_t signing_season[32];
    int32_t keep_player[16];                 /* v38: players the user keeps (player id + 1, 0 = free) */
    int32_t reserved[16];
} MarketExt;
static MarketExt g_ext __attribute__((aligned(8)));
static PlayerContract g_contract_extension[CONTRACT_EXTENSION_CAPACITY]
    __attribute__((aligned(8)));
static uint16_t g_contract_slot_by_player[0x10000] __attribute__((aligned(8)));
static MarketPlayer g_players[MAX_PLAYERS];
static uint16_t g_player_slot_by_id[0x10000];   /* g_players index + 1 (0 = none), rebuilt with the cache */
static uint32_t g_price_epoch;                   /* bumped when cached prices may have changed */
static PlayerInfo g_player_info;
static int32_t g_group_count[MAX_CLUBS][4];
static int32_t g_group_rating_sum[MAX_CLUBS][4];
static int32_t g_total_count[MAX_CLUBS];
static int32_t g_total_rating_sum[MAX_CLUBS];
static uint8_t g_account_seen[MAX_CLUBS];
static int32_t g_buy_attempt_window[MAX_CLUBS] __attribute__((aligned(8)));
static int32_t g_user_listed_player_ids[USER_LISTING_CAPACITY] __attribute__((aligned(8)));
static FinanceLedgerEntry g_finance_ledger[MAX_CLUBS][FINANCE_ENTRIES_PER_CLUB]
    __attribute__((aligned(8)));
static FinanceLedgerMeta g_finance_ledger_meta[MAX_CLUBS] __attribute__((aligned(8)));
static int32_t g_player_count;
static uint8_t g_market_changed;
static uint8_t g_market_dirty;
static uint8_t g_library_pinned;
static const char g_library_name[] = "libCareerMarket.so";

/*
 * Balance model (tuned with tests/harness.c on the extracted DLS18 dataset; see BALANCE.md).
 * Money is in DLS player-value units. Positions: 0 GK, 1 DEF, 2 MID, 3 FWD.
 */
#define SQUAD_MIN 16                 /* stock floor (CTransfers::CanRemovePlayer): AI clubs keep 16 */
#define USER_SQUAD_MIN 17            /* below 17 the game auto-signs free players for the user */
#define SQUAD_MAX 30                 /* the link array holds 32; keep two slots of headroom */
/* DLS18 club squads: GK 1-4 (median 2), DEF 4-10 (7), MID 4-13 (9), FWD 1-7 (3) */
static const int32_t k_pos_min[4] = {2, 5, 6, 2};
static const int32_t k_pos_target[4] = {2, 7, 8, 3};
static const int32_t k_pos_max[4] = {3, 10, 12, 5};
static const int32_t k_starters[4] = {1, 4, 4, 2};   /* best XI used for strength and roles */
#define WAGE_RATE_PCT 12             /* annual wage demand as % of player value */
#define REVENUE_BASE_PCT 16          /* annual revenue as % of squad value for the biggest club */
#define REVENUE_SMALL_PCT 8          /* extra % for the smallest club */
#define REVENUE_FLOOR_PCT 25         /* floor: % of the average club's revenue */
#define WAGE_BUDGET_PCT 66           /* wage budget as % of annual revenue */
#define OPENING_CASH_PCT 12          /* opening cash as % of squad value ... */
#define OPENING_CASH_REP_PCT 18      /* ... plus up to this % for the most reputable club */
#define SPEND_PCT_MIN 40             /* share of free cash released as transfer budget */
#define SPEND_PCT_MAX 75
#define CASH_RESERVE_PCT 25          /* clubs keep a quarter of the wage bill as reserve */
#define SALE_REINVEST_PCT 60         /* share of a sale fee added to the seller's budget */
#define NEGOTIATION_SPLIT_PCT 35     /* deal lands this far from the ask towards the buyer max */
#define MAX_AI_BUYS_PER_WINDOW 3
#define MAX_SALES_PER_WINDOW 2
#define UNSOLICITED_PREMIUM_PCT 115  /* AI bid for an unlisted user star: up to this % of his sale value */
#define USER_SALE_CEILING_PCT 100    /* AI clubs pay at most a user player's sale value (user_sale_value) */
#define USER_BUY_FLOOR_PCT 85        /* the user never buys a contracted player below this % of market value */
#define AI_SQUAD_COMFORT 26          /* above this an AI club only buys needs or big upgrades */
#define CLEARANCE_SQUAD 25           /* above this a club discounts its surplus players */

static int32_t g_club_rep[MAX_CLUBS];              /* 1..100 from squad value rank; not saved */
static int32_t g_top_rating[MAX_CLUBS][4][4];      /* best ratings per position, descending */
static int32_t g_avg_revenue;

static void market_ui_show_main(uint32_t base);

static PlayerContract *contract_slot(int32_t index) {
    if (index < 0 || index >= CONTRACT_STORAGE_CAPACITY) return (PlayerContract *)0;
    if (index < CONTRACT_CAPACITY) return &g_market.contracts[index];
    return &g_contract_extension[index - CONTRACT_CAPACITY];
}

static int32_t contract_slot_for_player(int32_t player_id) {
    if (player_id < 0 || player_id > 0xFFFF) return -1;
    uint32_t slot = g_contract_slot_by_player[player_id];
    if (slot >= CONTRACT_STORAGE_CAPACITY) return -1;
    PlayerContract *contract = contract_slot((int32_t)slot);
    return contract && contract->player_id == player_id ? (int32_t)slot : -1;
}

static void reset_contract_index(void) {
    uint32_t *words = (uint32_t *)g_contract_slot_by_player;
    for (uint32_t i = 0; i < 0x10000u / 2u; ++i) words[i] = 0xFFFFFFFFu;
}

static void rebuild_contract_index(void) {
    reset_contract_index();
    for (int32_t i = 0; i < CONTRACT_STORAGE_CAPACITY; ++i) {
        PlayerContract *contract = contract_slot(i);
        if (contract->player_id < 0 || contract->player_id > 0xFFFF ||
            contract->annual_wage <= 0) continue;
        if (g_contract_slot_by_player[contract->player_id] == 0xFFFFu) {
            g_contract_slot_by_player[contract->player_id] = (uint16_t)i;
        }
    }
}

static void pin_library(uint32_t base) {
    if (g_library_pinned) return;
    DlopenFn open_library = (DlopenFn)(base + DLOPEN_PLT);
    if (open_library(g_library_name, 2)) g_library_pinned = 1;
}

static int32_t clamp_add(int32_t left, int32_t right) {
    if (left < 0) left = 0;
    if (right < 0) right = 0;
    if (left > 0x7fffffff - right) return 0x7fffffff;
    return left + right;
}

static int32_t mul_div(int32_t value, int32_t multiplier, int32_t divisor) {
    if (value <= 0 || multiplier <= 0 || divisor <= 0) return 0;
    int32_t quotient = value / divisor;
    int32_t remainder = value % divisor;
    if (quotient > 0x7fffffff / multiplier) return 0x7fffffff;
    int32_t whole = quotient * multiplier;
    int32_t fractional = (remainder * multiplier) / divisor;
    return clamp_add(whole, fractional);
}

static void clear_ext2(void);
static void clear_ext3(void);
static void chem_serialize(void *serializer, void (*fn)(void *, uint64_t *, int32_t));
static void apply_price_schedule(uint32_t base);

/* modcore (modcore.c): runtime C hooks through the extended library's CAVE3/MODDATA segments.
 * Handlers are registered here by hook name; modcore_init fills their slots. A library without the
 * v8 segments has no descriptor and modcore_init does nothing. */
typedef struct { uint32_t apsr; uint32_t r[13]; uint32_t lr; uint32_t target; } ModCtx;
typedef uint32_t (*ModHookFn)(ModCtx *ctx, uint32_t base);
int32_t modcore_register(const char *name, ModHookFn fn);
int32_t modcore_init(uint32_t base);
uint32_t modcore_resume(const char *name);
static uint32_t career_market_new_screen_hook(ModCtx *ctx, uint32_t base);
static void ts_register_hooks(void);
static void ts_on_enter(uint32_t base);
static void ts_buy_dialog(uint32_t base);
static void ts_sell_dialog(uint32_t base);
static int32_t g_ts_full_terms;          /* Negotiate: the full terms pages instead of the card dialog */
static uint8_t g_modcore_booted;
static void mod_boot(uint32_t base, int32_t force) {
    if (!base || (g_modcore_booted && !force)) return;
    modcore_register("career_market_new_screen", career_market_new_screen_hook);
    ts_register_hooks();
    modcore_init(base);
    g_modcore_booted = 1;
}

static void clear_state(void) {
    uint64_t *words = (uint64_t *)&g_market;
    uint32_t count = (uint32_t)(sizeof(g_market) / sizeof(uint64_t));
    for (uint32_t i = 0; i < count; ++i) words[i] = 0;
    g_player_count = 0;
    for (int32_t i = 0; i < MAX_CLUBS; ++i) g_market.clubs[i].team_id = -1;
    for (int32_t i = 0; i < CONTRACT_CAPACITY; ++i) g_market.contracts[i].player_id = -1;
    uint64_t *contract_words = (uint64_t *)g_contract_extension;
    uint32_t contract_word_count = (uint32_t)(sizeof(g_contract_extension) / (sizeof(uint64_t)));
    for (uint32_t i = 0; i < contract_word_count; ++i) contract_words[i] = 0;
    for (int32_t i = 0; i < CONTRACT_EXTENSION_CAPACITY; ++i) {
        g_contract_extension[i].player_id = -1;
    }
    reset_contract_index();
    g_market.window_id = -1;
    g_market.last_turn_key = -1;
    g_market.next_offer_id = 1;
    g_market_dirty = 0;
    for (int32_t i = 0; i < MAX_CLUBS; ++i) g_buy_attempt_window[i] = -1;
    for (int32_t i = 0; i < USER_LISTING_CAPACITY; ++i) g_user_listed_player_ids[i] = -1;
    uint64_t *ledger_words = (uint64_t *)g_finance_ledger;
    uint32_t ledger_word_count = (uint32_t)(sizeof(g_finance_ledger) / sizeof(uint64_t));
    for (uint32_t i = 0; i < ledger_word_count; ++i) ledger_words[i] = 0;
    uint64_t *meta_words = (uint64_t *)g_finance_ledger_meta;
    uint32_t meta_word_count = (uint32_t)(sizeof(g_finance_ledger_meta) / sizeof(uint64_t));
    for (uint32_t i = 0; i < meta_word_count; ++i) meta_words[i] = 0;
    uint64_t *ext_words = (uint64_t *)&g_ext;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_ext) / sizeof(uint64_t)); ++i) ext_words[i] = 0;
    for (int32_t i = 0; i < HISTORY_CAPACITY; ++i) g_ext.history[i].rival_team = -1;
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) g_ext.offers[i].rival_team = -1;
    for (int32_t i = 0; i < TALK_CAPACITY; ++i) g_ext.talks[i].player_id = -1;
    for (int32_t i = 0; i < SHORTLIST_CAPACITY; ++i) g_ext.shortlist[i] = -1;
    for (int32_t i = 0; i < 32; ++i) g_ext.signing_player[i] = -1;
    g_ext.econ.division = -1;
    clear_ext2();
    clear_ext3();
}

static int32_t find_account(int32_t team_id) {
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        if (g_market.clubs[i].team_id == team_id) return i;
    }
    return -1;
}

static int32_t user_listing_slot(int32_t player_id) {
    for (int32_t i = 0; i < USER_LISTING_CAPACITY; ++i) {
        if (g_user_listed_player_ids[i] == player_id) return i;
    }
    return -1;
}

static int32_t user_player_is_listed(int32_t player_id) {
    return user_listing_slot(player_id) >= 0;
}

static void remove_user_listing(int32_t player_id) {
    int32_t slot = user_listing_slot(player_id);
    if (slot < 0) return;
    g_user_listed_player_ids[slot] = -1;
    g_market_dirty = 1;
}

static int32_t shortlist_slot(int32_t player_id) {
    for (int32_t i = 0; i < SHORTLIST_CAPACITY; ++i) {
        if (g_ext.shortlist[i] == player_id) return i;
    }
    return -1;
}

static int32_t shortlist_toggle(int32_t player_id) {
    if (player_id < 0 || player_id > 0xFFFF) return -1;
    int32_t slot = shortlist_slot(player_id);
    if (slot >= 0) {
        g_ext.shortlist[slot] = -1;
        return 0;
    }
    for (int32_t i = 0; i < SHORTLIST_CAPACITY; ++i) {
        if (g_ext.shortlist[i] < 0) {
            g_ext.shortlist[i] = player_id;
            return 1;
        }
    }
    return -1;
}

static int32_t shortlist_count(void) {
    int32_t count = 0;
    for (int32_t i = 0; i < SHORTLIST_CAPACITY; ++i) {
        if (g_ext.shortlist[i] >= 0) ++count;
    }
    return count;
}

/* window_flags: bits 0-15 = window id the counters belong to, 16-23 = purchases, 24-31 = sales.
 * Counters from another window read as zero, so no reset pass is needed. */
static int32_t window_buys(const ClubAccount *club, int32_t window_id) {
    if ((club->window_flags & 0xFFFFu) != ((uint32_t)window_id & 0xFFFFu)) return 0;
    return (int32_t)((club->window_flags >> 16) & 0xFFu);
}

static int32_t window_sales(const ClubAccount *club, int32_t window_id) {
    if ((club->window_flags & 0xFFFFu) != ((uint32_t)window_id & 0xFFFFu)) return 0;
    return (int32_t)((club->window_flags >> 24) & 0xFFu);
}

static void window_note(ClubAccount *club, int32_t window_id, int32_t buys, int32_t sales) {
    int32_t b = window_buys(club, window_id) + buys;
    int32_t s = window_sales(club, window_id) + sales;
    if (b > 255) b = 255;
    if (s > 255) s = 255;
    club->window_flags = ((uint32_t)window_id & 0xFFFFu) | ((uint32_t)b << 16) | ((uint32_t)s << 24);
}

static int32_t contract_wage(int32_t player_id, int32_t club_id, int32_t season, int32_t fallback) {
    int32_t slot = contract_slot_for_player(player_id);
    PlayerContract *contract = slot >= 0 ? contract_slot(slot) : (PlayerContract *)0;
    if (contract && contract->club_id == club_id &&
        contract->expiry_season >= season && contract->annual_wage > 0) {
        return contract->annual_wage;
    }
    return fallback;
}

static PlayerContract *find_contract_record(int32_t player_id, int32_t club_id) {
    int32_t slot = contract_slot_for_player(player_id);
    PlayerContract *contract = slot >= 0 ? contract_slot(slot) : (PlayerContract *)0;
    return contract && contract->club_id == club_id && contract->annual_wage > 0
        ? contract : (PlayerContract *)0;
}

static void ensure_contract_epoch(void) {
    if (g_market.reserved[0] > 0) return;
    int32_t season = g_market.season;
    if (season < 0) season = 0;
    if (season >= 0x7FFFFFFF) season = 0x7FFFFFFE;
    g_market.reserved[0] = season + 1;
    g_market_dirty = 1;
}

static int32_t modeled_contract_expiry(int32_t player_id) {
    if (player_id < 0 || player_id > 0xFFFF || g_market.reserved[0] <= 0) return -1;
    int32_t epoch = g_market.reserved[0] - 1;
    uint32_t mixed_id = (uint32_t)player_id * 0x9E3779B1u;
    int32_t years = 2 + (int32_t)(mixed_id >> 30);
    if (epoch > 0x7FFFFFFF - years) return 0x7FFFFFFF;
    return epoch + years;
}

static int32_t market_contract_available(int32_t player_id, int32_t club_id) {
    return find_contract_record(player_id, club_id) != (PlayerContract *)0 ||
           modeled_contract_expiry(player_id) >= 0;
}

static int32_t contract_seasons_to_expiry(int32_t player_id, int32_t club_id,
                                          int32_t season) {
    PlayerContract *contract = find_contract_record(player_id, club_id);
    if (contract) {
        return contract->expiry_season >= season
            ? contract->expiry_season - season : -1;
    }
    int32_t expiry = modeled_contract_expiry(player_id);
    if (expiry < season) return -1;
    return expiry - season;
}

static void add_contract(int32_t player_id, int32_t club_id, int32_t wage, int32_t expiry) {
    if (player_id < 0 || player_id > 0xFFFF || club_id < 0 || club_id > 0xFFFF || wage <= 0) return;
    int32_t slot = -1;
    slot = contract_slot_for_player(player_id);
    if (slot < 0) {
        int32_t start = g_market.contract_next % CONTRACT_STORAGE_CAPACITY;
        if (start < 0) start = 0;
        for (int32_t step = 0; step < CONTRACT_STORAGE_CAPACITY; ++step) {
            int32_t candidate = (start + step) % CONTRACT_STORAGE_CAPACITY;
            PlayerContract *candidate_contract = contract_slot(candidate);
            if (candidate_contract->player_id < 0) {
                slot = candidate;
                break;
            }
        }
    }
    if (slot < 0) {
        slot = g_market.contract_next % CONTRACT_STORAGE_CAPACITY;
        if (slot < 0) slot = 0;
        PlayerContract *old_contract = contract_slot(slot);
        if (old_contract->player_id >= 0 && old_contract->player_id <= 0xFFFF &&
            g_contract_slot_by_player[old_contract->player_id] == (uint16_t)slot) {
            g_contract_slot_by_player[old_contract->player_id] = 0xFFFFu;
        }
    }
    PlayerContract *contract = contract_slot(slot);
    contract->player_id = player_id;
    contract->club_id = club_id;
    contract->annual_wage = wage;
    contract->expiry_season = expiry;
    g_contract_slot_by_player[player_id] = (uint16_t)slot;
    g_market.contract_next = (slot + 1) % CONTRACT_STORAGE_CAPACITY;
    g_market_dirty = 1;
}

static uint32_t g_transfer_serial;   /* completed transfers this process (tests/diagnostics; not saved) */

static void record_transfer(int32_t season, int32_t turn, int32_t player_id,
                            int32_t seller, int32_t buyer, int32_t fee,
                            int32_t wage, int32_t years) {
    ++g_transfer_serial;
    int32_t slot = g_market.history_head % HISTORY_CAPACITY;
    TransferRecord *record = &g_market.history[slot];
    record->season = season;
    record->turn = turn;
    record->player_id = player_id;
    record->seller_id = seller;
    record->buyer_id = buyer;
    record->fee = fee;
    record->annual_wage = wage;
    record->contract_years = years;
    g_ext.history[slot].rival_team = -1;
    g_ext.history[slot].rival_fee = 0;
    g_market.history_head = (slot + 1) % HISTORY_CAPACITY;
    if (g_market.history_count < HISTORY_CAPACITY) ++g_market.history_count;
    g_market_dirty = 1;
}

/* A player the user signed cannot be sold again in the same season (no flipping). */
static void note_user_signing(int32_t player_id) {
    int32_t slot = 0;
    for (int32_t i = 0; i < 32; ++i) {
        if (g_ext.signing_player[i] == player_id || g_ext.signing_player[i] < 0 ||
            g_ext.signing_season[i] < g_market.season) { slot = i; break; }
        if (g_ext.signing_season[i] < g_ext.signing_season[slot]) slot = i;
    }
    g_ext.signing_player[slot] = player_id;
    g_ext.signing_season[slot] = g_market.season;
}

static int32_t user_signing_locked(int32_t player_id) {
    for (int32_t i = 0; i < 32; ++i) {
        if (g_ext.signing_player[i] == player_id && g_ext.signing_season[i] == g_market.season) return 1;
    }
    return 0;
}

static void record_finance(int32_t account_index, int32_t category,
                           int32_t cash_delta, int32_t budget_delta,
                           int32_t payroll_delta, int32_t player_id) {
    if (account_index < 0 || account_index >= MAX_CLUBS || category <= 0) return;
    if (!cash_delta && !budget_delta && !payroll_delta &&
        category != CM_FINANCE_OPENING && category != CM_FINANCE_CONTRACT_RENEWAL) return;
    if (category > CM_FINANCE_INVESTMENT) return;
    FinanceLedgerMeta *meta = &g_finance_ledger_meta[account_index];
    if (meta->head >= FINANCE_ENTRIES_PER_CLUB) meta->head = 0;
    if (meta->count > FINANCE_ENTRIES_PER_CLUB) meta->count = 0;
    FinanceLedgerEntry *entry = &g_finance_ledger[account_index][meta->head];
    entry->season = g_market.season;
    entry->turn = g_market.current_turn;
    entry->category = category;
    entry->cash_delta = cash_delta;
    entry->transfer_budget_delta = budget_delta;
    entry->payroll_delta = payroll_delta;
    entry->player_id = player_id;
    entry->reserved = 0;
    meta->head = (meta->head + 1u) % FINANCE_ENTRIES_PER_CLUB;
    if (meta->count < FINANCE_ENTRIES_PER_CLUB) ++meta->count;
    g_market_dirty = 1;
}

static FinanceLedgerEntry *finance_entry_newest(int32_t account_index, int32_t newest_index) {
    if (account_index < 0 || account_index >= MAX_CLUBS || newest_index < 0) return 0;
    FinanceLedgerMeta *meta = &g_finance_ledger_meta[account_index];
    if (meta->head >= FINANCE_ENTRIES_PER_CLUB ||
        meta->count > FINANCE_ENTRIES_PER_CLUB || newest_index >= (int32_t)meta->count) return 0;
    uint32_t slot = (meta->head + FINANCE_ENTRIES_PER_CLUB - 1u - (uint32_t)newest_index) %
                    FINANCE_ENTRIES_PER_CLUB;
    return &g_finance_ledger[account_index][slot];
}

static int32_t offer_is_active(const MarketOffer *offer) {
    return offer->status >= CM_OFFER_WAIT_USER_FEE &&
           offer->status <= CM_OFFER_WAIT_USER_SELLER_COUNTER;
}

static MarketOffer *find_offer(int32_t offer_id) {
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        if (g_market.offers[i].offer_id == offer_id && offer_is_active(&g_market.offers[i])) {
            return &g_market.offers[i];
        }
    }
    return (MarketOffer *)0;
}

static int32_t has_active_offer_for_player(int32_t player_id) {
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        MarketOffer *offer = &g_market.offers[i];
        if (offer_is_active(offer) && offer->player_id == player_id) return 1;
    }
    return 0;
}

static int32_t has_active_offer_for_buyer(int32_t buyer_id, int32_t window_id) {
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        MarketOffer *offer = &g_market.offers[i];
        if (offer_is_active(offer) && offer->buyer_id == buyer_id && offer->window_id == window_id) return 1;
    }
    return 0;
}

static int32_t has_active_offer_for_seller(int32_t seller_id, int32_t window_id) {
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        MarketOffer *offer = &g_market.offers[i];
        if (offer_is_active(offer) && offer->seller_id == seller_id && offer->window_id == window_id) return 1;
    }
    return 0;
}

static MarketOffer *allocate_offer(int32_t season, int32_t turn, int32_t window_id,
                                   int32_t player_id, int32_t seller_id, int32_t buyer_id,
                                   int32_t fee, int32_t wage, int32_t years, int32_t status) {
    int32_t start = g_market.offer_next;
    if (start < 0 || start >= OFFER_CAPACITY) start = 0;
    for (int32_t step = 0; step < OFFER_CAPACITY; ++step) {
        int32_t slot = (start + step) % OFFER_CAPACITY;
        MarketOffer *offer = &g_market.offers[slot];
        if (offer_is_active(offer)) continue;
        int32_t id = g_market.next_offer_id++;
        if (id <= 0 || g_market.next_offer_id <= 0) {
            id = 1;
            g_market.next_offer_id = 2;
        }
        offer->offer_id = id;
        offer->season = season;
        offer->turn = turn;
        offer->window_id = window_id;
        offer->player_id = player_id;
        offer->seller_id = seller_id;
        offer->buyer_id = buyer_id;
        offer->fee = fee;
        offer->counter_fee = 0;
        offer->annual_wage = wage;
        offer->counter_wage = 0;
        offer->contract_years = years;
        offer->rounds = 0;
        offer->status = status;
        g_ext.offers[slot].rival_team = -1;
        g_ext.offers[slot].rival_fee = 0;
        g_ext.offers[slot].rival_max = 0;
        g_ext.offers[slot].reservation = 0;
        g_market.offer_next = (slot + 1) % OFFER_CAPACITY;
        g_market_dirty = 1;
        return offer;
    }
    return (MarketOffer *)0;
}

static int32_t player_base_wage(int32_t value) {
    int32_t wage = mul_div(value, WAGE_RATE_PCT, 100);
    return wage > 0 ? wage : 1;
}

/*
 * One currency: the user club's money is the game's coin balance (CMyProfile credits). Purchases
 * and sales move coins through CCredits::SetCredits (which also refreshes the HUD), the user club
 * gets no simulated revenue (match rewards are its income), and it pays a small coin wage bill.
 * Player values are already coin prices, so AI clubs keep their own balances in the same units.
 */
#define PROFILE_INSTANCE 0x84A260          /* MP_cMyProfile */
#define PROFILE_CREDITS 0x2A7CC            /* TCreditsInfo: first int = coin balance */
#define USER_STAR_STRENGTH_GAP 18          /* a player this far above the user's best XI ... */
#define USER_STAR_REP_GAP 40               /* ... at a club this much bigger refuses to join */
#define USER_KEY_SALE_REP_GAP 50           /* clubs keep key players from users this much smaller */
#define USER_WAGE_RATE_PCT WAGE_RATE_PCT  /* v6: the user club earns like a club, so it pays like one */
typedef void (*SetCreditsFn)(int32_t);

static int32_t user_coins(uint32_t base) {
    if (!base) return 0;
    int32_t coins = *(int32_t *)(uintptr_t)(base + PROFILE_INSTANCE + PROFILE_CREDITS);
    return coins > 0 ? coins : 0;
}

static void set_user_coins(uint32_t base, int32_t coins) {
    if (!base) return;
    if (coins < 0) coins = 0;
    SetCreditsFn set_credits = (SetCreditsFn)(base + 0x26348D);   /* CCredits::SetCredits */
    set_credits(coins);
}

/* Wage basis for a player at a given club: small for the user, club-level for AI clubs. */
static int32_t club_base_wage(int32_t team_id, int32_t value) {
    if (team_id != USER_TEAM_ID) return player_base_wage(value);
    int32_t wage = mul_div(value, USER_WAGE_RATE_PCT, 100);
    return wage > 0 ? wage : 1;
}

/*
 * User club economy (v6). The stock game pays about 300-450 coins a season against players that
 * cost 300-2,000, which only works with the coin shop. The mod pays the user club like a club:
 * prize money for results, gate receipts from its stadium, match bonuses, cup prize money, and at
 * the end of the league season TV rights, sponsorship, league prize money and a promotion bonus.
 * Everything scales with the division, so climbing the pyramid is what makes the club rich.
 *
 * Season income targets (average results, base stadium), in coins:
 *   Elite 4,300  Junior Elite 3,400  Div 1 2,700  Div 2 2,100  Div 3 1,600  Academy 1,200
 * Shares of the target: match prize money ~30%, gate receipts ~20%, TV 25%, sponsor 15%,
 * league prize 0-20% (10% mid-table). Wages cost the same 12% of value a season as at AI clubs.
 */
enum EconCategory {
    ECON_PRIZE = 0,          /* match result prize money */
    ECON_GATE = 1,           /* gate receipts (home matches) */
    ECON_BONUS = 2,          /* goals and clean sheets */
    ECON_CUP = 3,            /* cup prize money and cup win */
    ECON_TV = 4,
    ECON_SPONSOR = 5,
    ECON_LEAGUE_PRIZE = 6,
    ECON_PROMOTION = 7,
    ECON_SALES = 8,
    ECON_PURCHASES = 9,      /* negative */
    ECON_WAGES = 10,         /* negative */
    ECON_OTHER = 11
};

#define DIVISION_COUNT 6
/* league tree index: 0 Elite, 1 Junior Elite, 2 Div 1, 3 Div 2, 4 Div 3, 5 Academy */
static const int32_t k_season_target[DIVISION_COUNT] = {4300, 3400, 2700, 2100, 1600, 1200};
/* The stadium a club needs to be promoted into each division (MCU_GetMinStadiumCapacity, bundled config);
 * the Academy figure is a starting ground. Gate receipts are measured against 60% of it. */
static const int32_t k_reference_capacity[DIVISION_COUNT] = {80000, 60000, 45000, 30000, 15000, 6000};
#define REFERENCE_FILL_PCT 60
#define LEAGUE_ROUNDS 15
#define LEAGUE_TEAMS 16

static int32_t division_target(int32_t division) {
    if (division < 0 || division >= DIVISION_COUNT) division = DIVISION_COUNT - 1;
    return k_season_target[division];
}

/* Per-match amounts derived from the season target (see the shares above). */
static int32_t econ_win_prize(int32_t division) { return mul_div(division_target(division), 366, 10000); }
static int32_t econ_draw_prize(int32_t division) { return mul_div(econ_win_prize(division), 40, 100); }
static int32_t econ_loss_prize(int32_t division) { return mul_div(econ_win_prize(division), 12, 100); }
static int32_t econ_goal_bonus(int32_t division) { return mul_div(econ_win_prize(division), 8, 100) + 1; }
static int32_t econ_clean_sheet_bonus(int32_t division) { return mul_div(econ_win_prize(division), 15, 100) + 1; }

/* Gate receipts scale with the stadium: a stadium the size of the division's reference earns 2.2% of
 * the season target per home match; bigger grounds earn more, with diminishing returns past 2x. */
/* Gate receipts follow the real attendance: a crowd of 60% of the division's reference stadium earns
 * 2.2% of the season target; bigger crowds earn more, with diminishing returns past twice that. */
static int32_t econ_gate(int32_t division, int32_t attendance) {
    if (division < 0 || division >= DIVISION_COUNT) division = DIVISION_COUNT - 1;
    int32_t base = mul_div(division_target(division), 220, 10000);
    int32_t reference = mul_div(k_reference_capacity[division], REFERENCE_FILL_PCT, 100);
    if (attendance <= 0) attendance = reference;                /* unknown crowd: pay the reference */
    int32_t ratio = mul_div(attendance, 100, reference);        /* % of the reference crowd */
    if (ratio > 200) ratio = 200 + (ratio - 200) / 3;
    if (ratio < 25) ratio = 25;
    return mul_div(base, ratio, 100);
}

typedef struct {
    int32_t division;         /* league tree index */
    int32_t cup;              /* 1 for a cup match (prize money goes to ECON_CUP) */
    int32_t friendly;         /* friendlies pay gate only */
    int32_t home;
    int32_t goals_for;
    int32_t goals_against;
    int32_t capacity;         /* the match attendance (home matches) */
} MatchFacts;

/* Per-match payout, by category. Returns the total. */
static int32_t econ_match_income(const MatchFacts *m, int32_t out[ECON_CATEGORIES]) {
    for (int32_t i = 0; i < ECON_CATEGORIES; ++i) out[i] = 0;
    int32_t d = m->division;
    if (!m->friendly) {
        int32_t prize = m->goals_for > m->goals_against ? econ_win_prize(d)
                      : m->goals_for == m->goals_against ? econ_draw_prize(d) : econ_loss_prize(d);
        out[m->cup ? ECON_CUP : ECON_PRIZE] = prize;
        int32_t goals = m->goals_for > 6 ? 6 : m->goals_for;
        out[ECON_BONUS] = goals * econ_goal_bonus(d) + (m->goals_against == 0 ? econ_clean_sheet_bonus(d) : 0);
    }
    if (m->home) out[ECON_GATE] = m->friendly ? econ_gate(d, m->capacity) / 2 : econ_gate(d, m->capacity);
    int32_t total = 0;
    for (int32_t i = 0; i < ECON_CATEGORIES; ++i) total = clamp_add(total, out[i]);
    return total;
}

/* End of the league season: TV rights, sponsorship, league prize by final position, promotion. */
static int32_t econ_season_end(int32_t division, int32_t position, int32_t promoted,
                               int32_t out[ECON_CATEGORIES]) {
    for (int32_t i = 0; i < ECON_CATEGORIES; ++i) out[i] = 0;
    int32_t target = division_target(division);
    out[ECON_TV] = mul_div(target, 25, 100);
    out[ECON_SPONSOR] = mul_div(target, 15, 100);
    if (position >= 1 && position <= LEAGUE_TEAMS) {
        /* champion 20% of the target, falling linearly to 2% for the bottom club */
        out[ECON_LEAGUE_PRIZE] = mul_div(target, 2000 - (1800 * (position - 1)) / (LEAGUE_TEAMS - 1), 10000);
    }
    if (promoted && division > 0) out[ECON_PROMOTION] = mul_div(division_target(division - 1), 15, 100);
    int32_t total = 0;
    for (int32_t i = 0; i < ECON_CATEGORIES; ++i) total = clamp_add(total, out[i]);
    return total;
}

static void econ_tally(int32_t category, int32_t amount) {
    if (category < 0 || category >= ECON_CATEGORIES || !amount) return;
    if (g_ext.econ.season != g_market.season) {
        for (int32_t i = 0; i < ECON_CATEGORIES; ++i) {
            g_ext.econ.last_season[i] = g_ext.econ.this_season[i];
            g_ext.econ.this_season[i] = 0;
        }
        g_ext.econ.season = g_market.season;
    }
    int64_t sum = (int64_t)g_ext.econ.this_season[category] + amount;
    if (sum > 0x7FFFFFFF) sum = 0x7FFFFFFF;
    if (sum < -0x7FFFFFFF) sum = -0x7FFFFFFF;
    g_ext.econ.this_season[category] = (int32_t)sum;
    g_market_dirty = 1;
}

/* Pay a match to the user club: coins, season tallies and the last-match breakdown. `credit` is 0
 * when the caller (the real post-match-screen hook) will let the stock SetMatchCredits/AddCredits
 * flow do the crediting itself, so the reward screen's coin counter animates and stays in sync with
 * the game's own pending-credits bookkeeping; the harness's simulated matches pass 1 (no such screen
 * exists there, so this function must credit directly). */
static int32_t user_match_payout(uint32_t base, const MatchFacts *facts, int32_t credit) {
    int32_t rows[ECON_CATEGORIES];
    int32_t total = econ_match_income(facts, rows);
    for (int32_t i = 0; i < ECON_CATEGORIES; ++i) econ_tally(i, rows[i]);
    for (int32_t i = 0; i < ECON_MATCH_ROWS; ++i) g_ext.econ.last_match[i] = rows[i];
    g_ext.econ.last_match_turn = g_market.current_turn;
    g_ext.econ.division = facts->division;
    if (credit && total > 0) set_user_coins(base, clamp_add(user_coins(base), total));
    return total;
}

static int32_t user_season_end_payout(uint32_t base, int32_t division, int32_t position, int32_t promoted) {
    if (g_ext.econ.season_end_paid == g_market.season + 1) return 0;
    int32_t rows[ECON_CATEGORIES];
    int32_t total = econ_season_end(division, position, promoted, rows);
    for (int32_t i = 0; i < ECON_CATEGORIES; ++i) econ_tally(i, rows[i]);
    g_ext.econ.season_end_paid = g_market.season + 1;
    if (total > 0) set_user_coins(base, clamp_add(user_coins(base), total));
    return total;
}

/*
 * Club books, part 2 (save version 0xB5): every coin the user club earns or spends outside the
 * market lands in a category, so the season books add up to the coin balance. Categories 0-15 live
 * in g_ext.econ (0xB4), 16-31 here, so a v6 save loads unchanged.
 */
#define MARKET_EXT2_SAVE_VERSION 0xB5
#define ECON2_CATEGORIES 16
enum EconCategory2 {
    ECON_AWARDS = 16,         /* achievements, season objectives, stock cup/friendly bonuses (income) */
    ECON_MEDICAL = 17,        /* treating injured players */
    ECON_FITNESS = 18,        /* energy refills */
    ECON_TRAINING = 19,       /* player development sessions */
    ECON_SCOUTING = 20,       /* scouting sessions */
    ECON_STADIUM = 21,        /* stadium construction */
    ECON_CUSTOMISATION = 22,  /* kits and pitch patterns */
    ECON_CREATE_PLAYER = 23,  /* created players (net of refunds when deleted) */
    ECON_FRIENDLY_FEES = 24,  /* friendly entry fees */
    ECON_EXTRA_INCOME = 25,   /* welcome bonus, rewarded videos, shares, purchases */
    ECON_OTHER_SPEND = 26,    /* anything else the game charges */
    ECON_ALL = 32
};
typedef struct {
    int32_t season;                          /* season the running tallies belong to */
    int32_t this_season[ECON2_CATEGORIES];   /* coins by category - 16, signed */
    int32_t last_season[ECON2_CATEGORIES];
    int32_t last_event_category;             /* the last spend/income routed here (for notices) */
    int32_t last_event_amount;
    int32_t reserved[61];
} MarketExt2;
static MarketExt2 g_ext2 __attribute__((aligned(8)));

static void clear_ext2(void) {
    uint64_t *words = (uint64_t *)&g_ext2;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_ext2) / sizeof(uint64_t)); ++i) words[i] = 0;
    g_ext2.last_event_category = -1;
}

static int32_t sat_add(int32_t a, int32_t b) {
    int64_t sum = (int64_t)a + b;
    if (sum > 0x7FFFFFFF) sum = 0x7FFFFFFF;
    if (sum < -0x7FFFFFFF) sum = -0x7FFFFFFF;
    return (int32_t)sum;
}

/* Book any category 0-31 against the current season. */
static void book(int32_t category, int32_t amount) {
    if (!amount || category < 0 || category >= ECON_ALL) return;
    if (category < ECON_CATEGORIES) { econ_tally(category, amount); return; }
    if (g_ext2.season != g_market.season) {
        for (int32_t i = 0; i < ECON2_CATEGORIES; ++i) {
            g_ext2.last_season[i] = g_ext2.this_season[i];
            g_ext2.this_season[i] = 0;
        }
        g_ext2.season = g_market.season;
    }
    g_ext2.this_season[category - ECON_CATEGORIES] = sat_add(g_ext2.this_season[category - ECON_CATEGORIES], amount);
    g_ext2.last_event_category = category;
    g_ext2.last_event_amount = amount;
    g_market_dirty = 1;
}

/* Season total of one category (this season or the last one). */
static int32_t book_total(int32_t category, int32_t last) {
    if (category < 0 || category >= ECON_ALL) return 0;
    if (category < ECON_CATEGORIES) {
        if (g_ext.econ.season != g_market.season) return last ? g_ext.econ.this_season[category] : 0;
        return last ? g_ext.econ.last_season[category] : g_ext.econ.this_season[category];
    }
    int32_t k = category - ECON_CATEGORIES;
    if (g_ext2.season != g_market.season) return last ? g_ext2.this_season[k] : 0;
    return last ? g_ext2.last_season[k] : g_ext2.this_season[k];
}

static const char *book_label(int32_t category) {
    switch (category) {
    case ECON_PRIZE: return "Match prize money";
    case ECON_GATE: return "Gate receipts";
    case ECON_BONUS: return "Goal & clean sheet bonuses";
    case ECON_CUP: return "Cup prize money";
    case ECON_TV: return "TV rights";
    case ECON_SPONSOR: return "Sponsorship";
    case ECON_LEAGUE_PRIZE: return "League prize";
    case ECON_PROMOTION: return "Promotion bonus";
    case ECON_SALES: return "Player sales";
    case ECON_PURCHASES: return "Player purchases";
    case ECON_WAGES: return "Wages";
    case ECON_AWARDS: return "Awards & objectives";
    case ECON_MEDICAL: return "Medical treatment";
    case ECON_FITNESS: return "Fitness (energy refills)";
    case ECON_TRAINING: return "Training";
    case ECON_SCOUTING: return "Scouting";
    case ECON_STADIUM: return "Stadium construction";
    case ECON_CUSTOMISATION: return "Kits & pitch";
    case ECON_CREATE_PLAYER: return "Created players";
    case ECON_FRIENDLY_FEES: return "Friendly fees";
    case ECON_EXTRA_INCOME: return "Other income";
    case ECON_OTHER_SPEND: return "Other spending";
    default: return "";
    }
}

/*
 * Price schedule. CConfig::GetVar(v) returns CConfig::ms_iVars[v] (base + 0x75BA54), so the mod sets
 * a cost by writing that array: the game then shows and charges the same number everywhere. Each
 * entry is either an absolute coin amount or a percentage of whatever the config loaded (bundled or
 * server file). The loaded value is remembered and re-derived whenever the config is reloaded (the
 * value in the array is no longer the one the mod wrote), so percentages never compound.
 *
 * Calibration against the v6 income (about 1,450 coins a season in the Academy, 4,300+ in Elite, wages
 * 12% of player value a season): running costs stay a few percent of a season's income, investments
 * (stadium, training) cost like investments, and nothing is priced to push the coin shop.
 */
#define CONFIG_VARS 0x75BA54
#define CONFIG_VAR_COUNT 436
enum { PRICE_ABSOLUTE = 0, PRICE_PERCENT = 1 };
typedef struct { int16_t var; int16_t mode; int32_t value; } PriceRule;
static const PriceRule k_price_rules[] = {
    /* disable dynamic-difficulty adjustments for player, stadium, and training purchases */
    {0x013, PRICE_ABSOLUTE, 0},
    {0x014, PRICE_ABSOLUTE, 0},
    {0x015, PRICE_ABSOLUTE, 0},
    /* medical: per week out injured, and a full energy refill (was 30 and 50) */
    {0x01A, PRICE_ABSOLUTE, 40},
    {0x01B, PRICE_ABSOLUTE, 20},
    /* training: cheapest and dearest session (was 20 and 200); stat gains raise the player's value */
    {0x04A, PRICE_ABSOLUTE, 30},
    {0x04B, PRICE_ABSOLUTE, 300},
    /* scouting: first session, each extra session, scouted-player surcharge, free-session chance */
    {0x177, PRICE_ABSOLUTE, 60},
    {0x178, PRICE_ABSOLUTE, 40},
    {0x179, PRICE_ABSOLUTE, 0},      /* the market prices players itself; scouting must not add 20% */
    {0x17B, PRICE_ABSOLUTE, 25},
    /* stadium sections (corners, ends, sides: min and max cost): 2.5x, since capacity now earns gate */
    {0x051, PRICE_PERCENT, 250}, {0x052, PRICE_PERCENT, 250},
    {0x054, PRICE_PERCENT, 250}, {0x055, PRICE_PERCENT, 250},
    {0x057, PRICE_PERCENT, 250}, {0x058, PRICE_PERCENT, 250},
    /* objectives: per season objective and per match objective (easy, medium, hard) */
    {0x009, PRICE_ABSOLUTE, 100},
    {0x00A, PRICE_ABSOLUTE, 20},
    {0x00B, PRICE_ABSOLUTE, 40},
    {0x00C, PRICE_ABSOLUTE, 60},
    /* rewarded videos: no coins for watching adverts */
    {0x007, PRICE_ABSOLUTE, 0},
};
#define PRICE_RULE_COUNT ((int32_t)(sizeof(k_price_rules) / sizeof(k_price_rules[0])))
static int32_t g_price_loaded[PRICE_RULE_COUNT];     /* config value before the mod touched it */
static int32_t g_price_written[PRICE_RULE_COUNT];
static uint8_t g_price_known[PRICE_RULE_COUNT];

static int32_t *config_vars(uint32_t base) {
    return base ? (int32_t *)(uintptr_t)(base + CONFIG_VARS) : (int32_t *)0;
}

static void apply_price_schedule(uint32_t base) {
    int32_t *vars = config_vars(base);
    if (!vars) return;
    for (int32_t i = 0; i < PRICE_RULE_COUNT; ++i) {
        const PriceRule *rule = &k_price_rules[i];
        if (rule->var < 0 || rule->var >= CONFIG_VAR_COUNT) continue;
        int32_t current = vars[rule->var];
        if (!g_price_known[i] || current != g_price_written[i]) {
            g_price_loaded[i] = current;           /* first sight, or the config was reloaded */
            g_price_known[i] = 1;
        }
        int32_t value = rule->mode == PRICE_ABSOLUTE ? rule->value
                                                    : mul_div(g_price_loaded[i], rule->value, 100);
        if (rule->mode == PRICE_PERCENT && g_price_loaded[i] > 0 && value <= 0) value = 1;
        vars[rule->var] = value;
        g_price_written[i] = value;
    }
}

/* Current configured cost (after the schedule) for the price list screen. */
static int32_t config_value(uint32_t base, int32_t var) {
    int32_t *vars = config_vars(base);
    return vars && var >= 0 && var < CONFIG_VAR_COUNT ? vars[var] : 0;
}


/*
 * Routing of the game's own coin movements into the books. The cave hooks the two profile primitives
 * (CMyProfile::SubtractCredits / AddCredits) and passes the amount and the original caller's return
 * address (as a library offset); the caller decides the category. The market's own coin changes go
 * through CCredits::SetCredits and match income is booked by career_market_on_match_awards, so neither
 * is booked twice (the match credit caller is skipped below).
 */
typedef struct { uint32_t return_offset; int32_t category; } CoinRoute;
/* Return addresses of every call to the two primitives (data/COIN_FLOWS.md sections 1 and 3). */
static const CoinRoute k_spend_routes[] = {
    {0x210556, ECON_PURCHASES},      /* stock SignPlayerAttempt (bypassed by the market; kept for safety) */
    {0x23E6E2, ECON_MEDICAL},        /* CFETeamManagement::HealSelected: injury or energy (split below) */
    {0x24AB2C, ECON_CREATE_PLAYER},  /* CFEMsgCreatePlayer */
    {0x24E3D0, ECON_TRAINING},       /* CFEMsgPlayerDevSelect: training session */
    {0x2504AC, ECON_SCOUTING},       /* CFEMsgBoxScoutPlayer::ScoutPlayersCB (direct profile call) */
    {0x26BFC0, ECON_FRIENDLY_FEES},  /* friendly entry fee */
    {0x2718F6, ECON_STADIUM},        /* stadium upgrades (batch) */
    {0x271974, ECON_CUSTOMISATION},  /* pitch pattern */
    {0x271E72, ECON_STADIUM},        /* single stadium section */
    {0x37BC42, ECON_CUSTOMISATION},  /* kit design unlock */
    {0x381276, ECON_OTHER_SPEND},    /* server reimbursement claw-back */
};
static const CoinRoute k_income_routes[] = {
    {0x203AF6, ECON_EXTRA_INCOME},   /* rewarded video (disabled by build_mod) */
    {0x23F8C6, ECON_SALES},          /* stock player sale (bypassed by the market) */
    {0x249A96, ECON_AWARDS},         /* achievements and season objectives */
    {0x263A16, ECON_EXTRA_INCOME},   /* come-back reward */
    {0x263B48, ECON_EXTRA_INCOME},   /* welcome bonus */
    {0x264252, ECON_EXTRA_INCOME},   /* share rewards */
    {0x26429A, ECON_EXTRA_INCOME},
    {0x269234, ECON_EXTRA_INCOME},   /* easter-egg team name */
    {0x287564, ECON_EXTRA_INCOME},   /* in-app purchase */
    {0x37803C, ECON_EXTRA_INCOME},   /* Facebook login reward */
    {0x381256, ECON_EXTRA_INCOME},   /* server reimbursement */
};
#define MATCH_CREDIT_RETURN 0x298CBA        /* CFE::Process: the match total, booked already */
#define COIN_ROUTE_COUNT(t) ((int32_t)(sizeof(t) / sizeof((t)[0])))

static int32_t route_category(const CoinRoute *routes, int32_t count, uint32_t return_offset, int32_t fallback) {
    for (int32_t i = 0; i < count; ++i) {
        if (routes[i].return_offset == (return_offset & ~1u)) return routes[i].category;
    }
    return fallback;
}

/* amount > 0: coins spent. caller: return address offset from the library base. */
__attribute__((visibility("default")))
int32_t career_market_on_spend(int32_t amount, int32_t caller, uint32_t base) {
    if (!base || amount <= 0 || g_market.magic != MARKET_MAGIC) return 0;
    int32_t category = route_category(k_spend_routes, COIN_ROUTE_COUNT(k_spend_routes), (uint32_t)caller,
                                      ECON_OTHER_SPEND);
    /* HealSelected charges both: an injury costs HealPlayerCost per week plus a full refill, an energy
     * refill alone costs at most EnergyMaxCost */
    if (category == ECON_MEDICAL && amount <= config_value(base, 0x01B)) category = ECON_FITNESS;
    int32_t coins = user_coins(base);
    if (amount > coins) amount = coins;       /* the game clamps the balance at 0 */
    book(category, -amount);
    return category;
}

__attribute__((visibility("default")))
int32_t career_market_on_income(int32_t amount, int32_t caller, uint32_t base) {
    if (!base || amount <= 0 || g_market.magic != MARKET_MAGIC) return 0;
    if (((uint32_t)caller & ~1u) == MATCH_CREDIT_RETURN) return -1;
    int32_t category = route_category(k_income_routes, COIN_ROUTE_COUNT(k_income_routes), (uint32_t)caller,
                                      ECON_AWARDS);
    book(category, amount);
    return category;
}

typedef int (*TeamPredicateFn)(int32_t);
static uint32_t g_base_for_predicates;

/* Real clubs only: IsValidSearchTeam also accepts national teams (league 9-13), all-star/misc
 * sides (17) and classic teams, whose players are duplicates of club players. */
static int32_t is_market_club(IsValidSearchTeamFn is_valid_team, int32_t team_id) {
    if (team_id == USER_TEAM_ID) return 1;
    if (is_valid_team(team_id) == 0) return 0;
    uint32_t base = g_base_for_predicates;
    if (!base) return 1;
    TeamPredicateFn international = (TeamPredicateFn)(base + 0x20C0A9);
    TeamPredicateFn miscellaneous = (TeamPredicateFn)(base + 0x20C08F);
    TeamPredicateFn classic = (TeamPredicateFn)(base + 0x20C049);
    return !international(team_id) && !miscellaneous(team_id) && !classic(team_id);
}

static int32_t make_account(GetTeamValueTotalFn get_team_value, int32_t team_id) {
    int32_t slot = -1;
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        if (g_market.clubs[i].team_id < 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;
    ClubAccount *club = &g_market.clubs[slot];
    int32_t value = get_team_value(team_id);
    if (value < 1000) value = 1000;
    club->team_id = team_id;
    club->cash = clamp_add(value / 2, 500);
    club->transfer_budget = value / 8;
    club->wage_budget = value / 16 + 100;
    club->payroll = 0;
    club->squad_value = value;
    club->strength = 0;
    club->window_flags = 0xFFFFFFFFu;
    g_buy_attempt_window[slot] = -1;
    ++g_market.club_count;
    g_market_dirty = 1;
    return slot;
}

static void sync_accounts(uint32_t base) {
    GetLinkCountFn get_count = (GetLinkCountFn)(base + 0x20C029);
    GetTeamLinkByIndexFn get_link = (GetTeamLinkByIndexFn)(base + 0x20BF6D);
    GetTeamValueTotalFn get_team_value = (GetTeamValueTotalFn)(base + 0x20C3A1);
    IsValidSearchTeamFn is_valid_team = (IsValidSearchTeamFn)(base + 0x210885);
    g_base_for_predicates = base;
    int32_t link_count = get_count();
    if (link_count < 0) link_count = 0;
    if (link_count > MAX_CLUBS) link_count = MAX_CLUBS;

    for (int32_t i = 0; i < MAX_CLUBS; ++i) g_account_seen[i] = 0;
    int32_t active = 0;
    for (int32_t i = 0; i < link_count; ++i) {
        uint8_t *link = (uint8_t *)get_link(i);
        if (!link) continue;
        int32_t team_id = *(int32_t *)(link + LINK_TEAM_ID);
        if (!is_market_club(is_valid_team, team_id)) continue;
        int32_t index = find_account(team_id);
        if (index < 0) index = make_account(get_team_value, team_id);
        if (index < 0) continue;
        g_account_seen[index] = 1;
        ++active;
    }
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        if (g_market.clubs[i].team_id >= 0 && !g_account_seen[i]) {
            g_market.clubs[i].team_id = -1;
            g_market.clubs[i].cash = 0;
            g_market.clubs[i].transfer_budget = 0;
            g_market.clubs[i].wage_budget = 0;
            g_market.clubs[i].payroll = 0;
            g_market.clubs[i].squad_value = 0;
            g_market.clubs[i].strength = 0;
            g_market.clubs[i].window_flags = 0xFFFFFFFFu;
            g_buy_attempt_window[i] = -1;
            g_finance_ledger_meta[i].head = 0;
            g_finance_ledger_meta[i].count = 0;
            g_market_dirty = 1;
        }
    }
    g_market.club_count = active;
    g_market.user_team_id = USER_TEAM_ID;
    int32_t user_index = find_account(USER_TEAM_ID);
    if (user_index >= 0) {
        ClubAccount *user = &g_market.clubs[user_index];
        user->cash = user_coins(base);
        user->transfer_budget = user->cash;
        /* account_wage_room = wage_budget - payroll; add payroll back so a new wage only has to fit
         * the cash on hand, not the cash on hand *and* a whole season of the existing squad's wages.
         * Wages are already paid incrementally out of ongoing cash (accrue_finances), gracefully
         * capped at whatever is available each turn, so there is no need to pre-fund a full season. */
        user->wage_budget = clamp_add(user->cash, user->payroll);
    }
}

static int32_t load_player_info(uint32_t base, int32_t player_id, PlayerInfo *info) {
    GetPlayerInfoSimpleFn get_info = (GetPlayerInfoSimpleFn)(base + 0x20DA0D);
    for (uint32_t i = 0; i < sizeof(info->bytes); ++i) info->bytes[i] = 0;
    return get_info(info, player_id, 0, 0);
}

static int32_t native_player_value(uint32_t base, PlayerInfo *info) {
    GetPlayerValueFn get_value = (GetPlayerValueFn)(base + 0x212B3D);
    int32_t value = get_value(info, -1, -1, 0, 1);
    return value > 0 ? value : 0;
}

static int32_t native_player_rating(uint32_t base, PlayerInfo *info) {
    GetPlayerRatingFn get_rating = (GetPlayerRatingFn)(base + 0x2B35D1);
    int32_t rating = get_rating(info);
    if (rating < 0) return 0;
    if (rating > 100) return 100;
    return rating;
}

static void top_insert(int32_t account_index, int32_t position, int32_t rating) {
    int32_t *top = g_top_rating[account_index][position];
    for (int32_t k = 0; k < 4; ++k) {
        if (rating > top[k]) {
            for (int32_t m = 3; m > k; --m) top[m] = top[m - 1];
            top[k] = rating;
            return;
        }
    }
}

/* Best-XI strength: 1 GK, 4 DEF, 4 MID, 2 FWD; an empty slot counts as rating 30. */
static int32_t best_xi_strength(int32_t account_index) {
    int32_t sum = 0;
    for (int32_t position = 0; position < 4; ++position) {
        for (int32_t k = 0; k < k_starters[position]; ++k) {
            int32_t rating = g_top_rating[account_index][position][k];
            sum += rating > 0 ? rating : 30;
        }
    }
    return sum / 11;
}

/* Rating of the weakest current starter at a position (0 when the position lacks starters). */
static int32_t starter_threshold(int32_t account_index, int32_t position) {
    return g_top_rating[account_index][position][k_starters[position] - 1];
}

/* Rebuild one club's position counts, best ratings, strength, value and payroll from the cache. */
static void recompute_club_profile(int32_t account_index) {
    if (account_index < 0 || account_index >= MAX_CLUBS) return;
    ClubAccount *club = &g_market.clubs[account_index];
    for (int32_t p = 0; p < 4; ++p) {
        g_group_count[account_index][p] = 0;
        g_group_rating_sum[account_index][p] = 0;
        for (int32_t k = 0; k < 4; ++k) g_top_rating[account_index][p][k] = 0;
    }
    g_total_count[account_index] = 0;
    g_total_rating_sum[account_index] = 0;
    int32_t value = 0, payroll = 0;
    for (int32_t i = 0; i < g_player_count; ++i) {
        MarketPlayer *player = &g_players[i];
        if (player->owner_index != account_index) continue;
        if (player->position >= 0 && player->position <= 3) {
            ++g_group_count[account_index][player->position];
            g_group_rating_sum[account_index][player->position] += player->rating;
            top_insert(account_index, player->position, player->rating);
        }
        ++g_total_count[account_index];
        g_total_rating_sum[account_index] += player->rating;
        value = clamp_add(value, player->value);
        payroll = clamp_add(payroll, player->wage);
    }
    club->squad_value = value;
    club->payroll = payroll;
    club->strength = g_total_count[account_index] > 0 ? best_xi_strength(account_index) : 0;
}

/* Reputation 1..100 from each club's squad-value rank among active market clubs. */
static void compute_reputation(void) {
    int32_t active = 0;
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        g_club_rep[i] = 0;
        if (g_market.clubs[i].team_id >= 0) ++active;
    }
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        ClubAccount *club = &g_market.clubs[i];
        if (club->team_id < 0) continue;
        int32_t below = 0;
        for (int32_t j = 0; j < MAX_CLUBS; ++j) {
            ClubAccount *other = &g_market.clubs[j];
            if (j == i || other->team_id < 0) continue;
            if (other->squad_value < club->squad_value ||
                (other->squad_value == club->squad_value && j < i)) ++below;
        }
        g_club_rep[i] = active > 1 ? 1 + (below * 99) / (active - 1) : 50;
        /* the user's standing also comes from the division it plays in */
        if (club->team_id == USER_TEAM_ID && g_ext.econ.division >= 0 && g_ext.econ.division < 6) {
            static const int32_t division_rep[6] = {80, 65, 50, 35, 20, 5};
            if (g_club_rep[i] < division_rep[g_ext.econ.division]) g_club_rep[i] = division_rep[g_ext.econ.division];
        }
    }
}

static void compute_average_revenue(void);
static void compute_market_index(void);

/* squad_caps.py (v33) lets the user squad grow past the 32 slots of the link: players 33..64 live in an
 * overflow table and are reached through the hooked CTeamLineup::GetID of the season lineup. The hook
 * replaces GetID's first instruction (stock `cmp r1, #0x1f`) with a Thumb-2 b.w into CAVE3. */
#define SEASON_LINEUP 0x84AA94            /* MP_cMyProfile + 0x14 + 0x6e0 (CTeamManagement) + 0x140 */
#define LINEUP_GET_ID 0x2F06D8
#define USER_SQUAD_HOOKED 62              /* squad_caps max 64, two slots of headroom like SQUAD_MAX */
typedef int (*LineupGetIDFn)(void *, int32_t);
static int32_t g_user_squad_max = SQUAD_MAX;

static int32_t squad_overflow_hooked(uint32_t base) {
    uint16_t first = *(volatile uint16_t *)(uintptr_t)(base + LINEUP_GET_ID);
    return (first & 0xF800) == 0xF000;
}

static int32_t squad_max_for(int32_t account_index) {
    return g_market.clubs[account_index].team_id == USER_TEAM_ID ? g_user_squad_max : SQUAD_MAX;
}

static int32_t pos_max_for(int32_t account_index, int32_t position) {
    int32_t max = k_pos_max[position];
    return g_market.clubs[account_index].team_id == USER_TEAM_ID ? max * g_user_squad_max / SQUAD_MAX : max;
}

static void cache_player(uint32_t base, int32_t player_id, int32_t team_id, int32_t account_index, int32_t *used) {
    if (player_id < 0 || player_id > 0xFFFF) return;
    if (!load_player_info(base, player_id, &g_player_info)) return;
    int32_t data_id = *(uint16_t *)&g_player_info.bytes[0];
    if (data_id != player_id) return;
    int32_t value = native_player_value(base, &g_player_info);
    int32_t rating = native_player_rating(base, &g_player_info);
    int32_t position = (int8_t)g_player_info.bytes[0x7F];
    int32_t wage = contract_wage(player_id, team_id, g_market.season, club_base_wage(team_id, value));
    MarketPlayer *player = &g_players[(*used)++];
    player->player_id = player_id;
    player->owner_id = team_id;
    player->owner_index = account_index;
    player->position = position;
    player->rating = rating;
    player->value = value;
    player->wage = wage;
    player->listed_for_sale = user_player_is_listed(player_id);
}

static void build_player_cache(uint32_t base) {
    GetLinkCountFn get_count = (GetLinkCountFn)(base + 0x20C029);
    int32_t overflow = squad_overflow_hooked(base);
    g_user_squad_max = overflow ? USER_SQUAD_HOOKED : SQUAD_MAX;
    GetTeamLinkByIndexFn get_link = (GetTeamLinkByIndexFn)(base + 0x20BF6D);
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        for (int32_t p = 0; p < 4; ++p) {
            g_group_count[i][p] = 0;
            g_group_rating_sum[i][p] = 0;
        }
        g_total_count[i] = 0;
        g_total_rating_sum[i] = 0;
        g_market.clubs[i].squad_value = 0;
        g_market.clubs[i].payroll = 0;
        g_market.clubs[i].strength = 0;
    }

    int32_t link_count = get_count();
    if (link_count < 0) link_count = 0;
    if (link_count > MAX_CLUBS) link_count = MAX_CLUBS;
    int32_t used = 0;
    g_player_count = 0;
    for (int32_t i = 0; i < link_count && used < MAX_PLAYERS; ++i) {
        uint8_t *link = (uint8_t *)get_link(i);
        if (!link) continue;
        int32_t team_id = *(int32_t *)(link + LINK_TEAM_ID);
        int32_t account_index = find_account(team_id);
        if (account_index < 0) continue;
        int32_t count = *(int32_t *)(link + LINK_PLAYER_COUNT);
        if (count < 0) continue;
        if (count > 32) count = 32;
        uint32_t *ids = (uint32_t *)(link + LINK_PLAYER_IDS);
        for (int32_t j = 0; j < count && used < MAX_PLAYERS; ++j)
            cache_player(base, (int32_t)ids[j], team_id, account_index, &used);
        /* the link's 32 slots mirror lineup slots 0..31; the hooked GetID returns -1 past the end */
        if (team_id == USER_TEAM_ID && overflow) {
            LineupGetIDFn get_id = (LineupGetIDFn)(base + LINEUP_GET_ID + 1);
            for (int32_t j = 32; j < 64 && used < MAX_PLAYERS; ++j) {
                int32_t player_id = get_id((void *)(uintptr_t)(base + SEASON_LINEUP), j);
                if (player_id < 0 || player_id >= 0xFFDE) break;
                cache_player(base, player_id, team_id, account_index, &used);
            }
        }
    }
    g_player_count = used;
    for (int32_t i = 0; i < 0x10000; ++i) g_player_slot_by_id[i] = 0;
    for (int32_t i = 0; i < used; ++i) g_player_slot_by_id[g_players[i].player_id & 0xFFFF] = (uint16_t)(i + 1);
    ++g_price_epoch;
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        if (g_market.clubs[i].team_id >= 0) recompute_club_profile(i);
    }
    compute_reputation();
    compute_average_revenue();
    compute_market_index();
}

/* Annual revenue: bigger clubs earn more, smaller clubs earn relatively more per unit of squad
 * value (they live off selling and cheaper squads), with a floor so no club starves. */
static int32_t annual_revenue(int32_t account_index) {
    ClubAccount *club = &g_market.clubs[account_index];
    int32_t rep = g_club_rep[account_index] > 0 ? g_club_rep[account_index] : 50;
    int32_t pct100 = REVENUE_BASE_PCT * 100 + REVENUE_SMALL_PCT * (100 - rep);
    int32_t revenue = mul_div(club->squad_value, pct100, 10000);
    int32_t floor = mul_div(g_avg_revenue, REVENUE_FLOOR_PCT, 100);
    return revenue > floor ? revenue : floor;
}

static void compute_average_revenue(void) {
    int64_t sum = 0;
    int32_t count = 0;
    g_avg_revenue = 0;
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        if (g_market.clubs[i].team_id < 0) continue;
        sum += annual_revenue(i);
        ++count;
    }
    g_avg_revenue = count > 0 ? (int32_t)(sum / count) : 0;
}

static int32_t cash_reserve(const ClubAccount *club) {
    if (club->team_id == USER_TEAM_ID) return 0;
    return mul_div(club->payroll, CASH_RESERVE_PCT, 100);
}

/* Season planning: the board releases part of the free cash as transfer budget
 * and sets the wage budget from revenue. Unspent budget simply stays in cash. */
static void plan_club_budget(int32_t account_index) {
    ClubAccount *club = &g_market.clubs[account_index];
    if (club->team_id < 0 || club->team_id == USER_TEAM_ID) return;
    int32_t revenue = annual_revenue(account_index);
    int32_t wage_budget = mul_div(revenue, WAGE_BUDGET_PCT, 100);
    /* never force an immediate sale just to meet the budget: allow the current bill plus 5% */
    int32_t current = clamp_add(club->payroll, mul_div(club->payroll, 5, 100));
    club->wage_budget = wage_budget > current ? wage_budget : current;
    int32_t reserve = cash_reserve(club);
    int32_t free_cash = club->cash > reserve ? club->cash - reserve : 0;
    int32_t rep = g_club_rep[account_index];
    int32_t spend_pct = SPEND_PCT_MIN + ((SPEND_PCT_MAX - SPEND_PCT_MIN) * rep) / 100;
    if (club->team_id == USER_TEAM_ID) spend_pct = SPEND_PCT_MAX;   /* the user is his own board */
    int32_t previous = club->transfer_budget;
    club->transfer_budget = mul_div(free_cash, spend_pct, 100);
    if (club->transfer_budget > club->cash) club->transfer_budget = club->cash;
    if (club->transfer_budget != previous) {
        record_finance(account_index, CM_FINANCE_TRANSFER_ALLOCATION, 0,
                       club->transfer_budget - previous, 0, -1);
    }
}

static void plan_all_budgets(void) {
    compute_average_revenue();
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        if (g_market.clubs[i].team_id >= 0) plan_club_budget(i);
    }
}

/* Season rollover: cash far beyond what the club can use goes into the stadium and facilities, so
 * club wealth stays tied to club size instead of piling up when the market is quiet. */
#define CASH_CAP_REVENUE_PCT 150
#define INVEST_EXCESS_PCT 50
static void invest_excess_cash(void) {
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        ClubAccount *club = &g_market.clubs[i];
        if (club->team_id < 0 || club->team_id == USER_TEAM_ID) continue;
        int32_t cap = mul_div(annual_revenue(i), CASH_CAP_REVENUE_PCT, 100);
        if (club->cash <= cap) continue;
        int32_t invest = mul_div(club->cash - cap, INVEST_EXCESS_PCT, 100);
        if (invest <= 0) continue;
        int32_t budget_before = club->transfer_budget;
        club->cash -= invest;
        if (club->transfer_budget > club->cash) club->transfer_budget = club->cash;
        record_finance(i, CM_FINANCE_INVESTMENT, -invest, club->transfer_budget - budget_before, 0, -1);
    }
}

static void initialize_market(uint32_t base, void *season) {
    clear_state();
    GetSeasonCountFn get_season = (GetSeasonCountFn)(base + 0x36AA89);
    g_market.season = get_season(season);
    ensure_contract_epoch();
    g_market.current_turn = -1;
    g_market.turns_per_season = 38;
    sync_accounts(base);
    build_player_cache(base);
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        ClubAccount *club = &g_market.clubs[i];
        if (club->team_id < 0) continue;
        if (club->payroll < 1) club->payroll = 1;
        int32_t cash_pct100 = OPENING_CASH_PCT * 100 + OPENING_CASH_REP_PCT * g_club_rep[i];
        club->cash = club->team_id == USER_TEAM_ID ? user_coins(base)
                                                   : mul_div(club->squad_value, cash_pct100, 10000);
        club->transfer_budget = club->team_id == USER_TEAM_ID ? club->cash : 0;
    }
    compute_average_revenue();
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        ClubAccount *club = &g_market.clubs[i];
        if (club->team_id < 0) continue;
        /* the floor revenue also guarantees small clubs a minimum float */
        int32_t floor_cash = mul_div(annual_revenue(i), 30, 100);
        if (club->team_id != USER_TEAM_ID && club->cash < floor_cash) club->cash = floor_cash;
        plan_club_budget(i);
        record_finance(i, CM_FINANCE_OPENING, club->cash, club->transfer_budget,
                       club->payroll, -1);
    }
    g_market.magic = MARKET_MAGIC;
    g_market.format = MARKET_FORMAT;
}

static int32_t average_group_rating(int32_t account_index, int32_t position) {
    int32_t count = g_group_count[account_index][position];
    return count > 0 ? g_group_rating_sum[account_index][position] / count : 0;
}

enum { ROLE_SURPLUS = 0, ROLE_ROTATION = 1, ROLE_STARTER = 2, ROLE_KEY = 3 };

/* v36: continuous curves for prices, wages and decisions (no hard cutoffs).
 * lerp_pts: piecewise linear through (xs[i], ys[i]), flat beyond the ends. ramp_pm: 0..1000 between lo and hi. */
static int32_t lerp_pts(int32_t x, const int32_t *xs, const int32_t *ys, int32_t n) {
    if (x <= xs[0]) return ys[0];
    for (int32_t i = 1; i < n; ++i) {
        if (x <= xs[i]) return ys[i - 1] + (ys[i] - ys[i - 1]) * (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
    }
    return ys[n - 1];
}

static int32_t ramp_pm(int32_t x, int32_t lo, int32_t hi) {
    if (x <= lo) return 0;
    if (x >= hi) return 1000;
    return (x - lo) * 1000 / (hi - lo);
}

/* Deterministic 0..999 draw for a decision that should vary smoothly in probability, fixed per
 * player, club and window so the answer does not flicker between screens. */
static int32_t decision_draw(int32_t player_id, int32_t club, int32_t salt) {
    uint32_t x = (uint32_t)player_id * 0x9E3779B1u ^ (uint32_t)(club + 1) * 0x85EBCA6Bu ^
                 (uint32_t)(g_market.window_id + 7) * 0xC2B2AE35u ^ (uint32_t)salt;
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return (int32_t)(x % 1000u);
}

/* Role as weights (permille) instead of four boxes. margin = rating - starter threshold:
 * starter weight ramps in over margin -4..0, surplus ramps in below -4..-8 (4 margin points
 * per player over the target depth), key weight grows with the lead over the club's strength
 * and its best player at the position. surplus + rotation + starter + key = 1000. */
typedef struct { int32_t surplus, rotation, starter, key; } RoleMix;

static RoleMix role_mix(const MarketPlayer *player) {
    RoleMix m = {0, 1000, 0, 0};
    int32_t club = player->owner_index;
    int32_t position = player->position;
    if (club < 0 || club >= MAX_CLUBS || position < 0 || position > 3) return m;
    int32_t threshold = starter_threshold(club, position);
    int32_t margin10 = threshold > 0 ? (player->rating - threshold) * 10 : -40;
    int32_t starter = threshold > 0 ? ramp_pm(margin10, -40, 0) : 0;
    int32_t extra = g_group_count[club][position] - k_pos_target[position];
    int32_t bloat10 = margin10 - (extra > 0 ? 40 * extra : 0);
    if (threshold <= 0) bloat10 = (player->rating - (g_market.clubs[club].strength - 10)) * 10 - (extra > 0 ? 40 * extra : 0);
    int32_t surplus = 1000 - ramp_pm(bloat10, -80, -40);
    if (surplus + starter > 1000) surplus = 1000 - starter;
    int32_t lead = ramp_pm(player->rating - g_market.clubs[club].strength, 0, 8);
    int32_t top = ramp_pm(player->rating - g_top_rating[club][position][0], -3, 1);
    m.key = starter * lead / 1000 * top / 1000;
    m.starter = starter - m.key;
    m.surplus = surplus;
    m.rotation = 1000 - m.surplus - m.starter - m.key;
    return m;
}

/* Blend a per-role table by the role weights. */
static int32_t role_blend(const MarketPlayer *player, const int32_t *table) {
    RoleMix m = role_mix(player);
    return (table[0] * m.surplus + table[1] * m.rotation + table[2] * m.starter + table[3] * m.key) / 1000;
}

/* Contract time left in tenths of a season (0 = expired or ends now), counting the part of this
 * season still to play, so contract effects slide through the season instead of jumping. */
static int32_t contract_tenths_left(int32_t player_id, int32_t club_id) {
    int32_t years = contract_seasons_to_expiry(player_id, club_id, g_market.season);
    if (years < 0) return 0;
    int32_t progress = g_market.turns_per_season > 0 ? g_market.current_turn * 10 / g_market.turns_per_season : 0;
    progress = progress < 0 ? 0 : progress > 10 ? 10 : progress;
    return years * 10 + 10 - progress;
}

/* Price factor for the contract: 45% at expiry, 65% one season out, 80% two, 92% three, 100% from four. */
static int32_t contract_price_pct(int32_t tenths) {
    static const int32_t xs[5] = {0, 10, 20, 30, 40}, ys[5] = {45, 65, 80, 92, 100};
    return lerp_pts(tenths, xs, ys, 5);
}

/* Floor factor for the contract: 45% at expiry, 65% one season out, 80% two, full from three. */
static int32_t contract_floor_pct(int32_t tenths) {
    static const int32_t xs[4] = {0, 10, 20, 30}, ys[4] = {45, 65, 80, 100};
    return lerp_pts(tenths, xs, ys, 4);
}

/* A player's role in his current club: key player, starter, rotation or surplus. */
static int32_t player_role(const MarketPlayer *player) {
    int32_t club = player->owner_index;
    int32_t position = player->position;
    if (club < 0 || club >= MAX_CLUBS || position < 0 || position > 3) return ROLE_ROTATION;
    int32_t threshold = starter_threshold(club, position);
    if (threshold > 0 && player->rating >= threshold) {
        if (player->rating >= g_top_rating[club][position][0] &&
            player->rating >= g_market.clubs[club].strength + 4) return ROLE_KEY;
        return ROLE_STARTER;
    }
    if (g_group_count[club][position] > k_pos_target[position] ||
        player->rating + 8 < threshold) return ROLE_SURPLUS;
    return ROLE_ROTATION;
}

/* 3 = below the position minimum, 1 = below the target depth, 0 = covered. */
static int32_t position_need(int32_t account_index, int32_t position) {
    int32_t count = g_group_count[account_index][position];
    if (count < k_pos_min[position]) return 3;
    if (count < k_pos_target[position]) return 1;
    return 0;
}

/* How much a player would lift the buyer's best XI at his position (can be negative). */
static int32_t starter_improvement(const MarketPlayer *player, int32_t buyer_index) {
    int32_t threshold = starter_threshold(buyer_index, player->position);
    if (threshold <= 0) threshold = g_market.clubs[buyer_index].strength - 10;
    return player->rating - threshold;
}

/* An AI club only lets a player go if it keeps its squad and position minimums. */
static int32_t club_can_release(const MarketPlayer *player) {
    int32_t club = player->owner_index;
    if (club < 0 || club >= MAX_CLUBS) return 0;
    if (g_total_count[club] <= SQUAD_MIN) return 0;
    if (player->position < 0 || player->position > 3) return 1;
    int32_t count = g_group_count[club][player->position];
    if (count <= k_pos_min[player->position]) return 0;
    /* beyond the minimum, only surplus players go before the target depth is reached */
    if (count <= k_pos_target[player->position] && player_role(player) != ROLE_SURPLUS) return 0;
    return 1;
}

/* Clubs do not sell starters to clearly smaller clubs, and starters refuse big steps down. */
static RoleMix role_mix(const MarketPlayer *player);
static int32_t ramp_pm(int32_t x, int32_t lo, int32_t hi);
static int32_t decision_draw(int32_t player_id, int32_t club, int32_t salt);

/* Permille chance his club refuses to sell a starter or key player to this buyer. */
static int32_t move_refusal_pm(const MarketPlayer *player, int32_t buyer_index) {
    RoleMix mix = role_mix(player);
    if (mix.starter + mix.key == 0) return 0;
    int32_t rep_gap = g_club_rep[player->owner_index] - g_club_rep[buyer_index];
    /* the user pays for standing instead (user_price_premium_pct): a dream team starts at the
     * bottom of the value table, so the AI rule would block every starter in the game.
     * v36: the chance a club keeps him rises smoothly with the gap and with how key he is. */
    int32_t refuse;
    if (g_market.clubs[buyer_index].team_id == USER_TEAM_ID)
        refuse = mix.key * ramp_pm(rep_gap, USER_KEY_SALE_REP_GAP - 10, USER_KEY_SALE_REP_GAP + 10) / 1000;
    else
        refuse = (mix.starter * ramp_pm(rep_gap, 3, 13) + mix.key * ramp_pm(rep_gap, -5, 5)) / 1000;
    return refuse;
}

static int32_t starter_move_allowed(const MarketPlayer *player, int32_t buyer_index) {
    return decision_draw(player->player_id, buyer_index, 0x4A02) >= move_refusal_pm(player, buyer_index);
}

/* Extra fee a club wants to let a starter or key player go to a smaller user club. */
static int32_t user_price_premium_pct(const MarketPlayer *player, int32_t buyer_index) {
    if (buyer_index < 0 || g_market.clubs[buyer_index].team_id != USER_TEAM_ID) return 0;
    int32_t rep_gap = g_club_rep[player->owner_index] - g_club_rep[buyer_index];
    if (rep_gap <= 0) return 0;
    RoleMix mix = role_mix(player);     /* v36: starter and key premiums blended by role weight */
    int32_t starter = rep_gap / 3 < 25 ? rep_gap / 3 : 25;
    int32_t key = rep_gap / 2 < 40 ? rep_gap / 2 : 40;
    return (starter * mix.starter + key * mix.key) / 1000;
}

/*
 * Valuation.
 *
 * A player has three prices:
 *  - market value: the game's rating/position value scaled by the market index, i.e. how scarce
 *    players of his position and rating band are right now (supply = players clubs would release,
 *    demand = clubs that would improve with him and can afford him);
 *  - the seller's reservation price: the least his club takes. It depends on his role, contract,
 *    how easily the club can replace him, squad bloat and the club's finances. It is never shown;
 *  - the asking price: what the club quotes, a markup over the reservation that shrinks when the
 *    club wants him gone.
 * Buyers value him by need, how much he lifts their best XI, and how rich they are. Squad value,
 * wages and revenue keep using the stable native value so finances do not swing with the index.
 */
#define BAND_MIN_RATING 50
#define BAND_WIDTH 5
#define BAND_COUNT 10
#define MARKET_INDEX_MIN 88
#define MARKET_INDEX_MAX 122
static int32_t g_market_index[4][BAND_COUNT];      /* this turn's target; the saved index moves a third of the way */
static int32_t g_band_supply[4][BAND_COUNT];
static int32_t g_band_demand[4][BAND_COUNT];

static int32_t rating_band(int32_t rating) {
    int32_t band = (rating - BAND_MIN_RATING) / BAND_WIDTH;
    if (rating < BAND_MIN_RATING) band = 0;
    return band < 0 ? 0 : band >= BAND_COUNT ? BAND_COUNT - 1 : band;
}

static int32_t account_fee_room(const ClubAccount *club);
static int32_t club_can_release(const MarketPlayer *player);

static void compute_market_index(void) {
    static int32_t value_sum[4][BAND_COUNT], value_count[4][BAND_COUNT];
    for (int32_t p = 0; p < 4; ++p) {
        for (int32_t b = 0; b < BAND_COUNT; ++b) {
            value_sum[p][b] = value_count[p][b] = 0;
            g_band_supply[p][b] = g_band_demand[p][b] = 0;
        }
    }
    for (int32_t i = 0; i < g_player_count; ++i) {
        MarketPlayer *player = &g_players[i];
        if (player->position < 0 || player->position > 3 || player->owner_index < 0) continue;
        int32_t b = rating_band(player->rating);
        value_sum[player->position][b] = clamp_add(value_sum[player->position][b], player->value);
        ++value_count[player->position][b];
        int32_t available = player->owner_id == USER_TEAM_ID ? player->listed_for_sale
                                                             : club_can_release(player);
        if (available) ++g_band_supply[player->position][b];
    }
    for (int32_t c = 0; c < MAX_CLUBS; ++c) {
        ClubAccount *club = &g_market.clubs[c];
        if (club->team_id < 0 || club->team_id == USER_TEAM_ID || g_total_count[c] >= SQUAD_MAX) continue;
        int32_t room = account_fee_room(club);
        for (int32_t p = 0; p < 4; ++p) {
            if (g_group_count[c][p] >= k_pos_max[p]) continue;
            int32_t threshold = starter_threshold(c, p);
            if (threshold <= 0) threshold = club->strength - 10;
            int32_t need = position_need(c, p);
            for (int32_t b = 0; b < BAND_COUNT; ++b) {
                if (!value_count[p][b]) continue;
                int32_t mid = BAND_MIN_RATING + BAND_WIDTH * b + BAND_WIDTH / 2;
                int32_t wants = mid - threshold >= 2 || (need > 0 && mid + 10 >= club->strength);
                if (!wants || room < mul_div(value_sum[p][b] / value_count[p][b], 80, 100)) continue;
                ++g_band_demand[p][b];
            }
        }
    }
    for (int32_t p = 0; p < 4; ++p) {
        for (int32_t b = 0; b < BAND_COUNT; ++b) {
            int32_t ratio = (100 * (g_band_demand[p][b] + 4)) / (g_band_supply[p][b] + 4);
            int32_t index = 100 + (ratio - 100) / 3;
            g_market_index[p][b] = index < MARKET_INDEX_MIN ? MARKET_INDEX_MIN
                                 : index > MARKET_INDEX_MAX ? MARKET_INDEX_MAX : index;
        }
    }
}

/* Once per match turn: move the saved index a third of the way towards this turn's target, so
 * prices drift with the market instead of jumping, and stay fixed between screens. */
static void update_market_index(void) {
    for (int32_t p = 0; p < 4; ++p) {
        for (int32_t b = 0; b < BAND_COUNT; ++b) {
            int32_t current = g_ext.market_index[p][b];
            int32_t target = g_market_index[p][b] > 0 ? g_market_index[p][b] : 100;
            if (current < MARKET_INDEX_MIN || current > MARKET_INDEX_MAX) current = 100;
            g_ext.market_index[p][b] = current + (target - current) / 3;
        }
    }
    g_market_dirty = 1;
}

static int32_t band_index(int32_t position, int32_t band) {
    int32_t index = g_ext.market_index[position][band];
    return index >= MARKET_INDEX_MIN && index <= MARKET_INDEX_MAX ? index : 100;
}

/* v36: interpolated between band centres, so a rating point never jumps the price. */
static int32_t market_index_for(const MarketPlayer *player) {
    if (player->position < 0 || player->position > 3) return 100;
    int32_t t = player->rating - BAND_MIN_RATING - BAND_WIDTH / 2;
    if (t <= 0) return band_index(player->position, 0);
    int32_t band = t / BAND_WIDTH, frac = t % BAND_WIDTH;
    if (band >= BAND_COUNT - 1) return band_index(player->position, BAND_COUNT - 1);
    int32_t lo = band_index(player->position, band), hi = band_index(player->position, band + 1);
    return lo + (hi - lo) * frac / BAND_WIDTH;
}

/* Native value scaled by the current scarcity of his position and rating band. */
static int32_t market_value(const MarketPlayer *player) {
    int32_t value = mul_div(player->value > 0 ? player->value : 1, market_index_for(player), 100);
    return value > 0 ? value : 1;
}

/* Least the seller accepts. Used for AI and user sellers alike (the user's AI buyers apply it
 * to the user's players as the user's own valuation). */
static int32_t seller_reservation(const MarketPlayer *player, int32_t seller_index) {
    static const int32_t role_pct[4] = {85, 100, 120, 150};
    ClubAccount *seller = &g_market.clubs[seller_index];
    RoleMix mix = role_mix(player);
    int32_t price = mul_div(market_value(player), role_blend(player, role_pct), 100);
    int32_t tenths = contract_tenths_left(player->player_id, seller->team_id);
    price = mul_div(price, contract_price_pct(tenths), 100);
    int32_t position = player->position;
    if (position >= 0 && position <= 3) {
        /* replaceability: the gap to the next man up, 95% at a 2-point gap to 110% at 6, weighted by
         * how much of a starter he is */
        int32_t next = k_starters[position] < 4 ? g_top_rating[seller_index][position][k_starters[position]] : 0;
        int32_t gap = next > 0 ? player->rating - next : 10;
        static const int32_t gx[2] = {2, 6}, gy[2] = {95, 110};
        int32_t weight = mix.starter + mix.key;
        price = mul_div(price, 1000 * 100 + (lerp_pts(gap, gx, gy, 2) - 100) * weight, 1000 * 100);
        if (g_group_count[seller_index][position] > k_pos_max[position]) price = mul_div(price, 85, 100);
    }
    if (g_total_count[seller_index] > CLEARANCE_SQUAD) price = mul_div(price, 1000 * 100 - 25 * mix.surplus, 1000 * 100);
    if (seller->team_id != USER_TEAM_ID) {
        /* short of cash: 85% at half the reserve, rising to 100% at the reserve */
        int32_t reserve = cash_reserve(seller);
        if (reserve > 0) {
            static const int32_t cx[2] = {50, 100}, cy[2] = {85, 100};
            int32_t ratio = seller->cash <= 0 ? 0 : seller->cash >= reserve ? 100 : mul_div(seller->cash, 100, reserve);
            price = mul_div(price, lerp_pts(ratio, cx, cy, 2), 100);
        }
    }
    /* discounts do not stack below 70% of market value (before the contract effect below) */
    int32_t floor = mul_div(mul_div(market_value(player), 70, 100), contract_floor_pct(tenths), 100);
    if (price < floor) price = floor;
    return price > 0 ? price : 1;
}

/* The quote: a markup over the reservation, small for players the club wants to move on. */
static int32_t asking_markup_pct(const MarketPlayer *player) {
    static const int32_t markup[4] = {100, 108, 112, 120};
    return role_blend(player, markup);
}

static int32_t seller_asking_price(const MarketPlayer *player, int32_t seller_index) {
    return mul_div(seller_reservation(player, seller_index), asking_markup_pct(player), 100);
}

/* Most a buyer would pay: market value plus premiums for a real need and for lifting the best XI,
 * and a little more from clubs with plenty of room in their budget. */
static int32_t buyer_max_price(const MarketPlayer *player, int32_t buyer_index) {
    int32_t improvement = starter_improvement(player, buyer_index);
    int32_t need = player->position >= 0 && player->position <= 3
        ? position_need(buyer_index, player->position) : 0;
    int32_t premium = need * 10;
    if (improvement > 0) premium += (improvement > 10 ? 10 : improvement) * 3;
    int32_t value = market_value(player);
    if (account_fee_room(&g_market.clubs[buyer_index]) > value * 3) premium += 10;
    return mul_div(value, 100 + premium, 100);
}

/* v37d: what AI clubs value one of the user's players at. The same importance premium an AI seller
 * puts on its players (role_blend), but the part above market value shrinks smoothly with the
 * user club's standing (reputation, which has a floor from the division): a key player fetches up
 * to +50% at a top club, about +30% mid-pyramid and +10% at the bottom. */
static int32_t user_sale_value(const MarketPlayer *player) {
    static const int32_t role_pct[4] = {90, 100, 120, 150};
    static const int32_t rx[5] = {1, 20, 50, 80, 100}, ry[5] = {200, 330, 600, 900, 1000};
    int32_t pct = role_blend(player, role_pct);
    if (pct > 100) {
        int32_t owner = player->owner_index;
        int32_t rep = owner >= 0 && owner < MAX_CLUBS ? g_club_rep[owner] : 50;
        pct = 100 + (pct - 100) * lerp_pts(rep, rx, ry, 5) / 1000;
    }
    return mul_div(market_value(player), pct, 100);
}

static int32_t negotiated_fee(int32_t ask, int32_t buyer_max) {
    if (buyer_max < ask) return 0;
    return clamp_add(ask, mul_div(buyer_max - ask, NEGOTIATION_SPLIT_PCT, 100));
}

/* Stars do not drop to much smaller clubs; nobody takes a huge step down. */
/* Permille chance he refuses to join this buyer. */
static int32_t join_refusal_pm(const MarketPlayer *player, int32_t buyer_index) {
    int32_t seller_index = player->owner_index;
    int32_t rep_gap = g_club_rep[seller_index] - g_club_rep[buyer_index];
    int32_t strength_gap = player->rating - g_market.clubs[buyer_index].strength;
    /* the user: coins are the real gate, and a step down costs a higher wage
     * (transfer_wage_demand). Only a much better player at a much bigger club says no. */
    /* v36: refusal chance rises smoothly with both gaps instead of a wall (the draw is fixed per player,
     * club and window) */
    int32_t refuse;
    if (g_market.clubs[buyer_index].team_id == USER_TEAM_ID) {
        refuse = ramp_pm(strength_gap, USER_STAR_STRENGTH_GAP - 6, USER_STAR_STRENGTH_GAP + 6) *
                 ramp_pm(rep_gap, USER_STAR_REP_GAP - 15, USER_STAR_REP_GAP + 15) / 1000;
    } else {
        int32_t step_down = ramp_pm(strength_gap, 8, 16) * ramp_pm(rep_gap, 5, 15) / 1000;
        int32_t big_drop = ramp_pm(rep_gap, 35, 55);
        refuse = step_down > big_drop ? step_down : big_drop;
    }
    return refuse;
}

static int32_t player_will_join(const MarketPlayer *player, int32_t buyer_index) {
    return decision_draw(player->player_id, buyer_index, 0x4A01) >= join_refusal_pm(player, buyer_index);
}

/* Wage a player asks to move: base on value, more to step down, a little less to step up. */
static int32_t transfer_wage_demand(const MarketPlayer *player, int32_t buyer_index,
                                    int32_t seller_index) {
    int32_t demand = club_base_wage(g_market.clubs[buyer_index].team_id, player->value);
    int32_t rep_gap = g_club_rep[seller_index] - g_club_rep[buyer_index];
    /* v36: +0.5% per reputation point down, up to -5% for a step up of 20 or more */
    int32_t rep_pct = rep_gap >= 0 ? 100 + rep_gap / 2 : 100 - (-rep_gap >= 20 ? 5 : -rep_gap / 4);
    demand = mul_div(demand, rep_pct, 100);
    /* playing time: +15% for a clear bench player, down to -5% for a clear starter */
    static const int32_t ix[3] = {-4, 0, 3}, iy[3] = {115, 100, 95};
    demand = mul_div(demand, lerp_pts(starter_improvement(player, buyer_index), ix, iy, 3), 100);
    return demand > 0 ? demand : 1;
}

static int32_t ai_buy_limit(int32_t account_index) {
    int32_t limit = 1;
    if (g_club_rep[account_index] >= 45) ++limit;
    if (g_club_rep[account_index] >= 80) ++limit;
    return limit < MAX_AI_BUYS_PER_WINDOW ? limit : MAX_AI_BUYS_PER_WINDOW;
}

/* Best affordable signing for an AI club: upgrades its weakest starting positions first, then
 * fills thin positions with decent depth. Returns the cached player index or -1. */
static int32_t user_is_kept(int32_t player_id);

static int32_t ai_find_target(int32_t buyer_index, int32_t fee_room, int32_t wage_room,
                              int32_t allow_unlisted_user, int32_t *out_fee, int32_t *out_wage,
                              int32_t *out_max) {
    ClubAccount *buyer = &g_market.clubs[buyer_index];
    if (g_total_count[buyer_index] >= SQUAD_MAX || fee_room <= 0 || wage_room <= 0) return -1;
    int32_t best = -1, best_score = 0, best_fee = 0, best_wage = 0, best_max = 0;
    for (int32_t i = 0; i < g_player_count; ++i) {
        MarketPlayer *player = &g_players[i];
        int32_t seller_index = player->owner_index;
        if (seller_index < 0 || seller_index >= MAX_CLUBS || seller_index == buyer_index) continue;
        int32_t position = player->position;
        if (position < 0 || position > 3 || player->value <= 0) continue;
        if (g_group_count[buyer_index][position] >= k_pos_max[position]) continue;
        ClubAccount *seller = &g_market.clubs[seller_index];
        if (seller->team_id < 0 || (seller->team_id != USER_TEAM_ID && window_sales(seller, g_market.window_id) >= MAX_SALES_PER_WINDOW)) continue;
        int32_t need = position_need(buyer_index, position);
        int32_t improvement = starter_improvement(player, buyer_index);
        if (need == 0 && improvement < 2) continue;
        if (g_total_count[buyer_index] >= AI_SQUAD_COMFORT && need == 0 && improvement < 5) continue;
        int32_t cheap_depth = 0;
        if (improvement < 2 && player->rating + 6 < buyer->strength) {
            /* a thin position may still take a modest player, but only cheaply */
            if (need == 0 || player->rating + 10 < buyer->strength) continue;
            cheap_depth = 1;
        }
        int32_t user_seller = seller->team_id == USER_TEAM_ID;
        int32_t unsolicited = 0;
        if (user_seller) {
            if (user_signing_locked(player->player_id)) continue;
            if (!player->listed_for_sale) {
                if (!allow_unlisted_user || player_role(player) < ROLE_STARTER || user_is_kept(player->player_id)) continue;
                unsolicited = 1;
            }
            if (g_total_count[seller_index] <= USER_SQUAD_MIN) continue;
        } else if (!club_can_release(player) || !starter_move_allowed(player, buyer_index)) {
            continue;
        }
        if (has_active_offer_for_player(player->player_id)) continue;
        if (!player_will_join(player, buyer_index)) continue;
        /* clubs deal between reservation and the buyer's valuation; the quote is for the user */
        int32_t ask = seller_reservation(player, seller_index);
        int32_t ceiling = buyer_max_price(player, buyer_index);
        if (user_seller) {
            /* AI clubs bid for user players around his sale value (importance premium scaled by the
             * user's standing); an unsolicited bid for a starter starts at the full sale value */
            int32_t value = user_sale_value(player);
            ask = mul_div(value, unsolicited ? 100 : 90, 100);
            int32_t cap = mul_div(value, unsolicited ? UNSOLICITED_PREMIUM_PCT : USER_SALE_CEILING_PCT, 100);
            if (ceiling > cap) ceiling = cap;
            if (unsolicited && ceiling < ask) continue;   /* only a club that really wants him bids */
        }
        if (cheap_depth && ask > fee_room / 6) continue;
        int32_t fee = negotiated_fee(ask, ceiling);
        if (fee <= 0) continue;
        if (fee > fee_room) {
            if (ask > fee_room) continue;
            fee = fee_room;
        }
        int32_t wage = transfer_wage_demand(player, buyer_index, seller_index);
        if (wage > wage_room) continue;
        int32_t score = (improvement > 0 ? improvement * 100 : 0) + need * 250 +
                        player->rating * 2 - mul_div(fee, 200, fee_room);
        if (score <= 0) score = 1;
        if (best < 0 || score > best_score) {
            best = i;
            best_score = score;
            best_fee = fee;
            best_wage = wage;
            best_max = ceiling < fee_room ? ceiling : fee_room;
        }
    }
    if (best >= 0) {
        *out_fee = best_fee;
        *out_wage = best_wage;
        if (out_max) *out_max = best_max > best_fee ? best_max : best_fee;
    }
    return best;
}

/*
 * Roster persistence (see data/ROSTER_MECHANICS.md).
 *
 * The game keeps a default link table (DB+0x24, built from the links file) and a live table (DB+0x28).
 * CDataBase::CalculateLinks rebuilds the live table from the default one and keeps only the user
 * team's live link, and the career save stores only the user link. So AI roster moves are made in
 * the default table (the way PopulateDefaultLinksArray applies server roster updates), the default
 * table is saved with the market, and a pristine copy is restored before every load.
 */
#define DB_INSTANCE 0x75CCA4          /* CDataBase::ms_pInstance */
#define DB_OVERRIDE_LINKS 0x1C
#define DB_DEFAULT_LINKS 0x24
#define DB_LINK_COUNT 0x38
#define DB_DEFAULT_SIMPLE 0x90
#define DB_DEFAULT_SIMPLE_COUNT 0x94
#define LINK_STRIDE 0x108             /* 33 u64 words */
#define LINK_WORDS (LINK_STRIDE / 8)
#define MAX_LINKS 300
#define ROSTER_SAVE_VERSION 0xB3

typedef void (*SetOverrideLinksFn)(void *, int32_t);
typedef int (*CanRemovePlayerFn)(int32_t, PlayerInfo *);
typedef int (*CanAddPlayerFn)(int32_t, const PlayerInfo *, int32_t);
typedef void (*VerifyLinkFn)(void *, PlayerInfo *, int32_t, const void *, int32_t, int32_t,
                             void *, int32_t, int32_t);
typedef void *(*GenerateSimpleLinksFn)(void *, int32_t, int32_t *);
typedef void (*SortSimpleLinksFn)(void *, int32_t, int32_t);
typedef void (*DeleteArrayFn)(void *);
typedef void (*CalculateTeamRatingFn)(void *, int32_t, void *);

static uint64_t g_pristine_links[MAX_LINKS * LINK_WORDS] __attribute__((aligned(8)));
static int32_t g_pristine_count;                 /* 0 until captured in this process */
static uint64_t g_loaded_links[MAX_LINKS * LINK_WORDS] __attribute__((aligned(8)));
static uint64_t g_loaded_link_count;             /* serialized; 0 = no roster block */
static uint8_t g_links_dirty;
static uint8_t g_rating_dirty[MAX_CLUBS];

/* The game is 32-bit: its structures hold 4-byte pointers (read them as such, also on test hosts). */
static uint8_t *game_ptr(const void *at) {
    return (uint8_t *)(uintptr_t)*(const uint32_t *)at;
}

static void set_game_ptr(void *at, const void *value) {
    *(uint32_t *)at = (uint32_t)(uintptr_t)value;
}

static uint8_t *db_instance(uint32_t base) {
    return game_ptr((const void *)(uintptr_t)(base + DB_INSTANCE));
}

static int32_t default_links(uint32_t base, uint8_t **links) {
    uint8_t *db = db_instance(base);
    *links = db ? game_ptr(db + DB_DEFAULT_LINKS) : (uint8_t *)0;
    int32_t count = db ? *(int32_t *)(db + DB_LINK_COUNT) : 0;
    if (!*links || count <= 0 || count > MAX_LINKS) return 0;
    return count;
}

static void copy_words(uint64_t *dst, const uint64_t *src, uint32_t words) {
    for (uint32_t i = 0; i < words; ++i) dst[i] = src[i];
}

/* The first call in a process sees the untouched default table: keep it. */
static void capture_pristine_links(uint32_t base) {
    if (g_pristine_count) return;
    uint8_t *links;
    int32_t count = default_links(base, &links);
    if (!count) return;
    copy_words(g_pristine_links, (const uint64_t *)links, (uint32_t)count * LINK_WORDS);
    g_pristine_count = count;
}

/* Same team ids in the same order: a table image can be copied over the live default table. */
static int32_t link_image_matches(const uint8_t *image, int32_t count, const uint8_t *links,
                                  int32_t link_count) {
    if (count != link_count) return 0;
    for (int32_t i = 0; i < count; ++i) {
        const uint8_t *a = image + i * LINK_STRIDE, *b = links + i * LINK_STRIDE;
        int32_t players = *(const int32_t *)(a + 4);
        if (*(const int32_t *)a != *(const int32_t *)b || players < 0 || players > 32) return 0;
    }
    return 1;
}

static void regenerate_default_simple_links(uint32_t base) {
    uint8_t *db = db_instance(base);
    if (!db) return;
    DeleteArrayFn delete_array = (DeleteArrayFn)(base + 0x1C05E0);        /* PLT (ARM): even */
    GenerateSimpleLinksFn generate = (GenerateSimpleLinksFn)(base + 0x20A751);
    SortSimpleLinksFn sort_links = (SortSimpleLinksFn)(base + 0x20A857);
    void *old = game_ptr(db + DB_DEFAULT_SIMPLE);
    if (old) delete_array(old);
    void *simple = generate(game_ptr(db + DB_DEFAULT_LINKS), *(int32_t *)(db + DB_LINK_COUNT),
                            (int32_t *)(db + DB_DEFAULT_SIMPLE_COUNT));
    set_game_ptr(db + DB_DEFAULT_SIMPLE, simple);
    sort_links(simple, *(int32_t *)(db + DB_DEFAULT_SIMPLE_COUNT), 1);
}

/* Rebuild the live table from the (edited) default table and refresh cached team ratings. */
static void flush_roster_changes(uint32_t base) {
    if (!g_links_dirty) return;
    g_links_dirty = 0;
    regenerate_default_simple_links(base);
    CalculateLinksFn calculate_links = (CalculateLinksFn)(base + 0x2093B1);
    calculate_links(1, 0, 0, 0);
    CalculateTeamRatingFn team_rating = (CalculateTeamRatingFn)(base + 0x20B71D);
    uint8_t *db = db_instance(base);
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        if (!g_rating_dirty[i]) continue;
        g_rating_dirty[i] = 0;
        if (g_market.clubs[i].team_id >= 0) team_rating(db, g_market.clubs[i].team_id, (void *)0);
    }
}

/* Market club that holds the player in the default table, or -1 (e.g. a user-created player).
 * National and all-star squads also list club players, so only market clubs count. */
static int32_t default_owner(uint32_t base, int32_t player_id, uint32_t *spec_out) {
    uint8_t *links;
    int32_t count = default_links(base, &links);
    for (int32_t i = 0; i < count; ++i) {
        uint8_t *link = links + i * LINK_STRIDE;
        int32_t team = *(int32_t *)link;
        if (team == USER_TEAM_ID || find_account(team) < 0) continue;
        int32_t players = *(int32_t *)(link + 4);
        if (players < 0 || players > 32) continue;
        for (int32_t j = 0; j < players; ++j) {
            if (*(int32_t *)(link + 0x88 + 4 * j) != player_id) continue;
            if (spec_out) *spec_out = *(uint32_t *)(link + 8 + 4 * j);
            return *(int32_t *)link;
        }
    }
    return -1;
}

/* Move a player's default-table entry to another club, with the stock squad checks. */
/* check_remove = 0 when the player actually plays for the user: his default-table club only keeps
 * a stale entry for him, so its squad limits do not apply. */
static int32_t default_move(uint32_t base, int32_t player_id, int32_t buyer_id, PlayerInfo *info,
                            int32_t check_remove) {
    uint8_t *links;
    int32_t count = default_links(base, &links);
    uint32_t spec = 0;
    int32_t owner = default_owner(base, player_id, &spec);
    if (!count || owner < 0 || buyer_id == USER_TEAM_ID) return 0;
    if (owner == buyer_id) return 1;
    uint8_t *db = db_instance(base);
    SetOverrideLinksFn set_override = (SetOverrideLinksFn)(base + 0x20BF85);
    CanRemovePlayerFn can_remove = (CanRemovePlayerFn)(base + 0x213071);
    CanAddPlayerFn can_add = (CanAddPlayerFn)(base + 0x2102D5);
    RemovePlayerFromLinkFn remove_player = (RemovePlayerFromLinkFn)(base + 0x209DD9);
    VerifyLinkFn verify_link = (VerifyLinkFn)(base + 0x209E6D);
    AddPlayerToLinkFn add_player = (AddPlayerToLinkFn)(base + 0x20A40D);
    GetTeamSpecificDataFn get_specific = (GetTeamSpecificDataFn)(base + 0x20A621);
    set_override(links, count);
    int32_t ok = (!check_remove || can_remove(owner, info) == 2) && can_add(buyer_id, info, -2) == 2;
    if (ok) {
        remove_player(owner, owner, player_id, 1);
        verify_link(db, info, owner, &spec, USER_TEAM_ID, -1, (void *)0, 0, -1);
        add_player(buyer_id, buyer_id, info, &spec, 0, 1);
        ok = get_specific(buyer_id, player_id) != (void *)0;
    }
    set_override((void *)0, 0);
    if (ok) {
        g_links_dirty = 1;
        int32_t a = find_account(owner), b = find_account(buyer_id);
        if (a >= 0) g_rating_dirty[a] = 1;
        if (b >= 0) g_rating_dirty[b] = 1;
    }
    return ok;
}

/* Unattached players (the free players a dream team starts with) have no default-table club. To
 * sell one, give him an entry at the buyer; to undo a failed sale, take it away again. */
/* created_caps.py (v33) moves created ids to 0xFEDF..0xFFDD (255); ids at or above this are never
 * database players (0xFFDE empty, 0xFFFF invalid, 0xFFDF..0xFFFE stale stock created ids). */
#define CREATED_PLAYER_FIRST_ID 0xFEDF
static int32_t default_add(uint32_t base, int32_t player_id, int32_t buyer_id, PlayerInfo *info,
                           uint32_t spec) {
    uint8_t *links;
    int32_t count = default_links(base, &links);
    if (!count || buyer_id == USER_TEAM_ID || player_id >= CREATED_PLAYER_FIRST_ID) return 0;
    SetOverrideLinksFn set_override = (SetOverrideLinksFn)(base + 0x20BF85);
    CanAddPlayerFn can_add = (CanAddPlayerFn)(base + 0x2102D5);
    AddPlayerToLinkFn add_player = (AddPlayerToLinkFn)(base + 0x20A40D);
    GetTeamSpecificDataFn get_specific = (GetTeamSpecificDataFn)(base + 0x20A621);
    set_override(links, count);
    int32_t ok = can_add(buyer_id, info, -2) == 2;
    if (ok) {
        add_player(buyer_id, buyer_id, info, &spec, 0, 1);
        ok = get_specific(buyer_id, player_id) != (void *)0;
    }
    set_override((void *)0, 0);
    if (ok) {
        g_links_dirty = 1;
        int32_t b = find_account(buyer_id);
        if (b >= 0) g_rating_dirty[b] = 1;
    }
    return ok;
}

static void default_remove(uint32_t base, int32_t player_id, int32_t team_id) {
    uint8_t *links;
    int32_t count = default_links(base, &links);
    if (!count) return;
    SetOverrideLinksFn set_override = (SetOverrideLinksFn)(base + 0x20BF85);
    RemovePlayerFromLinkFn remove_player = (RemovePlayerFromLinkFn)(base + 0x209DD9);
    set_override(links, count);
    remove_player(team_id, team_id, player_id, 1);
    set_override((void *)0, 0);
    g_links_dirty = 1;
}

/* Career load: start from the pristine table, then apply the saved roster image if it fits. */
static void restore_career_rosters(uint32_t base, int32_t use_loaded_image) {
    uint8_t *links;
    int32_t count = default_links(base, &links);
    if (!count || !g_pristine_count) return;
    const uint8_t *image = (const uint8_t *)g_pristine_links;
    int32_t image_count = g_pristine_count;
    if (use_loaded_image && g_loaded_link_count > 0 && g_loaded_link_count <= MAX_LINKS &&
        link_image_matches((const uint8_t *)g_loaded_links, (int32_t)g_loaded_link_count,
                           links, count)) {
        image = (const uint8_t *)g_loaded_links;
        image_count = (int32_t)g_loaded_link_count;
    }
    if (!link_image_matches(image, image_count, links, count)) return;
    copy_words((uint64_t *)links, (const uint64_t *)image, (uint32_t)count * LINK_WORDS);
    for (int32_t i = 0; i < MAX_CLUBS; ++i) g_rating_dirty[i] = g_market.clubs[i].team_id >= 0;
    g_links_dirty = 1;
    flush_roster_changes(base);
}

/* Serialize the default table (variable length: a count word, then 33 words per link). */
static void serialize_rosters(uint32_t base, void *serializer, int32_t writing,
                              SerializeU64Fn serialize_u64) {
    if (writing) {
        uint8_t *links;
        int32_t count = default_links(base, &links);
        g_loaded_link_count = (uint64_t)count;
        serialize_u64(serializer, &g_loaded_link_count, ROSTER_SAVE_VERSION);
        uint64_t *words = (uint64_t *)links;
        for (uint32_t i = 0; i < (uint32_t)count * LINK_WORDS; ++i) {
            serialize_u64(serializer, &words[i], ROSTER_SAVE_VERSION);
        }
        return;
    }
    g_loaded_link_count = 0;
    serialize_u64(serializer, &g_loaded_link_count, ROSTER_SAVE_VERSION);
    if (g_loaded_link_count > MAX_LINKS) {
        g_loaded_link_count = 0;          /* corrupt: skip nothing further we can trust */
        return;
    }
    for (uint32_t i = 0; i < (uint32_t)g_loaded_link_count * LINK_WORDS; ++i) {
        serialize_u64(serializer, &g_loaded_links[i], ROSTER_SAVE_VERSION);
    }
}

static int32_t move_player(uint32_t base, int32_t seller_id, int32_t buyer_id,
                           int32_t player_id, PlayerInfo *info,
                           int32_t *user_lifecycle_recalculated) {
    GetTeamSpecificDataFn get_specific = (GetTeamSpecificDataFn)(base + 0x20A621);
    void *seller_specific = get_specific(seller_id, player_id);
    if (!seller_specific || get_specific(buyer_id, player_id)) return 0;
    uint32_t copied_specific = *(uint32_t *)seller_specific;
    if (user_lifecycle_recalculated) *user_lifecycle_recalculated = 0;

    if (buyer_id == USER_TEAM_ID) {
        /* stock SignPlayerAttempt arguments: calcLinks 1, forceShirt 0, useDefaultXI 1 */
        SignPlayerFn sign_player = (SignPlayerFn)(base + 0x20CBD5);
        sign_player(info, seller_id, &copied_specific, 1, 0, 1);
        if (get_specific(seller_id, player_id) || !get_specific(buyer_id, player_id)) return 0;
        PlayerDevelopmentAddFn development_add = (PlayerDevelopmentAddFn)(base + 0x20E131);
        development_add(player_id, 0);
        if (user_lifecycle_recalculated) *user_lifecycle_recalculated = 1;
        return 1;
    }

    if (seller_id == USER_TEAM_ID) {
        /* SellPlayer ignores its buyer: the player returns to his default-table club when links
         * are recalculated. So place him at the buyer in the default table first. */
        if (player_id >= CREATED_PLAYER_FIRST_ID) return 0;   /* created players: stock delete only */
        int32_t original_owner = default_owner(base, player_id, (uint32_t *)0);
        if (original_owner >= 0 ? !default_move(base, player_id, buyer_id, info, 0)
                                : !default_add(base, player_id, buyer_id, info, copied_specific)) return 0;
        SellPlayerFn sell_player = (SellPlayerFn)(base + 0x20CC69);
        regenerate_default_simple_links(base);
        sell_player(info, buyer_id, &copied_specific, 1);  /* removes from user + CalculateLinks */
        g_links_dirty = 0;
        if (get_specific(seller_id, player_id) || !get_specific(buyer_id, player_id)) {
            if (original_owner < 0) {
                default_remove(base, player_id, buyer_id);
                flush_roster_changes(base);
            } else if (!get_specific(seller_id, player_id) && original_owner != buyer_id) {
                default_move(base, player_id, original_owner, info, 0);
                flush_roster_changes(base);
            }
            return 0;
        }
        if (user_lifecycle_recalculated) *user_lifecycle_recalculated = 1;
        return 1;
    }

    /* AI to AI: default table only; the live table follows at the next flush_roster_changes. */
    return default_move(base, player_id, buyer_id, info, 1);
}

static int32_t account_fee_room(const ClubAccount *club) {
    int32_t reserve = cash_reserve(club);
    int32_t cash_room = club->cash > reserve ? club->cash - reserve : 0;
    return club->transfer_budget < cash_room ? club->transfer_budget : cash_room;
}

static int32_t account_wage_room(const ClubAccount *club) {
    return club->wage_budget > club->payroll ? club->wage_budget - club->payroll : 0;
}

#define USER_BUY_LIMIT_PER_WINDOW 5

static int32_t g_commit_fail;   /* which check refused the last commit (diagnostics) */

static int32_t commit_transfer(uint32_t base, int32_t buyer_index, int32_t player_index,
                               int32_t window_id, int32_t turn, int32_t season,
                               int32_t fee, int32_t wage, int32_t years,
                               int32_t recalculate_links) {
    MarketPlayer *player = &g_players[player_index];
    int32_t seller_index = player->owner_index;
    g_commit_fail = 0;   /* reset so a stale code from an earlier, unrelated call can't be misread */
    if (seller_index < 0 || seller_index >= MAX_CLUBS || seller_index == buyer_index) { g_commit_fail = 1; return 0; }
    ClubAccount *buyer = &g_market.clubs[buyer_index];
    ClubAccount *seller = &g_market.clubs[seller_index];
    int32_t buy_limit = buyer->team_id == USER_TEAM_ID ? USER_BUY_LIMIT_PER_WINDOW
                                                       : ai_buy_limit(buyer_index);
    if (window_buys(buyer, window_id) >= buy_limit ||
        (buyer->team_id != USER_TEAM_ID && seller->team_id != USER_TEAM_ID &&
         window_sales(seller, window_id) >= MAX_SALES_PER_WINDOW)) { g_commit_fail = 2; return 0; }
    if (seller->team_id == USER_TEAM_ID && user_signing_locked(player->player_id)) { g_commit_fail = 20; return 0; }
    int32_t seller_floor = seller->team_id == USER_TEAM_ID ? USER_SQUAD_MIN : SQUAD_MIN;
    /* stock CanRemovePlayer: a club may never lose its last goalkeeper */
    if (player->position == 0 && g_group_count[seller_index][0] <= 1) { g_commit_fail = 3; return 0; }
    if (g_total_count[buyer_index] >= squad_max_for(buyer_index) || g_total_count[seller_index] <= seller_floor) { g_commit_fail = 4; return 0; }
    if (player->position >= 0 && player->position <= 3 &&
        g_group_count[buyer_index][player->position] >= pos_max_for(buyer_index, player->position) + 1) { g_commit_fail = 5; return 0; }
    if (fee <= 0 || fee > account_fee_room(buyer) || wage <= 0 || wage > account_wage_room(buyer)) { g_commit_fail = 6; return 0; }
    if (years < 1 || years > 5) { g_commit_fail = 7; return 0; }
    if (buyer->team_id == USER_TEAM_ID &&
        (fee > user_coins(base) || user_coins(base) - fee < clamp_add(buyer->payroll, wage))) { g_commit_fail = 8; return 0; }
    if (!load_player_info(base, player->player_id, &g_player_info)) { g_commit_fail = 9; return 0; }
    capture_pristine_links(base);
    int32_t user_lifecycle_recalculated = 0;
    if (!move_player(base, seller->team_id, buyer->team_id, player->player_id,
                     &g_player_info, &user_lifecycle_recalculated)) { g_commit_fail = 10; return 0; }

    if (buyer->team_id == USER_TEAM_ID && fee > user_coins(base)) return 0;
    int32_t buyer_cash_before = buyer->cash;
    int32_t buyer_budget_before = buyer->transfer_budget;
    int32_t buyer_payroll_before = buyer->payroll;
    int32_t seller_cash_before = seller->cash;
    int32_t seller_budget_before = seller->transfer_budget;
    int32_t seller_payroll_before = seller->payroll;
    buyer->cash -= fee;
    if (buyer->cash < 0) buyer->cash = 0;
    buyer->transfer_budget -= fee;
    if (buyer->transfer_budget < 0) buyer->transfer_budget = 0;
    seller->cash = clamp_add(seller->cash, fee);
    seller->transfer_budget = clamp_add(seller->transfer_budget,
                                        mul_div(fee, SALE_REINVEST_PCT, 100));
    if (seller->transfer_budget > seller->cash) seller->transfer_budget = seller->cash;
    if (buyer->team_id == USER_TEAM_ID) {
        set_user_coins(base, user_coins(base) - fee);
        econ_tally(ECON_PURCHASES, -fee);
        note_user_signing(player->player_id);
    }
    if (seller->team_id == USER_TEAM_ID) {
        set_user_coins(base, user_coins(base) + fee);
        econ_tally(ECON_SALES, fee);
    }
    if (buyer->team_id == USER_TEAM_ID || seller->team_id == USER_TEAM_ID) {
        ClubAccount *user = buyer->team_id == USER_TEAM_ID ? buyer : seller;
        user->cash = user_coins(base);
        user->transfer_budget = user->cash;
        user->wage_budget = user->cash;
    }
    player->owner_id = buyer->team_id;
    player->owner_index = buyer_index;
    player->wage = wage;
    player->listed_for_sale = 0;
    /* counts, best XI, strength, squad value and payroll follow from the moved cache entry */
    recompute_club_profile(buyer_index);
    recompute_club_profile(seller_index);
    if (seller->team_id == USER_TEAM_ID) remove_user_listing(player->player_id);
    window_note(buyer, window_id, 1, 0);
    window_note(seller, window_id, 0, 1);
    add_contract(player->player_id, buyer->team_id, wage, season + years);
    record_transfer(season, turn, player->player_id, seller->team_id, buyer->team_id, fee, wage, years);
    record_finance(buyer_index, CM_FINANCE_PURCHASE,
                   buyer->cash - buyer_cash_before,
                   buyer->transfer_budget - buyer_budget_before,
                   buyer->payroll - buyer_payroll_before, player->player_id);
    record_finance(seller_index, CM_FINANCE_SALE,
                   seller->cash - seller_cash_before,
                   seller->transfer_budget - seller_budget_before,
                   seller->payroll - seller_payroll_before, player->player_id);
    g_market_changed = 1;
    g_market_dirty = 1;
    if (recalculate_links && !user_lifecycle_recalculated) flush_roster_changes(base);
    if (recalculate_links) g_market_changed = 0;
    return 1;
}

/* One window per season, open until the first PRESEASON_WINDOW_TURNS league matches are played.
 * CSeason::GetCurrentTurn is a schedule slot, not a match count: league round r sits on slot
 * GetStartLeagueTurn() + 2 * (r - 1) with cup rounds on the odd slots between. Everything up to
 * league round 6 (slot start + 10) is open, including the season rollover and the optional friendly.
 * There is no mid-season window; the DLS schedule is too tight for one. Window ids stay season * 2.
 * The last open slot is saved in g_market.reserved[1] so screens without a season can check it. */
#define PRESEASON_WINDOW_TURNS 6
#define DEFAULT_START_LEAGUE_TURN 18
static int32_t window_last_turn(void) {
    int32_t last = g_market.reserved[1];
    return last > 0 && last < 4096 ? last : DEFAULT_START_LEAGUE_TURN + 2 * (PRESEASON_WINDOW_TURNS - 1);
}

static int32_t market_window(int32_t turn, int32_t turn_count) {
    (void)turn_count;
    return turn >= 0 && turn <= window_last_turn() ? 0 : -1;
}

/* Matches left in the window as the player sees them (league rounds still to play inside it). */
static int32_t window_matches_left(void) {
    int32_t left = (window_last_turn() - g_market.current_turn) / 2 + 1;
    if (left > PRESEASON_WINDOW_TURNS) left = PRESEASON_WINDOW_TURNS;
    return left > 0 ? left : 0;
}

static int32_t find_cached_player(int32_t player_id) {
    for (int32_t i = 0; i < g_player_count; ++i) {
        if (g_players[i].player_id == player_id) return i;
    }
    return -1;
}

static int32_t toggle_user_listing(int32_t player_id) {
    int32_t player_index = find_cached_player(player_id);
    if (player_index < 0 || g_players[player_index].owner_id != USER_TEAM_ID ||
        has_active_offer_for_player(player_id)) return -1;
    int32_t slot = user_listing_slot(player_id);
    if (slot < 0 && user_signing_locked(player_id)) return -3;
    if (slot >= 0) {
        g_user_listed_player_ids[slot] = -1;
        g_players[player_index].listed_for_sale = 0;
        g_market_dirty = 1;
        return 1;
    }
    for (int32_t i = 0; i < USER_LISTING_CAPACITY; ++i) {
        if (g_user_listed_player_ids[i] >= 0) continue;
        g_user_listed_player_ids[i] = player_id;
        g_players[player_index].listed_for_sale = 1;
        for (int32_t k = 0; k < 16; ++k) if (g_ext.keep_player[k] == player_id + 1) g_ext.keep_player[k] = 0;
        g_market_dirty = 1;
        return 0;
    }
    return -2;
}

/* v38: a kept player gets no unsolicited bids; keeping him takes him off the sale list. */
static int32_t user_is_kept(int32_t player_id) {
    if (player_id < 0) return 0;
    for (int32_t k = 0; k < 16; ++k) if (g_ext.keep_player[k] == player_id + 1) return 1;
    return 0;
}

/* 1 kept, 0 released, -1 no room (16 at most), -2 not the user's player. */
static int32_t user_keep_toggle(int32_t player_id) {
    int32_t player_index = find_cached_player(player_id);
    if (player_index < 0 || g_players[player_index].owner_id != USER_TEAM_ID) return -2;
    for (int32_t k = 0; k < 16; ++k) {
        if (g_ext.keep_player[k] != player_id + 1) continue;
        g_ext.keep_player[k] = 0;
        g_market_dirty = 1;
        return 0;
    }
    for (int32_t k = 0; k < 16; ++k) {
        if (g_ext.keep_player[k] > 0) continue;
        if (user_listing_slot(player_id) >= 0) toggle_user_listing(player_id);
        g_ext.keep_player[k] = player_id + 1;
        g_market_dirty = 1;
        return 1;
    }
    return -1;
}


static void reject_offer(MarketOffer *offer, int32_t status) {
    offer->status = status;
    offer->counter_fee = 0;
    offer->counter_wage = 0;
    g_market_dirty = 1;
}

static void reject_other_user_sale_offers(int32_t window_id, int32_t accepted_offer_id) {
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        MarketOffer *pending = &g_market.offers[i];
        if (!offer_is_active(pending) || pending->offer_id == accepted_offer_id ||
            pending->seller_id != USER_TEAM_ID || pending->buyer_id == USER_TEAM_ID ||
            pending->window_id != window_id) continue;
        reject_offer(pending, CM_OFFER_REJECTED);
    }
}

static int32_t finish_ai_player_offer(uint32_t base, MarketOffer *offer,
                                      int32_t player_index, int32_t fee) {
    int32_t buyer_index = find_account(offer->buyer_id);
    int32_t seller_index = find_account(offer->seller_id);
    if (buyer_index < 0 || seller_index < 0) {
        reject_offer(offer, CM_OFFER_REJECTED);
        return 0;
    }
    ClubAccount *buyer = &g_market.clubs[buyer_index];
    ClubAccount *seller = &g_market.clubs[seller_index];
    MarketPlayer *player = &g_players[player_index];
    int32_t wage = transfer_wage_demand(player, buyer_index, seller_index);
    if (wage > account_wage_room(buyer)) wage = 0;
    (void)seller;
    if (wage <= 0 || !commit_transfer(base, buyer_index, player_index, offer->window_id,
                                      offer->turn, offer->season, fee, wage,
                                      offer->contract_years, 1)) {
        reject_offer(offer, CM_OFFER_REJECTED);
        return 0;
    }
    if (offer->seller_id == USER_TEAM_ID) {
        reject_other_user_sale_offers(offer->window_id, offer->offer_id);
    }
    offer->fee = fee;
    offer->annual_wage = wage;
    offer->counter_fee = 0;
    offer->counter_wage = 0;
    offer->status = CM_OFFER_COMPLETED;
    return 1;
}

static int32_t user_player_wage_demand(const MarketPlayer *player,
                                       const ClubAccount *buyer,
                                       const ClubAccount *seller) {
    return transfer_wage_demand(player, (int32_t)(buyer - g_market.clubs),
                                (int32_t)(seller - g_market.clubs));
}

/* v37: the least wage he accepts, hidden between 88% and 100% of his demand (fixed per player and window). */
static int32_t wage_accept_point(const MarketPlayer *player, int32_t demand, int32_t window_id) {
    uint32_t x = (uint32_t)player->player_id * 0x9E3779B1u ^ (uint32_t)(window_id + 11) * 0x85EBCA6Bu ^ 0x5A17u;
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15;
    int32_t point = mul_div(demand, 880 + (int32_t)(x % 121u), 1000);
    return point > 0 ? point : 1;
}

static int32_t begin_user_player_negotiation(uint32_t base, MarketOffer *offer,
                                             int32_t player_index) {
    int32_t buyer_index = find_account(offer->buyer_id);
    int32_t seller_index = find_account(offer->seller_id);
    if (buyer_index < 0 || seller_index < 0) {
        reject_offer(offer, CM_OFFER_REJECTED);
        return 0;
    }
    ClubAccount *buyer = &g_market.clubs[buyer_index];
    ClubAccount *seller = &g_market.clubs[seller_index];
    MarketPlayer *player = &g_players[player_index];
    int32_t demand = user_player_wage_demand(player, buyer, seller);
    int32_t minimum = wage_accept_point(player, demand, offer->window_id);
    int32_t room = account_wage_room(buyer);
    if (offer->annual_wage >= minimum && offer->annual_wage <= room) {
        if (!commit_transfer(base, buyer_index, player_index, offer->window_id,
                             offer->turn, offer->season, offer->fee,
                             offer->annual_wage, offer->contract_years, 1)) {
            reject_offer(offer, CM_OFFER_REJECTED);
            return 0;
        }
        offer->status = CM_OFFER_COMPLETED;
        return 1;
    }
    if (demand > room) {
        reject_offer(offer, CM_OFFER_REJECTED);
        return 0;
    }
    offer->counter_wage = demand;
    offer->status = CM_OFFER_WAIT_USER_WAGE;
    offer->rounds = 0;
    return 1;
}

/* AI bid for a user player at the fee/wage its own target search settled on. */
static MarketOffer *create_ai_offer_for_user(int32_t buyer_index, int32_t player_index,
                                              int32_t window_id, int32_t turn, int32_t season,
                                              int32_t fee, int32_t wage) {
    MarketPlayer *player = &g_players[player_index];
    if (player->owner_id != USER_TEAM_ID || has_active_offer_for_player(player->player_id)) return 0;
    ClubAccount *buyer = &g_market.clubs[buyer_index];
    if (fee > account_fee_room(buyer)) fee = account_fee_room(buyer);
    if (fee < 1 || wage < 1) return 0;
    MarketOffer *offer = allocate_offer(season, turn, window_id, player->player_id, USER_TEAM_ID,
                                        buyer->team_id, fee, wage, 3, CM_OFFER_WAIT_USER_SELLER);
    if (offer) {
        /* the buyer's valuation caps how far it moves when the user counters */
        int32_t ceiling = buyer_max_price(player, buyer_index);
        int32_t cap = mul_div(user_sale_value(player), USER_SALE_CEILING_PCT, 100);
        if (ceiling > cap) ceiling = cap;
        if (ceiling > account_fee_room(buyer)) ceiling = account_fee_room(buyer);
        g_ext.offers[offer - g_market.offers].reservation = ceiling > fee ? ceiling : fee;
    }
    return offer;
}

static void expire_window_offers(int32_t window_id) {
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        MarketOffer *offer = &g_market.offers[i];
        if (offer_is_active(offer) && offer->window_id == window_id) {
            reject_offer(offer, CM_OFFER_EXPIRED);
        }
    }
}

/* Per-turn operating result: a share of annual revenue in, wages out. Transfer budgets are
 * planned by the board when the season's window opens (plan_all_budgets), not dripped. */
static void accrue_finances(uint32_t base, int32_t turn_count) {
    if (turn_count < 1) turn_count = 38;
    g_market_dirty = 1;
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        ClubAccount *club = &g_market.clubs[i];
        if (club->team_id < 0) continue;
        if (club->team_id == USER_TEAM_ID) {
            int32_t wage_cost = club->payroll / turn_count;
            if (club->payroll % turn_count) ++wage_cost;
            int32_t coins = user_coins(base);
            int32_t paid = wage_cost < coins ? wage_cost : coins;
            if (paid > 0) {
                set_user_coins(base, coins - paid);
                record_finance(i, CM_FINANCE_WAGE_COST, -paid, 0, 0, -1);
                econ_tally(ECON_WAGES, -paid);
            }
            club->cash = user_coins(base);
            club->transfer_budget = club->cash;
            club->wage_budget = club->cash;
            continue;
        }
        int32_t income = annual_revenue(i) / turn_count;
        if (income < 1) income = 1;
        int32_t wage_cost = club->payroll / turn_count;
        if (club->payroll % turn_count) ++wage_cost;
        club->cash = clamp_add(club->cash, income);
        club->cash = club->cash > wage_cost ? club->cash - wage_cost : 0;
        if (club->transfer_budget > club->cash) club->transfer_budget = club->cash;
    }
}

static void roll_season(int32_t season) {
    if (g_market.season == season) return;
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        if (offer_is_active(&g_market.offers[i])) reject_offer(&g_market.offers[i], CM_OFFER_EXPIRED);
    }
    g_market.season = season;
    g_market.window_id = -1;
    g_market.last_turn_key = -1;
    g_market_dirty = 1;
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        if (g_market.clubs[i].team_id >= 0) g_market.clubs[i].window_flags = 0xFFFFFFFFu;
    }
}

/* Renewal demand: anchored to value (not the current wage, so payrolls cannot compound), scaled
 * by how important the player is to his club and nudged up when the deal is about to run out. */
static int32_t renewal_wage_demand(const MarketPlayer *player, int32_t club_team_id) {
    static const int32_t role_pct[4] = {85, 95, 105, 115};
    int32_t demand = mul_div(club_base_wage(club_team_id, player->value), role_blend(player, role_pct), 100);
    /* +5% as the deal runs out: full at 1.5 seasons left, none from 2.5 */
    static const int32_t ex[2] = {15, 25}, ey[2] = {105, 100};
    demand = mul_div(demand, lerp_pts(contract_tenths_left(player->player_id, club_team_id), ex, ey, 2), 100);
    return demand > 0 ? demand : 1;
}

static int32_t ai_contract_renewal_wage_demand(const MarketPlayer *player,
                                                const ClubAccount *club) {
    return renewal_wage_demand(player, club->team_id);
}

static void renew_ai_contracts(void) {
    for (int32_t club_index = 0; club_index < MAX_CLUBS; ++club_index) {
        ClubAccount *club = &g_market.clubs[club_index];
        if (club->team_id < 0 || club->team_id == USER_TEAM_ID) continue;
        for (int32_t renewed = 0; renewed < AI_CONTRACT_RENEWAL_LIMIT; ++renewed) {
            int32_t best_index = -1;
            int32_t best_score = -1;
            for (int32_t player_index = 0; player_index < g_player_count; ++player_index) {
                MarketPlayer *player = &g_players[player_index];
                if (player->owner_index != club_index || player->wage <= 0 ||
                    player->rating + 5 < club->strength) continue;
                int32_t seasons_to_expiry = contract_seasons_to_expiry(
                    player->player_id, club->team_id, g_market.season);
                if (seasons_to_expiry > 1) continue;
                int32_t demand = ai_contract_renewal_wage_demand(player, club);
                int32_t max_wage = clamp_add(player->wage, account_wage_room(club));
                if (demand > max_wage) continue;
                int32_t score = player->rating * 4 + (seasons_to_expiry <= 0 ? 2 : 1);
                if (best_index < 0 || score > best_score) {
                    best_index = player_index;
                    best_score = score;
                }
            }
            if (best_index < 0) break;

            MarketPlayer *player = &g_players[best_index];
            int32_t old_wage = player->wage;
            int32_t new_wage = ai_contract_renewal_wage_demand(player, club);
            int32_t payroll_before = club->payroll;
            int32_t payroll_without_player = payroll_before > old_wage
                ? payroll_before - old_wage : 0;
            club->payroll = clamp_add(payroll_without_player, new_wage);
            player->wage = new_wage;
            int32_t expiry = g_market.season <= 0x7FFFFFFF - AI_CONTRACT_RENEWAL_YEARS
                ? g_market.season + AI_CONTRACT_RENEWAL_YEARS : 0x7FFFFFFF;
            add_contract(player->player_id, club->team_id, new_wage, expiry);
            record_finance(club_index, CM_FINANCE_CONTRACT_RENEWAL, 0, 0,
                           club->payroll - payroll_before, player->player_id);
        }
    }
}

static int32_t state_is_valid(void) {
    if (g_market.magic != MARKET_MAGIC || g_market.format != MARKET_FORMAT) return 0;
    if (g_market.club_count < 0 || g_market.club_count > MAX_CLUBS) return 0;
    if (g_market.history_head < 0 || g_market.history_head >= HISTORY_CAPACITY) return 0;
    if (g_market.history_count < 0 || g_market.history_count > HISTORY_CAPACITY) return 0;
    if (g_market.contract_next < 0 || g_market.contract_next >= CONTRACT_STORAGE_CAPACITY) return 0;
    if (g_market.offer_next < 0 || g_market.offer_next >= OFFER_CAPACITY || g_market.next_offer_id <= 0) return 0;
    if (g_market.current_turn < -1 || g_market.current_turn >= 4096) return 0;
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        ClubAccount *club = &g_market.clubs[i];
        if (club->team_id < -1 || club->team_id > 0xFFFF) return 0;
        if (g_buy_attempt_window[i] < -1) return 0;
        if (g_finance_ledger_meta[i].head >= FINANCE_ENTRIES_PER_CLUB ||
            g_finance_ledger_meta[i].count > FINANCE_ENTRIES_PER_CLUB) return 0;
        if (club->cash < 0 || club->transfer_budget < 0 || club->wage_budget < 0 ||
            club->payroll < 0 || club->squad_value < 0) return 0;
        for (int32_t j = 0; j < FINANCE_ENTRIES_PER_CLUB; ++j) {
            int32_t category = g_finance_ledger[i][j].category;
            int32_t player_id = g_finance_ledger[i][j].player_id;
            if (category < 0 || category > CM_FINANCE_INVESTMENT ||
                player_id < -1 || player_id > 0xFFFF) return 0;
        }
    }
    for (int32_t i = 0; i < USER_LISTING_CAPACITY; ++i) {
        int32_t player_id = g_user_listed_player_ids[i];
        if (player_id < -1 || player_id > 0xFFFF) return 0;
        if (player_id < 0) continue;
        for (int32_t j = i + 1; j < USER_LISTING_CAPACITY; ++j) {
            if (g_user_listed_player_ids[j] == player_id) return 0;
        }
    }
    for (int32_t i = 0; i < CONTRACT_STORAGE_CAPACITY; ++i) {
        PlayerContract *contract = contract_slot(i);
        if (contract->player_id < -1 || contract->player_id > 0xFFFF) return 0;
        if (contract->annual_wage < 0) return 0;
    }
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        MarketOffer *offer = &g_market.offers[i];
        if (offer->status < CM_OFFER_FREE || offer->status > CM_OFFER_EXPIRED) return 0;
        if (offer->status != CM_OFFER_FREE &&
            (offer->player_id < 0 || offer->player_id > 0xFFFF ||
             offer->seller_id < 0 || offer->seller_id > 0xFFFF ||
             offer->buyer_id < 0 || offer->buyer_id > 0xFFFF ||
             offer->fee < 0 || offer->annual_wage < 0 ||
             offer->contract_years < 1 || offer->contract_years > 5 ||
             offer->rounds < 0 || offer->rounds > 8)) return 0;
    }
    return 1;
}

static void repair_hacked_coins(uint32_t base);

__attribute__((visibility("default")))
void career_market_on_serialize(void *season, void *serializer, uint32_t base) {
    if (!season || !serializer || !base) return;
    pin_library(base);
    mod_boot(base, 1);
    capture_pristine_links(base);
    uint8_t writing = *((uint8_t *)serializer + 0x1C);
    int32_t version = *(int32_t *)((uint8_t *)serializer + 0x18);
    /* coins (TCreditsInfo) are written after this hook, so a repaired balance is saved right away */
    if (writing) repair_hacked_coins(base);
    if (!writing) {
        clear_state();
        if (version < SAVE_VERSION) initialize_market(base, season);
    } else if (g_market.magic != MARKET_MAGIC || g_market.format != MARKET_FORMAT) {
        initialize_market(base, season);
    } else {
        ensure_contract_epoch();
    }

    SerializeU64Fn serialize_u64 = (SerializeU64Fn)(base + 0x3FFAfd);
    uint64_t *words = (uint64_t *)&g_market;
    uint32_t word_count = (uint32_t)(sizeof(g_market) / sizeof(uint64_t));
    for (uint32_t i = 0; i < word_count; ++i) serialize_u64(serializer, &words[i], SAVE_VERSION);
    uint64_t *attempt_words = (uint64_t *)g_buy_attempt_window;
    uint32_t attempt_word_count = MAX_CLUBS / 2u;
    for (uint32_t i = 0; i < attempt_word_count; ++i) {
        serialize_u64(serializer, &attempt_words[i], BUY_ATTEMPT_SAVE_VERSION);
    }
    uint64_t *listing_words = (uint64_t *)g_user_listed_player_ids;
    uint32_t listing_word_count = USER_LISTING_CAPACITY / 2u;
    for (uint32_t i = 0; i < listing_word_count; ++i) {
        serialize_u64(serializer, &listing_words[i], BUY_ATTEMPT_SAVE_VERSION);
    }
    uint64_t *finance_words = (uint64_t *)g_finance_ledger;
    uint32_t finance_word_count = (uint32_t)(sizeof(g_finance_ledger) / sizeof(uint64_t));
    for (uint32_t i = 0; i < finance_word_count; ++i) {
        serialize_u64(serializer, &finance_words[i], FINANCE_LEDGER_SAVE_VERSION);
    }
    uint64_t *finance_meta_words = (uint64_t *)g_finance_ledger_meta;
    uint32_t finance_meta_word_count =
        (uint32_t)(sizeof(g_finance_ledger_meta) / sizeof(uint64_t));
    for (uint32_t i = 0; i < finance_meta_word_count; ++i) {
        serialize_u64(serializer, &finance_meta_words[i], FINANCE_LEDGER_SAVE_VERSION);
    }
    uint64_t *contract_words = (uint64_t *)g_contract_extension;
    uint32_t contract_word_count =
        (uint32_t)(sizeof(g_contract_extension) / (sizeof(uint64_t)));
    for (uint32_t i = 0; i < contract_word_count; ++i) {
        serialize_u64(serializer, &contract_words[i], CONTRACT_EXTENSION_SAVE_VERSION);
    }
    serialize_rosters(base, serializer, writing, serialize_u64);
    uint64_t *ext_words = (uint64_t *)&g_ext;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_ext) / sizeof(uint64_t)); ++i) {
        serialize_u64(serializer, &ext_words[i], MARKET_EXT_SAVE_VERSION);
    }
    uint64_t *ext2_words = (uint64_t *)&g_ext2;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_ext2) / sizeof(uint64_t)); ++i) {
        serialize_u64(serializer, &ext2_words[i], MARKET_EXT2_SAVE_VERSION);
    }
    chem_serialize(serializer, serialize_u64);     /* v39 chemistry, block version 0xB7 */

    if (!writing) {
        rebuild_contract_index();
        int32_t valid = state_is_valid();
        /* AI rosters: pristine default table, then this career's saved moves */
        restore_career_rosters(base, valid && version >= ROSTER_SAVE_VERSION);
        if (!valid) initialize_market(base, season);
        else ensure_contract_epoch();
    }
    if (writing) g_market_dirty = 0;
    apply_price_schedule(base);     /* boot: the career loads after the config, before any screen */
}

typedef void (*SaveProfileFn)(void *, int32_t);

static void *profile_instance(uint32_t base) {
    if (!base) return (void *)0;
    return (void *)(base + 0x84A260);  /* MP_cMyProfile object (the game reads it via GOT slot 0x73052C) */
}

static void *current_season(uint32_t base) {
    void *profile = profile_instance(base);
    return profile ? (void *)((uint8_t *)profile + 0x14) : (void *)0;
}

/* The base APK was a pre-hacked copy whose CMyProfile::GetCredits always returned 16,777,215, and
 * CCredits::SubtractCredits wrote that value back after every purchase. build_mod.py restores the
 * getter; saves made with the hack still hold exactly 0xFFFFFF, which is reset once to a fair
 * balance. Any other balance is left alone. */
#define HACKED_COIN_BALANCE 0xFFFFFF
#define FAIR_COIN_BALANCE 1000

static void repair_hacked_coins(uint32_t base) {
    uint8_t *profile = (uint8_t *)profile_instance(base);
    if (!profile || *(int32_t *)(profile + PROFILE_CREDITS) != HACKED_COIN_BALANCE) return;
    SetCreditsFn set_credits = (SetCreditsFn)(base + 0x26348D);   /* CCredits::SetCredits: profile + HUD */
    set_credits(FAIR_COIN_BALANCE);
}

static void save_profile(uint32_t base) {
    void *profile = profile_instance(base);
    if (!profile) return;
    SaveProfileFn save = (SaveProfileFn)(base + 0x375B89);
    save(profile, 1);
}

/*
 * AI clubs pick their targets in parallel, then each contested player goes to the club that
 * values him most, at a second-price fee: enough to beat the runner-up's valuation, never below
 * its own opening bid and never above its own valuation. The runner-up is kept for the news feed.
 */
typedef struct {
    int32_t buyer_index;
    int32_t player_index;
    int32_t fee;
    int32_t max;
    int32_t wage;
} AiBid;
static AiBid g_bids[MAX_CLUBS];
static int32_t g_bid_count;

static int32_t bid_beats(const AiBid *a, const AiBid *b) {
    if (a->max != b->max) return a->max > b->max;
    int32_t ra = g_club_rep[a->buyer_index], rb = g_club_rep[b->buyer_index];
    if (ra != rb) return ra > rb;          /* players prefer the bigger club */
    return a < b;                          /* then the earlier bid */
}

static void resolve_ai_bids(uint32_t base, int32_t window_id, int32_t turn, int32_t season_id) {
    for (int32_t i = 0; i < g_bid_count; ++i) {
        AiBid *first = &g_bids[i];
        if (first->player_index < 0) continue;
        int32_t player_index = first->player_index;
        AiBid *best = 0, *second = 0;
        for (int32_t j = i; j < g_bid_count; ++j) {
            AiBid *bid = &g_bids[j];
            if (bid->player_index != player_index) continue;
            if (!best || bid_beats(bid, best)) { second = best; best = bid; }
            else if (!second || bid_beats(bid, second)) second = bid;
        }
        int32_t fee = best->fee;
        if (second) {
            int32_t beat = mul_div(second->max, 105, 100);
            if (beat > best->max) beat = best->max;
            if (beat > fee) fee = beat;
        }
        int32_t rival_team = second ? g_market.clubs[second->buyer_index].team_id : -1;
        int32_t rival_fee = second ? second->max : 0;
        if (commit_transfer(base, best->buyer_index, player_index, window_id, turn, season_id,
                            fee, best->wage, 3, 0)) {
            HistoryExt *news = &g_ext.history[(g_market.history_head + HISTORY_CAPACITY - 1) % HISTORY_CAPACITY];
            news->rival_team = rival_team;
            news->rival_fee = rival_fee;
        }
        for (int32_t j = i; j < g_bid_count; ++j) {
            if (g_bids[j].player_index == player_index) g_bids[j].player_index = -1;
        }
    }
    g_bid_count = 0;
}

static void career_market_tick(void *season, uint32_t base) {
    if (!season || !base) return;
    pin_library(base);
    if (!state_is_valid()) {
        initialize_market(base, season);
    }
    ensure_contract_epoch();
    GetSeasonCountFn get_season = (GetSeasonCountFn)(base + 0x36AA89);
    GetCurrentTurnFn get_turn = (GetCurrentTurnFn)(base + 0x36A67D);
    GetEndTurnFn get_end_turn = (GetEndTurnFn)(base + 0x36B031);
    int32_t season_id = get_season(season);
    int32_t turn = get_turn(season);
    int32_t end_turn = get_end_turn(season);
    typedef int (*SeasonTurnFn)(void *);
    SeasonTurnFn get_start_turn = (SeasonTurnFn)(base + 0x36B02B);
    SeasonTurnFn get_start_league_turn = (SeasonTurnFn)(base + 0x36B037);
    int32_t start_turn = get_start_turn(season);
    int32_t start_league = get_start_league_turn(season);
    /* per-turn money is spread over the slots the season actually plays */
    int32_t turn_count = end_turn >= 0 && end_turn < 200 && start_turn >= 0 && start_turn <= end_turn
        ? end_turn - start_turn + 1 : 38;
    if (start_league > 0 && start_league < 200) {
        int32_t last = start_league + 2 * (PRESEASON_WINDOW_TURNS - 1);
        if (g_market.reserved[1] != last) {
            g_market.reserved[1] = last;
            g_market_dirty = 1;
        }
    }
    if (turn < 0 || turn >= 4096) return;
    int32_t season_changed = g_market.season != season_id;
    int32_t turn_key = season_id * 4096 + turn;
    if (g_market.last_turn_key == turn_key) return;
    roll_season(season_id);
    sync_accounts(base);
    /* rebuild the cache from the live rosters every turn, so a career plays out the same whether or
     * not the game was restarted (reputation, best XI and revenue floor are not saved) */
    build_player_cache(base);
    g_market.current_turn = turn;
    g_market.turns_per_season = turn_count;
    update_market_index();
    if (season_changed) {
        renew_ai_contracts();
        compute_average_revenue();
        invest_excess_cash();
    }
    accrue_finances(base, turn_count);
    int32_t window = market_window(turn, turn_count);
    int32_t previous_window = g_market.window_id;
    int32_t current_window_id = window < 0 ? -1 : season_id * 2 + window;
    if (previous_window >= 0 && current_window_id != previous_window) {
        expire_window_offers(previous_window);
    }
    g_market.window_id = current_window_id;
    g_market_changed = 0;
    if (window >= 0) {
        int32_t window_id = season_id * 2 + window;
        /* the board sets budgets when the window opens */
        if (current_window_id != previous_window) plan_all_budgets();
        int32_t user_index = find_account(USER_TEAM_ID);
        int32_t user_rep = user_index >= 0 ? g_club_rep[user_index] : 100;
        int32_t unsolicited_left = 1;          /* at most one unsolicited bid for a user player per turn */
        int32_t start = (season_id * 37 + turn * 101) % MAX_CLUBS;
        for (int32_t round = 0; round < 2; ++round) {
            g_bid_count = 0;
            for (int32_t step = 0; step < MAX_CLUBS; ++step) {
                int32_t buyer_index = (start + step) % MAX_CLUBS;
                ClubAccount *buyer = &g_market.clubs[buyer_index];
                if (buyer->team_id < 0 || buyer->team_id == USER_TEAM_ID) continue;
                if (window_buys(buyer, window_id) >= ai_buy_limit(buyer_index)) continue;
                if (g_total_count[buyer_index] >= SQUAD_MAX) continue;
                if (has_active_offer_for_buyer(buyer->team_id, window_id)) continue;
                int32_t allow_unlisted = unsolicited_left > 0 && g_club_rep[buyer_index] >= user_rep + 15;
                int32_t fee = 0, wage = 0, max = 0;
                int32_t player_index = ai_find_target(buyer_index, account_fee_room(buyer),
                                                      account_wage_room(buyer), allow_unlisted,
                                                      &fee, &wage, &max);
                if (player_index < 0) continue;
                g_buy_attempt_window[buyer_index] = window_id;
                g_market_dirty = 1;
                if (g_players[player_index].owner_id == USER_TEAM_ID) {
                    if (!g_players[player_index].listed_for_sale) --unsolicited_left;
                    create_ai_offer_for_user(buyer_index, player_index, window_id, turn, season_id,
                                             fee, wage);
                } else if (g_bid_count < MAX_CLUBS) {
                    AiBid *bid = &g_bids[g_bid_count++];
                    bid->buyer_index = buyer_index;
                    bid->player_index = player_index;
                    bid->fee = fee;
                    bid->max = max;
                    bid->wage = wage;
                }
            }
            resolve_ai_bids(base, window_id, turn, season_id);
        }
        flush_roster_changes(base);
    }
    g_market.last_turn_key = turn_key;
    if (g_market_dirty) save_profile(base);
}

__attribute__((visibility("default")))
void career_market_on_turn(void *season, void *context, uint32_t base) {
    (void)context;
    if (!base) return;
    pin_library(base);
    mod_boot(base, 0);
    apply_price_schedule(base);
    repair_hacked_coins(base);
    if (season) {
        career_market_tick(season, base);
        return;
    }
    void *active_season = current_season(base);
    if (active_season) {
        career_market_tick(active_season, base);
        sync_accounts(base);
        build_player_cache(base);
    }
#if MARKET_TRANSFER_SCREEN_UI
    /* v35: the transfer screen itself is the market (ts_* below); no box on entry */
    ts_on_enter(base);
#else
    market_ui_show_main(base);
#endif
}

static int32_t contract_renewal_wage_demand(const MarketPlayer *player,
                                            const ClubAccount *club) {
    (void)club;
    return renewal_wage_demand(player, USER_TEAM_ID);
}

static int32_t commit_user_contract_renewal(uint32_t base, int32_t player_id,
                                           int32_t annual_wage, int32_t years) {
    if (!base || !state_is_valid() || annual_wage <= 0 || years < 1 || years > 5) return -1;
    sync_accounts(base);
    build_player_cache(base);
    int32_t account_index = find_account(USER_TEAM_ID);
    int32_t player_index = find_cached_player(player_id);
    if (account_index < 0 || player_index < 0 ||
        g_players[player_index].owner_id != USER_TEAM_ID ||
        !market_contract_available(player_id, USER_TEAM_ID)) return -2;
    ClubAccount *club = &g_market.clubs[account_index];
    MarketPlayer *player = &g_players[player_index];
    int32_t old_wage = player->wage;
    int32_t max_wage = clamp_add(old_wage, account_wage_room(club));
    if (annual_wage > max_wage) return -3;
    int32_t payroll_before = club->payroll;
    int32_t payroll_without_player = payroll_before > old_wage
        ? payroll_before - old_wage : 0;
    club->payroll = clamp_add(payroll_without_player, annual_wage);
    player->wage = annual_wage;
    int32_t expiry = g_market.season <= 0x7fffffff - years
        ? g_market.season + years : 0x7fffffff;
    add_contract(player_id, USER_TEAM_ID, annual_wage, expiry);
    record_finance(account_index, CM_FINANCE_CONTRACT_RENEWAL, 0, 0,
                   club->payroll - payroll_before, player_id);
    g_market_dirty = 1;
    save_profile(base);
    return 1;
}

/*
 * v38: contract renewal talks for the Club Hub. He signs at or above a hidden point between 88% and 100%
 * of his demand (fixed per player and season). Below it he counters, and every short offer uses up
 * patience in proportion to how short it is: 0.4 of a strike plus 4 strikes per 100% short, 3 strikes
 * end the talks until next season. Returns 1 signed, 0 countered (*out_counter), -4 talks over,
 * -3 over the wage room, -2 no contract to renew, -1 bad input.
 */
#define RENEW_TALK_KEY(season) (0x40000000 | ((season) & 0xFFFF))

static int32_t renewal_accept_point(const MarketPlayer *player, int32_t demand) {
    return wage_accept_point(player, demand, RENEW_TALK_KEY(g_market.season));
}

/* A talk slot for a renewal: never one that holds a running transfer talk (this window) or another
 * renewal of this season. */
static TalkMemory *renewal_talk(int32_t player_id, int32_t create) {
    int32_t key = RENEW_TALK_KEY(g_market.season);
    TalkMemory *free_slot = 0;
    for (int32_t i = 0; i < TALK_CAPACITY; ++i) {
        TalkMemory *talk = &g_ext.talks[i];
        if (talk->player_id == player_id && talk->window_id == key) return talk;
        int32_t busy = talk->player_id >= 0 && (talk->window_id == key || talk->window_id == g_market.window_id);
        if (!busy && !free_slot) free_slot = talk;
    }
    if (!create || !free_slot) return 0;
    free_slot->player_id = player_id;
    free_slot->window_id = key;
    free_slot->strikes = 0;
    free_slot->best_bid = 0;
    g_market_dirty = 1;
    return free_slot;
}

static int32_t renewal_patience_left(int32_t player_id) {
    TalkMemory *talk = renewal_talk(player_id, 0);
    int32_t left = 3000 - (talk ? talk->strikes : 0);
    return left > 0 ? left : 0;
}

static int32_t hub_renewal_offer(uint32_t base, int32_t player_id, int32_t wage, int32_t years,
                                 int32_t *out_counter) {
    if (out_counter) *out_counter = 0;
    if (!base || wage <= 0 || years < 1 || years > 5) return -1;
    int32_t player_index = find_cached_player(player_id);
    int32_t account_index = find_account(USER_TEAM_ID);
    if (player_index < 0 || account_index < 0 || g_players[player_index].owner_id != USER_TEAM_ID ||
        !market_contract_available(player_id, USER_TEAM_ID)) return -2;
    if (renewal_patience_left(player_id) <= 0) return -4;
    MarketPlayer *player = &g_players[player_index];
    ClubAccount *club = &g_market.clubs[account_index];
    if (wage > clamp_add(player->wage, account_wage_room(club))) return -3;
    int32_t demand = renewal_wage_demand(player, USER_TEAM_ID);
    int32_t accept = renewal_accept_point(player, demand);
    if (wage >= accept) {
        int32_t result = commit_user_contract_renewal(base, player_id, wage, years);
        return result > 0 ? 1 : result;
    }
    TalkMemory *talk = renewal_talk(player_id, 1);
    if (!talk) return -4;
    int32_t short_pm = mul_div(accept - wage, 1000, accept);
    talk->strikes += 400 + short_pm * 4;
    if (wage > talk->best_bid) talk->best_bid = wage;
    g_market_dirty = 1;
    if (talk->strikes >= 3000) return -4;
    /* he meets you part of the way, never below his own point */
    int32_t counter = wage + (demand - wage) * 2 / 3;
    if (counter < accept) counter = accept;
    if (out_counter) *out_counter = counter;
    return 0;
}

__attribute__((visibility("default")))
int32_t career_market_get_window(void) {
    return g_market.window_id;
}

__attribute__((visibility("default")))
int32_t career_market_get_club(int32_t team_id, CareerMarketClubView *out) {
    if (!out) return 0;
    int32_t index = find_account(team_id);
    if (index < 0) return 0;
    ClubAccount *club = &g_market.clubs[index];
    out->team_id = club->team_id;
    out->cash = club->cash;
    out->transfer_budget = club->transfer_budget;
    out->wage_budget = club->wage_budget;
    out->payroll = club->payroll;
    out->squad_value = club->squad_value;
    out->strength = club->strength;
    out->window_flags = club->window_flags;
    return 1;
}

__attribute__((visibility("default")))
int32_t career_market_get_finance_count(int32_t team_id) {
    int32_t account_index = find_account(team_id);
    if (account_index < 0 ||
        g_finance_ledger_meta[account_index].count > FINANCE_ENTRIES_PER_CLUB) return 0;
    return (int32_t)g_finance_ledger_meta[account_index].count;
}

__attribute__((visibility("default")))
int32_t career_market_get_finance_entry(int32_t team_id, int32_t newest_index,
                                        CareerMarketFinanceEntryView *out) {
    if (!out) return 0;
    int32_t account_index = find_account(team_id);
    FinanceLedgerEntry *entry = finance_entry_newest(account_index, newest_index);
    if (!entry) return 0;
    out->season = entry->season;
    out->turn = entry->turn;
    out->category = entry->category;
    out->cash_delta = entry->cash_delta;
    out->transfer_budget_delta = entry->transfer_budget_delta;
    out->payroll_delta = entry->payroll_delta;
    out->player_id = entry->player_id;
    return 1;
}

__attribute__((visibility("default")))
int32_t career_market_get_offer_count(void) {
    int32_t count = 0;
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        if (offer_is_active(&g_market.offers[i])) ++count;
    }
    return count;
}

__attribute__((visibility("default")))
int32_t career_market_get_offer(int32_t active_index, CareerMarketOfferView *out) {
    if (!out || active_index < 0) return 0;
    int32_t active = 0;
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        MarketOffer *offer = &g_market.offers[i];
        if (!offer_is_active(offer)) continue;
        if (active++ != active_index) continue;
        out->offer_id = offer->offer_id;
        out->season = offer->season;
        out->turn = offer->turn;
        out->window_id = offer->window_id;
        out->player_id = offer->player_id;
        out->seller_id = offer->seller_id;
        out->buyer_id = offer->buyer_id;
        out->fee = offer->fee;
        out->counter_fee = offer->counter_fee;
        out->annual_wage = offer->annual_wage;
        out->counter_wage = offer->counter_wage;
        out->contract_years = offer->contract_years;
        out->rounds = offer->rounds;
        out->status = offer->status;
        return 1;
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t career_market_get_history_count(void) {
    return g_market.history_count;
}

__attribute__((visibility("default")))
int32_t career_market_get_history(int32_t index, CareerMarketTransferView *out) {
    if (!out || index < 0 || index >= g_market.history_count) return 0;
    int32_t first = g_market.history_head - g_market.history_count;
    if (first < 0) first += HISTORY_CAPACITY;
    TransferRecord *record = &g_market.history[(first + index) % HISTORY_CAPACITY];
    out->season = record->season;
    out->turn = record->turn;
    out->player_id = record->player_id;
    out->seller_id = record->seller_id;
    out->buyer_id = record->buyer_id;
    out->fee = record->fee;
    out->annual_wage = record->annual_wage;
    out->contract_years = record->contract_years;
    return 1;
}

__attribute__((visibility("default")))
int32_t career_market_get_player_count(void) {
    return g_market.window_id >= 0 ? g_player_count : 0;
}

__attribute__((visibility("default")))
int32_t career_market_get_player(int32_t index, CareerMarketPlayerView *out) {
    if (!out || g_market.window_id < 0 || index < 0 || index >= g_player_count) return 0;
    MarketPlayer *player = &g_players[index];
    out->player_id = player->player_id;
    out->club_id = player->owner_id;
    out->position = player->position;
    out->rating = player->rating;
    out->value = player->value;
    out->annual_wage = player->wage;
    return 1;
}

/*
 * User negotiations (v6).
 *
 * Buying: the seller has a hidden reservation price (seller_reservation plus the premium a smaller
 * user club pays for a starter) and quotes an asking price above it. A bid at or near the asking
 * price is accepted; a bid above the reservation gets a counter halfway between; a bid a little
 * under the reservation gets a slightly softened quote back; a bid far under it is an insult and
 * counts as a strike. Three strikes, or too many rounds, and the club breaks off talks for the
 * window. An AI club that also wants the player may enter the race: it bids, raises in steps up to
 * its own valuation, and signs the player itself if the user walks away.
 *
 * Selling: the AI buyer's valuation is kept with the offer. A counter it can afford is accepted;
 * otherwise it moves halfway towards the user's price, up to its valuation, then makes a final offer.
 *
 * Every step leaves a NegotiationEvent for the UI to explain what happened.
 */
#define TALK_STRIKE_LIMIT 3
#define USER_BID_ROUND_LIMIT 6
#define RIVAL_MARGIN_PCT 105
#define INSULT_PCT 80

enum NegotiationEvent {
    NEG_NONE = 0,
    NEG_COUNTER = 1,            /* club answered with counter_fee */
    NEG_SOFTENED = 2,           /* bid under the reservation; the club trimmed its quote */
    NEG_INSULT = 3,             /* bid far too low: a strike */
    NEG_AGREED = 4,             /* fee agreed, now personal terms */
    NEG_TALKS_ENDED = 5,        /* strikes or rounds used up */
    NEG_RIVAL_BID = 6,          /* a rival club has bid rival_fee; counter_fee beats it */
    NEG_RIVAL_RAISED = 7,       /* the rival answered the user's bid */
    NEG_RIVAL_WITHDREW = 8,     /* the user's bid is beyond the rival's valuation */
    NEG_RIVAL_SIGNED = 9,       /* the user walked away and the rival signed him */
    NEG_WAGE_COUNTER = 10,      /* player asks for counter_wage */
    NEG_SIGNED = 11,            /* transfer completed */
    NEG_BUYER_RAISED = 12,      /* AI buyer of a user player moved up */
    NEG_BUYER_FINAL = 13,       /* AI buyer is at its valuation: final offer */
    NEG_BUYER_WALKED = 14,      /* AI buyer walked away */
    NEG_SOLD = 15
};
static int32_t g_neg_event;
static int32_t g_neg_rival_team = -1;
static int32_t g_neg_rival_fee;

static uint32_t mix32(uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static int32_t round5(int32_t value) {
    if (value <= 0) return 0;
    int32_t rounded = ((value + 2) / 5) * 5;
    return rounded > 0 ? rounded : 5;
}

static OfferExt *offer_ext(const MarketOffer *offer) {
    return &g_ext.offers[offer - g_market.offers];
}

static TalkMemory *talk_memory(int32_t player_id, int32_t window_id, int32_t create) {
    TalkMemory *free_slot = 0, *oldest = &g_ext.talks[0];
    for (int32_t i = 0; i < TALK_CAPACITY; ++i) {
        TalkMemory *talk = &g_ext.talks[i];
        if (talk->player_id == player_id && talk->window_id == window_id) return talk;
        if (talk->player_id < 0 || talk->window_id != window_id) { if (!free_slot) free_slot = talk; }
        else if (talk->window_id < oldest->window_id) oldest = talk;
    }
    if (!create) return 0;
    TalkMemory *talk = free_slot ? free_slot : oldest;
    talk->player_id = player_id;
    talk->window_id = window_id;
    talk->strikes = 0;
    talk->best_bid = 0;
    g_market_dirty = 1;
    return talk;
}

static int32_t talk_strikes(int32_t player_id) {
    TalkMemory *talk = talk_memory(player_id, g_market.window_id, 0);
    return talk ? talk->strikes / 1000 : 0;
}

/* The selling club must stay at 16 players and keep a goalkeeper. */
static int32_t club_can_sell_now(const MarketPlayer *player) {
    int32_t club = player->owner_index;
    if (club < 0 || club >= MAX_CLUBS) return 0;
    if (g_total_count[club] <= SQUAD_MIN) return 0;
    if (player->position == 0 && g_group_count[club][0] <= 1) return 0;
    return 1;
}

/* Hidden floor for a user bid: the seller's reservation plus the smaller-club premium. */
static int32_t user_bid_reservation(const MarketPlayer *player, int32_t buyer_index) {
    int32_t reservation = seller_reservation(player, player->owner_index);
    /* clubs sell squad players to each other cheaply, but not to a user who would flip them */
    int32_t floor = mul_div(mul_div(market_value(player), USER_BUY_FLOOR_PCT, 100),
                            contract_floor_pct(contract_tenths_left(player->player_id, player->owner_id)), 100);
    if (reservation < floor) reservation = floor;
    return round5(mul_div(reservation, 100 + user_price_premium_pct(player, buyer_index), 100));
}

static int32_t user_bid_asking(const MarketPlayer *player, int32_t buyer_index) {
    return round5(mul_div(user_bid_reservation(player, buyer_index), asking_markup_pct(player), 100));
}

/* The AI club that values the player most among those that want him and could sign him now. */
static int32_t find_rival(const MarketPlayer *player, int32_t exclude_index,
                          int32_t *out_max, int32_t *out_count) {
    int32_t best = -1, best_max = 0, count = 0;
    int32_t position = player->position;
    if (position < 0 || position > 3) return -1;
    ClubAccount *seller = &g_market.clubs[player->owner_index];
    if (window_sales(seller, g_market.window_id) >= MAX_SALES_PER_WINDOW) return -1;  /* AI clubs cannot buy here */
    for (int32_t c = 0; c < MAX_CLUBS; ++c) {
        ClubAccount *club = &g_market.clubs[c];
        if (club->team_id < 0 || club->team_id == USER_TEAM_ID || c == exclude_index ||
            c == player->owner_index) continue;
        if (g_total_count[c] >= SQUAD_MAX || window_buys(club, g_market.window_id) >= ai_buy_limit(c)) continue;
        if (g_group_count[c][position] >= k_pos_max[position]) continue;
        int32_t need = position_need(c, position);
        int32_t improvement = starter_improvement(player, c);
        if (!(improvement >= 2 || (need > 0 && player->rating + 10 >= club->strength))) continue;
        if (!player_will_join(player, c) || !starter_move_allowed(player, c)) continue;
        int32_t room = account_fee_room(club);
        int32_t max = buyer_max_price(player, c);
        if (max > room) max = room;
        if (transfer_wage_demand(player, c, player->owner_index) > account_wage_room(club)) continue;
        if (max <= 0) continue;
        ++count;
        if (max > best_max) { best = c; best_max = max; }
    }
    *out_max = best_max;
    *out_count = count;
    return best;
}

static int32_t agree_user_fee(uint32_t base, MarketOffer *offer, int32_t player_index, int32_t amount) {
    offer->fee = amount;
    offer->counter_fee = 0;
    g_neg_event = NEG_AGREED;
    int32_t result = begin_user_player_negotiation(base, offer, player_index);
    if (offer->status == CM_OFFER_COMPLETED) g_neg_event = NEG_SIGNED;
    else if (offer->status == CM_OFFER_WAIT_USER_WAGE) g_neg_event = NEG_WAGE_COUNTER;
    return result;
}

/* The user walked away or talks collapsed: an active rival completes the deal itself. */
static void rival_takes_player(uint32_t base, MarketOffer *offer, int32_t player_index) {
    OfferExt *ext = offer_ext(offer);
    if (ext->rival_team < 0) return;
    int32_t rival_index = find_account(ext->rival_team);
    if (rival_index < 0) return;
    MarketPlayer *player = &g_players[player_index];
    int32_t wage = transfer_wage_demand(player, rival_index, player->owner_index);
    if (commit_transfer(base, rival_index, player_index, offer->window_id, g_market.current_turn,
                        g_market.season, ext->rival_fee, wage, 3, 1)) {
        g_neg_event = NEG_RIVAL_SIGNED;
        g_neg_rival_team = ext->rival_team;
        g_neg_rival_fee = ext->rival_fee;
    }
}

static void end_user_talks(uint32_t base, MarketOffer *offer, int32_t player_index) {
    reject_offer(offer, CM_OFFER_REJECTED);
    g_neg_event = NEG_TALKS_ENDED;
    rival_takes_player(base, offer, player_index);
}

/* One user fee proposal against the seller (and a rival, if any). Returns 1 while talks go on or
 * after moving to personal terms, 0 when talks ended. */
static int32_t evaluate_user_fee(uint32_t base, MarketOffer *offer, int32_t player_index, int32_t amount) {
    OfferExt *ext = offer_ext(offer);
    MarketPlayer *player = &g_players[player_index];
    int32_t buyer_index = find_account(USER_TEAM_ID);
    int32_t reservation = ext->reservation;
    int32_t ask = round5(mul_div(reservation, asking_markup_pct(player), 100));
    TalkMemory *talk = talk_memory(player->player_id, offer->window_id, 1);
    if (amount > talk->best_bid) talk->best_bid = amount;
    offer->fee = amount;
    g_neg_rival_team = ext->rival_team;
    g_neg_rival_fee = ext->rival_fee;

    if (ext->rival_team >= 0) {
        if (amount > ext->rival_max) {
            g_neg_event = NEG_RIVAL_WITHDREW;          /* then the seller judges the bid alone */
            ext->rival_team = -1;
        } else {
            if (amount >= mul_div(ext->rival_fee, RIVAL_MARGIN_PCT, 100)) {
                int32_t raise = round5(mul_div(amount, RIVAL_MARGIN_PCT, 100));
                ext->rival_fee = raise < ext->rival_max ? raise : ext->rival_max;
                g_neg_event = NEG_RIVAL_RAISED;
            } else {
                g_neg_event = NEG_RIVAL_BID;
            }
            g_neg_rival_fee = ext->rival_fee;
            offer->counter_fee = round5(mul_div(ext->rival_fee, RIVAL_MARGIN_PCT, 100));
            if (offer->counter_fee < ask) offer->counter_fee = ask;
            g_market_dirty = 1;
            return 1;
        }
    }
    (void)buyer_index;
    /* v36: a continuous response. The seller's walk-away point sits somewhere between halfway from the
     * reservation to the ask and the ask itself (fixed per talk); counters move smoothly with the bid;
     * a bid under the reservation costs patience in proportion to how far it falls short. */
    int32_t accept_at = reservation + mul_div(ask > reservation ? ask - reservation : 0,
                                              500 + decision_draw(player->player_id, offer->window_id, 0x4A03) / 2, 1000);
    if (amount >= accept_at || amount >= ask) {
        return agree_user_fee(base, offer, player_index, amount);
    }
    int32_t standing = offer->counter_fee > 0 && offer->counter_fee < ask ? offer->counter_fee : ask;
    if (amount >= reservation) {
        int32_t counter = round5((amount + standing) / 2);
        if (counter <= amount) return agree_user_fee(base, offer, player_index, amount);
        offer->counter_fee = counter;
        if (g_neg_event != NEG_RIVAL_WITHDREW) g_neg_event = NEG_COUNTER;
        return 1;
    }
    /* under the reservation: the concession shrinks and the patience cost grows with the shortfall
     * (one full strike at INSULT_PCT of the reservation, none at the reservation) */
    int32_t ratio = mul_div(amount, 1000, reservation > 0 ? reservation : 1);
    int32_t shortfall = 1000 - (ratio < 0 ? 0 : ratio);
    int32_t soft = 1000 - ramp_pm(shortfall, 0, (100 - INSULT_PCT) * 10);
    int32_t counter = round5(standing - mul_div(standing - reservation, soft, 4000));
    offer->counter_fee = counter > reservation ? counter : reservation;
    talk->strikes += shortfall * 1000 / ((100 - INSULT_PCT) * 10);
    g_market_dirty = 1;
    if (talk->strikes >= TALK_STRIKE_LIMIT * 1000) {
        end_user_talks(base, offer, player_index);
        return 0;
    }
    g_neg_event = soft > 0 ? NEG_SOFTENED : NEG_INSULT;
    return 1;
}

#define USER_ACTIVE_BIDS_MAX 3             /* v37b: bids the user can run at once (was 1) */

static int32_t user_active_bids(int32_t window_id) {
    int32_t n = 0;
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        const MarketOffer *offer = &g_market.offers[i];
        if (offer_is_active(offer) && offer->buyer_id == USER_TEAM_ID && offer->window_id == window_id) ++n;
    }
    return n;
}

/* Why the user cannot open a bid for this player now (0 = he can). One check for the market and the
 * screen, so a disabled button always states the real reason:
 *  -2 window closed, -5 not for sale (own / free / no value), -18 window purchase limit, -19 his club is
 *  busy in another deal, -20 too many bids running, -21 already in talks for him, -11 he will not join,
 *  -15 his club keeps him, -16 talks ended this window, -17 his club cannot let a player go, -8 roster. */
static int32_t user_bid_block(uint32_t base, const MarketPlayer *player, int32_t buyer_index) {
    int32_t window = market_window(g_market.current_turn, g_market.turns_per_season);
    if (window < 0 || g_market.window_id != g_market.season * 2 + window) return -2;
    if (buyer_index < 0 || player->owner_id == USER_TEAM_ID || player->owner_index < 0 ||
        player->owner_index >= MAX_CLUBS || player->value <= 0) return -5;
    int32_t window_id = g_market.window_id;
    ClubAccount *buyer = &g_market.clubs[buyer_index];
    ClubAccount *seller = &g_market.clubs[player->owner_index];
    if (has_active_offer_for_player(player->player_id)) return -21;
    if (window_buys(buyer, window_id) >= USER_BUY_LIMIT_PER_WINDOW) return -18;
    if (user_active_bids(window_id) >= USER_ACTIVE_BIDS_MAX) return -20;
    if (has_active_offer_for_seller(seller->team_id, window_id)) return -19;
    if (talk_strikes(player->player_id) >= TALK_STRIKE_LIMIT) return -16;
    if (!club_can_sell_now(player)) return -17;
    if (!player_will_join(player, buyer_index)) return -11;
    if (!starter_move_allowed(player, buyer_index)) return -15;
    if (base) {
        GetTeamSpecificDataFn get_specific = (GetTeamSpecificDataFn)(base + 0x20A621);
        if (!get_specific(seller->team_id, player->player_id) || get_specific(USER_TEAM_ID, player->player_id)) return -8;
    }
    return 0;
}

__attribute__((visibility("default")))
int32_t career_market_submit_user_bid(uint32_t base, int32_t player_id, int32_t fee,
                                      int32_t annual_wage, int32_t contract_years) {
    g_neg_event = NEG_NONE;
    g_neg_rival_team = -1;
    g_neg_rival_fee = 0;
    if (!base || !state_is_valid()) return -1;
    int32_t window = market_window(g_market.current_turn, g_market.turns_per_season);
    if (window < 0 || g_market.window_id != g_market.season * 2 + window) return -2;
    if (contract_years < 1 || contract_years > 5 || fee <= 0 || annual_wage <= 0) return -3;

    sync_accounts(base);
    build_player_cache(base);
    int32_t buyer_index = find_account(USER_TEAM_ID);
    int32_t player_index = find_cached_player(player_id);
    if (buyer_index < 0 || player_index < 0) return -4;
    MarketPlayer *player = &g_players[player_index];
    int32_t block = user_bid_block(base, player, buyer_index);
    if (block) return block;
    int32_t seller_index = player->owner_index;
    int32_t window_id = g_market.window_id;
    ClubAccount *buyer = &g_market.clubs[buyer_index];
    ClubAccount *seller = &g_market.clubs[seller_index];
    if (fee > account_fee_room(buyer) || annual_wage > account_wage_room(buyer)) return -7;
    if (user_coins(base) - fee < clamp_add(buyer->payroll, annual_wage)) return -12;

    MarketOffer *offer = allocate_offer(g_market.season, g_market.current_turn, window_id,
                                        player_id, seller->team_id, USER_TEAM_ID,
                                        fee, annual_wage, contract_years,
                                        CM_OFFER_WAIT_USER_FEE);
    if (!offer) return -9;
    OfferExt *ext = offer_ext(offer);
    ext->reservation = user_bid_reservation(player, buyer_index);
    /* word gets around: a club that wants him too may join the race */
    int32_t rival_max = 0, interested = 0;
    int32_t rival = find_rival(player, buyer_index, &rival_max, &interested);
    if (rival >= 0 && rival_max >= ext->reservation) {
        uint32_t roll = mix32((uint32_t)player_id * 2654435761u ^ (uint32_t)window_id * 40503u) % 100u;
        uint32_t chance = 30u + 15u * (uint32_t)(interested > 3 ? 3 : interested);
        if (roll < chance) {
            ext->rival_team = g_market.clubs[rival].team_id;
            ext->rival_max = rival_max;
            int32_t opening = ext->reservation + mul_div(rival_max - ext->reservation, NEGOTIATION_SPLIT_PCT, 100);
            ext->rival_fee = round5(opening > ext->reservation ? opening : ext->reservation);
            if (ext->rival_fee > rival_max) ext->rival_fee = rival_max;
        }
    }
    offer->rounds = 1;
    evaluate_user_fee(base, offer, player_index, fee);
    if (offer->status == CM_OFFER_REJECTED) return -10;
    return offer->offer_id;
}

/* Personal terms: a player who would sit on the bench at the user club wants more. */
static int32_t user_terms_demand(const MarketPlayer *player, int32_t buyer_index, int32_t seller_index) {
    return transfer_wage_demand(player, buyer_index, seller_index);
}

__attribute__((visibility("default")))
int32_t career_market_respond_to_offer(uint32_t base, int32_t offer_id,
                                       int32_t decision, int32_t amount) {
    g_neg_event = NEG_NONE;
    if (!base || !state_is_valid() || decision < CM_DECISION_REJECT ||
        decision > CM_DECISION_COUNTER) return -1;
    MarketOffer *offer = find_offer(offer_id);
    if (!offer) return -2;
    int32_t window = market_window(g_market.current_turn, g_market.turns_per_season);
    if (window < 0 || g_market.window_id != offer->window_id ||
        g_market.season != offer->season) {
        reject_offer(offer, CM_OFFER_EXPIRED);
        return -3;
    }

    sync_accounts(base);
    build_player_cache(base);
    int32_t player_index = find_cached_player(offer->player_id);
    if (player_index < 0 || g_players[player_index].owner_id != offer->seller_id) {
        reject_offer(offer, CM_OFFER_REJECTED);
        return -4;
    }
    int32_t buyer_index = find_account(offer->buyer_id);
    int32_t seller_index = find_account(offer->seller_id);
    if (buyer_index < 0 || seller_index < 0) {
        reject_offer(offer, CM_OFFER_REJECTED);
        return -5;
    }
    ClubAccount *buyer = &g_market.clubs[buyer_index];
    MarketPlayer *player = &g_players[player_index];
    OfferExt *ext = offer_ext(offer);

    if (offer->status == CM_OFFER_WAIT_USER_FEE) {
        if (offer->buyer_id != USER_TEAM_ID || offer->seller_id == USER_TEAM_ID) return -6;
        if (decision == CM_DECISION_REJECT) {
            reject_offer(offer, CM_OFFER_REJECTED);
            rival_takes_player(base, offer, player_index);
            return 1;
        }
        int32_t proposal = decision == CM_DECISION_ACCEPT ? offer->counter_fee : amount;
        if (proposal <= 0 || proposal > account_fee_room(buyer) ||
            user_coins(base) - proposal < clamp_add(buyer->payroll, offer->annual_wage)) return -7;
        if (offer->rounds >= USER_BID_ROUND_LIMIT) {
            end_user_talks(base, offer, player_index);
            return 0;
        }
        ++offer->rounds;
        return evaluate_user_fee(base, offer, player_index, proposal);
    }

    if (offer->status == CM_OFFER_WAIT_USER_WAGE) {
        if (offer->buyer_id != USER_TEAM_ID || offer->counter_wage <= 0) return -9;
        if (decision == CM_DECISION_REJECT) {
            reject_offer(offer, CM_OFFER_REJECTED);
            return 1;
        }
        if (decision == CM_DECISION_ACCEPT) {
            offer->annual_wage = offer->counter_wage;
            offer->counter_wage = 0;
        } else {
            if (amount <= 0 || amount > account_wage_room(buyer)) return -10;
            /* v37: continuous. He accepts from a hidden point between 88% and 100% of his demand; under it
             * he meets you halfway, and patience drains in proportion to the shortfall (one strike at 80%). */
            int32_t demand = user_terms_demand(player, buyer_index, seller_index);
            int32_t accept_at = wage_accept_point(player, demand, offer->window_id);
            if (amount >= accept_at) {
                offer->annual_wage = amount;
                offer->counter_wage = 0;
            } else {
                TalkMemory *talk = talk_memory(player->player_id, offer->window_id, 1);
                int32_t shortfall = 1000 - mul_div(amount, 1000, demand > 0 ? demand : 1);
                if (shortfall < 0) shortfall = 0;
                talk->strikes += shortfall * 1000 / 200;
                ++offer->rounds;
                g_market_dirty = 1;
                if (talk->strikes >= TALK_STRIKE_LIMIT * 1000 || offer->rounds > USER_BID_ROUND_LIMIT + 2) {
                    reject_offer(offer, CM_OFFER_REJECTED);
                    g_neg_event = NEG_TALKS_ENDED;
                    return 0;
                }
                int32_t counter = (amount + offer->counter_wage + 1) / 2;
                offer->counter_wage = counter > accept_at ? counter : accept_at;
                g_neg_event = NEG_WAGE_COUNTER;
                return 1;
            }
        }
        if (!commit_transfer(base, buyer_index, player_index, offer->window_id,
                             offer->turn, offer->season, offer->fee,
                             offer->annual_wage, offer->contract_years, 1)) {
            reject_offer(offer, CM_OFFER_REJECTED);
            return -11;
        }
        offer->status = CM_OFFER_COMPLETED;
        g_neg_event = NEG_SIGNED;
        return 1;
    }

    if (offer->status == CM_OFFER_WAIT_USER_SELLER ||
        offer->status == CM_OFFER_WAIT_USER_SELLER_COUNTER) {
        if (offer->seller_id != USER_TEAM_ID || offer->buyer_id == USER_TEAM_ID) return -12;
        if (decision == CM_DECISION_REJECT) {
            reject_offer(offer, CM_OFFER_REJECTED);
            g_neg_event = NEG_BUYER_WALKED;
            return 1;
        }
        int32_t current_fee = offer->status == CM_OFFER_WAIT_USER_SELLER_COUNTER
            ? offer->counter_fee : offer->fee;
        if (decision == CM_DECISION_ACCEPT) {
            if (current_fee <= 0 || current_fee > account_fee_room(buyer)) {
                reject_offer(offer, CM_OFFER_REJECTED);
                return -13;
            }
            int32_t sold = finish_ai_player_offer(base, offer, player_index, current_fee);
            if (sold) g_neg_event = NEG_SOLD;
            return sold;
        }
        if (amount <= 0 || offer->rounds >= 4) {
            reject_offer(offer, CM_OFFER_REJECTED);
            g_neg_event = NEG_BUYER_WALKED;
            return -14;
        }
        ++offer->rounds;
        int32_t ceiling = ext->reservation > 0 ? ext->reservation : buyer_max_price(player, buyer_index);
        if (ceiling > account_fee_room(buyer)) ceiling = account_fee_room(buyer);
        if (amount <= ceiling) {
            int32_t sold = finish_ai_player_offer(base, offer, player_index, amount);
            if (sold) g_neg_event = NEG_SOLD;
            return sold;
        }
        if (current_fee >= ceiling) {
            /* already at its valuation: a second push over it and the buyer walks */
            reject_offer(offer, CM_OFFER_REJECTED);
            g_neg_event = NEG_BUYER_WALKED;
            return 0;
        }
        int32_t counter = round5(current_fee + (amount - current_fee) / 2);
        if (counter > ceiling) counter = ceiling;
        offer->counter_fee = counter;
        offer->status = CM_OFFER_WAIT_USER_SELLER_COUNTER;
        g_neg_event = counter >= ceiling ? NEG_BUYER_FINAL : NEG_BUYER_RAISED;
        g_market_dirty = 1;
        return 1;
    }
    return -15;
}

/*
 * Quick sale: the user can always move a player on at a clearance price, like the stock sale, so a
 * squad full of players nobody bids for never blocks the market. The buyer is the club where he
 * would matter most among those with squad room, a place at his position and the money; the fee
 * is QUICK_SALE_PCT of his market value (less if that club cannot afford it, never below half).
 */
#define QUICK_SALE_PCT 45

static int32_t quick_sale_buyer(const MarketPlayer *player, int32_t *out_fee, int32_t *out_wage) {
    int32_t position = player->position;
    int32_t target_fee = round5(mul_div(market_value(player), QUICK_SALE_PCT, 100));
    if (target_fee < 5) target_fee = 5;
    int32_t best = -1, best_score = -0x7FFFFFFF, best_fee = 0, best_wage = 0;
    for (int32_t c = 0; c < MAX_CLUBS; ++c) {
        ClubAccount *club = &g_market.clubs[c];
        if (club->team_id < 0 || club->team_id == USER_TEAM_ID) continue;
        if (g_total_count[c] >= SQUAD_MAX || window_buys(club, g_market.window_id) >= ai_buy_limit(c)) continue;
        if (position >= 0 && position <= 3 && g_group_count[c][position] >= k_pos_max[position] + 1) continue;
        int32_t fee = target_fee;
        int32_t room = account_fee_room(club);
        if (room < fee) fee = round5(room);
        if (fee < target_fee / 2 || fee <= 0) continue;
        int32_t wage = transfer_wage_demand(player, c, player->owner_index);
        if (wage > account_wage_room(club)) continue;
        /* where he would play: closest to the club's strength, a starter if possible */
        int32_t gap = player->rating - club->strength;
        int32_t score = -(gap < 0 ? -gap : gap) * 10 + (starter_improvement(player, c) >= 0 ? 50 : 0) + fee / 10;
        if (score > best_score) { best = c; best_score = score; best_fee = fee; best_wage = wage; }
    }
    if (best >= 0) { *out_fee = best_fee; *out_wage = best_wage; }
    return best;
}

/* Returns the fee received (> 0), or a negative error: -1 state, -2 window, -3 player, -4 squad
 * minimum, -5 no club can take him, -6 transfer failed. */
__attribute__((visibility("default")))
int32_t career_market_quick_sale(uint32_t base, int32_t player_id) {
    g_neg_event = NEG_NONE;
    if (!base || !state_is_valid()) return -1;
    int32_t window = market_window(g_market.current_turn, g_market.turns_per_season);
    if (window < 0 || g_market.window_id != g_market.season * 2 + window) return -2;
    sync_accounts(base);
    build_player_cache(base);
    int32_t player_index = find_cached_player(player_id);
    int32_t user_index = find_account(USER_TEAM_ID);
    if (player_index < 0 || user_index < 0 || g_players[player_index].owner_id != USER_TEAM_ID ||
        has_active_offer_for_player(player_id)) return -3;
    MarketPlayer *player = &g_players[player_index];
    if (user_signing_locked(player_id)) return -7;
    if (g_total_count[user_index] <= USER_SQUAD_MIN ||
        (player->position == 0 && g_group_count[user_index][0] <= 1)) return -4;
    int32_t fee = 0, wage = 0;
    int32_t buyer = quick_sale_buyer(player, &fee, &wage);
    if (buyer < 0) return -5;
    int32_t buyer_team = g_market.clubs[buyer].team_id;
    if (!commit_transfer(base, buyer, player_index, g_market.window_id, g_market.current_turn,
                         g_market.season, fee, wage, 2, 1)) return -6;
    g_neg_event = NEG_SOLD;
    g_neg_rival_team = buyer_team;
    g_neg_rival_fee = fee;
    save_profile(base);
    return fee;
}

#define MARKET_USE_FULLSCREEN 0      /* 1 = v27 full-screen market (broken on device), 0 = option boxes */
#define MARKET_UI_OPTION_LIMIT 10
#define MARKET_UI_OPTION_STRIDE 24
#define MARKET_UI_TEXT_CAPACITY 512

enum MarketUiAfterNotice {
    UI_AFTER_MENU = 0,
    UI_AFTER_BROWSER = 1,
    UI_AFTER_INBOX = 2,
    UI_AFTER_SALES = 3,
    UI_AFTER_ROSTER = 4,
    UI_AFTER_CLUB_FILTER = 5,
    UI_AFTER_CLOSE = 6,
    UI_AFTER_SHORTLIST = 7
};

enum MarketUiOfferOrigin {
    UI_OFFER_FROM_INBOX = 0,
    UI_OFFER_FROM_BROWSER = 1
};

enum MarketUiExactAmountTarget {
    UI_EXACT_AMOUNT_NONE = 0,
    UI_EXACT_AMOUNT_BID_FEE = 1,
    UI_EXACT_AMOUNT_BID_WAGE = 2,
    UI_EXACT_AMOUNT_RENEWAL_WAGE = 3
};

enum MarketUiAmountError {
    UI_AMOUNT_ERROR_NONE = 0,
    UI_AMOUNT_ERROR_INVALID = 1,
    UI_AMOUNT_ERROR_FEE_BUDGET = 2,
    UI_AMOUNT_ERROR_WAGE_BUDGET = 3,
    UI_AMOUNT_ERROR_UNAVAILABLE = 4
};

typedef struct {
    uint16_t *text;
    int32_t length;
    int32_t capacity;
} MarketUiText;

typedef void *(*GameNewFn)(uint32_t, int32_t, int32_t);
typedef void (*MessageOptionsCtorFn)(void *, const uint16_t *, int32_t, int32_t,
                                     int32_t (*)(int32_t), const uint16_t *,
                                     const uint16_t *, int32_t);
typedef void (*MessageKeyboardCtorFn)(void *, const uint16_t *, const uint16_t *,
                                      const uint16_t *, int32_t, int32_t,
                                      int32_t (*)(int32_t));
typedef void (*AddMessageBoxFn)(void *);
typedef void (*SignPlayerCtorFn)(void *, PlayerInfo *, int32_t, int32_t, int32_t, int32_t, int32_t (*)(int32_t));
typedef void (*SellPlayerCtorFn)(void *, PlayerInfo *, int32_t (*)(int32_t));
typedef void (*BoxTextFn)(void *, const uint16_t *);
typedef void (*GetPlayerNameFn)(uint16_t *, int32_t, PlayerInfo *, float, int32_t, int32_t);
typedef const uint16_t *(*GetTeamNameFn)(int32_t, int32_t, int32_t);
typedef const uint16_t *(*TextFieldGetTextFn)(void *);
typedef int32_t (*MarketUiCallback)(int32_t);

#define MARKET_SCREEN_ID 32
#ifndef MARKET_TM2
#define MARKET_TM2 1                        /* v37: own transfer screen (tm_screen.c) */
#endif
#define MARKET_SCREEN_TAB_COUNT 7
#define MARKET_SCREEN_LIST_ROWS 4
#define MARKET_SCREEN_CARD_LIMIT MARKET_SCREEN_LIST_ROWS
#define MARKET_SCREEN_FILTER_BUTTONS 4
#define MARKET_SCREEN_BUTTON_LIMIT (MARKET_SCREEN_TAB_COUNT + 1 + MARKET_SCREEN_LIST_ROWS + MARKET_UI_OPTION_LIMIT + 2 + MARKET_SCREEN_FILTER_BUTTONS)
#define MARKET_SCREEN_VTABLE_ENTRIES 47

enum MarketScreenListMode {
    UI_SCREEN_LIST_NONE = 0,
    UI_SCREEN_LIST_MARKET = 1,
    UI_SCREEN_LIST_SALES = 2,
    UI_SCREEN_LIST_INBOX = 3,
    UI_SCREEN_LIST_SQUAD = 4,
    UI_SCREEN_LIST_HISTORY = 5,
    UI_SCREEN_LIST_FINANCES = 6,
    UI_SCREEN_LIST_SHORTLIST = 7
};

enum MarketScreenButtonAction {
    UI_SCREEN_BUTTON_TAB = 1,
    UI_SCREEN_BUTTON_BACK = 2,
    UI_SCREEN_BUTTON_ROW = 3,
    UI_SCREEN_BUTTON_OPTION = 4,
    UI_SCREEN_BUTTON_PAGE_PREVIOUS = 5,
    UI_SCREEN_BUTTON_PAGE_NEXT = 6,
    UI_SCREEN_BUTTON_FILTER_POSITION = 7,
    UI_SCREEN_BUTTON_FILTER_CLUB = 8,
    UI_SCREEN_BUTTON_FILTER_SEARCH = 9,
    UI_SCREEN_BUTTON_SORT = 10,
    UI_SCREEN_BUTTON_CLUB_PREVIOUS = 11,
    UI_SCREEN_BUTTON_CLUB_NEXT = 12
};

static uint16_t g_ui_title[48];
static uint16_t g_ui_description[MARKET_UI_TEXT_CAPACITY];
static uint16_t g_ui_keyboard_initial[48];
static uint16_t g_ui_search_query[48];
static int32_t g_ui_search_matches[MAX_PLAYERS];
static int32_t g_ui_search_match_count;
static int32_t g_ui_search_results_valid;
static uint16_t g_ui_options[MARKET_UI_OPTION_LIMIT][MARKET_UI_OPTION_STRIDE];
static int32_t g_ui_after_notice;
static int32_t g_ui_offer_origin;
static int32_t g_ui_offer_id;
static int32_t g_ui_offer_layout;            /* buttons of the offer page shown last: 5 or 3 */
static int32_t g_ui_player_index = -1;
static int32_t g_ui_quick_sale_player_id = -1;
static int32_t g_ui_bid_player_id = -1;
static int32_t g_ui_bid_fee;
static int32_t g_ui_bid_wage;
static int32_t g_ui_bid_contract_years;
static int32_t g_ui_position_filter = -1;
static int32_t g_ui_sales_position_filter = -1;
static int32_t g_ui_browser_mode;
static int32_t g_ui_club_filter = -1;
static int32_t g_ui_market_sort;
static int32_t g_ui_club_cursor = -1;
static int32_t g_ui_finance_club_cursor = -1;
static int32_t g_ui_finance_history_cursor;
static int32_t g_ui_roster_player_cursor = -1;
static int32_t g_ui_renewal_player_id = -1;
static int32_t g_ui_renewal_wage;
static int32_t g_ui_renewal_years = 3;
static int32_t g_ui_renewal_counter_wage;
static int32_t g_ui_renewal_stage;
static int32_t g_ui_exact_amount_target;
static int32_t g_ui_amount_error;
static void *g_ui_keyboard_box;
static int32_t g_ui_history_index;
static uint32_t g_ui_base;
static int32_t g_ui_option_count;
static MarketUiCallback g_ui_page_callback;
static int32_t g_ui_screen_tab;
static int32_t g_ui_screen_dirty;
static int32_t g_ui_screen_request;
static int32_t g_ui_screen_list_mode;
static void *g_ui_screen;
static void *g_ui_screen_buttons[MARKET_SCREEN_BUTTON_LIMIT];
static int32_t g_ui_screen_button_actions[MARKET_SCREEN_BUTTON_LIMIT];
static int32_t g_ui_screen_button_values[MARKET_SCREEN_BUTTON_LIMIT];
static int32_t g_ui_screen_button_count;
static int32_t g_ui_screen_row_count;
static int32_t g_ui_screen_total_rows;
static int32_t g_ui_screen_page_offsets[UI_SCREEN_LIST_SHORTLIST + 1];
static int32_t g_ui_screen_all_targets[MAX_PLAYERS];
static int32_t g_ui_screen_sort_targets[MAX_PLAYERS];
static int32_t g_ui_screen_row_targets[MARKET_SCREEN_LIST_ROWS];
static uint16_t g_ui_screen_row_labels[MARKET_SCREEN_LIST_ROWS][48];
static uint16_t g_ui_screen_row_details[MARKET_SCREEN_LIST_ROWS][112];
static void *g_ui_screen_cards[MARKET_SCREEN_CARD_LIMIT];
static int32_t g_ui_screen_card_rows[MARKET_SCREEN_CARD_LIMIT];
static int32_t g_ui_screen_card_count;
static uint32_t g_ui_screen_vtable[MARKET_SCREEN_VTABLE_ENTRIES];
static uint16_t g_ui_screen_line[96];
static uint16_t g_ui_screen_status[160];

static void ui_text_reset(MarketUiText *builder, uint16_t *text, int32_t capacity) {
    builder->text = text;
    builder->length = 0;
    builder->capacity = capacity;
    if (capacity > 0) text[0] = 0;
}

static void ui_text_append_char(MarketUiText *builder, uint16_t value) {
    if (builder->length + 1 >= builder->capacity) return;
    builder->text[builder->length++] = value;
    builder->text[builder->length] = 0;
}

static void ui_text_append_ascii(MarketUiText *builder, const char *text) {
    if (!text) return;
    while (*text && builder->length + 1 < builder->capacity) {
        ui_text_append_char(builder, (uint8_t)*text++);
    }
}

static void ui_text_append_wide(MarketUiText *builder, const uint16_t *text, int32_t limit) {
    if (!text) return;
    for (int32_t i = 0; i < limit && text[i] && builder->length + 1 < builder->capacity; ++i) {
        ui_text_append_char(builder, text[i]);
    }
}

static void ui_text_append_i32(MarketUiText *builder, int32_t value) {
    uint32_t number;
    uint16_t digits[12];
    int32_t count = 0;
    if (value < 0) {
        ui_text_append_char(builder, '-');
        number = (uint32_t)(-(value + 1)) + 1u;
    } else {
        number = (uint32_t)value;
    }
    do {
        digits[count++] = (uint16_t)('0' + number % 10u);
        number /= 10u;
    } while (number && count < 12);
    while (count > 0) ui_text_append_char(builder, digits[--count]);
}

static const int32_t k_income_categories[] = {
    ECON_PRIZE, ECON_GATE, ECON_BONUS, ECON_CUP, ECON_TV, ECON_SPONSOR, ECON_LEAGUE_PRIZE,
    ECON_PROMOTION, ECON_SALES, ECON_AWARDS, ECON_EXTRA_INCOME
};
static const int32_t k_spend_categories[] = {
    ECON_WAGES, ECON_PURCHASES, ECON_MEDICAL, ECON_FITNESS, ECON_TRAINING, ECON_SCOUTING,
    ECON_STADIUM, ECON_CUSTOMISATION, ECON_CREATE_PLAYER, ECON_FRIENDLY_FEES, ECON_OTHER_SPEND
};

static int32_t books_sum(const int32_t *categories, int32_t count, int32_t last) {
    int32_t sum = 0;
    for (int32_t i = 0; i < count; ++i) sum = sat_add(sum, book_total(categories[i], last));
    return sum;
}

static void ui_append_signed(MarketUiText *builder, int32_t value) {
    if (value < 0) { ui_text_append_char(builder, '-'); value = -value; }
    else if (value > 0) ui_text_append_char(builder, '+');
    ui_text_append_i32(builder, value);
}

static void ui_set_title(const char *title) {
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_title, 48);
    ui_text_append_ascii(&builder, title);
}

static void ui_set_option(int32_t index, const char *text) {
    if (index < 0 || index >= MARKET_UI_OPTION_LIMIT) return;
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_options[index], MARKET_UI_OPTION_STRIDE);
    ui_text_append_ascii(&builder, text);
}

static int32_t ui_wide_equals_ascii(const uint16_t *wide, const char *ascii) {
    if (!wide || !ascii) return 0;
    while (*wide && *ascii && *wide == (uint8_t)*ascii) {
        ++wide;
        ++ascii;
    }
    return *wide == 0 && *ascii == 0;
}

static int32_t market_ui_option_scheme(const uint16_t *label) {
    static const char *const primary_actions[] = {
        "Continue", "Make Offer", "Confirm Sale", "Submit Offer",
        "Accept fee", "Accept counter", "Accept player wage",
        "Accept player counter", "Renew contract", "Submit renewal",
        "Get offers now"
    };
    for (int32_t i = 0; i < (int32_t)(sizeof(primary_actions) / sizeof(primary_actions[0])); ++i) {
        if (ui_wide_equals_ascii(label, primary_actions[i])) return 2;
    }
    return 0;
}

static void market_ui_open_screen(uint32_t base);
static void market_ui_screen_rebuild(void *screen);
static void market_ui_select_tab(int32_t tab);

static int32_t ui_title_matches(const char *title, const char *candidate) {
    if (!title || !candidate) return 0;
    while (*title && *candidate && *title == *candidate) {
        ++title;
        ++candidate;
    }
    return *title == 0 && *candidate == 0;
}

static void ui_screen_update_tab(const char *title) {
    if (ui_title_matches(title, "Club Squad") ||
        ui_title_matches(title, "Contract Renewal") ||
        ui_title_matches(title, "Renewal Wage") ||
        ui_title_matches(title, "Renewal Length") ||
        (ui_title_matches(title, "Player Wage Counter") && g_ui_renewal_stage)) {
        g_ui_screen_tab = 1;
    } else if (ui_title_matches(title, "Player Sales") ||
               ui_title_matches(title, "Sales Position") ||
               ui_title_matches(title, "Quick Sale")) {
        g_ui_screen_tab = 2;
    } else if (ui_title_matches(title, "Offers Inbox") ||
               (ui_title_matches(title, "Incoming Transfer Offer") &&
                g_ui_offer_origin == UI_OFFER_FROM_INBOX)) {
        g_ui_screen_tab = 3;
    } else if (ui_title_matches(title, "Club Finances") ||
               ui_title_matches(title, "Club Finance Activity") ||
               ui_title_matches(title, "Season Books") ||
               ui_title_matches(title, "Price List")) {
        g_ui_screen_tab = 4;
    } else if (ui_title_matches(title, "Transfer History")) {
        g_ui_screen_tab = 5;
    } else if (ui_title_matches(title, "Shortlist")) {
        g_ui_screen_tab = 6;
    } else if (ui_title_matches(title, "Transfer Market") ||
               ui_title_matches(title, "Career Transfer Market") ||
               ui_title_matches(title, "Market Club Filter") ||
               ui_title_matches(title, "Market Position") ||
               ui_title_matches(title, "Offer Terms") ||
               ui_title_matches(title, "Opening Terms") ||
               ui_title_matches(title, "Transfer Fee") ||
               ui_title_matches(title, "Contract Length") ||
               ui_title_matches(title, "Annual Wage") ||
               ((ui_title_matches(title, "Club Fee Negotiation") ||
                 ui_title_matches(title, "Player Wage Negotiation")) &&
                g_ui_offer_origin != UI_OFFER_FROM_INBOX) ||
               (ui_title_matches(title, "Incoming Transfer Offer") &&
                g_ui_offer_origin != UI_OFFER_FROM_INBOX) ||
               (ui_title_matches(title, "Player Wage Counter") && !g_ui_renewal_stage)) {
        g_ui_screen_tab = 0;
    } else if ((ui_title_matches(title, "Club Fee Negotiation") ||
                ui_title_matches(title, "Player Wage Negotiation")) &&
               g_ui_offer_origin == UI_OFFER_FROM_INBOX) {
        g_ui_screen_tab = 3;
    }
}

static void ui_screen_update_list(const char *title) {
    g_ui_screen_list_mode = UI_SCREEN_LIST_NONE;
    if (ui_title_matches(title, "Transfer Market") ||
        ui_title_matches(title, "Market Club Filter") ||
        ui_title_matches(title, "Market Position") ||
        ui_title_matches(title, "Offer Terms") ||
        ui_title_matches(title, "Opening Terms") ||
        ui_title_matches(title, "Transfer Fee") ||
        ui_title_matches(title, "Contract Length") ||
        ui_title_matches(title, "Annual Wage") ||
        ((ui_title_matches(title, "Club Fee Negotiation") ||
          ui_title_matches(title, "Player Wage Negotiation") ||
          ui_title_matches(title, "Incoming Transfer Offer")) &&
         g_ui_offer_origin != UI_OFFER_FROM_INBOX)) {
        g_ui_screen_list_mode = UI_SCREEN_LIST_MARKET;
    } else if (ui_title_matches(title, "Player Sales") ||
               ui_title_matches(title, "Sales Position") ||
               ui_title_matches(title, "Quick Sale")) {
        g_ui_screen_list_mode = UI_SCREEN_LIST_SALES;
    } else if (ui_title_matches(title, "Shortlist")) {
        g_ui_screen_list_mode = UI_SCREEN_LIST_SHORTLIST;
    } else if (ui_title_matches(title, "Club Squad")) {
        g_ui_screen_list_mode = UI_SCREEN_LIST_SQUAD;
    } else if (ui_title_matches(title, "Transfer History")) {
        g_ui_screen_list_mode = UI_SCREEN_LIST_HISTORY;
    } else if (ui_title_matches(title, "Club Finances")) {
        g_ui_screen_list_mode = UI_SCREEN_LIST_FINANCES;
    } else if (g_ui_screen_tab == 3 && g_ui_offer_origin == UI_OFFER_FROM_INBOX &&
               (ui_title_matches(title, "Offers Inbox") ||
                ui_title_matches(title, "Incoming Transfer Offer") ||
                ui_title_matches(title, "Club Fee Negotiation") ||
                ui_title_matches(title, "Player Wage Negotiation") ||
                ui_title_matches(title, "Player Wage Counter"))) {
        g_ui_screen_list_mode = UI_SCREEN_LIST_INBOX;
    } else if (g_ui_after_notice == UI_AFTER_BROWSER && g_ui_screen_tab == 0) {
        g_ui_screen_list_mode = UI_SCREEN_LIST_MARKET;
    } else if (g_ui_after_notice == UI_AFTER_SALES && g_ui_screen_tab == 2) {
        g_ui_screen_list_mode = UI_SCREEN_LIST_SALES;
    } else if (g_ui_after_notice == UI_AFTER_INBOX && g_ui_screen_tab == 3) {
        g_ui_screen_list_mode = UI_SCREEN_LIST_INBOX;
    } else if (g_ui_after_notice == UI_AFTER_ROSTER && g_ui_screen_tab == 1) {
        g_ui_screen_list_mode = UI_SCREEN_LIST_SQUAD;
    } else if (g_ui_after_notice == UI_AFTER_SHORTLIST && g_ui_screen_tab == 6) {
        g_ui_screen_list_mode = UI_SCREEN_LIST_SHORTLIST;
    }
}

static int32_t ui_callback_notice(int32_t selection);
static int32_t ui_callback_main(int32_t selection);
static int32_t ui_callback_market_player(int32_t selection);
static int32_t ui_callback_sales_player(int32_t selection);
static int32_t ui_callback_quick_sale(int32_t selection);
static int32_t ui_callback_shortlist(int32_t selection);
static int32_t ui_callback_club_filter(int32_t selection);
static int32_t ui_callback_position_filter(int32_t selection);
static int32_t ui_callback_bid_terms(int32_t selection);
static int32_t ui_callback_offer(int32_t selection);
static int32_t ui_callback_finances(int32_t selection);
static int32_t ui_callback_finance_history(int32_t selection);
static int32_t ui_callback_roster(int32_t selection);
static int32_t ui_callback_contract_renewal(int32_t selection);
static int32_t ui_callback_exact_amount(int32_t selection);
static int32_t ui_callback_search(int32_t selection);
static int32_t ui_callback_offer_counter_amount(int32_t selection);
static int32_t ui_callback_history(int32_t selection);
static void ui_show_market_player(uint32_t base, int32_t index);
static void ui_show_sales_player(uint32_t base, int32_t index);
static void ui_show_quick_sale(uint32_t base, int32_t index);
static void ui_show_shortlist(uint32_t base);
static void ui_show_shortlist_player(uint32_t base, int32_t index);
static void ui_show_club_filter(uint32_t base);
static void ui_show_position_filter(uint32_t base);
static void ui_show_bid_terms(uint32_t base);
static void ui_show_offer(uint32_t base, MarketOffer *offer);
static void ui_show_offer_result(uint32_t base, MarketOffer *offer, int32_t after_notice);
static void ui_show_inbox(uint32_t base);
static void ui_show_finance_history(uint32_t base);
static void ui_show_books(uint32_t base);
static void ui_show_finances(uint32_t base);
static void ui_show_roster(uint32_t base);
static void ui_show_contract_renewal(uint32_t base);
static void ui_show_history(uint32_t base, int32_t index);
static void ui_show_club_filter(uint32_t base);
static void ui_show_transfers_hub(uint32_t base);
static void ui_show_club_hub(uint32_t base);
static void ui_show_market_actions(uint32_t base);
static void ui_show_market_filters(uint32_t base);
static void ui_show_sale_options(uint32_t base);

/* ---------------------------------------------------------------------------------------------
 * Option-box presentation layer (v31). Every market page is one stock CFEMessageBoxOptions box
 * (or a CFEMsgKeyboard box for typed amounts / names). Rules:
 *  - Exactly one market box is alive at a time (g_ui_live_box / g_ui_live_slot).
 *  - Each box is built from its own slot of a 2-slot ring (g_ui_boxes). The page builders still
 *    write the scratch buffers g_ui_title / g_ui_description / g_ui_options, which are copied into
 *    a slot that is not the live box's, so building the next page never rewrites a box on screen.
 *  - A box shows at most 3 buttons. A page with more options shows two of them plus "More...", and
 *    "More..." steps through the rest (ui_box_chunk_layout). Page callbacks still receive the page's
 *    own option index, so the market logic behind a page does not depend on the paging.
 *  - A page requested while a box is alive (in particular from inside a box callback) is only
 *    recorded (g_ui_pending_slot) and shown once that box has been deleted.
 *
 * Why the deferral, and where the pending page is shown (libDLS18 5.064 disassembly):
 * CFEMessageBoxQueue::Process 0x24ff06 calls the pressed box's callback (box+0x40c, blx at 0x24ff2e)
 * and only then deletes that box (DeleteChild 0x24ff74, slot cleared 0x24ff7a, SetActiveMessageBox
 * 0x24ff7e). A box added from the callback therefore coexisted with the old one. There is no stock
 * or runtime hook after that deletion, and a new runtime hook would need a new libDLS18. So the layer
 * wraps the queue's own virtual Process (vtable +0x14, called every frame by
 * CFEComponent::ProcessAll, cf. CFEEntity::ProcessAll 0x25f740): the queue object's vtable pointer
 * is pointed at a copy of _ZTV18CFEMessageBoxQueue (0x71d210, 176 bytes); the copied Process slot
 * runs the stock Process and then shows the pending page once the old box is gone from the queue
 * slots (+0xe8..+0xf4, 4 entries). Only a heap object's vtable word is written (the market screen
 * uses the same vtable-copy technique); no code page is touched. The library is pinned (dlopen)
 * before the copy is installed, so the wrapper cannot be unloaded. If the queue cannot be found or
 * its vtable is not the stock one, boxes are shown immediately as in v30.
 * ------------------------------------------------------------------------------------------- */
#define MARKET_UI_BOX_BUTTONS 3
#define MARKET_UI_BOX_SLOTS 2
#define MARKET_UI_MORE (-1)
#define UI_QUEUE_GET_FN 0x2609B5u          /* CFEEntityManager::GetMessageBoxQueue 0x2609b4 (Thumb) */
#define UI_QUEUE_VTABLE 0x71D210u          /* _ZTV18CFEMessageBoxQueue */
#define UI_QUEUE_VTABLE_WORDS 44           /* 176 bytes: offset-to-top, typeinfo, 42 methods */
#define UI_QUEUE_PROCESS_WORD 7            /* +8 header + Process at +0x14 */
#define UI_QUEUE_SLOTS 0xE8                /* CFEMessageBox *slots[4] */
#define UI_QUEUE_SLOT_COUNT 4

enum MarketUiBoxKind { UI_BOX_FREE = 0, UI_BOX_OPTIONS = 1, UI_BOX_KEYBOARD = 2,
                       UI_BOX_SIGN = 3, UI_BOX_SELL = 4 };   /* stock CFEMsgSignPlayer / CFEMsgSellPlayer */

typedef struct {
    int32_t kind;
    int32_t page_count;                    /* options of the page (<= MARKET_UI_OPTION_LIMIT) */
    int32_t chunk;                         /* which group of the page this box shows */
    int32_t button_count;                  /* buttons in this box (<= 3) */
    int32_t button_map[MARKET_UI_BOX_BUTTONS];   /* button -> page option, MARKET_UI_MORE */
    int32_t keyboard_max;
    MarketUiCallback callback;
    uint16_t title[48];
    uint16_t description[MARKET_UI_TEXT_CAPACITY];
    uint16_t page_options[MARKET_UI_OPTION_LIMIT][MARKET_UI_OPTION_STRIDE];
    uint16_t buttons[MARKET_UI_BOX_BUTTONS][MARKET_UI_OPTION_STRIDE];
    uint16_t keyboard_initial[48];
    int32_t card_player;                   /* UI_BOX_SIGN / UI_BOX_SELL: player, club, coin-button price */
    int32_t card_team;
    int32_t card_price;
} MarketUiBox;

typedef int32_t (*QueueProcessFn)(void *);
typedef void *(*QueueGetFn)(void);
typedef void *(*MarketUiBoxFactory)(uint32_t base, MarketUiBox *box);

static MarketUiBox g_ui_boxes[MARKET_UI_BOX_SLOTS];
static PlayerInfo g_ui_card_info[MARKET_UI_BOX_SLOTS];   /* the dialogs keep a raw TPlayerInfo pointer */
static void *g_ui_live_box;
static int32_t g_ui_live_slot = -1;
static int32_t g_ui_pending_slot = -1;
static int32_t g_ui_in_callback;
static MarketUiCallback g_ui_invoking_callback;
static int32_t g_ui_page_chunk;
static uint32_t g_ui_queue_vtable[UI_QUEUE_VTABLE_WORDS];
static QueueProcessFn g_ui_queue_process;
static void *g_ui_queue_hooked;            /* queue object that runs the copied vtable */
static void *g_ui_queue_override;          /* host tests: a fake queue */
static int32_t ui_box_callback(int32_t button);
static void *ui_box_create_native(uint32_t base, MarketUiBox *box);
static MarketUiBoxFactory g_ui_box_factory = ui_box_create_native;

static void ui_wide_copy(uint16_t *dst, const uint16_t *src, int32_t capacity) {
    int32_t i = 0;
    if (src) {
        for (; i + 1 < capacity && src[i]; ++i) dst[i] = src[i];
    }
    for (; i < capacity; ++i) dst[i] = 0;
}

/* Number of boxes a page with `count` options needs: up to 3 options fit one box; otherwise every
 * box but the last shows two options plus "More...", and the last shows the remaining 2 or 3. */
static int32_t ui_box_chunk_count(int32_t count) {
    if (count <= MARKET_UI_BOX_BUTTONS) return 1;
    return 1 + (count - 2) / 2;
}

/* Fills map with the page option shown on each button of box `chunk`; MARKET_UI_MORE = "More...".
 * A last box with a free button gets "More..." too, which wraps to the first box. */
static int32_t ui_box_chunk_layout(int32_t count, int32_t chunk, int32_t *map) {
    if (count < 1 || count > MARKET_UI_OPTION_LIMIT) return 0;
    int32_t chunks = ui_box_chunk_count(count);
    if (chunk < 0 || chunk >= chunks) chunk = 0;
    int32_t start = chunk * 2;
    int32_t end = chunk + 1 < chunks ? start + 2 : count;
    int32_t n = 0;
    for (int32_t i = start; i < end && n < MARKET_UI_BOX_BUTTONS; ++i) map[n++] = i;
    if (chunks > 1 && n < MARKET_UI_BOX_BUTTONS) map[n++] = MARKET_UI_MORE;
    return n;
}

static void ui_box_layout(MarketUiBox *box) {
    if (box->chunk < 0 || box->chunk >= ui_box_chunk_count(box->page_count)) box->chunk = 0;
    box->button_count = ui_box_chunk_layout(box->page_count, box->chunk, box->button_map);
    for (int32_t b = 0; b < MARKET_UI_BOX_BUTTONS; ++b) {
        MarketUiText builder;
        ui_text_reset(&builder, box->buttons[b], MARKET_UI_OPTION_STRIDE);
        for (int32_t c = 0; c < MARKET_UI_OPTION_STRIDE; ++c) box->buttons[b][c] = 0;
        if (b >= box->button_count) continue;
        if (box->button_map[b] == MARKET_UI_MORE) ui_text_append_ascii(&builder, "More...");
        else ui_text_append_wide(&builder, box->page_options[box->button_map[b]],
                                 MARKET_UI_OPTION_STRIDE - 1);
    }
}

static void *ui_queue_get(uint32_t base) {
    if (g_ui_queue_override) return g_ui_queue_override;
    if (!base) return (void *)0;
    QueueGetFn get_queue = (QueueGetFn)(base + UI_QUEUE_GET_FN);
    return get_queue();
}

static int32_t ui_queue_holds(void *queue, void *box) {
    if (!queue || !box) return 0;
    void *const *slots = (void *const *)((const char *)queue + UI_QUEUE_SLOTS);
    for (int32_t i = 0; i < UI_QUEUE_SLOT_COUNT; ++i) {
        if (slots[i] == box) return 1;
    }
    return 0;
}

static int32_t ui_box_open(uint32_t base, int32_t slot) {
    if (slot < 0 || slot >= MARKET_UI_BOX_SLOTS) return 0;
    MarketUiBox *box = &g_ui_boxes[slot];
    if (box->kind == UI_BOX_FREE) return 0;
    void *object = g_ui_box_factory(base, box);
    if (!object) {
        box->kind = UI_BOX_FREE;
        return 0;
    }
    g_ui_live_box = object;
    g_ui_live_slot = slot;
    if (box->kind == UI_BOX_KEYBOARD) g_ui_keyboard_box = object;
    return 1;
}

/* After the stock Process: forget a deleted box, then show the page that waited for it. */
static void ui_box_flush(void *queue) {
    if (g_ui_live_box && !ui_queue_holds(queue, g_ui_live_box)) {
        g_ui_live_box = (void *)0;
        g_ui_live_slot = -1;
    }
    if (g_ui_live_box || g_ui_in_callback || g_ui_pending_slot < 0) return;
    int32_t slot = g_ui_pending_slot;
    g_ui_pending_slot = -1;
    ui_box_open(g_ui_base, slot);
}

static int32_t ui_queue_process_hook(void *queue) {
    int32_t result = g_ui_queue_process ? g_ui_queue_process(queue) : 0;
    ui_box_flush(queue);
    return result;
}

static int32_t ui_queue_hook_install(uint32_t base) {
    if (g_ui_queue_override) {
        g_ui_queue_hooked = g_ui_queue_override;
        return 1;
    }
    if (!base) return 0;
    void *queue = ui_queue_get(base);
    if (!queue) return 0;
    uint32_t *object = (uint32_t *)queue;
    uint32_t copy = (uint32_t)(uintptr_t)&g_ui_queue_vtable[2];
    if (object[0] == copy && g_ui_queue_process) {
        g_ui_queue_hooked = queue;
        return 1;
    }
    if (object[0] != base + UI_QUEUE_VTABLE + 8u) return 0;
    pin_library(base);                     /* the copied vtable points into this library */
    if (!g_library_pinned) return 0;
    const uint32_t *source = (const uint32_t *)(uintptr_t)(base + UI_QUEUE_VTABLE);
    for (int32_t i = 0; i < UI_QUEUE_VTABLE_WORDS; ++i) g_ui_queue_vtable[i] = source[i];
    g_ui_queue_process = (QueueProcessFn)(uintptr_t)source[UI_QUEUE_PROCESS_WORD];
    g_ui_queue_vtable[UI_QUEUE_PROCESS_WORD] = (uint32_t)(uintptr_t)ui_queue_process_hook | 1u;
    object[0] = copy;
    g_ui_queue_hooked = queue;
    return 1;
}

/* The ring slot for the next box: never the live box's; a page requested again before the pending
 * one was shown replaces it (the last request wins). */
static int32_t ui_box_acquire(void) {
    if (g_ui_pending_slot >= 0 && g_ui_pending_slot < MARKET_UI_BOX_SLOTS) return g_ui_pending_slot;
    for (int32_t i = 0; i < MARKET_UI_BOX_SLOTS; ++i) {
        if (i != g_ui_live_slot) return i;
    }
    return 0;
}

/* Show the box in `slot` now, or record it until the live box has been deleted. Returns 0 only if
 * an immediate box could not be created. */
static int32_t ui_box_submit(uint32_t base, int32_t slot) {
    if (ui_queue_hook_install(base)) {
        if (g_ui_live_box && !ui_queue_holds(g_ui_queue_hooked, g_ui_live_box)) {
            g_ui_live_box = (void *)0;
            g_ui_live_slot = -1;
        }
        if (g_ui_in_callback || g_ui_live_box) {
            g_ui_pending_slot = slot;
            return 1;
        }
    }
    if (g_ui_pending_slot == slot) g_ui_pending_slot = -1;
    return ui_box_open(base, slot);
}

static void *ui_box_create_native(uint32_t base, MarketUiBox *box) {
    if (!base || !box) return (void *)0;
    GameNewFn game_new = (GameNewFn)(base + 0x1C06AC);                  /* PLT (ARM): even */
    AddMessageBoxFn add_message = (AddMessageBoxFn)(base + 0x298609);
    void *object;
    if (box->kind == UI_BOX_KEYBOARD) {
        MessageKeyboardCtorFn constructor = (MessageKeyboardCtorFn)(base + 0x24D785);
        object = game_new(0xCE8, 0, 0);
        if (!object) return (void *)0;
        constructor(object, box->title, box->description, box->keyboard_initial,
                    box->keyboard_max, 0, ui_box_callback);
    } else if (box->kind == UI_BOX_SIGN || box->kind == UI_BOX_SELL) {
        /* data/UI_COMPONENTS.md 1-2: the stock player-card dialogs with our price, title, text and callback.
         * Button 0 = cross (decline), 1 = coin (accept). The coin button reads the price at Init. */
        PlayerInfo *info = &g_ui_card_info[box - g_ui_boxes];
        if (!load_player_info(base, box->card_player, info)) return (void *)0;
        object = game_new(0x4E0, 0, 0);
        if (!object) return (void *)0;
        if (box->kind == UI_BOX_SIGN) {
            SignPlayerCtorFn constructor = (SignPlayerCtorFn)(base + 0x251301);
            constructor(object, info, box->card_team, box->card_team, 0, 0, ui_box_callback);
            *(volatile int32_t *)(uintptr_t)(base + 0x7601DC) = box->card_price;   /* ms_iPlayerValue */
            *(uint8_t *)((uint8_t *)object + 0x4DC) = 0;                          /* no secret-player art */
        } else {
            SellPlayerCtorFn constructor = (SellPlayerCtorFn)(base + 0x251161);
            constructor(object, info, ui_box_callback);
        }
        *(int32_t *)((uint8_t *)object + 0x47C) = box->card_price;
        ((BoxTextFn)(base + 0x21766B))(object, box->title);                      /* CFEArea::SetTitle */
        ((BoxTextFn)(base + 0x248BCD))(object, box->description);                /* SetDescriptionText */
    } else {
        MessageOptionsCtorFn constructor = (MessageOptionsCtorFn)(base + 0x24DD59);
        if (box->button_count < 1) return (void *)0;
        object = game_new(0x4E0, 0, 0);
        if (!object) return (void *)0;
        constructor(object, &box->buttons[0][0], box->button_count, MARKET_UI_OPTION_STRIDE,
                    ui_box_callback, box->title, box->description, 0);
    }
    add_message(object);
    return object;
}

/* "More...": the same page (copied from the pressed box) with its next group of options. */
static void ui_box_show_chunk(const MarketUiBox *source, int32_t chunk) {
    int32_t slot = ui_box_acquire();
    MarketUiBox *box = &g_ui_boxes[slot];
    if (box != source) *box = *source;
    box->chunk = chunk;
    ui_box_layout(box);
    g_ui_page_chunk = box->chunk;
    ui_box_submit(g_ui_base, slot);
}

/* The one callback every market box gets: maps the pressed button to the page option (or pages on
 * "More..."), runs the page callback, and lets the queue delete the box (return 1). */
static int32_t ui_box_callback(int32_t button) {
    int32_t slot = g_ui_live_slot;
    if (slot < 0 || slot >= MARKET_UI_BOX_SLOTS) return 1;
    MarketUiBox *box = &g_ui_boxes[slot];
    int32_t selection = button;
    if (box->kind == UI_BOX_OPTIONS) {
        selection = button >= 0 && button < box->button_count ? box->button_map[button] : -2;
    }
    MarketUiCallback callback = box->callback;
    g_ui_in_callback = 1;
    if (box->kind == UI_BOX_OPTIONS && selection == MARKET_UI_MORE) {
        ui_box_show_chunk(box, (box->chunk + 1) % ui_box_chunk_count(box->page_count));
    } else if (callback) {
        g_ui_invoking_callback = callback;
        callback(selection);
    }
    g_ui_invoking_callback = (MarketUiCallback)0;
    g_ui_in_callback = 0;
    return 1;
}

/* Keyboard box from the scratch title/description/initial text. Returns 0 if it could not open. */
static int32_t ui_show_keyboard(uint32_t base, int32_t max_chars, MarketUiCallback callback) {
    if (!base || !callback) return 0;
    int32_t slot = ui_box_acquire();
    MarketUiBox *box = &g_ui_boxes[slot];
    box->kind = UI_BOX_KEYBOARD;
    box->page_count = 0;
    box->chunk = 0;
    box->button_count = 0;
    box->keyboard_max = max_chars;
    box->callback = callback;
    ui_wide_copy(box->title, g_ui_title, 48);
    ui_wide_copy(box->description, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_wide_copy(box->keyboard_initial, g_ui_keyboard_initial, 48);
    g_ui_base = base;
    return ui_box_submit(base, slot);
}

static void ui_open_player_search(uint32_t base) {
    if (!base) return;
    ui_set_title("Search Players");
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_keyboard_initial, 48);
    ui_text_append_wide(&builder, g_ui_search_query, 47);
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Player name");
    if (!ui_show_keyboard(base, 31, ui_callback_search)) {
        g_ui_keyboard_box = (void *)0;
        ui_show_club_filter(base);
    }
}

static uint16_t ui_lower_ascii(uint16_t c) {
    return c >= 'A' && c <= 'Z' ? (uint16_t)(c + ('a' - 'A')) : c;
}

static int32_t ui_wide_contains(const uint16_t *text, const uint16_t *query) {
    if (!query || !query[0]) return 1;
    if (!text) return 0;
    for (int32_t start = 0; text[start]; ++start) {
        int32_t offset = 0;
        while (query[offset] && text[start + offset] &&
               ui_lower_ascii(text[start + offset]) == ui_lower_ascii(query[offset])) {
            ++offset;
        }
        if (!query[offset]) return 1;
    }
    return 0;
}

static void ui_rebuild_search_results(uint32_t base) {
    g_ui_search_match_count = 0;
    g_ui_search_results_valid = 1;
    if (!g_ui_search_query[0]) return;
    GetPlayerNameFn get_name = (GetPlayerNameFn)(base + 0x2938CD);
    for (int32_t i = 0; i < g_player_count && i < MAX_PLAYERS; ++i) {
        PlayerInfo info;
        uint16_t name[128];
        for (int32_t c = 0; c < 128; ++c) name[c] = 0;
        if (!load_player_info(base, g_players[i].player_id, &info)) continue;
        get_name(name, 128, &info, 500.0f, 0, 0);
        if (ui_wide_contains(name, g_ui_search_query)) {
            g_ui_search_matches[g_ui_search_match_count++] = i;
        }
    }
}

static int32_t ui_parse_positive_amount(const uint16_t *text, int32_t *amount) {
    uint32_t value = 0;
    int32_t digits = 0;
    if (!text || !amount) return 0;
    for (int32_t i = 0; text[i]; ++i) {
        uint16_t c = text[i];
        if (c < '0' || c > '9' || digits >= 10) return 0;
        uint32_t digit = (uint32_t)(c - '0');
        if (value > (0x7fffffffu - digit) / 10u) return 0;
        value = value * 10u + digit;
        ++digits;
    }
    if (digits < 1 || value == 0) return 0;
    *amount = (int32_t)value;
    return 1;
}

static void ui_append_amount_error(MarketUiText *builder) {
    if (g_ui_amount_error == UI_AMOUNT_ERROR_NONE) return;
    ui_text_append_ascii(builder, "\nAmount not changed: ");
    if (g_ui_amount_error == UI_AMOUNT_ERROR_INVALID) {
        ui_text_append_ascii(builder, "enter a positive whole number using digits only.");
    } else if (g_ui_amount_error == UI_AMOUNT_ERROR_FEE_BUDGET) {
        ui_text_append_ascii(builder, "the fee exceeds your available transfer budget.");
    } else if (g_ui_amount_error == UI_AMOUNT_ERROR_WAGE_BUDGET) {
        ui_text_append_ascii(builder, "the wage exceeds your available annual wage budget.");
    } else {
        ui_text_append_ascii(builder, "exact entry could not open; use the step choices.");
    }
    g_ui_amount_error = UI_AMOUNT_ERROR_NONE;
}

static void ui_open_exact_amount(uint32_t base, int32_t target, const char *title,
                                 int32_t current_amount) {
    if (!base || target == UI_EXACT_AMOUNT_NONE) return;
    ui_set_title(title);
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_keyboard_initial, 16);
    ui_text_append_i32(&builder, current_amount);
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Digits only");

    g_ui_exact_amount_target = target;
    if (!ui_show_keyboard(base, 10, ui_callback_exact_amount)) {
        g_ui_exact_amount_target = UI_EXACT_AMOUNT_NONE;
        g_ui_keyboard_box = (void *)0;
        g_ui_amount_error = UI_AMOUNT_ERROR_UNAVAILABLE;
        if (target == UI_EXACT_AMOUNT_RENEWAL_WAGE) ui_show_contract_renewal(base);
        else ui_show_bid_terms(base);
    }
}

static void ui_show_options(uint32_t base, const char *title, const uint16_t *description,
                            const char *const *options, int32_t option_count,
                            int32_t (*callback)(int32_t)) {
    if (!base || !description || !options || option_count < 1 ||
        option_count > MARKET_UI_OPTION_LIMIT) return;
    /* A page that re-shows itself from its own button (e.g. "Next club") keeps its "More..." group. */
    int32_t same_page = g_ui_in_callback && g_ui_invoking_callback == callback &&
                        g_ui_page_callback == callback && g_ui_option_count == option_count &&
                        ui_wide_equals_ascii(g_ui_title, title);
    if (description != g_ui_description) {
        MarketUiText builder;
        ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
        ui_text_append_wide(&builder, description, MARKET_UI_TEXT_CAPACITY - 1);
    }
    for (int32_t i = 0; i < option_count; ++i) ui_set_option(i, options[i]);
    for (int32_t i = option_count; i < MARKET_UI_OPTION_LIMIT; ++i) {
        for (int32_t c = 0; c < MARKET_UI_OPTION_STRIDE; ++c) g_ui_options[i][c] = 0;
    }
    ui_set_title(title);
    g_ui_base = base;
    g_ui_option_count = option_count;
    g_ui_page_callback = callback;
    ui_screen_update_tab(title);
    ui_screen_update_list(title);
    g_ui_screen_dirty = 1;
#if MARKET_USE_FULLSCREEN
    (void)same_page;
    market_ui_open_screen(base);
#else
    /* v30: the full-screen market (v15-v27) was never run on a device and on the tablet it drew over
     * the stock screen-32 frame ("SAFE MODE") with overlapping boxes and no working input. Every
     * market page is the game's own option box, through the v31 presentation layer above. */
    int32_t slot = ui_box_acquire();
    MarketUiBox *box = &g_ui_boxes[slot];
    box->kind = UI_BOX_OPTIONS;
    box->page_count = option_count;
    box->chunk = same_page ? g_ui_page_chunk : 0;
    box->keyboard_max = 0;
    box->callback = callback;
    ui_wide_copy(box->title, g_ui_title, 48);
    ui_wide_copy(box->description, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    for (int32_t i = 0; i < MARKET_UI_OPTION_LIMIT; ++i) {
        ui_wide_copy(box->page_options[i], g_ui_options[i], MARKET_UI_OPTION_STRIDE);
    }
    ui_box_layout(box);
    g_ui_page_chunk = box->chunk;
    ui_box_submit(base, slot);
#endif
}

static void ui_show_notice(uint32_t base, const char *title, const char *message,
                           int32_t after_notice) {
    static const char *const options[] = {"Continue"};
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, message);
    g_ui_after_notice = after_notice;
    ui_show_options(base, title, g_ui_description, options, 1, ui_callback_notice);
}

/* Why a user bid is refused, with the numbers the player or club is judging. */
static void ui_show_refusal(uint32_t base, const MarketPlayer *player, int32_t code,
                            int32_t after_notice) {
    static const char *const options[] = {"Continue"};
    int32_t user_index = find_account(USER_TEAM_ID);
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    if (code == -11) {
        ui_text_append_ascii(&builder, "He is rated ");
        ui_text_append_i32(&builder, player->rating);
        ui_text_append_ascii(&builder, " and your best XI is ");
        ui_text_append_i32(&builder, user_index >= 0 ? g_market.clubs[user_index].strength : 0);
        ui_text_append_ascii(&builder, ". He will join a club this much smaller only if your best XI is within ");
        ui_text_append_i32(&builder, USER_STAR_STRENGTH_GAP - 1);
        ui_text_append_ascii(&builder, " of his rating. Sign stronger starters first.");
    } else if (code == -17) {
        ui_text_append_ascii(&builder, "The club needs to keep at least 16 players and one goalkeeper. "
                                       "They cannot release this player without falling below that cover.");
    } else if (code == -16) {
        ui_text_append_ascii(&builder, "Talks with this club have already broken down for this transfer window. "
                                       "Try again when a new window opens.");
    } else {
        ui_text_append_ascii(&builder, "He is their key player and they will not sell him to a club this much smaller. "
                                       "Grow your squad value and try again.");
    }
    g_ui_after_notice = after_notice;
    const char *title = code == -11 ? "Player Not Interested"
        : code == -17 ? "Squad Too Thin"
        : code == -16 ? "Talks Ended" : "Club Will Not Sell";
    ui_show_options(base, title,
                    g_ui_description, options, 1, ui_callback_notice);
}

/* ---------------------------------------------------------------------------------------------
 * Real per-match income (v6 hook a): wired from build_mod.py at 0x23a274, over the stock
 * `blx SetMatchCredits` at the end of CFEPostMatchCreditAwards::SetupCreditAwardInfo. Runs once per
 * finished career match, after the stock reward rows are built and before they credit; replaces the
 * stock result/goals/clean-sheet/stadium rows with the mod's own income breakdown and lets the real
 * SetMatchCredits/CFE::Process flow do the crediting, so the post-match screen's coin counter still
 * animates. See data/ECONOMY_HOOKS.md section 1 for every address below (tagged [V] there).
 * ------------------------------------------------------------------------------------------- */
#define MATCH_SETUP_INFO 0x7C2AAC          /* CMatchSetup::ms_tInfo */
#define TGAME_BASE 0x78C5F8                /* tGame */
#include "chemistry.c"
#define MC_IN_POST_MATCH_CALLBACK 0x83F250
#define CREDIT_AWARD_ROWS 0x75D7D4         /* CFEPostMatchCreditAwards::ms_tCreditAwardInfo, stride 0x208 */
#define CREDIT_AWARD_ROW_STRIDE 0x208
#define CREDIT_AWARD_COUNT 0x75F034        /* ms_iCreditAwardCount: directly after the row array */
#define CREDIT_ROW_TOTAL_TYPE 8
#define CREDIT_KEEP_MAX 6                  /* stock rows kept alongside the mod's income rows */
#define CREDIT_DETAIL_MAX 9                /* CFESMatchSummary draws 9 detail rows + the total */

/* Winning a cup pays 12% of the division's season target (the stock award was 50 coins). */
static int32_t cup_winners_prize(int32_t division) {
    return mul_div(division_target(division), 12, 100);
}
#define PROFILE_PENDING_MATCH_CREDITS 0x2A7E8
typedef void (*SetMatchCreditsFn)(uint32_t, int32_t);
typedef void *(*GetSpecificTournamentByTurnFn)(void *, int32_t);
typedef int32_t (*GetTournamentIdFn)(void *);
typedef int32_t (*IsTournamentLeagueFn)(int32_t);
typedef int32_t (*GetUserLeagueInTreeFn)(void *);
typedef int32_t (*GetTeamLeaguePosFn)(void *, int32_t);

/* Writes one TCreditAwardInfo row in place: +0 type, +4 label (u16, NUL-terminated), +0x204 coins. */
static void write_credit_row(uint8_t *row, const char *label, int32_t type, int32_t coins) {
    MarketUiText builder;
    ui_text_reset(&builder, (uint16_t *)(row + 4), 256);
    ui_text_append_ascii(&builder, label);
    *(int32_t *)row = type;
    *(int32_t *)(row + 0x204) = coins;
}

__attribute__((visibility("default")))
int32_t career_market_on_match_awards(int32_t unused0, int32_t unused1, uint32_t base) {
    (void)unused0; (void)unused1;
    if (!base || !state_is_valid()) return 0;
    apply_price_schedule(base);
    uint8_t *tinfo = (uint8_t *)(uintptr_t)(base + MATCH_SETUP_INFO);
    if (*(int32_t *)(tinfo + 0xfb0) != -1) return 0;             /* DLO match: leave to the stock path */
    if (*(uint8_t *)(uintptr_t)(base + MC_IN_POST_MATCH_CALLBACK) != 1) return 0;
    uint8_t *tgame = (uint8_t *)(uintptr_t)(base + TGAME_BASE);
    if (*(int32_t *)(tgame + 0x9ebc) != 0) return 0;             /* forfeit/quit: keep the stock reduced rows */

    void *season = (void *)(uintptr_t)(base + PROFILE_INSTANCE + 0x14);
    GetUserLeagueInTreeFn get_league = (GetUserLeagueInTreeFn)(base + 0x36CC41);
    int32_t division = get_league(season);
    if (division < 0 || division >= DIVISION_COUNT) return 0;

    int32_t raw_side = *(int32_t *)(tinfo + 0x40);
    int32_t side = raw_side == 2 ? -1 : raw_side;
    if (side < 0) return 0;
    int32_t sw = *(int32_t *)(tgame + 0x9ed4);
    chem_on_match(base, (side ^ sw) & 1, side);    /* v39: match ratings and chemistry */
    int32_t us = *(uint8_t *)(tgame + 0x9edc + (side ^ sw));
    int32_t them = *(uint8_t *)(tgame + 0x9edc + ((1 - side) ^ sw));
    int32_t home = *(int32_t *)(tinfo + 0xf6c) == USER_TEAM_ID;
    int32_t neutral = *(uint8_t *)(tinfo + 0x14) != 0;

    int32_t turn_slot = *(int32_t *)((uint8_t *)season + 0x6030);
    GetSpecificTournamentByTurnFn get_tournament = (GetSpecificTournamentByTurnFn)(base + 0x36A8FD);
    void *t = get_tournament(season, turn_slot);
    int32_t friendly = 0, cup = 0, is_league = 1;
    if (t) {
        GetTournamentIdFn get_id = (GetTournamentIdFn)(base + 0x360A31);
        IsTournamentLeagueFn is_league_fn = (IsTournamentLeagueFn)(base + 0x3646A1);
        int32_t tid = get_id(t);
        is_league = is_league_fn(tid);
        friendly = !is_league && tid == 11;      /* tid list: data/STOCK_TRANSFER_FLOW.md 5.2 */
        cup = !is_league && !friendly;
    }

    MatchFacts facts;
    facts.division = division;
    facts.cup = cup;
    facts.friendly = friendly;
    facts.home = home && !neutral;
    facts.goals_for = us;
    facts.goals_against = them;
    /* the real crowd (CSeason::CalculateAttendance for a user home match); 0 falls back to the
     * division's reference crowd */
    int32_t attendance = *(int32_t *)(tinfo + 0xf64);
    facts.capacity = attendance > 0 && attendance < 200000 ? attendance : 0;

    int32_t total = user_match_payout(base, &facts, 0);   /* SetMatchCredits below does the crediting */

    /* keep the stock rows the mod does not replace: achievements (types 4 and 7) and the tournament
     * award (type 5) - a cup win becomes the mod's cup winners' prize, a friendly or all-star bonus
     * stays; the league position award is dropped because the season-end payout replaces it */
    static uint8_t kept[CREDIT_KEEP_MAX][CREDIT_AWARD_ROW_STRIDE];
    int32_t kept_count = 0, kept_total = 0;
    {
        uint8_t *stock = (uint8_t *)(uintptr_t)(base + CREDIT_AWARD_ROWS);
        int32_t stock_count = *(int32_t *)(uintptr_t)(base + CREDIT_AWARD_COUNT);
        if (stock_count < 0 || stock_count > 12) stock_count = 0;
        for (int32_t i = 0; i < stock_count && kept_count < CREDIT_KEEP_MAX; ++i) {
            uint8_t *row = stock + (uint32_t)i * CREDIT_AWARD_ROW_STRIDE;
            int32_t type = *(int32_t *)row;
            int32_t coins = *(int32_t *)(row + 0x204);
            if (type == 5 && is_league) continue;
            if (type != 4 && type != 5 && type != 7) continue;
            uint8_t *copy = kept[kept_count];
            for (uint32_t b = 0; b < CREDIT_AWARD_ROW_STRIDE; ++b) copy[b] = row[b];
            if (type == 5 && cup) {
                coins = cup_winners_prize(division);
                write_credit_row(copy, "Cup Winners Prize", 5, coins);
                book(ECON_CUP, coins);
            } else {
                book(ECON_AWARDS, coins);
            }
            if (coins <= 0) continue;
            kept_total = clamp_add(kept_total, coins);
            ++kept_count;
        }
    }

    uint8_t *rows = (uint8_t *)(uintptr_t)(base + CREDIT_AWARD_ROWS);
    static const char *const labels[ECON_MATCH_ROWS] = {
        "Match Prize", "Gate Receipts", "Goal & Clean Sheet Bonus", "Cup Prize",
        "TV Rights", "Sponsorship", "League Position Prize", "Promotion Bonus"
    };
    int32_t count = 0;
    for (int32_t i = 0; i < ECON_MATCH_ROWS; ++i) {
        int32_t coins = g_ext.econ.last_match[i];
        if (!coins) continue;
        write_credit_row(rows + (uint32_t)count * CREDIT_AWARD_ROW_STRIDE, labels[i], i, coins);
        ++count;
    }
    /* the screen shows at most 9 detail rows above the total; fold surplus achievements into one */
    for (int32_t k = 0; k < kept_count; ++k) {
        uint8_t *row = rows + (uint32_t)count * CREDIT_AWARD_ROW_STRIDE;
        if (count == CREDIT_DETAIL_MAX - 1 && k < kept_count - 1) {
            int32_t rest = 0;
            for (int32_t m = k; m < kept_count; ++m) rest = clamp_add(rest, *(int32_t *)(kept[m] + 0x204));
            write_credit_row(row, "Achievements", 4, rest);
            ++count;
            break;
        }
        for (uint32_t b = 0; b < CREDIT_AWARD_ROW_STRIDE; ++b) row[b] = kept[k][b];
        ++count;
    }
    total = clamp_add(total, kept_total);
    write_credit_row(rows + (uint32_t)count * CREDIT_AWARD_ROW_STRIDE, "Total", CREDIT_ROW_TOTAL_TYPE, total);
    ++count;
    *(int32_t *)(uintptr_t)(base + CREDIT_AWARD_COUNT) = count;

    int32_t pending = *(int32_t *)(uintptr_t)(base + PROFILE_INSTANCE + PROFILE_PENDING_MATCH_CREDITS);
    SetMatchCreditsFn set_match_credits = (SetMatchCreditsFn)(base + 0x3776A5);
    set_match_credits(base + PROFILE_INSTANCE, total + pending);
    return 1;
}

/* ---------------------------------------------------------------------------------------------
 * Season-end payout (v6 hook b): wired from build_mod.py at 0x29900a, over the stock
 * `blx CSeason::NextSeason`, in CAVE2 (build_mod.py's cave_econ_season). The cave calls this symbol
 * twice: once before the rollover with season = profile+0x14 (tables still final for the season just
 * played) and once right after with season = NULL (the league tree has rolled over, so
 * promotion/relegation is known) - the two share one dlsym symbol/rodata string to save cave space,
 * discriminated by whether `season` is non-NULL. See data/ECONOMY_HOOKS.md section 3.
 * ------------------------------------------------------------------------------------------- */
static int32_t g_season_payout_division = -1;
static int32_t g_season_payout_position = -1;

__attribute__((visibility("default")))
int32_t career_market_on_season(void *season, int32_t block, uint32_t base) {
    if (season) {
        /* pre-rollover: record this season's final league position for the post-rollover payout */
        g_season_payout_division = -1;
        g_season_payout_position = -1;
        if (!base || !state_is_valid()) return block;
        GetUserLeagueInTreeFn get_league = (GetUserLeagueInTreeFn)(base + 0x36CC41);
        int32_t division = get_league(season);
        if (division < 0 || division >= DIVISION_COUNT) return block;
        void *league = *(void **)((uint8_t *)season + 0x6AC);   /* ETournamentIndex 0: the main league */
        int32_t position = -1;
        if (league) {
            GetTeamLeaguePosFn get_pos = (GetTeamLeaguePosFn)(base + 0x36202F);
            position = get_pos(league, USER_TEAM_ID);            /* 0-based, -1 if no table */
        }
        g_season_payout_division = division;
        g_season_payout_position = position;
        return block;
    }

    /* post-rollover: pay the season-end coins recorded above and show a summary box */
    int32_t old_division = g_season_payout_division;
    int32_t position = g_season_payout_position;
    g_season_payout_division = -1;
    g_season_payout_position = -1;
    if (!base || !state_is_valid() || old_division < 0) return 0;
    void *cur_season = (void *)(uintptr_t)(base + PROFILE_INSTANCE + 0x14);
    GetUserLeagueInTreeFn get_league = (GetUserLeagueInTreeFn)(base + 0x36CC41);
    int32_t new_division = get_league(cur_season);
    int32_t promoted = new_division >= 0 && new_division < old_division;
    int32_t relegated = new_division > old_division;
    int32_t position_1based = position >= 0 ? position + 1 : -1;
    int32_t total = user_season_end_payout(base, old_division, position_1based, promoted);
    if (total > 0) {
        static const char *const options[] = {"Continue"};
        MarketUiText builder;
        ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
        ui_text_append_ascii(&builder, promoted ? "Promoted! " : relegated ? "Relegated. " : "");
        if (position_1based > 0) {
            ui_text_append_ascii(&builder, "Final position: ");
            ui_text_append_i32(&builder, position_1based);
            ui_text_append_char(&builder, '\n');
        }
        int32_t rows[ECON_CATEGORIES];
        econ_season_end(old_division, position_1based, promoted, rows);
        static const int32_t lines[] = {ECON_TV, ECON_SPONSOR, ECON_LEAGUE_PRIZE, ECON_PROMOTION};
        for (int32_t i = 0; i < 4; ++i) {
            if (!rows[lines[i]]) continue;
            ui_text_append_ascii(&builder, book_label(lines[i]));
            ui_text_append_ascii(&builder, ": ");
            ui_text_append_i32(&builder, rows[lines[i]]);
            ui_text_append_char(&builder, '\n');
        }
        ui_text_append_ascii(&builder, "Total paid: ");
        ui_text_append_i32(&builder, total);
        ui_text_append_ascii(&builder, " coins\nSeason net (books): ");
        ui_append_signed(&builder, sat_add(books_sum(k_income_categories, (int32_t)(sizeof(k_income_categories) / sizeof(k_income_categories[0])), 0),
                                           books_sum(k_spend_categories, (int32_t)(sizeof(k_spend_categories) / sizeof(k_spend_categories[0])), 0)));
        g_ui_after_notice = UI_AFTER_CLOSE;
        ui_show_options(base, "Season Complete", g_ui_description, options, 1, ui_callback_notice);
    }
    return 1;
}

static int32_t ui_active_offer_count(void) {
    int32_t count = 0;
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        MarketOffer *offer = &g_market.offers[i];
        if (offer_is_active(offer) &&
            (offer->seller_id == USER_TEAM_ID || offer->buyer_id == USER_TEAM_ID)) ++count;
    }
    return count;
}

static MarketOffer *ui_find_offer_any(int32_t offer_id) {
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        if (g_market.offers[i].offer_id == offer_id &&
            g_market.offers[i].status != CM_OFFER_FREE) return &g_market.offers[i];
    }
    return (MarketOffer *)0;
}

static MarketOffer *ui_next_active_offer(int32_t start_index) {
    for (int32_t step = 0; step < OFFER_CAPACITY; ++step) {
        int32_t index = (start_index + step) % OFFER_CAPACITY;
        MarketOffer *offer = &g_market.offers[index];
        if (!offer_is_active(offer)) continue;
        if (offer->seller_id == USER_TEAM_ID || offer->buyer_id == USER_TEAM_ID) return offer;
    }
    return (MarketOffer *)0;
}

static void ui_append_player_name(MarketUiText *builder, uint32_t base, int32_t player_id) {
    PlayerInfo info;
    uint16_t name[128];
    for (int32_t i = 0; i < 128; ++i) name[i] = 0;
    if (load_player_info(base, player_id, &info)) {
        GetPlayerNameFn get_name = (GetPlayerNameFn)(base + 0x2938CD);
        get_name(name, 128, &info, 500.0f, 0, 0);
        if (name[0]) {
            ui_text_append_wide(builder, name, 127);
            return;
        }
    }
    ui_text_append_char(builder, '#');
    ui_text_append_i32(builder, player_id);
}

static void ui_append_team_name(MarketUiText *builder, uint32_t base, int32_t team_id) {
    GetTeamNameFn get_name = (GetTeamNameFn)(base + 0x20C0C9);
    const uint16_t *name = get_name(team_id, 0, 0);
    if (name && name[0]) {
        ui_text_append_wide(builder, name, 48);
        return;
    }
    ui_text_append_ascii(builder, "Club ");
    ui_text_append_i32(builder, team_id);
}

static void ui_append_position(MarketUiText *builder, int32_t position) {
    static const char *const labels[] = {"Goalkeeper", "Defender", "Midfielder", "Forward"};
    if (position >= 0 && position <= 3) {
        ui_text_append_ascii(builder, labels[position]);
    } else {
        ui_text_append_ascii(builder, "Position ");
        ui_text_append_i32(builder, position);
    }
}

static int32_t ui_find_market_player(int32_t from_index, int32_t direction) {
    int32_t count = g_player_count;
    if (count < 1) return -1;
    if (g_ui_search_query[0]) {
        if (!g_ui_search_results_valid) ui_rebuild_search_results(g_ui_base);
        count = g_ui_search_match_count;
        if (count < 1) return -1;
        int32_t step_direction = direction < 0 ? -1 : 1;
        int32_t cursor = step_direction < 0 ? 0 : -1;
        if (from_index >= 0) {
            int32_t low = 0;
            int32_t high = count - 1;
            while (low <= high) {
                int32_t middle = low + (high - low) / 2;
                int32_t candidate = g_ui_search_matches[middle];
                if (candidate == from_index) {
                    cursor = middle;
                    break;
                }
                if (candidate < from_index) low = middle + 1;
                else high = middle - 1;
            }
        }
        for (int32_t step = 0; step < count; ++step) {
            cursor += step_direction;
            if (cursor < 0) cursor = count - 1;
            if (cursor >= count) cursor = 0;
            int32_t index = g_ui_search_matches[cursor];
            MarketPlayer *player = &g_players[index];
            if (player->owner_id == USER_TEAM_ID || player->value <= 0) continue;
            if (g_ui_position_filter >= 0 && player->position != g_ui_position_filter) continue;
            if (g_ui_club_filter >= 0 && player->owner_id != g_ui_club_filter) continue;
            if (has_active_offer_for_player(player->player_id)) continue;
            return index;
        }
        return -1;
    }
    int32_t index = from_index;
    for (int32_t step = 0; step < count; ++step) {
        if (index < 0) index = direction < 0 ? count - 1 : 0;
        else {
            index += direction;
            if (index < 0) index = count - 1;
            if (index >= count) index = 0;
        }
        MarketPlayer *player = &g_players[index];
        if (player->owner_id == USER_TEAM_ID || player->value <= 0) continue;
        if (g_ui_position_filter >= 0 && player->position != g_ui_position_filter) continue;
        if (g_ui_club_filter >= 0 && player->owner_id != g_ui_club_filter) continue;
        if (has_active_offer_for_player(player->player_id)) continue;
        return index;
    }
    return -1;
}

static int32_t ui_screen_sales_player_visible(int32_t index) {
    if (index < 0 || index >= g_player_count) return 0;
    MarketPlayer *player = &g_players[index];
    return player->owner_id == USER_TEAM_ID && player->value > 0 &&
           (g_ui_sales_position_filter < 0 || player->position == g_ui_sales_position_filter);
}

static int32_t ui_find_user_player(int32_t from_index, int32_t direction) {
    int32_t count = g_player_count;
    if (count < 1) return -1;
    int32_t index = from_index;
    for (int32_t step = 0; step < count; ++step) {
        if (index < 0) index = direction < 0 ? count - 1 : 0;
        else {
            index += direction;
            if (index < 0) index = count - 1;
            if (index >= count) index = 0;
        }
        MarketPlayer *player = &g_players[index];
        if (!ui_screen_sales_player_visible(index)) continue;
        if (has_active_offer_for_player(player->player_id)) continue;
        return index;
    }
    return -1;
}

static int32_t ui_find_shortlist_player(int32_t from_index, int32_t direction) {
    if (shortlist_count() < 1) return -1;
    int32_t step_direction = direction < 0 ? -1 : 1;
    int32_t cursor = step_direction < 0 ? 0 : -1;
    if (from_index >= 0 && from_index < g_player_count) {
        int32_t selected = shortlist_slot(g_players[from_index].player_id);
        if (selected >= 0) cursor = selected;
    }
    for (int32_t step = 0; step < SHORTLIST_CAPACITY; ++step) {
        cursor += step_direction;
        if (cursor < 0) cursor = SHORTLIST_CAPACITY - 1;
        if (cursor >= SHORTLIST_CAPACITY) cursor = 0;
        int32_t player_id = g_ext.shortlist[cursor];
        if (player_id < 0) continue;
        int32_t index = find_cached_player(player_id);
        if (index >= 0) return index;
    }
    return -1;
}

static int32_t ui_find_club_player(int32_t team_id, int32_t from_index, int32_t direction) {
    int32_t count = g_player_count;
    if (count < 1) return -1;
    int32_t index = from_index;
    for (int32_t step = 0; step < count; ++step) {
        if (index < 0) index = direction < 0 ? count - 1 : 0;
        else {
            index += direction;
            if (index < 0) index = count - 1;
            if (index >= count) index = 0;
        }
        if (g_players[index].owner_id == team_id) return index;
    }
    return -1;
}

static uint8_t g_ui_from_stock;   /* action started from the game's own transfer/squad screen */

static int32_t ui_callback_market_empty(int32_t selection);
static int32_t ui_callback_market_actions(int32_t selection);
static int32_t ui_callback_market_filters(int32_t selection);
static void ui_append_filter_summary(MarketUiText *builder, uint32_t base);

/* Market browser (v31): one player at a time. The v30 page offered only the shortlist/offer
 * actions and, because it passed 6 as the count of its 2-entry arrays, four more buttons read past
 * the arrays (on the tablet: "Remove Shortlist", "Make Offer", "Continue", "Confirm Sale") that
 * hung under the box. Now: Next Player, Player Actions, (More) Previous Player, Filters, Back. */
static void ui_show_market_player(uint32_t base, int32_t index) {
    static const char *const options[] = {
        "Next Player", "Player Actions", "Previous Player", "Filters", "Back"
    };
    static const char *const empty_options[] = {"Filters", "Back"};
    if (g_ui_from_stock) {           /* "Back" from a bid started on the game's screen: just close */
        g_ui_from_stock = 0;
        return;
    }
    if (g_market.window_id < 0) {
        ui_show_notice(base, "Transfer Market", "The transfer window is closed or no eligible players were found.", UI_AFTER_MENU);
        return;
    }
    MarketUiText builder;
    if (index < 0 || index >= g_player_count) {
        ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
        ui_text_append_ascii(&builder, "No eligible players match the current filters.\n");
        ui_append_filter_summary(&builder, base);
        ui_show_options(base, "Transfer Market", g_ui_description, empty_options, 2,
                        ui_callback_market_empty);
        return;
    }
    g_ui_player_index = index;
    MarketPlayer *player = &g_players[index];
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_append_player_name(&builder, base, player->player_id);
    ui_text_append_ascii(&builder, "\n");
    ui_append_position(&builder, player->position);
    ui_text_append_ascii(&builder, ", rating ");
    ui_text_append_i32(&builder, player->rating);
    ui_text_append_ascii(&builder, "\nClub: ");
    ui_append_team_name(&builder, base, player->owner_id);
    ui_text_append_ascii(&builder, "\nValue: ");
    ui_text_append_char(&builder, 0x0180);
    ui_text_append_char(&builder, ' ');
    ui_text_append_i32(&builder, player->value);
    ui_text_append_ascii(&builder, "  Wage: ");
    ui_text_append_i32(&builder, player->wage);
    if (shortlist_slot(player->player_id) >= 0) ui_text_append_ascii(&builder, "\nOn your shortlist");
    ui_text_append_ascii(&builder, "\n\n");
    ui_append_filter_summary(&builder, base);
    ui_show_options(base, "Transfer Market", g_ui_description, options, 5,
                    ui_callback_market_player);
}

/* Player page: full details plus Make Offer, Add to / Remove from Shortlist, Back. */
static void ui_show_market_actions(uint32_t base) {
    static const char *const add_options[] = {"Make Offer", "Add to Shortlist", "Back"};
    static const char *const remove_options[] = {"Make Offer", "Remove from Shortlist", "Back"};
    int32_t index = g_ui_player_index;
    if (g_market.window_id < 0 || index < 0 || index >= g_player_count) {
        ui_show_market_player(base, index);
        return;
    }
    MarketPlayer *player = &g_players[index];
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Player: ");
    ui_append_player_name(&builder, base, player->player_id);
    ui_text_append_ascii(&builder, "\nClub: ");
    ui_append_team_name(&builder, base, player->owner_id);
    ui_text_append_ascii(&builder, "\nPosition: ");
    ui_append_position(&builder, player->position);
    ui_text_append_ascii(&builder, "\nRating: ");
    ui_text_append_i32(&builder, player->rating);
    ui_text_append_ascii(&builder, "\nValue: ");
    ui_text_append_i32(&builder, player->value);
    ui_text_append_ascii(&builder, "\nAnnual wage: ");
    ui_text_append_i32(&builder, player->wage);
    ui_text_append_ascii(&builder, "\nShortlist: ");
    ui_text_append_ascii(&builder, shortlist_slot(player->player_id) >= 0 ? "Saved" : "Not saved");
    int32_t contract_term = contract_seasons_to_expiry(
        player->player_id, player->owner_id, g_market.season);
    if (contract_term >= 0) {
        ui_text_append_ascii(&builder, "\nContract expires: ");
        if (contract_term == 0) ui_text_append_ascii(&builder, "this season");
        else {
            ui_text_append_ascii(&builder, "about ");
            ui_text_append_i32(&builder, contract_term);
            ui_text_append_ascii(&builder, contract_term == 1 ? " season" : " seasons");
        }
    }
    ui_show_options(base, "Market Player", g_ui_description,
                    shortlist_slot(player->player_id) >= 0 ? remove_options : add_options,
                    3, ui_callback_market_actions);
}

/* Market filters: Position, Club / Name Search, Back. */
static void ui_show_market_filters(uint32_t base) {
    static const char *const options[] = {"Position", "Club / Name Search", "Back"};
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_append_filter_summary(&builder, base);
    ui_text_append_ascii(&builder, "\nFilters stay set while you browse.");
    ui_show_options(base, "Market Filters", g_ui_description, options, 3,
                    ui_callback_market_filters);
}

static int32_t ui_find_club_cursor(int32_t from_index, int32_t direction) {
    int32_t index = from_index;
    for (int32_t step = 0; step < MAX_CLUBS; ++step) {
        if (index < 0) index = direction < 0 ? MAX_CLUBS - 1 : 0;
        else {
            index += direction;
            if (index < 0) index = MAX_CLUBS - 1;
            if (index >= MAX_CLUBS) index = 0;
        }
        if (g_market.clubs[index].team_id >= 0) return index;
    }
    return -1;
}

static void ui_show_club_filter(uint32_t base) {
    static const char *const options[] = {
        "Next club", "Use this club", "Previous club", "All clubs",
        "Search player names", "Back"
    };
    MarketUiText builder;
    if (g_ui_club_cursor < 0 || g_ui_club_cursor >= MAX_CLUBS ||
        g_market.clubs[g_ui_club_cursor].team_id < 0) {
        g_ui_club_cursor = ui_find_club_cursor(-1, 1);
    }
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    if (g_ui_club_cursor >= 0) {
        ui_text_append_ascii(&builder, "Selected club: ");
        ui_append_team_name(&builder, base, g_market.clubs[g_ui_club_cursor].team_id);
    } else {
        ui_text_append_ascii(&builder, "No clubs are available in the current career.");
    }
    ui_text_append_ascii(&builder, "\nCurrent filter: ");
    if (g_ui_club_filter < 0) ui_text_append_ascii(&builder, "All clubs");
    else ui_append_team_name(&builder, base, g_ui_club_filter);
    ui_text_append_ascii(&builder, "\nName search: ");
    if (g_ui_search_query[0]) ui_text_append_wide(&builder, g_ui_search_query, 47);
    else ui_text_append_ascii(&builder, "All players");
    ui_show_options(base, "Market Club Filter", g_ui_description, options, 6,
                    ui_callback_club_filter);
}

static int32_t ui_callback_sales_browse(int32_t selection);
static int32_t ui_callback_sales_empty(int32_t selection);

/* Player Sales browser (v31): Next Player, Sale Options, (More) Previous Player, Position Filter,
 * Back. The sale actions themselves are on ui_show_sale_options. */
static void ui_show_sales_player(uint32_t base, int32_t index) {
    static const char *const options[] = {
        "Next Player", "Sale Options", "Previous Player", "Position Filter", "Back"
    };
    static const char *const empty_options[] = {"Position Filter", "Back"};
    MarketUiText builder;
    if (index < 0 || index >= g_player_count) {
        ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
        ui_text_append_ascii(&builder, "No eligible players were found in your squad.\nPosition filter: ");
        if (g_ui_sales_position_filter < 0) ui_text_append_ascii(&builder, "All");
        else ui_append_position(&builder, g_ui_sales_position_filter);
        ui_show_options(base, "Player Sales", g_ui_description, empty_options, 2,
                        ui_callback_sales_empty);
        return;
    }
    g_ui_player_index = index;
    MarketPlayer *player = &g_players[index];
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Player: ");
    ui_append_player_name(&builder, base, player->player_id);
    ui_text_append_ascii(&builder, "\nPosition: ");
    ui_append_position(&builder, player->position);
    ui_text_append_ascii(&builder, "\nRating: ");
    ui_text_append_i32(&builder, player->rating);
    ui_text_append_ascii(&builder, "\nEstimated value: ");
    ui_text_append_i32(&builder, player->value);
    ui_text_append_ascii(&builder, "\nAnnual wage: ");
    ui_text_append_i32(&builder, player->wage);
    ui_text_append_ascii(&builder, "\nSale status: ");
    ui_text_append_ascii(&builder, player->listed_for_sale ? "Listed" : "Not listed");
    ui_text_append_ascii(&builder, "\nPosition filter: ");
    if (g_ui_sales_position_filter < 0) ui_text_append_ascii(&builder, "All");
    else ui_append_position(&builder, g_ui_sales_position_filter);
    if (g_market.window_id < 0) {
        ui_text_append_ascii(&builder, "\nPlayer listings change during transfer windows.");
    } else {
        ui_text_append_ascii(&builder, "\nAI clubs may bid for listed players during this window.");
    }
    ui_show_options(base, "Player Sales", g_ui_description, options, 5, ui_callback_sales_browse);
}

/* Sale options for the selected squad player: List / Unlist, Quick Sale, Back. */
static void ui_show_sale_options(uint32_t base) {
    static const char *const options[] = {"List / Unlist", "Quick Sale", "Back"};
    int32_t index = g_ui_player_index;
    if (index < 0 || index >= g_player_count || g_players[index].owner_id != USER_TEAM_ID) {
        ui_show_sales_player(base, ui_find_user_player(-1, 1));
        return;
    }
    MarketPlayer *player = &g_players[index];
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Player: ");
    ui_append_player_name(&builder, base, player->player_id);
    ui_text_append_ascii(&builder, "\nSale status: ");
    ui_text_append_ascii(&builder, player->listed_for_sale ? "Listed" : "Not listed");
    ui_text_append_ascii(&builder, "\nList / Unlist: AI clubs may bid for listed players while the window is open."
                                   "\nQuick Sale: sell now for a clearance fee (you confirm first).");
    ui_show_options(base, "Sale Options", g_ui_description, options, 3, ui_callback_sales_player);
}

static void ui_show_quick_sale(uint32_t base, int32_t index) {
    static const char *const options[] = {"Confirm Sale", "Keep Player"};
    if (index < 0 || index >= g_player_count || !base) {
        ui_show_sales_player(base, index);
        return;
    }
    MarketPlayer *player = &g_players[index];
    g_ui_player_index = index;
    g_ui_quick_sale_player_id = player->player_id;
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Sell ");
    ui_append_player_name(&builder, base, player->player_id);
    ui_text_append_ascii(&builder, " immediately for a clearance fee.");
    if (g_market.window_id < 0) {
        ui_text_append_ascii(&builder, "\nQuick sales are only available during the transfer window.");
    } else if (user_signing_locked(player->player_id)) {
        ui_text_append_ascii(&builder, "\nThis signing is protected until next season.");
    } else if (has_active_offer_for_player(player->player_id)) {
        ui_text_append_ascii(&builder, "\nResolve the active offer before selling this player.");
    } else {
        int32_t fee = 0, wage = 0;
        int32_t buyer = quick_sale_buyer(player, &fee, &wage);
        if (buyer >= 0) {
            ui_text_append_ascii(&builder, "\nBuyer: ");
            ui_append_team_name(&builder, base, g_market.clubs[buyer].team_id);
            ui_text_append_ascii(&builder, "\nClearance fee: ");
            ui_text_append_char(&builder, 0x0180);
            ui_text_append_char(&builder, ' ');
            ui_text_append_i32(&builder, fee);
        } else {
            ui_text_append_ascii(&builder, "\nNo club can take him at the moment.");
        }
    }
    ui_text_append_ascii(&builder, "\nThe transfer cannot be reversed.");
    ui_show_options(base, "Quick Sale", g_ui_description, options, 2,
                    ui_callback_quick_sale);
}

/* Shortlist (v31): Make Offer, Next Player, (More) Remove, Previous Player, Back. */
static void ui_show_shortlist_player(uint32_t base, int32_t index) {
    static const char *const options[] = {
        "Make Offer", "Next Player", "Remove", "Previous Player", "Back"
    };
    if (!base || index < 0 || index >= g_player_count ||
        shortlist_slot(g_players[index].player_id) < 0) {
        ui_show_shortlist(base);
        return;
    }
    g_ui_player_index = index;
    MarketPlayer *player = &g_players[index];
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Player: ");
    ui_append_player_name(&builder, base, player->player_id);
    ui_text_append_ascii(&builder, "\nClub: ");
    ui_append_team_name(&builder, base, player->owner_id);
    ui_text_append_ascii(&builder, "\nPosition: ");
    ui_append_position(&builder, player->position);
    ui_text_append_ascii(&builder, "\nRating: ");
    ui_text_append_i32(&builder, player->rating);
    ui_text_append_ascii(&builder, "\nMarket value: ");
    ui_text_append_char(&builder, 0x0180);
    ui_text_append_char(&builder, ' ');
    ui_text_append_i32(&builder, market_value(player));
    ui_text_append_ascii(&builder, "\nAnnual wage: ");
    ui_text_append_i32(&builder, player->wage);
    ui_text_append_ascii(&builder, "\nStatus: ");
    if (player->owner_id == USER_TEAM_ID) {
        ui_text_append_ascii(&builder, "Already in your squad");
    } else if (g_market.window_id < 0) {
        ui_text_append_ascii(&builder, "Transfer window closed");
    } else if (has_active_offer_for_player(player->player_id)) {
        ui_text_append_ascii(&builder, "An offer is already active");
    } else {
        ui_text_append_ascii(&builder, "Saved to shortlist");
    }
    ui_text_append_ascii(&builder, "\nShortlisted players stay saved across seasons.");
    ui_show_options(base, "Shortlist", g_ui_description, options, 5,
                    ui_callback_shortlist);
}

static void ui_show_shortlist(uint32_t base) {
    static const char *const empty_options[] = {"Open Market", "Open Player Sales", "Back"};
    if (!base) return;
    int32_t index = g_ui_player_index >= 0 && g_ui_player_index < g_player_count &&
                    shortlist_slot(g_players[g_ui_player_index].player_id) >= 0
        ? g_ui_player_index : ui_find_shortlist_player(-1, 1);
    if (index >= 0) {
        ui_show_shortlist_player(base, index);
        return;
    }
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Your shortlist is empty. Add a player from the market's player page to keep him here.");
    ui_show_options(base, "Shortlist", g_ui_description, empty_options, 3,
                    ui_callback_shortlist);
}

static void ui_show_position_filter(uint32_t base) {
    static const char *const options[] = {
        "Any position", "Goalkeeper", "Defender", "Midfielder", "Forward", "Back"
    };
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Choose the broad role to browse.");
    ui_show_options(base, g_ui_browser_mode == 1 ? "Sales Position" : "Market Position",
                    g_ui_description, options, 6, ui_callback_position_filter);
}

/* Offer terms (v31). The v30 page had 10 buttons; it is now a small hierarchy whose buttons map to
 * the v30 option indices (ui_bid_stage_to_v30), so ui_callback_bid_terms / ui_adjust_bid_terms keep
 * their logic:
 *   stage 0 "Offer Terms":    Submit Offer, Change Terms, Back
 *   stage 1 "Change Terms":   Transfer Fee, Annual Wage, (More) Contract Length, Done
 *   stage 2 "Offer Fee":      Fee -10%, Fee +10%, (More) Exact Fee, Done
 *   stage 3 "Offer Wage":     Wage -10%, Wage +10%, (More) Exact Wage, Done
 *   stage 4 "Offer Contract": Years -1, Years +1, Done */
enum { UI_BID_TERMS = 0, UI_BID_CHANGE = 1, UI_BID_FEE = 2, UI_BID_WAGE = 3, UI_BID_YEARS = 4 };
static int32_t g_ui_bid_stage;
static int32_t g_ui_bid_stage_player = -1;

/* Button of the current stage -> v30 option (0..9), or -1 for a move between stages. */
static int32_t ui_bid_stage_to_v30(int32_t stage, int32_t selection) {
    static const int32_t terms[] = {8, -1, 9};
    static const int32_t fee[] = {0, 1, 6, -1};
    static const int32_t wage[] = {2, 3, 7, -1};
    static const int32_t years[] = {4, 5, -1};
    if (stage == UI_BID_TERMS) return selection >= 0 && selection < 3 ? terms[selection] : 9;
    if (stage == UI_BID_FEE) return selection >= 0 && selection < 4 ? fee[selection] : -1;
    if (stage == UI_BID_WAGE) return selection >= 0 && selection < 4 ? wage[selection] : -1;
    if (stage == UI_BID_YEARS) return selection >= 0 && selection < 3 ? years[selection] : -1;
    return -1;
}

static void ui_show_bid_terms(uint32_t base) {
    static const char *const terms_options[] = {"Submit Offer", "Change Terms", "Back"};
    static const char *const change_options[] = {
        "Transfer Fee", "Annual Wage", "Contract Length", "Done"
    };
    static const char *const fee_options[] = {"Fee -10%", "Fee +10%", "Exact Fee", "Done"};
    static const char *const wage_options[] = {"Wage -10%", "Wage +10%", "Exact Wage", "Done"};
    static const char *const years_options[] = {"Years -1", "Years +1", "Done"};
    if (g_ui_bid_stage_player != g_ui_bid_player_id) {
        g_ui_bid_stage = UI_BID_TERMS;         /* a new negotiation starts on the summary */
        g_ui_bid_stage_player = g_ui_bid_player_id;
    }
    if (g_ui_player_index < 0 || g_ui_player_index >= g_player_count ||
        g_ui_bid_player_id != g_players[g_ui_player_index].player_id) {
        ui_show_market_player(base, g_ui_player_index);
        return;
    }
    MarketPlayer *player = &g_players[g_ui_player_index];
    int32_t buyer_index = find_account(USER_TEAM_ID);
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Player: ");
    ui_append_player_name(&builder, base, player->player_id);
    ui_text_append_ascii(&builder, "\nTransfer fee: ");
    ui_text_append_i32(&builder, g_ui_bid_fee);
    ui_text_append_ascii(&builder, "\nAnnual wage: ");
    ui_text_append_i32(&builder, g_ui_bid_wage);
    ui_text_append_ascii(&builder, "\nContract: ");
    ui_text_append_i32(&builder, g_ui_bid_contract_years);
    ui_text_append_ascii(&builder, " years");
    if (buyer_index >= 0) {
        ClubAccount *buyer = &g_market.clubs[buyer_index];
        ui_text_append_ascii(&builder, "\nFee room: ");
        ui_text_append_i32(&builder, account_fee_room(buyer));
        ui_text_append_ascii(&builder, "  Wage room: ");
        ui_text_append_i32(&builder, account_wage_room(buyer));
    }
    if (g_ui_bid_stage == UI_BID_TERMS) {
        ui_text_append_ascii(&builder, "\nSeller and player may counter your offer.");
    } else if (g_ui_bid_stage == UI_BID_CHANGE) {
        ui_text_append_ascii(&builder, "\nChoose the term to change.");
    } else if (g_ui_bid_stage == UI_BID_YEARS) {
        ui_text_append_ascii(&builder, "\nThe contract can run 1-5 years.");
    } else {
        ui_text_append_ascii(&builder, "\nEach tap changes the amount by 10%, or enter an exact amount.");
    }
    ui_append_amount_error(&builder);
    if (g_ui_bid_stage == UI_BID_CHANGE) {
        ui_show_options(base, "Change Terms", g_ui_description, change_options, 4,
                        ui_callback_bid_terms);
    } else if (g_ui_bid_stage == UI_BID_FEE) {
        ui_show_options(base, "Offer Fee", g_ui_description, fee_options, 4,
                        ui_callback_bid_terms);
    } else if (g_ui_bid_stage == UI_BID_WAGE) {
        ui_show_options(base, "Offer Wage", g_ui_description, wage_options, 4,
                        ui_callback_bid_terms);
    } else if (g_ui_bid_stage == UI_BID_YEARS) {
        ui_show_options(base, "Offer Contract", g_ui_description, years_options, 3,
                        ui_callback_bid_terms);
    } else {
        ui_show_options(base, "Offer Terms", g_ui_description, terms_options, 3,
                        ui_callback_bid_terms);
    }
}

static void ui_open_offer_counter_amount(uint32_t base, MarketOffer *offer) {
    if (!base || !offer || !offer_is_active(offer)) return;

    int32_t current_fee = offer->fee;
    const char *title = "Enter Counter Fee";
    if (offer->status == CM_OFFER_WAIT_USER_FEE) {
        current_fee = offer->fee + (offer->counter_fee - offer->fee) / 2;
    } else if (offer->status == CM_OFFER_WAIT_USER_WAGE) {
        current_fee = offer->annual_wage +
                      (offer->counter_wage - offer->annual_wage) / 2;
        title = "Enter Counter Wage";
    } else if (offer->status == CM_OFFER_WAIT_USER_SELLER_COUNTER &&
               offer->counter_fee > 0) {
        current_fee = offer->counter_fee;
    }

    ui_set_title(title);
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_keyboard_initial, 16);
    ui_text_append_i32(&builder, current_fee);
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Digits only");

    int32_t previous_offer_id = g_ui_offer_id;
    g_ui_offer_id = offer->offer_id;
    if (!ui_show_keyboard(base, 10, ui_callback_offer_counter_amount)) {
        g_ui_offer_id = previous_offer_id;
        g_ui_keyboard_box = (void *)0;
        g_ui_amount_error = UI_AMOUNT_ERROR_UNAVAILABLE;
        ui_show_offer(base, offer);
    }
}

static void ui_show_offer(uint32_t base, MarketOffer *offer) {
    /* v31: accept and decline first, the counters behind "More...", and a Back that leaves the
     * offer open. ui_callback_offer maps these to the v30 order (accept, counter, exact, decline). */
    const char *const seller_options[] = {
        "Accept fee", "Reject", "Counter higher", "Enter exact fee", "Back"
    };
    const char *const buyer_fee_options[] = {
        "Accept counter", "Walk away", "Counter lower", "Enter exact fee", "Back"
    };
    const char *const wage_options[] = {
        "Accept player wage", "Walk away", "Counter lower", "Enter exact wage", "Back"
    };
    const char *const wage_final_options[] = {"Accept player counter", "Walk away", "Back"};
    MarketUiText builder;
    if (!offer || !offer_is_active(offer)) {
        ui_show_inbox(base);
        return;
    }
    g_ui_offer_id = offer->offer_id;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    switch (g_neg_event) {
        case NEG_COUNTER:
            ui_text_append_ascii(&builder, "The seller countered your fee.\n");
            break;
        case NEG_SOFTENED:
            ui_text_append_ascii(&builder, "Your bid was low, but the seller lowered the asking fee.\n");
            break;
        case NEG_INSULT:
            ui_text_append_ascii(&builder, "The offer was far below the seller's value. One strike was recorded.\n");
            break;
        case NEG_AGREED:
            ui_text_append_ascii(&builder, "The clubs agreed the fee; the player is reviewing personal terms.\n");
            break;
        case NEG_TALKS_ENDED:
            ui_text_append_ascii(&builder, "Talks have ended.\n");
            break;
        case NEG_RIVAL_BID:
            ui_text_append_ascii(&builder, "A rival club is also bidding: ");
            ui_append_team_name(&builder, base, g_neg_rival_team);
            ui_text_append_ascii(&builder, " is at ");
            ui_text_append_i32(&builder, g_neg_rival_fee);
            ui_text_append_ascii(&builder, " coins. The counter fee shown must beat them.\n");
            break;
        case NEG_RIVAL_RAISED:
            ui_text_append_ascii(&builder, "Rival ");
            ui_append_team_name(&builder, base, g_neg_rival_team);
            ui_text_append_ascii(&builder, " raised to ");
            ui_text_append_i32(&builder, g_neg_rival_fee);
            ui_text_append_ascii(&builder, " coins. The counter fee shown beats that bid.\n");
            break;
        case NEG_RIVAL_WITHDREW:
            ui_text_append_ascii(&builder, "Your fee passed ");
            ui_append_team_name(&builder, base, g_neg_rival_team);
            ui_text_append_ascii(&builder, "'s valuation, so they withdrew. The seller is considering your bid.\n");
            break;
        case NEG_RIVAL_SIGNED:
            ui_text_append_ascii(&builder, "You walked away; ");
            ui_append_team_name(&builder, base, g_neg_rival_team);
            ui_text_append_ascii(&builder, " signed the player.\n");
            break;
        case NEG_WAGE_COUNTER:
            ui_text_append_ascii(&builder, "The player countered your wage.\n");
            break;
        case NEG_SIGNED:
            ui_text_append_ascii(&builder, "Deal agreed and signed.\n");
            break;
        case NEG_BUYER_RAISED:
            ui_text_append_ascii(&builder, "Buyer ");
            ui_append_team_name(&builder, base, offer->buyer_id);
            ui_text_append_ascii(&builder, " raised their fee offer.\n");
            break;
        case NEG_BUYER_FINAL:
            ui_text_append_ascii(&builder, "Buyer ");
            ui_append_team_name(&builder, base, offer->buyer_id);
            ui_text_append_ascii(&builder, " has reached its limit. This is their final fee.\n");
            break;
        case NEG_BUYER_WALKED:
            ui_text_append_ascii(&builder, "The buyer walked away; no transfer was made.\n");
            break;
        case NEG_SOLD:
            ui_text_append_ascii(&builder, "Transfer completed.\n");
            break;
        default:
            break;
    }
    ui_text_append_ascii(&builder, "Player: ");
    ui_append_player_name(&builder, base, offer->player_id);
    ui_text_append_ascii(&builder, "\n");
    if (offer->seller_id == USER_TEAM_ID) {
        ui_text_append_ascii(&builder, "Bid from: ");
        ui_append_team_name(&builder, base, offer->buyer_id);
        ui_text_append_ascii(&builder, "\nTheir fee: ");
        ui_text_append_i32(&builder, offer->fee);
        if (offer->counter_fee > 0) {
            ui_text_append_ascii(&builder, "\nLatest counter: ");
            ui_text_append_i32(&builder, offer->counter_fee);
        }
    } else if (offer->status == CM_OFFER_WAIT_USER_FEE) {
        ui_text_append_ascii(&builder, "Seller: ");
        ui_append_team_name(&builder, base, offer->seller_id);
        ui_text_append_ascii(&builder, "\nYour fee: ");
        ui_text_append_i32(&builder, offer->fee);
        ui_text_append_ascii(&builder, "\nSeller counter: ");
        ui_text_append_i32(&builder, offer->counter_fee);
    } else {
        ui_text_append_ascii(&builder, "Seller: ");
        ui_append_team_name(&builder, base, offer->seller_id);
        ui_text_append_ascii(&builder, "\nAgreed fee: ");
        ui_text_append_i32(&builder, offer->fee);
        ui_text_append_ascii(&builder, offer->rounds > 0
            ? "\nPlayer's final wage counter: " : "\nPlayer wage request: ");
        ui_text_append_i32(&builder, offer->counter_wage);
    }
    ui_text_append_ascii(&builder, offer->status == CM_OFFER_WAIT_USER_WAGE
        ? "\nYour annual wage offer: " : "\nAnnual wage: ");
    ui_text_append_i32(&builder, offer->annual_wage);
    ui_text_append_ascii(&builder, "\nContract: ");
    ui_text_append_i32(&builder, offer->contract_years);
    ui_text_append_ascii(&builder, " years");
    ui_append_amount_error(&builder);

    g_ui_offer_layout = 5;
    if (offer->status == CM_OFFER_WAIT_USER_FEE) {
        ui_show_options(base, "Club Fee Negotiation", g_ui_description,
                        buyer_fee_options, 5, ui_callback_offer);
    } else if (offer->status == CM_OFFER_WAIT_USER_WAGE) {
        if (offer->rounds > 0) {
            g_ui_offer_layout = 3;
            ui_show_options(base, "Player Wage Counter", g_ui_description,
                            wage_final_options, 3, ui_callback_offer);
        } else {
            ui_show_options(base, "Player Wage Negotiation", g_ui_description,
                            wage_options, 5, ui_callback_offer);
        }
    } else {
        ui_show_options(base, "Incoming Transfer Offer", g_ui_description,
                        seller_options, 5, ui_callback_offer);
    }
}

static void ui_show_inbox(uint32_t base) {
    MarketOffer *offer = ui_next_active_offer(0);
    g_ui_offer_origin = UI_OFFER_FROM_INBOX;
    if (!offer) {
        ui_show_notice(base, "Offers Inbox", "There are no transfer offers waiting for your decision.", UI_AFTER_MENU);
        return;
    }
    ui_show_offer(base, offer);
}

static void ui_show_finances(uint32_t base) {
    static const char *const options[] = {
        "Club squad", "Finance activity", "Season books", "Back"
    };
    CareerMarketClubView club;
    MarketUiText builder;
    if (g_ui_finance_club_cursor < 0 || g_ui_finance_club_cursor >= MAX_CLUBS ||
        g_market.clubs[g_ui_finance_club_cursor].team_id < 0) {
        g_ui_finance_club_cursor = find_account(USER_TEAM_ID);
    }
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    if (g_ui_finance_club_cursor < 0 ||
        !career_market_get_club(g_market.clubs[g_ui_finance_club_cursor].team_id, &club)) {
        ui_text_append_ascii(&builder, "Club finance data is not available yet.");
    } else {
        ui_text_append_ascii(&builder, "Club: ");
        ui_append_team_name(&builder, base, club.team_id);
        if (club.team_id == USER_TEAM_ID) {
            ui_text_append_ascii(&builder, "\nCoins: ");
            ui_text_append_i32(&builder, club.cash);
        } else {
            ui_text_append_ascii(&builder, "\nClub funds: ");
            ui_text_append_i32(&builder, club.cash);
            ui_text_append_ascii(&builder, " coins\nTransfer budget: ");
            ui_text_append_i32(&builder, club.transfer_budget);
            ui_text_append_ascii(&builder, "\nAnnual wage budget: ");
            ui_text_append_i32(&builder, club.wage_budget);
        }
        ui_text_append_ascii(&builder, "\nCurrent payroll: ");
        ui_text_append_i32(&builder, club.payroll);
        ui_text_append_ascii(&builder, "\nSquad value: ");
        ui_text_append_i32(&builder, club.squad_value);
        ui_text_append_ascii(&builder, "\nSquad strength: ");
        ui_text_append_i32(&builder, club.strength);
        ui_text_append_ascii(&builder, "\nSaved activity entries: ");
        ui_text_append_i32(&builder, career_market_get_finance_count(club.team_id));
        ui_text_append_ascii(&builder, "\nAll amounts are in coins.");
    }
    ui_show_options(base, "Club Finances", g_ui_description, options, 4, ui_callback_finances);
}

/*
 * Season books: every coin the user club earned or spent this season (or last), by category, so the
 * books reconcile with the coin balance. Page 0 income, page 1 spending, page 2 the price list.
 */
static int32_t g_ui_books_page;
static int32_t g_ui_books_last;
static int32_t ui_callback_books(int32_t selection);

static void ui_append_price_line(MarketUiText *builder, const char *label, int32_t value, const char *unit) {
    ui_text_append_ascii(builder, label);
    ui_text_append_i32(builder, value);
    ui_text_append_ascii(builder, unit);
}

static void ui_show_books(uint32_t base) {
    static const char *const options[] = {"Income", "Spending", "Price list", "Switch season", "Back"};
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    int32_t last = g_ui_books_last;
    if (g_ui_books_page == 2) {
        ui_append_price_line(&builder, "Medical: ", config_value(base, 0x01A), " coins per week out injured\n");
        ui_append_price_line(&builder, "Energy refill: up to ", config_value(base, 0x01B), " coins a player (less when fitter)\n");
        ui_append_price_line(&builder, "Training session: ", config_value(base, 0x04A), " to ");
        ui_append_price_line(&builder, "", config_value(base, 0x04B), " coins (rises with the player's level)\n");
        ui_append_price_line(&builder, "Scouting: ", config_value(base, 0x177), " coins, then ");
        ui_append_price_line(&builder, "", config_value(base, 0x178), " more for each extra session\n");
        ui_append_price_line(&builder, "Stadium sections: ", config_value(base, 0x051), " to ");
        ui_append_price_line(&builder, "", config_value(base, 0x058), " coins (plus a roof)\n");
        ui_append_price_line(&builder, "Wages: ", WAGE_RATE_PCT, "% of a player's value a season, paid every match\n");
        ui_append_price_line(&builder, "Season objective reward: ", config_value(base, 0x009), " coins each\n");
        int32_t division = g_ext.econ.division >= 0 ? g_ext.econ.division : DIVISION_COUNT - 1;
        ui_append_price_line(&builder, "Win prize in your division: ", econ_win_prize(division), " coins\n");
        ui_append_price_line(&builder, "Season TV + sponsor money: ",
                             mul_div(division_target(division), 40, 100), " coins");
        ui_show_options(base, "Price List", g_ui_description, options, 5, ui_callback_books);
        return;
    }
    const int32_t *categories = g_ui_books_page == 0 ? k_income_categories : k_spend_categories;
    int32_t count = g_ui_books_page == 0
        ? (int32_t)(sizeof(k_income_categories) / sizeof(k_income_categories[0]))
        : (int32_t)(sizeof(k_spend_categories) / sizeof(k_spend_categories[0]));
    ui_text_append_ascii(&builder, last ? "Last season" : "This season");
    ui_text_append_ascii(&builder, g_ui_books_page == 0 ? " - income\n" : " - spending\n");
    int32_t shown = 0;
    for (int32_t i = 0; i < count; ++i) {
        int32_t amount = book_total(categories[i], last);
        if (!amount) continue;
        ui_text_append_ascii(&builder, book_label(categories[i]));
        ui_text_append_ascii(&builder, ": ");
        ui_append_signed(&builder, amount);
        ui_text_append_char(&builder, '\n');
        ++shown;
    }
    if (!shown) ui_text_append_ascii(&builder, "Nothing yet.\n");
    int32_t income = books_sum(k_income_categories, (int32_t)(sizeof(k_income_categories) / sizeof(k_income_categories[0])), last);
    int32_t spend = books_sum(k_spend_categories, (int32_t)(sizeof(k_spend_categories) / sizeof(k_spend_categories[0])), last);
    ui_text_append_ascii(&builder, g_ui_books_page == 0 ? "Total income: " : "Total spending: ");
    ui_append_signed(&builder, g_ui_books_page == 0 ? income : spend);
    ui_text_append_ascii(&builder, "\nNet: ");
    ui_append_signed(&builder, sat_add(income, spend));
    ui_text_append_ascii(&builder, "  Coins now: ");
    ui_text_append_i32(&builder, user_coins(base));
    ui_show_options(base, "Season Books", g_ui_description, options, 5, ui_callback_books);
}

static int32_t ui_callback_books(int32_t selection) {
    if (selection >= 0 && selection <= 2) {
        g_ui_books_page = selection;
        ui_show_books(g_ui_base);
    } else if (selection == 3) {
        g_ui_books_last = !g_ui_books_last;
        ui_show_books(g_ui_base);
    } else {
        ui_show_finances(g_ui_base);
    }
    return 1;
}

static void ui_append_finance_category(MarketUiText *builder, int32_t category) {
    switch (category) {
        case CM_FINANCE_OPENING: ui_text_append_ascii(builder, "Opening account"); break;
        case CM_FINANCE_REVENUE: ui_text_append_ascii(builder, "Simulated revenue"); break;
        case CM_FINANCE_WAGE_COST: ui_text_append_ascii(builder, "Wage cost"); break;
        case CM_FINANCE_TRANSFER_ALLOCATION: ui_text_append_ascii(builder, "Transfer budget allocation"); break;
        case CM_FINANCE_PURCHASE: ui_text_append_ascii(builder, "Player purchase"); break;
        case CM_FINANCE_SALE: ui_text_append_ascii(builder, "Player sale"); break;
        case CM_FINANCE_CONTRACT_RENEWAL: ui_text_append_ascii(builder, "Contract renewal"); break;
        case CM_FINANCE_INVESTMENT: ui_text_append_ascii(builder, "Stadium & facilities investment"); break;
        default: ui_text_append_ascii(builder, "Other"); break;
    }
}

static void ui_show_finance_history(uint32_t base) {
    static const char *const options[] = {"Earlier", "Later", "Back"};
    CareerMarketClubView club;
    CareerMarketFinanceEntryView entry;
    MarketUiText builder;
    int32_t team_id = -1;
    if (g_ui_finance_club_cursor >= 0 && g_ui_finance_club_cursor < MAX_CLUBS) {
        team_id = g_market.clubs[g_ui_finance_club_cursor].team_id;
    }
    int32_t count = team_id >= 0 ? career_market_get_finance_count(team_id) : 0;
    if (g_ui_finance_history_cursor >= count) {
        g_ui_finance_history_cursor = count > 0 ? count - 1 : 0;
    }
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    if (team_id < 0 || !career_market_get_club(team_id, &club)) {
        ui_text_append_ascii(&builder, "Club finance data is not available.");
    } else {
        ui_text_append_ascii(&builder, "Club: ");
        ui_append_team_name(&builder, base, team_id);
        if (count <= 0 || !career_market_get_finance_entry(
                team_id, g_ui_finance_history_cursor, &entry)) {
            ui_text_append_ascii(&builder, "\nNo finance activity has been recorded yet.");
        } else {
            ui_text_append_ascii(&builder, "\nActivity: ");
            ui_append_finance_category(&builder, entry.category);
            if (entry.player_id >= 0) {
                ui_text_append_ascii(&builder, "\nPlayer: ");
                ui_append_player_name(&builder, base, entry.player_id);
            }
            ui_text_append_ascii(&builder, "\nSeason ");
            ui_text_append_i32(&builder, entry.season + 1);
            if (entry.turn < 0) {
                ui_text_append_ascii(&builder, " opening balance");
            } else {
                ui_text_append_ascii(&builder, ", turn ");
                ui_text_append_i32(&builder, entry.turn + 1);
            }
            ui_text_append_ascii(&builder, "\nCoins change: ");
            ui_text_append_i32(&builder, entry.cash_delta);
            ui_text_append_ascii(&builder, "\nTransfer budget change: ");
            ui_text_append_i32(&builder, entry.transfer_budget_delta);
            ui_text_append_ascii(&builder, "\nPayroll change: ");
            ui_text_append_i32(&builder, entry.payroll_delta);
            ui_text_append_ascii(&builder, "\nEntry ");
            ui_text_append_i32(&builder, g_ui_finance_history_cursor + 1);
            ui_text_append_ascii(&builder, " of ");
            ui_text_append_i32(&builder, count);
        }
    }
    ui_show_options(base, "Club Finance Activity", g_ui_description, options, 3,
                    ui_callback_finance_history);
}

static void ui_show_roster(uint32_t base) {
    /* v31 (option boxes): the action sits beside Next player; "More..." reaches the rest. */
    static const char *const club_options[] = {
        "Next player", "Club finances", "Previous player", "Next club", "Previous club", "Back"
    };
    static const char *const user_options[] = {
        "Next player", "Renew contract", "Previous player", "Next club", "Previous club", "Back"
    };
    static const char *const user_untracked_options[] = {
        "Next player", "Contract unavailable", "Previous player", "Next club", "Previous club", "Back"
    };
    MarketUiText builder;
    if (g_ui_finance_club_cursor < 0 || g_ui_finance_club_cursor >= MAX_CLUBS ||
        g_market.clubs[g_ui_finance_club_cursor].team_id < 0) {
        g_ui_finance_club_cursor = find_account(USER_TEAM_ID);
    }
    int32_t team_id = g_ui_finance_club_cursor >= 0
        ? g_market.clubs[g_ui_finance_club_cursor].team_id : -1;
    if (team_id >= 0 && (g_ui_roster_player_cursor < 0 ||
        g_ui_roster_player_cursor >= g_player_count ||
        g_players[g_ui_roster_player_cursor].owner_id != team_id)) {
        g_ui_roster_player_cursor = ui_find_club_player(team_id, -1, 1);
    }
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    if (team_id < 0) {
        ui_text_append_ascii(&builder, "Club roster data is not available.");
    } else {
        ui_text_append_ascii(&builder, "Club: ");
        ui_append_team_name(&builder, base, team_id);
        if (g_ui_roster_player_cursor < 0 || g_ui_roster_player_cursor >= g_player_count) {
            ui_text_append_ascii(&builder, "\nNo players are available in this roster.");
        } else {
            MarketPlayer *player = &g_players[g_ui_roster_player_cursor];
            int32_t count = 0;
            int32_t ordinal = 0;
            for (int32_t i = 0; i < g_player_count; ++i) {
                if (g_players[i].owner_id != team_id) continue;
                ++count;
                if (i == g_ui_roster_player_cursor) ordinal = count;
            }
            ui_text_append_ascii(&builder, "\nPlayer: ");
            ui_append_player_name(&builder, base, player->player_id);
            ui_text_append_ascii(&builder, "\nPosition: ");
            ui_append_position(&builder, player->position);
            ui_text_append_ascii(&builder, "\nRating: ");
            ui_text_append_i32(&builder, player->rating);
            ui_text_append_ascii(&builder, "\nMarket value: ");
            ui_text_append_i32(&builder, player->value);
            ui_text_append_ascii(&builder, "\nAnnual wage: ");
            ui_text_append_i32(&builder, player->wage);
            int32_t contract_term = contract_seasons_to_expiry(
                player->player_id, player->owner_id, g_market.season);
            if (contract_term >= 0) {
                ui_text_append_ascii(&builder, "\nContract expires: ");
                if (contract_term == 0) ui_text_append_ascii(&builder, "this season");
                else {
                    ui_text_append_ascii(&builder, "about ");
                    ui_text_append_i32(&builder, contract_term);
                    ui_text_append_ascii(&builder, contract_term == 1 ? " season" : " seasons");
                }
            } else {
                ui_text_append_ascii(&builder, "\nContract status: expired");
            }
            if (team_id == USER_TEAM_ID) {
                PlayerContract *contract = find_contract_record(player->player_id, USER_TEAM_ID);
                ui_text_append_ascii(&builder, "\nRenewal status: ");
                if (!market_contract_available(player->player_id, USER_TEAM_ID)) {
                    ui_text_append_ascii(&builder, "No contract data");
                } else if (contract && contract->expiry_season < g_market.season) {
                    ui_text_append_ascii(&builder, "Tracked contract expired; renewal available");
                } else if (!contract && contract_term < 0) {
                    ui_text_append_ascii(&builder, "Estimated contract expired; renewal available");
                } else if (!contract) {
                    ui_text_append_ascii(&builder, "Estimated contract; renewal available");
                } else {
                    ui_text_append_ascii(&builder, "Tracked contract; renewal available");
                }
                ui_text_append_ascii(&builder, "\nSale listing: ");
                ui_text_append_ascii(&builder, player->listed_for_sale ? "Listed" : "Not listed");
            }
            ui_text_append_ascii(&builder, "\nPlayer ");
            ui_text_append_i32(&builder, ordinal);
            ui_text_append_ascii(&builder, " of ");
            ui_text_append_i32(&builder, count);
        }
    }
    const char *const *options = club_options;
    if (team_id == USER_TEAM_ID) {
        int32_t player_id = g_ui_roster_player_cursor >= 0 &&
                            g_ui_roster_player_cursor < g_player_count
            ? g_players[g_ui_roster_player_cursor].player_id : -1;
        options = market_contract_available(player_id, USER_TEAM_ID)
            ? user_options : user_untracked_options;
    }
    ui_show_options(base, "Club Squad", g_ui_description, options, 6,
                    ui_callback_roster);
}

static void ui_show_contract_renewal(uint32_t base) {
    /* v31: at most 3 buttons a box; ui_callback_contract_renewal maps these to the v30 indices. */
    static const char *const main_options[] = {"Submit renewal", "Change terms", "Back"};
    static const char *const terms_options[] = {"Annual wage", "Contract length", "Done"};
    static const char *const amount_options[] = {
        "Lower by 10%", "Raise by 10%", "Lower by 20%", "Raise by 20%",
        "Enter exact amount", "Back"
    };
    static const char *const contract_options[] = {"Years -1", "Years +1", "Done"};
    static const char *const counter_options[] = {"Accept player counter", "Walk away"};
    int32_t player_index = find_cached_player(g_ui_renewal_player_id);
    int32_t account_index = find_account(USER_TEAM_ID);
    if (player_index < 0 || account_index < 0 ||
        g_players[player_index].owner_id != USER_TEAM_ID ||
        !market_contract_available(g_ui_renewal_player_id, USER_TEAM_ID)) {
        ui_show_notice(base, "Contract Renewal", "This player has no career-market contract available to renew.", UI_AFTER_ROSTER);
        return;
    }
    MarketPlayer *player = &g_players[player_index];
    PlayerContract *contract = find_contract_record(g_ui_renewal_player_id, USER_TEAM_ID);
    ClubAccount *club = &g_market.clubs[account_index];
    int32_t max_wage = clamp_add(player->wage, account_wage_room(club));
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Player: ");
    ui_append_player_name(&builder, base, player->player_id);
    ui_text_append_ascii(&builder, "\nCurrent annual wage: ");
    ui_text_append_i32(&builder, player->wage);
    ui_text_append_ascii(&builder, "\nContract: ");
    int32_t contract_term = contract
        ? contract->expiry_season - g_market.season
        : contract_seasons_to_expiry(player->player_id, USER_TEAM_ID, g_market.season);
    if (contract_term < 0) {
        ui_text_append_ascii(&builder, contract ? "Tracked contract expired" : "Estimated contract expired");
    } else {
        if (!contract) ui_text_append_ascii(&builder, "Estimated; ");
        if (contract_term == 0) ui_text_append_ascii(&builder, "expires this season");
        else {
            ui_text_append_ascii(&builder, "about ");
            ui_text_append_i32(&builder, contract_term);
            ui_text_append_ascii(&builder, contract_term == 1 ? " season remaining" : " seasons remaining");
        }
    }
    ui_text_append_ascii(&builder, "\nProposed annual wage: ");
    ui_text_append_i32(&builder, g_ui_renewal_wage);
    ui_text_append_ascii(&builder, "\nContract length: ");
    ui_text_append_i32(&builder, g_ui_renewal_years);
    ui_text_append_ascii(&builder, " years\nMaximum affordable wage: ");
    ui_text_append_i32(&builder, max_wage);
    if (g_ui_renewal_stage == 1) {
        ui_text_append_ascii(&builder, "\nAdjust the offer in small steps or enter a whole number.");
        ui_append_amount_error(&builder);
        ui_show_options(base, "Renewal Wage", g_ui_description, amount_options, 6,
                        ui_callback_contract_renewal);
    } else if (g_ui_renewal_stage == 2) {
        ui_text_append_ascii(&builder, "\nChoose the new contract term (1-5 years).");
        ui_show_options(base, "Renewal Length", g_ui_description, contract_options, 3,
                        ui_callback_contract_renewal);
    } else if (g_ui_renewal_stage == 4) {
        ui_text_append_ascii(&builder, "\nChange the wage offer or the contract length.");
        ui_show_options(base, "Renewal Terms", g_ui_description, terms_options, 3,
                        ui_callback_contract_renewal);
    } else if (g_ui_renewal_stage == 3) {
        ui_text_append_ascii(&builder, "\nPlayer counteroffer: ");
        ui_text_append_i32(&builder, g_ui_renewal_counter_wage);
        ui_show_options(base, "Player Wage Counter", g_ui_description, counter_options, 2,
                        ui_callback_contract_renewal);
    } else {
        ui_text_append_ascii(&builder, "\nThe player may counter your wage before agreeing.");
        ui_show_options(base, "Contract Renewal", g_ui_description, main_options, 3,
                        ui_callback_contract_renewal);
    }
}

static void ui_show_history(uint32_t base, int32_t index) {
    static const char *const options[] = {"Earlier", "Later", "Back"};
    CareerMarketTransferView record;
    MarketUiText builder;
    int32_t count = career_market_get_history_count();
    if (count <= 0 || index < 0 || index >= count ||
        !career_market_get_history(index, &record)) {
        ui_show_notice(base, "Transfer History", "No completed transfers have been recorded yet.", UI_AFTER_MENU);
        return;
    }
    g_ui_history_index = index;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Player: ");
    ui_append_player_name(&builder, base, record.player_id);
    ui_text_append_ascii(&builder, "\nFrom: ");
    ui_append_team_name(&builder, base, record.seller_id);
    ui_text_append_ascii(&builder, "\nTo: ");
    ui_append_team_name(&builder, base, record.buyer_id);
    ui_text_append_ascii(&builder, "\nFee: ");
    ui_text_append_i32(&builder, record.fee);
    ui_text_append_ascii(&builder, "\nAnnual wage: ");
    ui_text_append_i32(&builder, record.annual_wage);
    ui_text_append_ascii(&builder, "\nContract: ");
    ui_text_append_i32(&builder, record.contract_years);
    ui_text_append_ascii(&builder, " years\nSeason ");
    ui_text_append_i32(&builder, record.season + 1);
    ui_text_append_ascii(&builder, ", turn ");
    ui_text_append_i32(&builder, record.turn + 1);
    int32_t first = g_market.history_head - g_market.history_count;
    if (first < 0) first += HISTORY_CAPACITY;
    int32_t history_slot = (first + index) % HISTORY_CAPACITY;
    HistoryExt *news = &g_ext.history[history_slot];
    if (news->rival_team >= 0) {
        ui_text_append_ascii(&builder, "\nAuction: beat ");
        ui_append_team_name(&builder, base, news->rival_team);
        ui_text_append_ascii(&builder, " to the signing; their valuation was ");
        ui_text_append_char(&builder, 0x0180);
        ui_text_append_char(&builder, ' ');
        ui_text_append_i32(&builder, news->rival_fee);
        ui_text_append_ascii(&builder, ".");
    }
    ui_show_options(base, "Transfer History", g_ui_description, options, 3, ui_callback_history);
}

static void market_ui_show_main(uint32_t base) {
    g_ui_from_stock = 0;
    static const char *const options[] = {"Transfers", "My Club", "Close"};
    CareerMarketClubView club;
    MarketUiText builder;
    g_ui_base = base;
    for (int32_t i = 0; i <= UI_SCREEN_LIST_SHORTLIST; ++i) {
        g_ui_screen_page_offsets[i] = 0;
    }
    for (int32_t i = 0; i < 48; ++i) g_ui_search_query[i] = 0;
    g_ui_search_match_count = 0;
    g_ui_search_results_valid = 0;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Season ");
    ui_text_append_i32(&builder, g_market.season + 1);
    ui_text_append_ascii(&builder, "  Turn ");
    ui_text_append_i32(&builder, g_market.current_turn + 1);
    ui_text_append_ascii(&builder, "\nWindow: ");
    if (g_market.window_id < 0) {
        ui_text_append_ascii(&builder, "Closed");
        ui_text_append_ascii(&builder, "\nNext window: start of next season (until the first ");
        ui_text_append_i32(&builder, PRESEASON_WINDOW_TURNS);
        ui_text_append_ascii(&builder, " league matches are played)");
    } else {
        int32_t left = window_matches_left();
        ui_text_append_ascii(&builder, "Open (closes after ");
        ui_text_append_i32(&builder, left > 0 ? left : 1);
        ui_text_append_ascii(&builder, left == 1 ? " more match)" : " more matches)");
    }
    ui_text_append_ascii(&builder, "\nYour club: ");
    ui_append_team_name(&builder, base, USER_TEAM_ID);
    if (career_market_get_club(USER_TEAM_ID, &club)) {
        ui_text_append_ascii(&builder, "\nYour coins: ");
        ui_text_append_i32(&builder, club.cash);
        ui_text_append_ascii(&builder, "\nWage bill: ");
        ui_text_append_i32(&builder, club.payroll);
        ui_text_append_ascii(&builder, " coins a season");
    }
    ui_text_append_ascii(&builder, "\nOffers waiting: ");
    ui_text_append_i32(&builder, ui_active_offer_count());
    ui_text_append_ascii(&builder, "\nAll prices and wages are in coins.");
    ui_show_options(base, "Career Transfer Market", g_ui_description,
                    options, 3, ui_callback_main);
}

static int32_t ui_callback_transfers_hub(int32_t selection);
static int32_t ui_callback_club_hub(int32_t selection);

static void ui_append_filter_summary(MarketUiText *builder, uint32_t base) {
    ui_text_append_ascii(builder, "Position: ");
    if (g_ui_position_filter < 0) ui_text_append_ascii(builder, "Any");
    else ui_append_position(builder, g_ui_position_filter);
    ui_text_append_ascii(builder, "\nClub: ");
    if (g_ui_club_filter < 0) ui_text_append_ascii(builder, "All clubs");
    else ui_append_team_name(builder, base, g_ui_club_filter);
    ui_text_append_ascii(builder, "\nName search: ");
    if (g_ui_search_query[0]) ui_text_append_wide(builder, g_ui_search_query, 47);
    else ui_text_append_ascii(builder, "All players");
}

/* Transfers hub: buying. */
static void ui_show_transfers_hub(uint32_t base) {
    static const char *const options[] = {"Browse Market", "Shortlist", "Back"};
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Window: ");
    if (g_market.window_id < 0) {
        ui_text_append_ascii(&builder, "Closed");
    } else {
        int32_t left = window_matches_left();
        ui_text_append_ascii(&builder, "Open (closes after ");
        ui_text_append_i32(&builder, left > 0 ? left : 1);
        ui_text_append_ascii(&builder, left == 1 ? " more match)" : " more matches)");
    }
    ui_text_append_ascii(&builder, "\nShortlisted players: ");
    ui_text_append_i32(&builder, shortlist_count());
    ui_text_append_char(&builder, '\n');
    ui_append_filter_summary(&builder, base);
    ui_show_options(base, "Transfers", g_ui_description, options, 3, ui_callback_transfers_hub);
}

/* My Club hub: selling, offers, money and records. */
static void ui_show_club_hub(uint32_t base) {
    static const char *const options[] = {
        "Offers / Inbox", "Player Sales", "Club Finances", "Transfer History", "Back"
    };
    CareerMarketClubView club;
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Club: ");
    ui_append_team_name(&builder, base, USER_TEAM_ID);
    if (career_market_get_club(USER_TEAM_ID, &club)) {
        ui_text_append_ascii(&builder, "\nCoins: ");
        ui_text_append_i32(&builder, club.cash);
        ui_text_append_ascii(&builder, "\nWage bill: ");
        ui_text_append_i32(&builder, club.payroll);
        ui_text_append_ascii(&builder, " coins a season");
    }
    ui_text_append_ascii(&builder, "\nOffers waiting: ");
    ui_text_append_i32(&builder, ui_active_offer_count());
    ui_show_options(base, "My Club", g_ui_description, options, 5, ui_callback_club_hub);
}

typedef void (*CfeForwardMarketFn)(int32_t, int32_t, void *, void *, int32_t, int32_t);
typedef void (*CfeBackMarketFn)(int32_t);
typedef void (*CfeScreenCtorMarketFn)(void *);
typedef void (*CfeScreenSetIdMarketFn)(void *, int32_t);
typedef void (*CfeTextButtonCtorMarketFn)(void *, const uint16_t *, uint32_t, int32_t, int32_t);
typedef void (*CfePlayerCardCtorMarketFn)(void *, PlayerInfo *, int32_t, int32_t, int32_t,
                                           int32_t, int32_t, uint32_t, int32_t);
typedef void (*CfeSetAlignmentMarketFn)(void *, int32_t);
typedef void (*CfeSetPixelRectMarketFn)(void *, float, float, float, float);
typedef void (*CfeAddChildMarketFn)(void *, void *, float, float, float, float, uint32_t);
typedef void (*CfeDeleteChildMarketFn)(void *, void *);
typedef void (*CfeProcessAllMarketFn)(void *);
typedef void (*CfeRenderAllMarketFn)(void *);
typedef int32_t (*CfeIsReleasedMarketFn)(void *);
typedef float (*CfeGetSizeMarketFn)(void *);
typedef void (*FeDrawRectMarketFn)(float, float, float, float, uint32_t);
typedef void (*FeDrawTextBoldMarketFn)(const uint16_t *, float, float, uint32_t);

static void market_ui_return_to_transfers(uint32_t base) {
    if (!base) return;
    CfeBackMarketFn back = (CfeBackMarketFn)(base + 0x29910D);
    back(1);
}

static int32_t market_ui_screen_is_root_page(void) {
    static const char *const root_titles[] = {
        "Transfer Market", "Club Squad", "Player Sales", "Offers Inbox",
        "Club Finances", "Transfer History", "Shortlist"
    };
    if (g_ui_page_callback == ui_callback_notice && g_ui_option_count == 1) return 0;
    for (int32_t i = 0; i < (int32_t)(sizeof(root_titles) / sizeof(root_titles[0])); ++i) {
        if (ui_wide_equals_ascii(g_ui_title, root_titles[i])) return 1;
    }
    return 0;
}

static void market_ui_screen_go_back(uint32_t base) {
    if (!base) return;
    if (g_ui_page_callback == ui_callback_notice &&
        (g_ui_after_notice == UI_AFTER_MENU || g_ui_after_notice == UI_AFTER_CLOSE)) {
        market_ui_return_to_transfers(base);
        return;
    }
    if (market_ui_screen_is_root_page()) {
        market_ui_return_to_transfers(base);
        return;
    }
    g_ui_quick_sale_player_id = -1;
    g_ui_amount_error = UI_AMOUNT_ERROR_NONE;
    if (g_ui_screen_tab == 0) {
        ui_show_market_player(base, g_ui_player_index);
    } else if (g_ui_screen_tab == 1) {
        g_ui_renewal_stage = 0;
        ui_show_roster(base);
    } else if (g_ui_screen_tab == 2) {
        ui_show_sales_player(base, g_ui_player_index);
    } else if (g_ui_screen_tab == 3) {
        ui_show_inbox(base);
    } else if (g_ui_screen_tab == 4) {
        ui_show_finances(base);
    } else if (g_ui_screen_tab == 5) {
        ui_show_history(base, g_ui_history_index);
    } else {
        ui_show_shortlist(base);
    }
}

static void market_ui_select_tab(int32_t tab) {
    uint32_t base = g_ui_base;
    if (!base || tab < 0 || tab >= MARKET_SCREEN_TAB_COUNT) return;
    g_ui_screen_tab = tab;
    if (tab == 0) {
        g_ui_screen_page_offsets[UI_SCREEN_LIST_MARKET] = 0;
        if (g_market.window_id < 0) {
            ui_show_notice(base, "Transfer Market",
                           "The transfer window is closed. It reopens at the start of next season, until 6 league matches are played.",
                           UI_AFTER_MENU);
        } else {
            g_ui_browser_mode = 0;
            g_ui_player_index = ui_find_market_player(-1, 1);
            ui_show_market_player(base, g_ui_player_index);
        }
    } else if (tab == 1) {
        g_ui_screen_page_offsets[UI_SCREEN_LIST_SQUAD] = 0;
        g_ui_finance_club_cursor = find_account(USER_TEAM_ID);
        g_ui_roster_player_cursor = -1;
        ui_show_roster(base);
    } else if (tab == 2) {
        g_ui_screen_page_offsets[UI_SCREEN_LIST_SALES] = 0;
        g_ui_browser_mode = 1;
        g_ui_player_index = ui_find_user_player(-1, 1);
        ui_show_sales_player(base, g_ui_player_index);
    } else if (tab == 3) {
        g_ui_screen_page_offsets[UI_SCREEN_LIST_INBOX] = 0;
        ui_show_inbox(base);
    } else if (tab == 4) {
        g_ui_screen_page_offsets[UI_SCREEN_LIST_FINANCES] = 0;
        g_ui_finance_club_cursor = find_account(USER_TEAM_ID);
        g_ui_finance_history_cursor = 0;
        ui_show_finances(base);
    } else if (tab == 5) {
        g_ui_screen_page_offsets[UI_SCREEN_LIST_HISTORY] = 0;
        ui_show_history(base, career_market_get_history_count() - 1);
    } else {
        g_ui_screen_page_offsets[UI_SCREEN_LIST_SHORTLIST] = 0;
        g_ui_player_index = ui_find_shortlist_player(-1, 1);
        ui_show_shortlist(base);
    }
}

static void ui_screen_clear_rows(void) {
    g_ui_screen_row_count = 0;
    for (int32_t i = 0; i < MARKET_SCREEN_LIST_ROWS; ++i) {
        g_ui_screen_row_targets[i] = -1;
        g_ui_screen_row_labels[i][0] = 0;
        g_ui_screen_row_details[i][0] = 0;
    }
}

static int32_t ui_screen_row_exists(int32_t target) {
    for (int32_t i = 0; i < g_ui_screen_row_count; ++i) {
        if (g_ui_screen_row_targets[i] == target) return 1;
    }
    return 0;
}

static void ui_screen_add_player_row(uint32_t base, int32_t index, int32_t mode) {
    if (index < 0 || index >= g_player_count ||
        g_ui_screen_row_count >= MARKET_SCREEN_LIST_ROWS) return;
    MarketPlayer *player = &g_players[index];
    int32_t row = g_ui_screen_row_count++;
    g_ui_screen_row_targets[row] = index;
    MarketUiText label, detail;
    ui_text_reset(&label, g_ui_screen_row_labels[row], 48);
    ui_text_reset(&detail, g_ui_screen_row_details[row], 112);
    if (mode == UI_SCREEN_LIST_SHORTLIST || mode == UI_SCREEN_LIST_MARKET) {
        ui_append_team_name(&label, base, player->owner_id);
    } else if (mode == UI_SCREEN_LIST_SALES) {
        ui_text_append_ascii(&label, player->listed_for_sale ? "LISTED" : "NOT LISTED");
    } else if (mode == UI_SCREEN_LIST_SQUAD) {
        ui_append_position(&label, player->position);
    } else {
        ui_append_player_name(&label, base, player->player_id);
    }
    if (mode == UI_SCREEN_LIST_MARKET) {
        ui_append_position(&detail, player->position);
        ui_text_append_ascii(&detail, "  OVR ");
        ui_text_append_i32(&detail, player->rating);
        ui_text_append_ascii(&detail, "  ");
        ui_text_append_char(&detail, 0x0180);
        ui_text_append_char(&detail, ' ');
        ui_text_append_i32(&detail, market_value(player));
        ui_text_append_ascii(&detail, "  W ");
        ui_text_append_i32(&detail, player->wage);
    } else if (mode == UI_SCREEN_LIST_SALES) {
        ui_text_append_ascii(&detail, "OVR ");
        ui_text_append_i32(&detail, player->rating);
        ui_text_append_ascii(&detail, " | Value ");
        ui_text_append_i32(&detail, player->value);
        if (has_active_offer_for_player(player->player_id)) {
            ui_text_append_ascii(&detail, " | Offer");
        }
    } else if (mode == UI_SCREEN_LIST_SHORTLIST) {
        ui_append_position(&detail, player->position);
        ui_text_append_ascii(&detail, "  OVR ");
        ui_text_append_i32(&detail, player->rating);
        ui_text_append_ascii(&detail, "  ");
        ui_text_append_char(&detail, 0x0180);
        ui_text_append_char(&detail, ' ');
        ui_text_append_i32(&detail, market_value(player));
    } else {
        ui_text_append_ascii(&detail, "OVR ");
        ui_text_append_i32(&detail, player->rating);
        ui_text_append_ascii(&detail, " | Value ");
        ui_text_append_i32(&detail, player->value);
        ui_text_append_ascii(&detail, " | Wage ");
        ui_text_append_i32(&detail, player->wage);
    }
}

static int32_t ui_screen_market_player_visible(uint32_t base, int32_t index) {
    if (index < 0 || index >= g_player_count) return 0;
    MarketPlayer *player = &g_players[index];
    if (player->owner_id == USER_TEAM_ID || player->value <= 0 ||
        (g_ui_position_filter >= 0 && player->position != g_ui_position_filter) ||
        (g_ui_club_filter >= 0 && player->owner_id != g_ui_club_filter) ||
        has_active_offer_for_player(player->player_id)) return 0;
    if (g_ui_search_query[0]) {
        if (!g_ui_search_results_valid) ui_rebuild_search_results(base);
        int32_t low = 0;
        int32_t high = g_ui_search_match_count - 1;
        while (low <= high) {
            int32_t middle = low + (high - low) / 2;
            int32_t candidate = g_ui_search_matches[middle];
            if (candidate == index) return 1;
            if (candidate < index) low = middle + 1;
            else high = middle - 1;
        }
        return 0;
    }
    return 1;
}

static int32_t ui_screen_next_team_player(int32_t team_id, int32_t from_index,
                                          int32_t position_filter) {
    int32_t count = g_player_count;
    if (count < 1) return -1;
    int32_t index = from_index;
    for (int32_t step = 0; step < count; ++step) {
        if (index < 0) index = 0;
        else if (++index >= count) index = 0;
        MarketPlayer *player = &g_players[index];
        if (player->owner_id != team_id || player->value <= 0 ||
            (position_filter >= 0 && player->position != position_filter)) continue;
        return index;
    }
    return -1;
}

static void ui_screen_add_offer_row(uint32_t base, MarketOffer *offer) {
    if (!offer || !offer_is_active(offer) ||
        (offer->seller_id != USER_TEAM_ID && offer->buyer_id != USER_TEAM_ID) ||
        g_ui_screen_row_count >= MARKET_SCREEN_LIST_ROWS ||
        ui_screen_row_exists(offer->offer_id)) return;
    int32_t row = g_ui_screen_row_count++;
    g_ui_screen_row_targets[row] = offer->offer_id;
    MarketUiText label, detail;
    ui_text_reset(&label, g_ui_screen_row_labels[row], 48);
    ui_text_reset(&detail, g_ui_screen_row_details[row], 112);
    if (offer->seller_id == USER_TEAM_ID) {
        ui_text_append_ascii(&label, "Bid from ");
        ui_append_team_name(&label, base, offer->buyer_id);
    } else {
        ui_text_append_ascii(&label, "Seller ");
        ui_append_team_name(&label, base, offer->seller_id);
    }
    ui_text_append_ascii(&detail, "Fee ");
    ui_text_append_char(&detail, 0x0180);
    ui_text_append_char(&detail, ' ');
    ui_text_append_i32(&detail, offer->counter_fee > 0 ? offer->counter_fee : offer->fee);
    ui_text_append_ascii(&detail, "  ");
    if (offer->status == CM_OFFER_WAIT_USER_FEE) {
        ui_text_append_ascii(&detail, "Fee response");
    } else if (offer->status == CM_OFFER_WAIT_USER_WAGE) {
        ui_text_append_ascii(&detail, "Wage response");
    } else if (offer->status == CM_OFFER_WAIT_USER_SELLER_COUNTER) {
        ui_text_append_ascii(&detail, "Counter response");
    } else {
        ui_text_append_ascii(&detail, "Decision needed");
    }
}

static void ui_screen_target_append(int32_t target) {
    if (g_ui_screen_total_rows < MAX_PLAYERS) {
        g_ui_screen_all_targets[g_ui_screen_total_rows++] = target;
    }
}

static int32_t market_ui_sort_before(int32_t left_index, int32_t right_index) {
    if (left_index < 0 || left_index >= g_player_count) return 0;
    if (right_index < 0 || right_index >= g_player_count) return 1;
    MarketPlayer *left = &g_players[left_index];
    MarketPlayer *right = &g_players[right_index];
    int32_t left_value, right_value;
    if (g_ui_market_sort == 1) {
        left_value = market_value(left);
        right_value = market_value(right);
        if (left_value != right_value) return left_value < right_value;
    } else if (g_ui_market_sort == 2) {
        left_value = left->wage;
        right_value = right->wage;
        if (left_value != right_value) return left_value < right_value;
    } else {
        if (left->rating != right->rating) return left->rating > right->rating;
    }
    return left->player_id < right->player_id;
}

static void market_ui_sort_market_targets(void) {
    int32_t count = g_ui_screen_total_rows;
    if (count < 2) return;
    for (int32_t width = 1; width < count; width *= 2) {
        for (int32_t start = 0; start < count; start += width * 2) {
            int32_t middle = start + width;
            int32_t end = start + width * 2;
            if (middle > count) middle = count;
            if (end > count) end = count;
            int32_t left = start;
            int32_t right = middle;
            int32_t out = start;
            while (left < middle && right < end) {
                if (market_ui_sort_before(g_ui_screen_all_targets[left],
                                          g_ui_screen_all_targets[right])) {
                    g_ui_screen_sort_targets[out++] = g_ui_screen_all_targets[left++];
                } else {
                    g_ui_screen_sort_targets[out++] = g_ui_screen_all_targets[right++];
                }
            }
            while (left < middle) g_ui_screen_sort_targets[out++] = g_ui_screen_all_targets[left++];
            while (right < end) g_ui_screen_sort_targets[out++] = g_ui_screen_all_targets[right++];
        }
        for (int32_t i = 0; i < count; ++i) {
            g_ui_screen_all_targets[i] = g_ui_screen_sort_targets[i];
        }
        if (width > count / 2) break;
    }
}

static void ui_screen_collect_targets(uint32_t base) {
    g_ui_screen_total_rows = 0;
    if (!base) return;
    int32_t mode = g_ui_screen_list_mode;
    if (mode == UI_SCREEN_LIST_MARKET) {
        if (g_market.window_id < 0) return;
        for (int32_t i = 0; i < g_player_count && i < MAX_PLAYERS; ++i) {
            if (ui_screen_market_player_visible(base, i)) ui_screen_target_append(i);
        }
        market_ui_sort_market_targets();
    } else if (mode == UI_SCREEN_LIST_SALES) {
        for (int32_t i = 0; i < g_player_count && i < MAX_PLAYERS; ++i) {
            if (ui_screen_sales_player_visible(i)) {
                ui_screen_target_append(i);
            }
        }
    } else if (mode == UI_SCREEN_LIST_SHORTLIST) {
        for (int32_t slot = 0; slot < SHORTLIST_CAPACITY; ++slot) {
            int32_t player_id = g_ext.shortlist[slot];
            if (player_id < 0) continue;
            int32_t index = find_cached_player(player_id);
            if (index >= 0) ui_screen_target_append(index);
        }
    } else if (mode == UI_SCREEN_LIST_SQUAD) {
        int32_t team_id = g_ui_finance_club_cursor >= 0 && g_ui_finance_club_cursor < MAX_CLUBS
            ? g_market.clubs[g_ui_finance_club_cursor].team_id : USER_TEAM_ID;
        if (team_id < 0) return;
        for (int32_t i = 0; i < g_player_count && i < MAX_PLAYERS; ++i) {
            if (g_players[i].owner_id == team_id && g_players[i].value > 0) {
                ui_screen_target_append(i);
            }
        }
    } else if (mode == UI_SCREEN_LIST_INBOX) {
        int32_t offer_ids[OFFER_CAPACITY];
        int32_t count = 0;
        for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
            MarketOffer *offer = &g_market.offers[i];
            if (!offer_is_active(offer) ||
                (offer->seller_id != USER_TEAM_ID && offer->buyer_id != USER_TEAM_ID)) continue;
            int32_t insert = count;
            while (insert > 0 && offer_ids[insert - 1] < offer->offer_id) {
                offer_ids[insert] = offer_ids[insert - 1];
                --insert;
            }
            offer_ids[insert] = offer->offer_id;
            ++count;
        }
        for (int32_t i = 0; i < count; ++i) ui_screen_target_append(offer_ids[i]);
    } else if (mode == UI_SCREEN_LIST_HISTORY) {
        int32_t count = career_market_get_history_count();
        for (int32_t i = count - 1; i >= 0; --i) ui_screen_target_append(i);
    } else if (mode == UI_SCREEN_LIST_FINANCES) {
        int32_t user = find_account(USER_TEAM_ID);
        if (user >= 0 && user < MAX_CLUBS && g_market.clubs[user].team_id >= 0) {
            ui_screen_target_append(user);
        }
        for (int32_t i = 0; i < MAX_CLUBS; ++i) {
            if (i == user || g_market.clubs[i].team_id < 0) continue;
            ui_screen_target_append(i);
        }
    }
}

static void ui_screen_add_history_row(uint32_t base, int32_t index) {
    if (g_ui_screen_row_count >= MARKET_SCREEN_LIST_ROWS) return;
    CareerMarketTransferView record;
    if (!career_market_get_history(index, &record)) return;
    int32_t row = g_ui_screen_row_count++;
    g_ui_screen_row_targets[row] = index;
    MarketUiText label, detail;
    ui_text_reset(&label, g_ui_screen_row_labels[row], 48);
    ui_text_reset(&detail, g_ui_screen_row_details[row], 112);
    ui_append_player_name(&label, base, record.player_id);
    ui_append_team_name(&detail, base, record.seller_id);
    ui_text_append_ascii(&detail, " > ");
    ui_append_team_name(&detail, base, record.buyer_id);
    ui_text_append_ascii(&detail, " | ");
    ui_text_append_char(&detail, 0x0180);
    ui_text_append_char(&detail, ' ');
    ui_text_append_i32(&detail, record.fee);
}

static void ui_screen_add_finance_row(uint32_t base, int32_t index) {
    if (index < 0 || index >= MAX_CLUBS || g_ui_screen_row_count >= MARKET_SCREEN_LIST_ROWS) return;
    int32_t row = g_ui_screen_row_count++;
    g_ui_screen_row_targets[row] = index;
    MarketUiText label, detail;
    ui_text_reset(&label, g_ui_screen_row_labels[row], 48);
    ui_text_reset(&detail, g_ui_screen_row_details[row], 112);
    ui_append_team_name(&label, base, g_market.clubs[index].team_id);
    ui_text_append_ascii(&detail, "Strength ");
    ui_text_append_i32(&detail, g_market.clubs[index].strength);
    ui_text_append_ascii(&detail, " | Cash ");
    ui_text_append_i32(&detail, g_market.clubs[index].cash);
}

static void market_ui_page_window(int32_t total, int32_t requested_offset,
                                  int32_t *page_offset, int32_t *row_count) {
    int32_t offset = requested_offset;
    int32_t count = 0;
    if (total <= 0) {
        offset = 0;
    } else {
        if (offset < 0) offset = 0;
        if (offset >= total) {
            offset = ((total - 1) / MARKET_SCREEN_LIST_ROWS) * MARKET_SCREEN_LIST_ROWS;
        } else {
            offset = (offset / MARKET_SCREEN_LIST_ROWS) * MARKET_SCREEN_LIST_ROWS;
        }
        count = total - offset;
        if (count > MARKET_SCREEN_LIST_ROWS) count = MARKET_SCREEN_LIST_ROWS;
    }
    if (page_offset) *page_offset = offset;
    if (row_count) *row_count = count;
}

static int32_t market_ui_page_turn_offset(int32_t total, int32_t current_offset,
                                          int32_t direction) {
    int32_t offset, count;
    market_ui_page_window(total, current_offset, &offset, &count);
    int32_t requested = offset;
    if (direction < 0) {
        if (offset <= 0) return offset;
        requested -= MARKET_SCREEN_LIST_ROWS;
    } else if (direction > 0) {
        if (offset + count >= total) return offset;
        requested += MARKET_SCREEN_LIST_ROWS;
    } else {
        return offset;
    }
    market_ui_page_window(total, requested, &requested, &count);
    return requested;
}

static void market_ui_prepare_rows(uint32_t base) {
    ui_screen_clear_rows();
    ui_screen_collect_targets(base);
    int32_t mode = g_ui_screen_list_mode;
    if (mode <= UI_SCREEN_LIST_NONE || mode > UI_SCREEN_LIST_SHORTLIST) return;
    int32_t offset, row_count;
    market_ui_page_window(g_ui_screen_total_rows, g_ui_screen_page_offsets[mode],
                          &offset, &row_count);
    g_ui_screen_page_offsets[mode] = offset;
    int32_t end = offset + row_count;
    for (int32_t i = offset; i < end; ++i) {
        int32_t target = g_ui_screen_all_targets[i];
        if (mode == UI_SCREEN_LIST_MARKET || mode == UI_SCREEN_LIST_SALES ||
            mode == UI_SCREEN_LIST_SHORTLIST || mode == UI_SCREEN_LIST_SQUAD) {
            ui_screen_add_player_row(base, target, mode);
        } else if (mode == UI_SCREEN_LIST_INBOX) {
            ui_screen_add_offer_row(base, ui_find_offer_any(target));
        } else if (mode == UI_SCREEN_LIST_HISTORY) {
            ui_screen_add_history_row(base, target);
        } else if (mode == UI_SCREEN_LIST_FINANCES) {
            ui_screen_add_finance_row(base, target);
        }
    }
}

static void market_ui_add_button(uint32_t base, void *screen, const uint16_t *label,
                                 float x, float y, float w, float h, int32_t scheme,
                                 int32_t action, int32_t value) {
    if (!base || !screen || !label ||
        g_ui_screen_button_count >= MARKET_SCREEN_BUTTON_LIMIT) return;
    GameNewFn game_new = (GameNewFn)(base + 0x1C06AC);
    CfeTextButtonCtorMarketFn constructor = (CfeTextButtonCtorMarketFn)(base + 0x225F99);
    CfeSetPixelRectMarketFn set_rect = (CfeSetPixelRectMarketFn)(base + 0x25F3F5);
    void *button = game_new(0x11C, 0, 0);
    if (!button) return;
    constructor(button, label, 0, 0, scheme);
    set_rect(button, x, y, w, h);
    uint32_t *vtable = *(uint32_t **)screen;
    CfeAddChildMarketFn add_child = (CfeAddChildMarketFn)(uintptr_t)vtable[0x6C / 4];
    add_child(screen, button, 0.0f, 0.0f, 0.0f, 0.0f, 0);
    g_ui_screen_buttons[g_ui_screen_button_count] = button;
    g_ui_screen_button_actions[g_ui_screen_button_count] = action;
    g_ui_screen_button_values[g_ui_screen_button_count] = value;
    ++g_ui_screen_button_count;
}

static int32_t market_ui_uses_player_cards(void) {
    return g_ui_screen_list_mode == UI_SCREEN_LIST_MARKET ||
           g_ui_screen_list_mode == UI_SCREEN_LIST_SALES ||
           g_ui_screen_list_mode == UI_SCREEN_LIST_INBOX ||
           g_ui_screen_list_mode == UI_SCREEN_LIST_SQUAD ||
           g_ui_screen_list_mode == UI_SCREEN_LIST_SHORTLIST;
}

static void market_ui_screen_card_rect(float width, float height, float left_width,
                                       float margin, float content_y, float content_h,
                                       int32_t slot, int32_t count,
                                       float *out_x, float *out_y,
                                       float *out_w, float *out_h, float *out_meta_y) {
    if (!out_x || !out_y || !out_w || !out_h || !out_meta_y || count <= 0 ||
        slot < 0 || slot >= count) return;
    float grid_x = margin + width * 0.018f;
    float grid_width = left_width - width * 0.036f;
    float column_gap = width * 0.012f;
    float column_width = (grid_width - column_gap) * 0.5f;
    float row_gap = height * 0.018f;
    float meta_height = height * 0.075f;
    float outer_margin = height * 0.035f;
    int32_t row_count = (count + 1) / 2;
    float max_card_height = (content_h - outer_margin * 2.0f -
                             row_gap * (float)(row_count - 1) -
                             meta_height * (float)row_count) / (float)row_count;
    if (max_card_height < 1.0f) max_card_height = 1.0f;
    float card_width = column_width;
    float card_height = card_width * (148.0f / 252.0f);
    if (card_height > max_card_height) {
        card_height = max_card_height;
        card_width = card_height * (252.0f / 148.0f);
    }
    float total_height = (float)row_count * (card_height + meta_height) +
                         (float)(row_count - 1) * row_gap;
    float first_y = content_y + (content_h - total_height) * 0.5f;
    int32_t row = slot / 2;
    int32_t column = slot & 1;
    float x;
    if ((count & 1) && slot == count - 1) {
        x = grid_x + (grid_width - card_width) * 0.5f;
    } else {
        x = grid_x + (float)column * (column_width + column_gap) +
            (column_width - card_width) * 0.5f;
    }
    float y = first_y + (float)row * (card_height + meta_height + row_gap);
    *out_x = x;
    *out_y = y;
    *out_w = card_width;
    *out_h = card_height;
    *out_meta_y = y + card_height + height * 0.006f;
}

static float market_ui_screen_page_footer_height(float height, float content_h) {
    if (g_ui_screen_total_rows <= MARKET_SCREEN_LIST_ROWS || height <= 0.0f || content_h <= 0.0f) {
        return 0.0f;
    }
    float footer = height * 0.10f;
    if (footer > content_h * 0.22f) footer = content_h * 0.22f;
    return footer;
}

static float market_ui_screen_filter_height(float height, int32_t mode) {
    return (mode == UI_SCREEN_LIST_MARKET || mode == UI_SCREEN_LIST_SALES ||
            mode == UI_SCREEN_LIST_SQUAD)
        ? height * 0.090f : 0.0f;
}

static void market_ui_screen_action_layout(float height, float content_y, float content_h,
                                           int32_t option_count, int32_t show_details,
                                           int32_t *out_columns, float *out_detail_h,
                                           float *out_option_top, float *out_option_height) {
    int32_t columns = option_count > 6 || (show_details && option_count >= 4) ? 2 : 1;
    int32_t rows = option_count > 0 ? (option_count + columns - 1) / columns : 0;
    float option_gap = height * 0.016f;
    float option_start = height * 0.095f;
    float option_bottom = height * 0.050f;
    float detail_gap = height * 0.055f;
    float minimum_option_height = height * 0.055f;
    float detail_h = show_details ? height * 0.20f : 0.0f;
    if (show_details) {
        float max_detail_h = content_h - option_start - detail_gap - option_bottom -
                             option_gap * (float)(rows > 0 ? rows - 1 : 0) -
                             minimum_option_height * (float)rows;
        if (max_detail_h < 0.0f) max_detail_h = 0.0f;
        if (detail_h > max_detail_h) detail_h = max_detail_h;
    }
    float option_top = content_y + option_start + detail_h +
                       (detail_h > 0.0f ? detail_gap : 0.0f);
    float option_height = 0.0f;
    if (rows > 0) {
        float available = content_y + content_h - option_bottom - option_top -
                          option_gap * (float)(rows - 1);
        option_height = available / (float)rows;
        if (option_height > height * 0.115f) option_height = height * 0.115f;
        if (option_height < 0.0f) option_height = 0.0f;
    }
    if (out_columns) *out_columns = columns;
    if (out_detail_h) *out_detail_h = detail_h;
    if (out_option_top) *out_option_top = option_top;
    if (out_option_height) *out_option_height = option_height;
}

static int32_t market_ui_add_player_card(uint32_t base, void *screen, int32_t row,
                                         float width, float height, float left_width,
                                         float margin, float content_y, float content_h) {
    if (!base || !screen || row < 0 || row >= g_ui_screen_row_count ||
        g_ui_screen_card_count >= MARKET_SCREEN_CARD_LIMIT) return 0;
    int32_t player_index = -1;
    if (g_ui_screen_list_mode == UI_SCREEN_LIST_INBOX) {
        MarketOffer *offer = ui_find_offer_any(g_ui_screen_row_targets[row]);
        if (!offer || !offer_is_active(offer)) return 0;
        player_index = find_cached_player(offer->player_id);
    } else if (g_ui_screen_list_mode == UI_SCREEN_LIST_MARKET ||
               g_ui_screen_list_mode == UI_SCREEN_LIST_SALES ||
               g_ui_screen_list_mode == UI_SCREEN_LIST_SQUAD ||
               g_ui_screen_list_mode == UI_SCREEN_LIST_SHORTLIST) {
        player_index = g_ui_screen_row_targets[row];
    }
    if (player_index < 0 || player_index >= g_player_count) return 0;
    MarketPlayer *player = &g_players[player_index];
    PlayerInfo info;
    if (!load_player_info(base, player->player_id, &info)) return 0;

    float x, y, w, h, meta_y;
    market_ui_screen_card_rect(width, height, left_width, margin, content_y, content_h,
                               row, g_ui_screen_row_count, &x, &y, &w, &h, &meta_y);
    (void)meta_y;
    GameNewFn game_new = (GameNewFn)(base + 0x1C06AC);
    CfePlayerCardCtorMarketFn constructor = (CfePlayerCardCtorMarketFn)(base + 0x233B89);
    CfeSetAlignmentMarketFn set_alignment = (CfeSetAlignmentMarketFn)(base + 0x25EC91);
    CfeSetPixelRectMarketFn set_rect = (CfeSetPixelRectMarketFn)(base + 0x25F3F5);
    void *card = game_new(0x600, 0, 0);
    if (!card) return 0;
    constructor(card, &info, player->owner_id, 0, 0, -1, 0, 0x12u, 0);
    set_alignment(card, 0x12);
    set_rect(card, x, y, w, h);
    uint32_t *vtable = *(uint32_t **)screen;
    CfeAddChildMarketFn add_child = (CfeAddChildMarketFn)(uintptr_t)vtable[0x6C / 4];
    add_child(screen, card, 0.5f, 0.5f, 0.0f, 0.0f, 0);
    int32_t card_index = g_ui_screen_card_count++;
    g_ui_screen_cards[card_index] = card;
    g_ui_screen_card_rows[card_index] = row;
    return 1;
}

static int32_t market_ui_row_is_selected(int32_t row) {
    if (row < 0 || row >= g_ui_screen_row_count) return 0;
    int32_t target = g_ui_screen_row_targets[row];
    switch (g_ui_screen_list_mode) {
        case UI_SCREEN_LIST_MARKET:
        case UI_SCREEN_LIST_SALES:
        case UI_SCREEN_LIST_SHORTLIST:
            return target == g_ui_player_index;
        case UI_SCREEN_LIST_INBOX:
            return target == g_ui_offer_id;
        case UI_SCREEN_LIST_SQUAD:
            return target == g_ui_roster_player_cursor;
        case UI_SCREEN_LIST_HISTORY:
            return target == g_ui_history_index;
        case UI_SCREEN_LIST_FINANCES:
            return target == g_ui_finance_club_cursor;
        default:
            return 0;
    }
}

static void market_ui_select_target(int32_t mode, int32_t target) {
    uint32_t base = g_ui_base;
    if (!base) return;
    if (mode == UI_SCREEN_LIST_MARKET) {
        ui_show_market_player(base, target);
    } else if (mode == UI_SCREEN_LIST_SALES) {
        ui_show_sales_player(base, target);
    } else if (mode == UI_SCREEN_LIST_SHORTLIST) {
        ui_show_shortlist_player(base, target);
    } else if (mode == UI_SCREEN_LIST_INBOX) {
        MarketOffer *offer = ui_find_offer_any(target);
        if (offer && offer_is_active(offer)) {
            g_ui_offer_origin = UI_OFFER_FROM_INBOX;
            ui_show_offer(base, offer);
        } else {
            ui_show_inbox(base);
        }
    } else if (mode == UI_SCREEN_LIST_SQUAD) {
        g_ui_roster_player_cursor = target;
        ui_show_roster(base);
    } else if (mode == UI_SCREEN_LIST_HISTORY) {
        ui_show_history(base, target);
    } else if (mode == UI_SCREEN_LIST_FINANCES) {
        g_ui_finance_club_cursor = target;
        g_ui_finance_history_cursor = 0;
        ui_show_finances(base);
    }
}

static void market_ui_select_row(int32_t row) {
    if (row < 0 || row >= g_ui_screen_row_count) return;
    market_ui_select_target(g_ui_screen_list_mode, g_ui_screen_row_targets[row]);
}

static void market_ui_screen_turn_page(int32_t direction) {
    int32_t mode = g_ui_screen_list_mode;
    if (mode <= UI_SCREEN_LIST_NONE || mode > UI_SCREEN_LIST_SHORTLIST ||
        g_ui_screen_row_count < 1) return;
    int32_t offset = market_ui_page_turn_offset(g_ui_screen_total_rows,
                                                g_ui_screen_page_offsets[mode], direction);
    if (offset == g_ui_screen_page_offsets[mode]) return;
    if (offset < 0 || offset >= g_ui_screen_total_rows) return;
    g_ui_screen_page_offsets[mode] = offset;
    market_ui_select_target(mode, g_ui_screen_all_targets[offset]);
    g_ui_screen_dirty = 1;
}

static void market_ui_screen_rebuild(void *screen) {
    if (!screen || !g_ui_base) return;
    CfeDeleteChildMarketFn delete_child = (CfeDeleteChildMarketFn)(g_ui_base + 0x25FC2B);
    for (int32_t i = 0; i < g_ui_screen_button_count; ++i) {
        if (g_ui_screen_buttons[i]) delete_child(screen, g_ui_screen_buttons[i]);
        g_ui_screen_buttons[i] = (void *)0;
    }
    g_ui_screen_button_count = 0;
    for (int32_t i = 0; i < g_ui_screen_card_count; ++i) {
        if (g_ui_screen_cards[i]) delete_child(screen, g_ui_screen_cards[i]);
        g_ui_screen_cards[i] = (void *)0;
    }
    g_ui_screen_card_count = 0;

    CfeGetSizeMarketFn get_width = (CfeGetSizeMarketFn)(g_ui_base + 0x25EFED);
    CfeGetSizeMarketFn get_height = (CfeGetSizeMarketFn)(g_ui_base + 0x25EFF1);
    float width = get_width(screen);
    float height = get_height(screen);
    if (width < 400.0f || height < 300.0f) return;
    market_ui_prepare_rows(g_ui_base);
    float margin = width * 0.035f;
    float gap = width * 0.010f;
    float back_width = width * 0.11f;
    float tab_width = (width - margin * 2.0f - back_width - gap * (float)MARKET_SCREEN_TAB_COUNT) /
                      (float)MARKET_SCREEN_TAB_COUNT;
    float tab_y = height * 0.135f;
    float tab_height = height * 0.085f;
    static const char *const tab_names[MARKET_SCREEN_TAB_COUNT] = {
        "Market", "My Squad", "Sales", "Inbox", "Finances", "History", "Shortlist"
    };
    for (int32_t i = 0; i < MARKET_SCREEN_TAB_COUNT; ++i) {
        uint16_t label[24];
        MarketUiText builder;
        ui_text_reset(&builder, label, 24);
        ui_text_append_ascii(&builder, tab_names[i]);
        market_ui_add_button(g_ui_base, screen, label,
                             margin + (float)i * (tab_width + gap), tab_y,
                             tab_width, tab_height, i == g_ui_screen_tab ? 2 : 0,
                             UI_SCREEN_BUTTON_TAB, i);
    }
    uint16_t back_label[24];
    MarketUiText back_builder;
    ui_text_reset(&back_builder, back_label, 24);
    ui_text_append_ascii(&back_builder, "Back");
    market_ui_add_button(g_ui_base, screen, back_label,
                         width - margin - back_width, tab_y, back_width, tab_height, 0,
                         UI_SCREEN_BUTTON_BACK, 0);

    float content_y = tab_y + tab_height + height * 0.060f;
    float content_h = height - content_y - height * 0.050f;
    float left_width = width * 0.54f;
    float filter_h = market_ui_screen_filter_height(height, g_ui_screen_list_mode);
    float list_content_y = content_y + filter_h;
    float list_content_h = content_h - filter_h;
    if (list_content_h <= 0.0f) {
        list_content_y = content_y;
        list_content_h = content_h;
        filter_h = 0.0f;
    }
    float page_footer_h = market_ui_screen_page_footer_height(height, list_content_h);
    float card_content_h = list_content_h - page_footer_h;
    if (card_content_h <= 0.0f) card_content_h = list_content_h;
    float action_x = margin + left_width + width * 0.045f;
    float action_width = width - margin - action_x;
    int32_t option_columns = 1;
    float option_top = 0.0f;
    float option_height = 0.0f;
    market_ui_screen_action_layout(height, content_y, content_h, g_ui_option_count,
                                   market_ui_uses_player_cards(), &option_columns,
                                   0, &option_top, &option_height);
    float option_inner_x = action_x + width * 0.020f;
    float option_gap_x = width * 0.014f;
    float option_width = (action_width - width * 0.040f -
                          option_gap_x * (float)(option_columns - 1)) /
                         (float)option_columns;
    float option_gap = height * 0.016f;
    float row_x = margin + width * 0.018f;
    float row_width = left_width - width * 0.036f;
    float row_top = list_content_y + height * 0.080f;
    float row_height = height * 0.065f;
    float row_gap = height * 0.008f;
    static const uint16_t empty_label[] = {0};
    if (filter_h > 0.0f && g_ui_screen_list_mode == UI_SCREEN_LIST_MARKET) {
        uint16_t position_label[32], club_label[48], search_label[32], sort_label[32];
        MarketUiText position_builder, club_builder, search_builder, sort_builder;
        ui_text_reset(&position_builder, position_label, 32);
        ui_text_append_ascii(&position_builder, "Position: ");
        if (g_ui_position_filter < 0) ui_text_append_ascii(&position_builder, "All");
        else {
            static const char *const positions[] = {"GK", "DEF", "MID", "FWD"};
            ui_text_append_ascii(&position_builder, positions[g_ui_position_filter]);
        }
        ui_text_reset(&club_builder, club_label, 48);
        ui_text_append_ascii(&club_builder, "Club: ");
        if (g_ui_club_filter < 0) ui_text_append_ascii(&club_builder, "All");
        else ui_append_team_name(&club_builder, g_ui_base, g_ui_club_filter);
        ui_text_reset(&search_builder, search_label, 32);
        ui_text_append_ascii(&search_builder, g_ui_search_query[0] ? "Search: Active" : "Search by name");
        ui_text_reset(&sort_builder, sort_label, 32);
        ui_text_append_ascii(&sort_builder, "Sort: ");
        if (g_ui_market_sort == 1) ui_text_append_ascii(&sort_builder, "Value");
        else if (g_ui_market_sort == 2) ui_text_append_ascii(&sort_builder, "Wage");
        else ui_text_append_ascii(&sort_builder, "Rating");
        float filter_x = margin + width * 0.018f;
        float filter_gap = width * 0.010f;
        float filter_w = (left_width - width * 0.036f - filter_gap * 3.0f) / 4.0f;
        float filter_y = content_y + height * 0.012f;
        float filter_button_h = height * 0.060f;
        market_ui_add_button(g_ui_base, screen, position_label, filter_x, filter_y,
                             filter_w, filter_button_h, g_ui_position_filter >= 0 ? 2 : 0,
                             UI_SCREEN_BUTTON_FILTER_POSITION, 0);
        market_ui_add_button(g_ui_base, screen, club_label,
                             filter_x + filter_w + filter_gap, filter_y,
                             filter_w, filter_button_h, g_ui_club_filter >= 0 ? 2 : 0,
                             UI_SCREEN_BUTTON_FILTER_CLUB, 0);
        market_ui_add_button(g_ui_base, screen, search_label,
                             filter_x + (filter_w + filter_gap) * 2.0f, filter_y,
                             filter_w, filter_button_h, g_ui_search_query[0] ? 2 : 0,
                             UI_SCREEN_BUTTON_FILTER_SEARCH, 0);
        market_ui_add_button(g_ui_base, screen, sort_label,
                             filter_x + (filter_w + filter_gap) * 3.0f, filter_y,
                             filter_w, filter_button_h, 0,
                             UI_SCREEN_BUTTON_SORT, 0);
    } else if (filter_h > 0.0f && g_ui_screen_list_mode == UI_SCREEN_LIST_SALES) {
        uint16_t position_label[40];
        MarketUiText position_builder;
        ui_text_reset(&position_builder, position_label, 40);
        ui_text_append_ascii(&position_builder, "Squad position: ");
        if (g_ui_sales_position_filter < 0) ui_text_append_ascii(&position_builder, "All");
        else {
            static const char *const positions[] = {"GK", "DEF", "MID", "FWD"};
            ui_text_append_ascii(&position_builder, positions[g_ui_sales_position_filter]);
        }
        market_ui_add_button(g_ui_base, screen, position_label,
                             margin + width * 0.018f, content_y + height * 0.012f,
                             left_width - width * 0.036f, height * 0.060f,
                             g_ui_sales_position_filter >= 0 ? 2 : 0,
                             UI_SCREEN_BUTTON_FILTER_POSITION, 0);
    } else if (filter_h > 0.0f && g_ui_screen_list_mode == UI_SCREEN_LIST_SQUAD) {
        uint16_t previous_label[24], club_label[64], next_label[24];
        MarketUiText previous_builder, club_builder, next_builder;
        ui_text_reset(&previous_builder, previous_label, 24);
        ui_text_append_ascii(&previous_builder, "Previous club");
        ui_text_reset(&club_builder, club_label, 64);
        if (g_ui_finance_club_cursor >= 0 && g_ui_finance_club_cursor < MAX_CLUBS) {
            ui_append_team_name(&club_builder, g_ui_base,
                                g_market.clubs[g_ui_finance_club_cursor].team_id);
        } else {
            ui_text_append_ascii(&club_builder, "Club squad");
        }
        ui_text_reset(&next_builder, next_label, 24);
        ui_text_append_ascii(&next_builder, "Next club");
        float toolbar_x = margin + width * 0.018f;
        float toolbar_gap = width * 0.010f;
        float nav_w = (left_width - width * 0.036f) * 0.24f;
        float club_w = left_width - width * 0.036f - nav_w * 2.0f - toolbar_gap * 2.0f;
        float toolbar_y = content_y + height * 0.012f;
        float toolbar_h = height * 0.060f;
        market_ui_add_button(g_ui_base, screen, previous_label, toolbar_x, toolbar_y,
                             nav_w, toolbar_h, 0, UI_SCREEN_BUTTON_CLUB_PREVIOUS, 0);
        market_ui_add_button(g_ui_base, screen, club_label,
                             toolbar_x + nav_w + toolbar_gap, toolbar_y,
                             club_w, toolbar_h, 2, UI_SCREEN_BUTTON_OPTION, -1);
        market_ui_add_button(g_ui_base, screen, next_label,
                             toolbar_x + nav_w + toolbar_gap + club_w + toolbar_gap, toolbar_y,
                             nav_w, toolbar_h, 0, UI_SCREEN_BUTTON_CLUB_NEXT, 0);
    }
    int32_t cards_failed = 0;
    if (market_ui_uses_player_cards()) {
        for (int32_t i = 0; i < g_ui_screen_row_count; ++i) {
            market_ui_add_player_card(g_ui_base, screen, i, width, height,
                                      left_width, margin, list_content_y, card_content_h);
        }
        cards_failed = g_ui_screen_card_count != g_ui_screen_row_count;
        if (cards_failed) {
            for (int32_t i = 0; i < g_ui_screen_card_count; ++i) {
                if (g_ui_screen_cards[i]) delete_child(screen, g_ui_screen_cards[i]);
                g_ui_screen_cards[i] = (void *)0;
            }
            g_ui_screen_card_count = 0;
        }
    }
    if (!market_ui_uses_player_cards() || cards_failed) {
        for (int32_t i = 0; i < g_ui_screen_row_count; ++i) {
            market_ui_add_button(g_ui_base, screen, empty_label, row_x,
                                 row_top + (float)i * (row_height + row_gap),
                                 row_width, row_height, market_ui_row_is_selected(i) ? 2 : 0,
                                 UI_SCREEN_BUTTON_ROW, i);
        }
    }
    if (page_footer_h > 0.0f) {
        int32_t mode = g_ui_screen_list_mode;
        int32_t offset = mode > UI_SCREEN_LIST_NONE && mode <= UI_SCREEN_LIST_SHORTLIST
            ? g_ui_screen_page_offsets[mode] : 0;
        float footer_y = list_content_y + list_content_h - page_footer_h;
        float page_button_w = left_width * 0.21f;
        float page_button_h = height * 0.060f;
        if (page_button_h > page_footer_h * 0.72f) page_button_h = page_footer_h * 0.72f;
        float page_side = width * 0.018f;
        uint16_t previous_label[16], next_label[16];
        MarketUiText previous_builder, next_builder;
        ui_text_reset(&previous_builder, previous_label, 16);
        ui_text_append_ascii(&previous_builder, "Prev");
        ui_text_reset(&next_builder, next_label, 16);
        ui_text_append_ascii(&next_builder, "Next");
        if (offset > 0) {
            market_ui_add_button(g_ui_base, screen, previous_label,
                                 margin + page_side, footer_y + (page_footer_h - page_button_h) * 0.5f,
                                 page_button_w, page_button_h, 0,
                                 UI_SCREEN_BUTTON_PAGE_PREVIOUS, 0);
        }
        if (offset + g_ui_screen_row_count < g_ui_screen_total_rows) {
            market_ui_add_button(g_ui_base, screen, next_label,
                                 margin + left_width - page_side - page_button_w,
                                 footer_y + (page_footer_h - page_button_h) * 0.5f,
                                 page_button_w, page_button_h, 0,
                                 UI_SCREEN_BUTTON_PAGE_NEXT, 0);
        }
    }
    for (int32_t i = 0; i < g_ui_option_count; ++i) {
        int32_t row = i / option_columns;
        int32_t column = i % option_columns;
        market_ui_add_button(g_ui_base, screen, g_ui_options[i],
                             option_inner_x + (float)column * (option_width + option_gap_x),
                             option_top + (float)row * (option_height + option_gap),
                             option_width, option_height,
                             market_ui_option_scheme(g_ui_options[i]),
                             UI_SCREEN_BUTTON_OPTION, i);
    }
    g_ui_screen_dirty = 0;
}

static void market_ui_screen_init(void *screen) {
    g_ui_screen = screen;
    g_ui_screen_dirty = 1;
    market_ui_screen_rebuild(screen);
}

static void market_ui_screen_exit(void *screen) {
    if (screen == g_ui_screen) {
        g_ui_screen = (void *)0;
        g_ui_screen_button_count = 0;
        g_ui_screen_card_count = 0;
        g_ui_screen_row_count = 0;
        g_ui_screen_dirty = 0;
    }
}

static int32_t market_ui_screen_process(void *screen) {
    if (!screen || !g_ui_base) return 0;
    if (g_ui_screen_dirty) market_ui_screen_rebuild(screen);
    /* No ProcessAll here: the screen stack calls CFEScreen::ProcessAll, which processes the child
     * components and then calls this Process (vtable 0x14, CFEEntity::ProcessAll @0x25f746). Calling
     * ProcessAll from here recursed until the stack overflowed (tablet crash in ProcessInput). */
    uint32_t *vtable = *(uint32_t **)screen;
    CfeIsReleasedMarketFn is_released = (CfeIsReleasedMarketFn)(uintptr_t)vtable[0x98 / 4];
    for (int32_t i = 0; i < g_ui_screen_button_count; ++i) {
        void *button = g_ui_screen_buttons[i];
        if (!button || !is_released(button)) continue;
        g_ui_screen_dirty = 1;
        if (g_ui_screen_button_actions[i] == UI_SCREEN_BUTTON_TAB) {
            market_ui_select_tab(g_ui_screen_button_values[i]);
        } else if (g_ui_screen_button_actions[i] == UI_SCREEN_BUTTON_BACK) {
            market_ui_screen_go_back(g_ui_base);
        } else if (g_ui_screen_button_actions[i] == UI_SCREEN_BUTTON_PAGE_PREVIOUS) {
            market_ui_screen_turn_page(-1);
        } else if (g_ui_screen_button_actions[i] == UI_SCREEN_BUTTON_PAGE_NEXT) {
            market_ui_screen_turn_page(1);
        } else if (g_ui_screen_button_actions[i] == UI_SCREEN_BUTTON_FILTER_POSITION) {
            if (g_ui_screen_list_mode == UI_SCREEN_LIST_SALES) {
                ++g_ui_sales_position_filter;
                if (g_ui_sales_position_filter > 3) g_ui_sales_position_filter = -1;
                g_ui_screen_page_offsets[UI_SCREEN_LIST_SALES] = 0;
                g_ui_player_index = ui_find_user_player(-1, 1);
                ui_show_sales_player(g_ui_base, g_ui_player_index);
            } else {
                ++g_ui_position_filter;
                if (g_ui_position_filter > 3) g_ui_position_filter = -1;
                g_ui_screen_page_offsets[UI_SCREEN_LIST_MARKET] = 0;
                g_ui_player_index = ui_find_market_player(-1, 1);
                ui_show_market_player(g_ui_base, g_ui_player_index);
            }
        } else if (g_ui_screen_button_actions[i] == UI_SCREEN_BUTTON_FILTER_CLUB) {
            g_ui_club_cursor = g_ui_club_filter >= 0
                ? find_account(g_ui_club_filter) : ui_find_club_cursor(-1, 1);
            ui_show_club_filter(g_ui_base);
        } else if (g_ui_screen_button_actions[i] == UI_SCREEN_BUTTON_FILTER_SEARCH) {
            ui_open_player_search(g_ui_base);
        } else if (g_ui_screen_button_actions[i] == UI_SCREEN_BUTTON_SORT) {
            g_ui_market_sort = (g_ui_market_sort + 1) % 3;
            g_ui_screen_page_offsets[UI_SCREEN_LIST_MARKET] = 0;
            market_ui_prepare_rows(g_ui_base);
            if (g_ui_screen_total_rows > 0) {
                market_ui_select_target(UI_SCREEN_LIST_MARKET, g_ui_screen_all_targets[0]);
            }
        } else if (g_ui_screen_button_actions[i] == UI_SCREEN_BUTTON_CLUB_PREVIOUS ||
                   g_ui_screen_button_actions[i] == UI_SCREEN_BUTTON_CLUB_NEXT) {
            int32_t direction = g_ui_screen_button_actions[i] == UI_SCREEN_BUTTON_CLUB_PREVIOUS ? -1 : 1;
            g_ui_finance_club_cursor = ui_find_club_cursor(g_ui_finance_club_cursor, direction);
            g_ui_roster_player_cursor = -1;
            g_ui_screen_page_offsets[UI_SCREEN_LIST_SQUAD] = 0;
            ui_show_roster(g_ui_base);
        } else if (g_ui_screen_button_actions[i] == UI_SCREEN_BUTTON_ROW) {
            market_ui_select_row(g_ui_screen_button_values[i]);
        } else if (g_ui_screen_button_actions[i] == UI_SCREEN_BUTTON_OPTION) {
            int32_t selection = g_ui_screen_button_values[i];
            MarketUiCallback callback = g_ui_page_callback;
            if (callback && selection >= 0 && selection < g_ui_option_count) {
                callback(selection);
            }
        }
        return 0;
    }
    for (int32_t i = 0; i < g_ui_screen_card_count; ++i) {
        void *card = g_ui_screen_cards[i];
        if (!card) continue;
        uint32_t *card_vtable = *(uint32_t **)card;
        CfeIsReleasedMarketFn card_is_released =
            (CfeIsReleasedMarketFn)(uintptr_t)card_vtable[0x98 / 4];
        if (!card_is_released || !card_is_released(card)) continue;
        g_ui_screen_dirty = 1;
        market_ui_select_row(g_ui_screen_card_rows[i]);
        return 0;
    }
    return 0;
}

static void market_ui_screen_render(void *screen) {
    /* CFEScreen::Render is pure virtual: children are drawn by the framework's render-layer pass and
     * this screen draws in RenderPre/RenderPost. (vtable 0x38 is CFEEntity::CanHandleInput, not a
     * render function, so nothing is called here.) */
    (void)screen;
}

static void market_ui_screen_render_pre(void *screen) {
    if (!screen || !g_ui_base) return;
    CfeGetSizeMarketFn get_width = (CfeGetSizeMarketFn)(g_ui_base + 0x25EFED);
    CfeGetSizeMarketFn get_height = (CfeGetSizeMarketFn)(g_ui_base + 0x25EFF1);
    float width = get_width(screen);
    float height = get_height(screen);
    float margin = width * 0.035f;
    float tab_y = height * 0.135f;
    float content_y = tab_y + height * 0.085f + height * 0.060f;
    float content_h = height - content_y - height * 0.050f;
    float left_width = width * 0.54f;
    float filter_h = market_ui_screen_filter_height(height, g_ui_screen_list_mode);
    float list_content_y = content_y + filter_h;
    float list_content_h = content_h - filter_h;
    if (list_content_h <= 0.0f) {
        list_content_y = content_y;
        list_content_h = content_h;
        filter_h = 0.0f;
    }
    float action_x = margin + left_width + width * 0.045f;
    float action_width = width - margin - action_x;
    FeDrawRectMarketFn draw_rect = (FeDrawRectMarketFn)(g_ui_base + 0x28AF85);
    draw_rect(0.0f, 0.0f, width, height, 0xFF0D1620u);
    draw_rect(0.0f, 0.0f, width, height * 0.225f, 0xFF172633u);
    draw_rect(margin, content_y, left_width, content_h, 0xFF202D39u);
    draw_rect(action_x, content_y, action_width, content_h, 0xFF1A2733u);
    draw_rect(margin, content_y, left_width, 3.0f, 0xFF4FAE91u);
    draw_rect(action_x, content_y, action_width, 3.0f, 0xFF4FAE91u);
    float footer_h = market_ui_screen_page_footer_height(height, list_content_h);
    if (footer_h > 0.0f) {
        draw_rect(margin, list_content_y + list_content_h - footer_h, left_width, 2.0f, 0xFF344653u);
    }
    if (filter_h > 0.0f) {
        draw_rect(margin, list_content_y, left_width, 2.0f, 0xFF344653u);
    }
}

static void market_ui_draw_body(const uint16_t *text, float x, float y,
                                float width, float height, FeDrawTextBoldMarketFn draw_text) {
    if (!text || !draw_text || width <= 0.0f || height <= 0.0f) return;
    int32_t max_chars = (int32_t)(width / 10.5f);
    if (max_chars < 24) max_chars = 24;
    if (max_chars > 90) max_chars = 90;
    int32_t pos = 0;
    int32_t rows = 0;
    int32_t max_rows = (int32_t)(height / 27.0f);
    while (text[pos] && rows < max_rows) {
        if (text[pos] == '\n') {
            ++pos;
            ++rows;
            continue;
        }
        int32_t len = 0;
        int32_t last_space = -1;
        while (text[pos + len] && text[pos + len] != '\n' && len < max_chars) {
            if (text[pos + len] == ' ') last_space = len;
            g_ui_screen_line[len] = text[pos + len];
            ++len;
        }
        int32_t consume = len;
        if (text[pos + len] && text[pos + len] != '\n' && last_space > 0) {
            len = last_space;
            consume = last_space + 1;
        }
        g_ui_screen_line[len] = 0;
        draw_text(g_ui_screen_line, x, y + (float)rows * 27.0f, 0xFFE4EAF0u);
        pos += consume;
        while (text[pos] == ' ') ++pos;
        if (text[pos] == '\n') ++pos;
        ++rows;
    }
}

static void market_ui_draw_line(const uint16_t *text, float x, float y, float width,
                                uint32_t color, FeDrawTextBoldMarketFn draw_text) {
    if (!text || !draw_text || width <= 0.0f) return;
    int32_t max_chars = (int32_t)(width / 10.5f);
    if (max_chars < 1) return;
    if (max_chars > 95) max_chars = 95;
    int32_t len = 0;
    while (text[len] && len < max_chars) {
        g_ui_screen_line[len] = text[len];
        ++len;
    }
    if (text[len] && len > 3) {
        g_ui_screen_line[len - 1] = '.';
        g_ui_screen_line[len - 2] = '.';
        g_ui_screen_line[len - 3] = '.';
    }
    g_ui_screen_line[len] = 0;
    draw_text(g_ui_screen_line, x, y, color);
}

static void market_ui_draw_card_outline(float x, float y, float width, float height,
                                        float thickness, uint32_t color,
                                        FeDrawRectMarketFn draw_rect) {
    if (!draw_rect || width <= 0.0f || height <= 0.0f || thickness <= 0.0f) return;
    draw_rect(x - thickness, y - thickness, width + thickness * 2.0f, thickness, color);
    draw_rect(x - thickness, y + height, width + thickness * 2.0f, thickness, color);
    draw_rect(x - thickness, y, thickness, height, color);
    draw_rect(x + width, y, thickness, height, color);
}

static void market_ui_screen_render_post(void *screen) {
    if (!screen || !g_ui_base) return;
    CfeGetSizeMarketFn get_width = (CfeGetSizeMarketFn)(g_ui_base + 0x25EFED);
    CfeGetSizeMarketFn get_height = (CfeGetSizeMarketFn)(g_ui_base + 0x25EFF1);
    float width = get_width(screen);
    float height = get_height(screen);
    float margin = width * 0.035f;
    float content_y = height * 0.280f;
    float content_h = height - content_y - height * 0.050f;
    float left_width = width * 0.54f;
    float filter_h = market_ui_screen_filter_height(height, g_ui_screen_list_mode);
    float list_content_y = content_y + filter_h;
    float list_content_h = content_h - filter_h;
    if (list_content_h <= 0.0f) {
        list_content_y = content_y;
        list_content_h = content_h;
        filter_h = 0.0f;
    }
    float footer_h = market_ui_screen_page_footer_height(height, list_content_h);
    float card_content_h = list_content_h - footer_h;
    if (card_content_h <= 0.0f) card_content_h = list_content_h;
    float action_x = margin + left_width + width * 0.045f;
    float action_width = width - margin - action_x;
    FeDrawTextBoldMarketFn draw_text = (FeDrawTextBoldMarketFn)(g_ui_base + 0x293E29);
    FeDrawRectMarketFn draw_rect = (FeDrawRectMarketFn)(g_ui_base + 0x28AF85);
    float action_detail_h = 0.0f;
    float action_option_top = 0.0f;
    int32_t card_list = market_ui_uses_player_cards();
    market_ui_screen_action_layout(height, content_y, content_h, g_ui_option_count,
                                   card_list, 0, &action_detail_h,
                                   &action_option_top, 0);
    draw_text(g_ui_title, margin, height * 0.038f, 0xFFFFFFFFu);
    MarketUiText status;
    ui_text_reset(&status, g_ui_screen_status, 160);
    ui_text_append_ascii(&status, "Season ");
    ui_text_append_i32(&status, g_market.season + 1);
    ui_text_append_ascii(&status, "   Turn ");
    ui_text_append_i32(&status, g_market.current_turn + 1);
    ui_text_append_ascii(&status, "   ");
    ui_text_append_ascii(&status, g_market.window_id < 0 ? "Window closed" : "Window open");
    CareerMarketClubView club;
    if (career_market_get_club(USER_TEAM_ID, &club)) {
        ui_text_append_ascii(&status, "   ");
        ui_text_append_char(&status, 0x0180);
        ui_text_append_char(&status, ' ');
        ui_text_append_i32(&status, club.cash);
        ui_text_append_ascii(&status, " coins");
    }
    draw_text(g_ui_screen_status, margin, height * 0.082f, 0xFFB7C4D1u);
    static const uint16_t details_label[] = {'D','E','T','A','I','L','S',0};
    static const uint16_t selected_label[] = {
        'S','E','L','E','C','T','E','D',' ','D','E','T','A','I','L','S',0
    };
    static const uint16_t actions_label[] = {'A','C','T','I','O','N','S',0};
    float body_y = content_y + height * 0.105f;
    float body_h = content_h - height * 0.140f - footer_h;
    if (card_list && g_ui_screen_card_count > 0) {
        for (int32_t i = 0; i < g_ui_screen_card_count; ++i) {
            int32_t row = g_ui_screen_card_rows[i];
            float card_x, card_y, card_w, card_h, meta_y;
            market_ui_screen_card_rect(width, height, left_width, margin,
                                       list_content_y, card_content_h, row, g_ui_screen_row_count,
                                       &card_x, &card_y, &card_w, &card_h, &meta_y);
            uint32_t primary_color = market_ui_row_is_selected(row)
                ? 0xFF82D7BCu : 0xFFE4EAF0u;
            if (market_ui_row_is_selected(row)) {
                market_ui_draw_card_outline(card_x, card_y, card_w, card_h,
                                            3.0f, 0xFF4FAE91u, draw_rect);
            }
            market_ui_draw_line(g_ui_screen_row_labels[row], card_x + width * 0.006f,
                                meta_y + height * 0.006f, card_w - width * 0.012f,
                                primary_color, draw_text);
            market_ui_draw_line(g_ui_screen_row_details[row], card_x + width * 0.006f,
                                meta_y + height * 0.039f, card_w - width * 0.012f,
                                0xFFB7C4D1u, draw_text);
        }
        body_h = 0.0f;
    } else if (g_ui_screen_row_count > 0) {
        float row_top = list_content_y + height * 0.080f;
        float row_height = height * 0.065f;
        float row_gap = height * 0.008f;
        float row_x = margin + width * 0.036f;
        float row_text_width = left_width - width * 0.072f;
        float row_bottom = row_top + (float)g_ui_screen_row_count * row_height +
                           (float)(g_ui_screen_row_count - 1) * row_gap;
        for (int32_t i = 0; i < g_ui_screen_row_count; ++i) {
            float y = row_top + (float)i * (row_height + row_gap);
            uint32_t primary_color = market_ui_row_is_selected(i) ? 0xFFFFFFFFu : 0xFFE4EAF0u;
            market_ui_draw_line(g_ui_screen_row_labels[i], row_x,
                                y + height * 0.012f, row_text_width,
                                primary_color, draw_text);
            market_ui_draw_line(g_ui_screen_row_details[i], row_x,
                                y + height * 0.041f, row_text_width,
                                0xFFB7C4D1u, draw_text);
        }
        body_y = row_bottom + height * 0.034f;
        body_h = list_content_y + list_content_h - footer_h - body_y - height * 0.025f;
        draw_text(selected_label, margin + width * 0.018f, body_y - height * 0.020f,
                  0xFF82D7BCu);
    } else {
        draw_text(details_label, margin + width * 0.018f, content_y + height * 0.050f,
                  0xFF82D7BCu);
    }
    if (card_list && action_detail_h > 0.0f) {
        draw_text(selected_label, action_x + width * 0.020f, content_y + height * 0.050f,
                  0xFF82D7BCu);
        market_ui_draw_body(g_ui_description, action_x + width * 0.020f,
                            content_y + height * 0.095f,
                            action_width - width * 0.040f,
                            action_detail_h - height * 0.030f, draw_text);
        draw_text(actions_label, action_x + width * 0.020f,
                  action_option_top - height * 0.040f, 0xFF82D7BCu);
    } else {
        draw_text(actions_label, action_x + width * 0.020f, content_y + height * 0.050f,
                  0xFF82D7BCu);
    }
    if (!card_list) {
        market_ui_draw_body(g_ui_description, margin + width * 0.018f,
                            body_y,
                            left_width - width * 0.036f,
                            body_h, draw_text);
    }
    if (footer_h > 0.0f) {
        int32_t mode = g_ui_screen_list_mode;
        int32_t offset = mode > UI_SCREEN_LIST_NONE && mode <= UI_SCREEN_LIST_SHORTLIST
            ? g_ui_screen_page_offsets[mode] : 0;
        int32_t first = offset + 1;
        int32_t last = offset + g_ui_screen_row_count;
        if (last > g_ui_screen_total_rows) last = g_ui_screen_total_rows;
        MarketUiText page;
        ui_text_reset(&page, g_ui_screen_line, 96);
        ui_text_append_i32(&page, first);
        ui_text_append_char(&page, '-');
        ui_text_append_i32(&page, last);
        ui_text_append_ascii(&page, " / ");
        ui_text_append_i32(&page, g_ui_screen_total_rows);
        float page_text_width = (float)page.length * 10.5f;
        float page_x = margin + (left_width - page_text_width) * 0.5f;
        float footer_y = list_content_y + list_content_h - footer_h;
        draw_text(g_ui_screen_line, page_x,
                  footer_y + (footer_h - height * 0.030f) * 0.5f,
                  0xFFB7C4D1u);
    }
}

static int32_t market_ui_screen_is_fullscreen(void *screen) {
    (void)screen;
    return 1;
}

static void market_ui_open_screen(uint32_t base) {
    if (!base || g_ui_screen || g_ui_screen_request) return;
    mod_boot(base, 0);
    g_ui_base = base;
    g_ui_screen_request = 1;
    CfeForwardMarketFn forward = (CfeForwardMarketFn)(base + 0x2984CD);
    /* CFEScreenStack::Forward only queues the request; CFEScreenStack::NewScreen builds the screen on a
     * later frame. The request stays set until career_market_new_screen_hook consumes it there;
     * clearing it here let the stock NewScreen(32), DLS's Safe Mode screen, be built instead. */
    forward(MARKET_SCREEN_ID, 0, (void *)0, (void *)0, 1, 0);
}

static void *tm_build_screen(uint32_t base);

/* CFEScreen's deleting destructor (vtable slot 1) is a trap: the class is abstract. A cloned vtable needs its
 * own, or leaving the screen (CFEScreenStack::DeleteTopScreen) dies with SIGILL at 0x23B5B6. */
static void market_screen_delete(void *screen) {
    uint32_t base = g_ui_base;
    market_ui_screen_exit(screen);
    ((void (*)(void *))(uintptr_t)(base + 0x23B5B3))(screen);      /* CFEScreen::~CFEScreen */
    ((void (*)(void *))(uintptr_t)(base + 0x5C15C9))(screen);      /* operator delete (veneer) */
}

static uint32_t career_market_new_screen_hook(ModCtx *ctx, uint32_t base) {
#if MARKET_TM2
    if (ctx && base && ctx->r[1] == 0x19) {       /* v37: Transfer Market v2 replaces CFESDreamLeagueTransfers */
        mod_boot(base, 0);
        void *screen = tm_build_screen(base);
        if (!screen) return 0;
        ctx->r[0] = (uint32_t)(uintptr_t)screen;
        return ctx->lr;
    }
#endif
    if (!ctx || !base || !g_ui_screen_request || ctx->r[1] != MARKET_SCREEN_ID) return 0;
    GameNewFn game_new = (GameNewFn)(base + 0x1C06AC);
    CfeScreenCtorMarketFn constructor = (CfeScreenCtorMarketFn)(base + 0x23B52D);
    CfeScreenSetIdMarketFn set_id = (CfeScreenSetIdMarketFn)(base + 0x23B5AD);
    void *screen = game_new(0xF4, 0, 0);
    if (!screen) return 0;
    constructor(screen);
    const uint32_t *source_vtable = (const uint32_t *)(base + 0x71ABD0);
    for (int32_t i = 0; i < MARKET_SCREEN_VTABLE_ENTRIES; ++i) {
        g_ui_screen_vtable[i] = source_vtable[i];
    }
    g_ui_screen_vtable[1] = (uint32_t)(uintptr_t)market_screen_delete | 1u;
    g_ui_screen_vtable[3] = (uint32_t)(uintptr_t)market_ui_screen_init | 1u;
    g_ui_screen_vtable[4] = (uint32_t)(uintptr_t)market_ui_screen_exit | 1u;
    g_ui_screen_vtable[5] = (uint32_t)(uintptr_t)market_ui_screen_process | 1u;
    g_ui_screen_vtable[6] = (uint32_t)(uintptr_t)market_ui_screen_render | 1u;
    g_ui_screen_vtable[32] = (uint32_t)(uintptr_t)market_ui_screen_render_pre | 1u;
    g_ui_screen_vtable[36] = (uint32_t)(uintptr_t)market_ui_screen_render_post | 1u;
    g_ui_screen_vtable[44] = (uint32_t)(uintptr_t)market_ui_screen_is_fullscreen | 1u;
    *(uint32_t *)screen = (uint32_t)(uintptr_t)g_ui_screen_vtable;
    set_id(screen, MARKET_SCREEN_ID);
    g_ui_screen = screen;
    ctx->r[0] = (uint32_t)(uintptr_t)screen;
    g_ui_screen_request = 0;
    return ctx->lr;
}

static int32_t ui_callback_notice(int32_t selection) {
    (void)selection;
    uint32_t base = g_ui_base;
    if (g_ui_after_notice == UI_AFTER_CLOSE ||
        (g_ui_from_stock && (g_ui_after_notice == UI_AFTER_BROWSER ||
                             g_ui_after_notice == UI_AFTER_SALES))) {
        g_ui_from_stock = 0;
        return 1;
    }
    if (g_ui_after_notice == UI_AFTER_BROWSER) {
        ui_show_market_player(base, g_ui_player_index);
    } else if (g_ui_after_notice == UI_AFTER_INBOX) {
        ui_show_inbox(base);
    } else if (g_ui_after_notice == UI_AFTER_SALES) {
        ui_show_sales_player(base, g_ui_player_index);
    } else if (g_ui_after_notice == UI_AFTER_SHORTLIST) {
        ui_show_shortlist(base);
    } else if (g_ui_after_notice == UI_AFTER_ROSTER) {
        ui_show_roster(base);
    } else if (g_ui_after_notice == UI_AFTER_CLUB_FILTER) {
        ui_show_club_filter(base);
    } else {
        market_ui_show_main(base);
    }
    return 1;
}

/* The v4-v30 main-menu actions, by their old index (0 Browse Market, 1 Offers / Inbox, 2 Club
 * Finances, 3 Transfer History, 4 Player Sales, 5 Close); the v31 hubs route here unchanged. */
static void ui_main_action(int32_t selection);

static int32_t ui_callback_main(int32_t selection) {
    if (selection == 0) ui_show_transfers_hub(g_ui_base);
    else if (selection == 1) ui_show_club_hub(g_ui_base);
    else ui_main_action(5);
    return 1;
}

static int32_t ui_callback_transfers_hub(int32_t selection) {
    if (selection == 0) ui_main_action(0);
    else if (selection == 1) market_ui_select_tab(6);
    else market_ui_show_main(g_ui_base);
    return 1;
}

static int32_t ui_callback_club_hub(int32_t selection) {
    static const int32_t actions[] = {1, 4, 2, 3};
    if (selection >= 0 && selection < 4) ui_main_action(actions[selection]);
    else market_ui_show_main(g_ui_base);
    return 1;
}

static void ui_main_action(int32_t selection) {
    uint32_t base = g_ui_base;
    if (selection == 0) {
        if (g_market.window_id < 0) {
            ui_show_notice(base, "Transfer Market", "The transfer window is closed. It reopens at the start of next season, until 6 league matches are played.", UI_AFTER_MENU);
        } else {
            g_ui_browser_mode = 0;
            g_ui_player_index = ui_find_market_player(-1, 1);
            ui_show_market_player(base, g_ui_player_index);
        }
    } else if (selection == 1) {
        ui_show_inbox(base);
    } else if (selection == 2) {
        g_ui_finance_club_cursor = find_account(USER_TEAM_ID);
        g_ui_finance_history_cursor = 0;
        ui_show_finances(base);
    } else if (selection == 3) {
        int32_t count = career_market_get_history_count();
        ui_show_history(base, count - 1);
    } else if (selection == 4) {
        g_ui_browser_mode = 1;
        g_ui_player_index = ui_find_user_player(-1, 1);
        ui_show_sales_player(base, g_ui_player_index);
    } else if (selection == 5) {
        market_ui_return_to_transfers(base);
    }
}

/* Market browser: Next Player, Player Actions, (More) Previous Player, Filters, Back. */
static int32_t ui_callback_market_player(int32_t selection) {
    uint32_t base = g_ui_base;
    if (selection == 0 || selection == 2) {
        int32_t index = ui_find_market_player(g_ui_player_index, selection == 0 ? 1 : -1);
        if (index >= 0) g_ui_player_index = index;
        ui_show_market_player(base, g_ui_player_index);
    } else if (selection == 1) {
        ui_show_market_actions(base);
    } else if (selection == 3) {
        ui_show_market_filters(base);
    } else {
        ui_show_transfers_hub(base);
    }
    return 1;
}

/* No player matches the filters: Filters, Back. */
static int32_t ui_callback_market_empty(int32_t selection) {
    if (selection == 0) ui_show_market_filters(g_ui_base);
    else ui_show_transfers_hub(g_ui_base);
    return 1;
}

/* Player page: Make Offer, Add to / Remove from Shortlist, Back. */
static int32_t ui_callback_market_actions(int32_t selection) {
    uint32_t base = g_ui_base;
    if (selection == 1) {
        if (g_ui_player_index < 0 || g_ui_player_index >= g_player_count) {
            ui_show_market_player(base, g_ui_player_index);
            return 1;
        }
        MarketPlayer *player = &g_players[g_ui_player_index];
        if (shortlist_toggle(player->player_id) < 0) {
            ui_show_notice(base, "Shortlist Full", "The shortlist holds up to 16 players. Remove one before adding another.", UI_AFTER_BROWSER);
        } else {
            save_profile(base);
            ui_show_market_actions(base);
        }
    } else if (selection == 0) {
        if (g_ui_player_index < 0 || g_ui_player_index >= g_player_count) {
            ui_show_market_player(base, g_ui_player_index);
            return 1;
        }
        MarketPlayer *player = &g_players[g_ui_player_index];
        g_ui_bid_player_id = player->player_id;
        g_ui_bid_fee = player->value > 0 ? player->value : 1;
        g_ui_bid_wage = player->wage > 0 ? player->wage : 1;
        g_ui_bid_contract_years = 3;
        g_ui_offer_origin = UI_OFFER_FROM_BROWSER;
        ui_show_bid_terms(base);
    } else {
        ui_show_market_player(base, g_ui_player_index);
    }
    return 1;
}

/* Filters: Position, Club / Name Search, Back. */
static int32_t ui_callback_market_filters(int32_t selection) {
    if (selection == 0) {
        g_ui_browser_mode = 0;
        ui_show_position_filter(g_ui_base);
    } else if (selection == 1) {
        ui_show_club_filter(g_ui_base);
    } else {
        if (g_ui_player_index < 0 || g_ui_player_index >= g_player_count ||
            ui_find_market_player(g_ui_player_index, 1) < 0) {
            g_ui_player_index = ui_find_market_player(-1, 1);
        }
        ui_show_market_player(g_ui_base, g_ui_player_index);
    }
    return 1;
}

/* Club filter: Next club, Use this club, (More) Previous club, All clubs, (More) Search player
 * names, Back. */
static int32_t ui_callback_club_filter(int32_t selection) {
    if (selection == 2) {
        g_ui_club_cursor = ui_find_club_cursor(g_ui_club_cursor, -1);
        ui_show_club_filter(g_ui_base);
    } else if (selection == 0) {
        g_ui_club_cursor = ui_find_club_cursor(g_ui_club_cursor, 1);
        ui_show_club_filter(g_ui_base);
    } else if (selection == 1 && g_ui_club_cursor >= 0 && g_ui_club_cursor < MAX_CLUBS) {
        g_ui_club_filter = g_market.clubs[g_ui_club_cursor].team_id;
        g_ui_screen_page_offsets[UI_SCREEN_LIST_MARKET] = 0;
        g_ui_player_index = ui_find_market_player(-1, 1);
        ui_show_market_player(g_ui_base, g_ui_player_index);
    } else if (selection == 3) {
        g_ui_club_filter = -1;
        g_ui_screen_page_offsets[UI_SCREEN_LIST_MARKET] = 0;
        g_ui_player_index = ui_find_market_player(-1, 1);
        ui_show_market_player(g_ui_base, g_ui_player_index);
    } else if (selection == 4) {
        ui_open_player_search(g_ui_base);
    } else {
        ui_show_market_filters(g_ui_base);
    }
    return 1;
}

/* Club Finances: Club squad, Finance activity, (More) Season books, Back. */
static int32_t ui_callback_finances(int32_t selection) {
    if (selection == 0) {
        g_ui_roster_player_cursor = -1;
        ui_show_roster(g_ui_base);
    } else if (selection == 1) {
        g_ui_finance_history_cursor = 0;
        ui_show_finance_history(g_ui_base);
    } else if (selection == 2) {
        g_ui_books_page = 0;
        ui_show_books(g_ui_base);
    } else {
        ui_show_club_hub(g_ui_base);
    }
    return 1;
}

/* Club Squad: Next player, Renew contract / Club finances, (More) Previous player, Next club,
 * (More) Previous club, Back. Only the action button (1) reaches the v30 roster logic (its old 0). */
static int32_t ui_callback_roster(int32_t selection) {
    int32_t team_id = g_ui_finance_club_cursor >= 0 &&
                      g_ui_finance_club_cursor < MAX_CLUBS
        ? g_market.clubs[g_ui_finance_club_cursor].team_id : -1;
    if (selection == 0 || selection == 2) {
        if (team_id >= 0) {
            int32_t index = ui_find_club_player(team_id, g_ui_roster_player_cursor,
                                                selection == 0 ? 1 : -1);
            if (index >= 0) g_ui_roster_player_cursor = index;
        }
        ui_show_roster(g_ui_base);
        return 1;
    }
    if (selection == 3 || selection == 4) {
        int32_t cursor = ui_find_club_cursor(g_ui_finance_club_cursor, selection == 3 ? 1 : -1);
        if (cursor >= 0) {
            g_ui_finance_club_cursor = cursor;
            g_ui_roster_player_cursor = -1;
        }
        ui_show_roster(g_ui_base);
        return 1;
    }
    if (selection != 1) {
        ui_show_finances(g_ui_base);
        return 1;
    }
    if (team_id == USER_TEAM_ID) {
        if (g_ui_roster_player_cursor < 0 || g_ui_roster_player_cursor >= g_player_count) {
            ui_show_notice(g_ui_base, "Contract Renewal", "Select a user-club player first.", UI_AFTER_ROSTER);
        } else {
            MarketPlayer *player = &g_players[g_ui_roster_player_cursor];
            g_ui_renewal_player_id = player->player_id;
            g_ui_renewal_wage = player->wage > 0 ? player->wage : 1;
            g_ui_renewal_years = 3;
            g_ui_renewal_counter_wage = 0;
            g_ui_renewal_stage = 0;
            if (!market_contract_available(player->player_id, USER_TEAM_ID)) {
                ui_show_notice(g_ui_base, "Contract Renewal", "This player has no career-market contract available to renew yet.", UI_AFTER_ROSTER);
            } else {
                ui_show_contract_renewal(g_ui_base);
            }
        }
    } else {
        ui_show_finances(g_ui_base);
    }
    return 1;
}

static int32_t ui_callback_contract_renewal(int32_t selection) {
    uint32_t base = g_ui_base;
    int32_t player_index = find_cached_player(g_ui_renewal_player_id);
    int32_t account_index = find_account(USER_TEAM_ID);
    if (player_index < 0 || account_index < 0 ||
        g_players[player_index].owner_id != USER_TEAM_ID ||
        !market_contract_available(g_ui_renewal_player_id, USER_TEAM_ID)) {
        ui_show_notice(base, "Contract Renewal", "The player or market contract is no longer available.", UI_AFTER_ROSTER);
        return 1;
    }
    MarketPlayer *player = &g_players[player_index];
    ClubAccount *club = &g_market.clubs[account_index];
    int32_t max_wage = clamp_add(player->wage, account_wage_room(club));
    /* v31 pages (<= 3 buttons) -> the v30 option indices the logic below was written for. */
    if (g_ui_renewal_stage == 1) {
        static const int32_t wage_order[] = {1, 2, 0, 3, 4, 5};
        selection = selection >= 0 && selection < 6 ? wage_order[selection] : 5;
    } else if (g_ui_renewal_stage == 2) {
        if (selection == 0 && g_ui_renewal_years > 1) --g_ui_renewal_years;
        else if (selection == 1 && g_ui_renewal_years < 5) ++g_ui_renewal_years;
        else if (selection != 0 && selection != 1) g_ui_renewal_stage = 0;
        ui_show_contract_renewal(base);
        return 1;
    } else if (g_ui_renewal_stage == 4) {
        g_ui_renewal_stage = selection == 0 ? 1 : selection == 1 ? 2 : 0;
        ui_show_contract_renewal(base);
        return 1;
    } else if (g_ui_renewal_stage == 0) {
        if (selection == 1) {
            g_ui_renewal_stage = 4;
            ui_show_contract_renewal(base);
            return 1;
        }
        selection = selection == 0 ? 2 : 3;
    }
    if (g_ui_renewal_stage == 1) {
        if (selection == 4) {
            ui_open_exact_amount(base, UI_EXACT_AMOUNT_RENEWAL_WAGE,
                                 "Enter Renewal Wage", g_ui_renewal_wage);
            return 1;
        }
        if (selection >= 0 && selection < 4) {
            int32_t percent = selection == 0 || selection == 3 ? 20 : 10;
            int32_t step = mul_div(player->wage, percent, 100);
            if (step < 1) step = 1;
            if (selection < 2) {
                g_ui_renewal_wage = g_ui_renewal_wage > step
                    ? g_ui_renewal_wage - step : 1;
            } else {
                g_ui_renewal_wage = clamp_add(g_ui_renewal_wage, step);
                if (g_ui_renewal_wage > max_wage) g_ui_renewal_wage = max_wage;
            }
        } else {
            g_ui_renewal_stage = 0;
            g_ui_amount_error = UI_AMOUNT_ERROR_NONE;
        }
        ui_show_contract_renewal(base);
        return 1;
    }
    if (g_ui_renewal_stage == 3) {
        if (selection == 0) {
            int32_t result = commit_user_contract_renewal(
                base, g_ui_renewal_player_id, g_ui_renewal_counter_wage,
                g_ui_renewal_years);
            if (result > 0) {
                ui_show_notice(base, "Contract Renewed", "The player accepted the wage counter and extended the contract.", UI_AFTER_ROSTER);
            } else {
                ui_show_notice(base, "Renewal Not Completed", "The club can no longer meet the player's counteroffer.", UI_AFTER_ROSTER);
            }
        } else {
            g_ui_renewal_stage = 0;
            ui_show_roster(base);
        }
        return 1;
    }
    if (selection == 0) {
        g_ui_renewal_stage = 1;
        ui_show_contract_renewal(base);
    } else if (selection == 1) {
        g_ui_renewal_stage = 2;
        ui_show_contract_renewal(base);
    } else if (selection == 2) {
        if (g_ui_renewal_wage > max_wage) {
            ui_show_notice(base, "Renewal Over Budget", "The proposed wage exceeds the club's annual wage room.", UI_AFTER_ROSTER);
            return 1;
        }
        int32_t demand = contract_renewal_wage_demand(player, club);
        if (g_ui_renewal_wage >= demand) {
            int32_t result = commit_user_contract_renewal(
                base, g_ui_renewal_player_id, g_ui_renewal_wage,
                g_ui_renewal_years);
            if (result > 0) {
                ui_show_notice(base, "Contract Renewed", "The player accepted and extended the contract.", UI_AFTER_ROSTER);
            } else {
                ui_show_notice(base, "Renewal Not Completed", "The player or club terms changed before the renewal could be saved.", UI_AFTER_ROSTER);
            }
        } else if (demand <= max_wage &&
                   g_ui_renewal_wage >= mul_div(demand, 85, 100)) {
            g_ui_renewal_counter_wage = demand;
            g_ui_renewal_stage = 3;
            ui_show_contract_renewal(base);
        } else {
            ui_show_notice(base, "Renewal Declined", "The player rejected the wage offer.", UI_AFTER_ROSTER);
        }
    } else {
        g_ui_renewal_stage = 0;
        ui_show_roster(base);
    }
    return 1;
}

static int32_t ui_callback_finance_history(int32_t selection) {
    int32_t team_id = g_ui_finance_club_cursor >= 0 &&
                      g_ui_finance_club_cursor < MAX_CLUBS
        ? g_market.clubs[g_ui_finance_club_cursor].team_id : -1;
    int32_t count = team_id >= 0 ? career_market_get_finance_count(team_id) : 0;
    if (selection == 0 && g_ui_finance_history_cursor + 1 < count) {
        ++g_ui_finance_history_cursor;
        ui_show_finance_history(g_ui_base);
    } else if (selection == 1 && g_ui_finance_history_cursor > 0) {
        --g_ui_finance_history_cursor;
        ui_show_finance_history(g_ui_base);
    } else {
        ui_show_finances(g_ui_base);
    }
    return 1;
}

static int32_t ui_callback_sales_player(int32_t selection) {
    uint32_t base = g_ui_base;
    if (selection == 0) {
        if (g_market.window_id < 0) {
            ui_show_notice(base, "Player Sales", "Players can be listed or removed while the transfer window is open (start of each season, until 6 league matches are played).", UI_AFTER_SALES);
        } else if (g_ui_player_index < 0 || g_ui_player_index >= g_player_count) {
            ui_show_notice(base, "Player Sales", "No player is available to list in the current view.", UI_AFTER_SALES);
        } else if (user_signing_locked(g_players[g_ui_player_index].player_id)) {
            ui_show_notice(base, "Transfer Protection", "A player you just signed is protected from listing and sale until next season.", UI_AFTER_SALES);
        } else if (toggle_user_listing(g_players[g_ui_player_index].player_id) < 0) {
            ui_show_notice(base, "Player Sales", "This player cannot be changed while an offer is active, or the listing limit has been reached.", UI_AFTER_SALES);
        } else {
            save_profile(base);
            ui_show_sales_player(base, g_ui_player_index);
        }
    } else if (selection == 1) {
        ui_show_quick_sale(base, g_ui_player_index);
    } else {
        ui_show_sales_player(base, g_ui_player_index);
    }
    return 1;
}

/* Player Sales browser: Next Player, Sale Options, (More) Previous Player, Position Filter, Back. */
static int32_t ui_callback_sales_browse(int32_t selection) {
    uint32_t base = g_ui_base;
    if (selection == 0 || selection == 2) {
        int32_t index = ui_find_user_player(g_ui_player_index, selection == 0 ? 1 : -1);
        if (index >= 0) g_ui_player_index = index;
        ui_show_sales_player(base, g_ui_player_index);
    } else if (selection == 1) {
        ui_show_sale_options(base);
    } else if (selection == 3) {
        g_ui_browser_mode = 1;
        ui_show_position_filter(base);
    } else {
        ui_show_club_hub(base);
    }
    return 1;
}

static int32_t ui_callback_sales_empty(int32_t selection) {
    if (selection == 0) {
        g_ui_browser_mode = 1;
        ui_show_position_filter(g_ui_base);
    } else {
        ui_show_club_hub(g_ui_base);
    }
    return 1;
}

static int32_t ui_callback_quick_sale(int32_t selection) {
    static const char *const continue_options[] = {"Continue"};
    uint32_t base = g_ui_base;
    if (selection != 0 || g_ui_quick_sale_player_id < 0) {
        ui_show_sales_player(base, g_ui_player_index);
        return 1;
    }
    int32_t player_id = g_ui_quick_sale_player_id;
    int32_t fee = career_market_quick_sale(base, player_id);
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    if (fee > 0) {
        ui_text_append_ascii(&builder, "Sold ");
        ui_append_player_name(&builder, base, player_id);
        ui_text_append_ascii(&builder, " to ");
        ui_append_team_name(&builder, base, g_neg_rival_team);
        ui_text_append_ascii(&builder, " for ");
        ui_text_append_char(&builder, 0x0180);
        ui_text_append_char(&builder, ' ');
        ui_text_append_i32(&builder, fee);
        ui_text_append_ascii(&builder, " coins. The fee and squad update have been saved.");
        sync_accounts(base);
        build_player_cache(base);
        g_ui_screen_page_offsets[UI_SCREEN_LIST_SALES] = 0;
        g_ui_player_index = ui_find_user_player(-1, 1);
    } else if (fee == -2) {
        ui_text_append_ascii(&builder, "Quick sales are only available during the transfer window.");
    } else if (fee == -3) {
        ui_text_append_ascii(&builder, "This player is unavailable or already has an active offer.");
    } else if (fee == -4) {
        ui_text_append_ascii(&builder, "Selling him would leave too few players in your squad or remove your last goalkeeper.");
    } else if (fee == -5) {
        ui_text_append_ascii(&builder, "No club has the squad space and budget to take this player.");
    } else if (fee == -7) {
        ui_text_append_ascii(&builder, "A player you just signed is protected from sale until next season.");
    } else if (fee == -6) {
        ui_text_append_ascii(&builder, "The transfer could not be completed. The player remains in your squad.");
    } else {
        ui_text_append_ascii(&builder, "The career market is not ready to process this sale.");
    }
    g_ui_quick_sale_player_id = -1;
    g_ui_after_notice = UI_AFTER_SALES;
    ui_show_options(base, "Player Sales", g_ui_description, continue_options, 1,
                    ui_callback_notice);
    return 1;
}

static int32_t ui_callback_shortlist(int32_t selection) {
    uint32_t base = g_ui_base;
    int32_t valid = g_ui_player_index >= 0 && g_ui_player_index < g_player_count &&
                    shortlist_slot(g_players[g_ui_player_index].player_id) >= 0;
    if (!valid) {                    /* empty shortlist: Open Market, Open Player Sales, Back */
        if (selection == 0) {
            g_ui_browser_mode = 0;
            g_ui_player_index = ui_find_market_player(-1, 1);
            ui_show_market_player(base, g_ui_player_index);
        } else if (selection != 1) {
            ui_show_transfers_hub(base);
        } else {
            g_ui_browser_mode = 1;
            g_ui_player_index = ui_find_user_player(-1, 1);
            ui_show_sales_player(base, g_ui_player_index);
        }
        return 1;
    }
    /* v31 page: Make Offer, Next Player, (More) Remove, Previous Player, Back. */
    if (selection == 1 || selection == 3) {
        int32_t index = ui_find_shortlist_player(g_ui_player_index, selection == 1 ? 1 : -1);
        if (index >= 0) g_ui_player_index = index;
        ui_show_shortlist(base);
        return 1;
    }
    if (selection != 0 && selection != 2) {
        ui_show_transfers_hub(base);
        return 1;
    }
    selection = selection == 0 ? 1 : 0;   /* the v30 indices: 0 Remove, 1 Make Offer */
    if (selection == 0) {
        if (g_ui_player_index >= 0 && g_ui_player_index < g_player_count) {
            shortlist_toggle(g_players[g_ui_player_index].player_id);
            save_profile(base);
        }
        g_ui_screen_page_offsets[UI_SCREEN_LIST_SHORTLIST] = 0;
        g_ui_player_index = ui_find_shortlist_player(-1, 1);
        ui_show_shortlist(base);
    } else if (selection == 1) {
        if (g_ui_player_index < 0 || g_ui_player_index >= g_player_count ||
            shortlist_slot(g_players[g_ui_player_index].player_id) < 0) {
            ui_show_shortlist(base);
        } else {
            MarketPlayer *player = &g_players[g_ui_player_index];
            if (g_market.window_id < 0) {
                ui_show_notice(base, "Transfer Window Closed", "Shortlisted players stay saved. You can make an offer when the next transfer window opens.", UI_AFTER_SHORTLIST);
            } else if (player->owner_id == USER_TEAM_ID) {
                ui_show_notice(base, "Player Already Signed", "This player is already in your squad.", UI_AFTER_SHORTLIST);
            } else if (has_active_offer_for_player(player->player_id)) {
                ui_show_notice(base, "Offer Already Active", "Resolve the current offer before starting another negotiation.", UI_AFTER_SHORTLIST);
            } else {
                g_ui_bid_player_id = player->player_id;
                g_ui_bid_fee = player->value > 0 ? player->value : 1;
                g_ui_bid_wage = player->wage > 0 ? player->wage : 1;
                g_ui_bid_contract_years = 3;
                g_ui_offer_origin = UI_OFFER_FROM_BROWSER;
                ui_show_bid_terms(base);
            }
        }
    }
    return 1;
}

static int32_t ui_callback_position_filter(int32_t selection) {
    if (selection >= 0 && selection <= 4) {
        if (g_ui_browser_mode == 1) {
            g_ui_sales_position_filter = selection == 0 ? -1 : selection - 1;
            g_ui_screen_page_offsets[UI_SCREEN_LIST_SALES] = 0;
            g_ui_player_index = ui_find_user_player(-1, 1);
            ui_show_sales_player(g_ui_base, g_ui_player_index);
        } else {
            g_ui_position_filter = selection == 0 ? -1 : selection - 1;
            g_ui_screen_page_offsets[UI_SCREEN_LIST_MARKET] = 0;
            g_ui_player_index = ui_find_market_player(-1, 1);
            ui_show_market_player(g_ui_base, g_ui_player_index);
        }
    } else {
        if (g_ui_browser_mode == 1) ui_show_sales_player(g_ui_base, g_ui_player_index);
        else ui_show_market_player(g_ui_base, g_ui_player_index);
    }
    return 1;
}

static int32_t ui_adjust_bid_terms(int32_t selection) {
    if (selection >= 0 && selection <= 3) {
        int32_t *amount = selection <= 1 ? &g_ui_bid_fee : &g_ui_bid_wage;
        int32_t current = *amount > 0 ? *amount : 1;
        int32_t step = current / 10;
        if (step < 1) step = 1;
        if ((selection & 1) == 0) {
            *amount = current > step ? current - step : 1;
        } else {
            *amount = clamp_add(current, step);
        }
        return 1;
    }
    if (selection == 4 || selection == 5) {
        if (selection == 4 && g_ui_bid_contract_years > 1) {
            --g_ui_bid_contract_years;
        } else if (selection == 5 && g_ui_bid_contract_years < 5) {
            ++g_ui_bid_contract_years;
        }
        return 1;
    }
    return 0;
}

static int32_t ui_callback_bid_terms(int32_t selection) {
    uint32_t base = g_ui_base;
    if (g_ui_bid_stage == UI_BID_CHANGE) {
        g_ui_bid_stage = selection == 0 ? UI_BID_FEE : selection == 1 ? UI_BID_WAGE
                       : selection == 2 ? UI_BID_YEARS : UI_BID_TERMS;
        ui_show_bid_terms(base);
        return 1;
    }
    int32_t v30 = ui_bid_stage_to_v30(g_ui_bid_stage, selection);
    if (v30 < 0) {                         /* "Change Terms" or "Done" */
        g_ui_bid_stage = g_ui_bid_stage == UI_BID_TERMS ? UI_BID_CHANGE : UI_BID_TERMS;
        g_ui_amount_error = UI_AMOUNT_ERROR_NONE;
        ui_show_bid_terms(base);
        return 1;
    }
    selection = v30;
    if (selection < 0 || selection > 9 || g_ui_player_index < 0 ||
        g_ui_player_index >= g_player_count ||
        g_ui_bid_player_id != g_players[g_ui_player_index].player_id) {
        g_ui_bid_player_id = -1;
        ui_show_market_player(base, g_ui_player_index);
        return 1;
    }
    MarketPlayer *player = &g_players[g_ui_player_index];
    if (ui_adjust_bid_terms(selection)) {
        g_ui_amount_error = UI_AMOUNT_ERROR_NONE;
        ui_show_bid_terms(base);
        return 1;
    }
    if (selection == 6 || selection == 7) {
        int32_t target = selection == 6
            ? UI_EXACT_AMOUNT_BID_FEE : UI_EXACT_AMOUNT_BID_WAGE;
        int32_t amount = target == UI_EXACT_AMOUNT_BID_FEE
            ? g_ui_bid_fee : g_ui_bid_wage;
        g_ui_amount_error = UI_AMOUNT_ERROR_NONE;
        ui_open_exact_amount(base, target,
            target == UI_EXACT_AMOUNT_BID_FEE ? "Enter Transfer Fee" : "Enter Annual Wage",
            amount);
        return 1;
    }
    if (selection == 9) {
        g_ui_bid_player_id = -1;
        g_ui_amount_error = UI_AMOUNT_ERROR_NONE;
        ui_show_market_player(base, g_ui_player_index);
        return 1;
    }
    if (selection != 8) {
        ui_show_bid_terms(base);
        return 1;
    }

    int32_t offer_id = career_market_submit_user_bid(base, player->player_id,
                                                      g_ui_bid_fee, g_ui_bid_wage,
                                                      g_ui_bid_contract_years);
    g_ui_bid_player_id = -1;
    save_profile(base);
    if (offer_id == -12) {
        ui_show_notice(base, "Not Enough Coins", "After this fee you must still have enough coins to pay a season of wages, including his.", UI_AFTER_BROWSER);
        return 1;
    }
    if (offer_id == -11 || offer_id == -15 || offer_id == -16 || offer_id == -17) {
        ui_show_refusal(base, player, offer_id, UI_AFTER_BROWSER);
        return 1;
    }
    if (offer_id < 0) {
        ui_show_notice(base, "Offer Not Submitted", "The player, seller, window, or your budgets no longer allow this offer.", UI_AFTER_BROWSER);
        return 1;
    }
    g_ui_offer_origin = UI_OFFER_FROM_BROWSER;
    g_ui_offer_id = offer_id;
    MarketOffer *offer = ui_find_offer_any(offer_id);
    if (offer && offer_is_active(offer)) {
        ui_show_offer(base, offer);
    } else if (offer && offer->status == CM_OFFER_COMPLETED) {
        ui_show_offer_result(base, offer, UI_AFTER_BROWSER);
    } else {
        ui_show_notice(base, "Offer Declined", "The seller or player rejected those terms, or your budget no longer covers them.", UI_AFTER_BROWSER);
    }
    return 1;
}

static int32_t ui_callback_exact_amount(int32_t selection) {
    uint32_t base = g_ui_base;
    int32_t target = g_ui_exact_amount_target;
    void *box = g_ui_keyboard_box;
    const uint16_t *text = (const uint16_t *)0;
    int32_t amount = 0;
    int32_t error = UI_AMOUNT_ERROR_NONE;
    g_ui_exact_amount_target = UI_EXACT_AMOUNT_NONE;
    g_ui_keyboard_box = (void *)0;

    if (target == UI_EXACT_AMOUNT_NONE) {
        ui_show_bid_terms(base);
        return 1;
    }
    /* CFEMsgKeyboard uses flags 3 (Cancel, OK): UI_COMPONENTS.md 0.2 -> button order is Cancel = index 0,
     * OK = index 1, so selection 0 is Cancel and selection != 0 (1) is OK. */
    if (selection != 0 && box) {
        void *field = *(void **)((char *)box + 0xCE4);
        if (field) {
            TextFieldGetTextFn get_text = (TextFieldGetTextFn)(base + 0x241FB5);
            text = get_text(field);
        }
    }

    if (selection == 0) {
        if (target == UI_EXACT_AMOUNT_RENEWAL_WAGE) {
            g_ui_renewal_stage = 0;
            ui_show_contract_renewal(base);
        } else {
            ui_show_bid_terms(base);
        }
        return 1;
    }
    if (!ui_parse_positive_amount(text, &amount)) {
        error = UI_AMOUNT_ERROR_INVALID;
    } else if (target == UI_EXACT_AMOUNT_BID_FEE ||
               target == UI_EXACT_AMOUNT_BID_WAGE) {
        if (g_ui_player_index < 0 || g_ui_player_index >= g_player_count ||
            g_ui_bid_player_id != g_players[g_ui_player_index].player_id) {
            error = UI_AMOUNT_ERROR_INVALID;
        } else {
            int32_t buyer_index = find_account(USER_TEAM_ID);
            if (buyer_index < 0) {
                error = UI_AMOUNT_ERROR_INVALID;
            } else if (target == UI_EXACT_AMOUNT_BID_FEE &&
                       amount > account_fee_room(&g_market.clubs[buyer_index])) {
                error = UI_AMOUNT_ERROR_FEE_BUDGET;
            } else if (target == UI_EXACT_AMOUNT_BID_WAGE &&
                       amount > account_wage_room(&g_market.clubs[buyer_index])) {
                error = UI_AMOUNT_ERROR_WAGE_BUDGET;
            }
        }
    } else if (target == UI_EXACT_AMOUNT_RENEWAL_WAGE) {
        int32_t player_index = find_cached_player(g_ui_renewal_player_id);
        int32_t account_index = find_account(USER_TEAM_ID);
        if (player_index < 0 || account_index < 0 ||
            g_players[player_index].owner_id != USER_TEAM_ID ||
            !market_contract_available(g_ui_renewal_player_id, USER_TEAM_ID)) {
            error = UI_AMOUNT_ERROR_INVALID;
        } else {
            int32_t max_wage = clamp_add(g_players[player_index].wage,
                account_wage_room(&g_market.clubs[account_index]));
            if (amount > max_wage) error = UI_AMOUNT_ERROR_WAGE_BUDGET;
        }
    } else {
        error = UI_AMOUNT_ERROR_INVALID;
    }

    if (target == UI_EXACT_AMOUNT_BID_FEE ||
        target == UI_EXACT_AMOUNT_BID_WAGE) {
        if (error == UI_AMOUNT_ERROR_NONE) {
            if (target == UI_EXACT_AMOUNT_BID_FEE) g_ui_bid_fee = amount;
            else g_ui_bid_wage = amount;
        }
        g_ui_amount_error = error;
        ui_show_bid_terms(base);
    } else {
        if (error == UI_AMOUNT_ERROR_NONE) {
            g_ui_renewal_wage = amount;
            g_ui_renewal_stage = 0;
        } else {
            g_ui_renewal_stage = 1;
        }
        g_ui_amount_error = error;
        ui_show_contract_renewal(base);
    }
    return 1;
}

static int32_t ui_callback_search(int32_t selection) {
    uint32_t base = g_ui_base;
    void *box = g_ui_keyboard_box;
    g_ui_keyboard_box = (void *)0;
    /* CFEMsgKeyboard flags 3: selection 0 = Cancel, selection != 0 (1) = OK. See ui_callback_exact_amount. */
    if (selection == 0 || !box) {
        ui_show_club_filter(base);
        return 1;
    }

    void *field = *(void **)((char *)box + 0xCE4);
    const uint16_t *text = (const uint16_t *)0;
    if (field) {
        TextFieldGetTextFn get_text = (TextFieldGetTextFn)(base + 0x241FB5);
        text = get_text(field);
    }
    if (!text) {
        ui_show_club_filter(base);
        return 1;
    }

    int32_t length = 0;
    for (int32_t i = 0; i < 48; ++i) g_ui_search_query[i] = 0;
    while (text[length] && length < 31) {
        g_ui_search_query[length] = text[length];
        ++length;
    }
    int32_t first = 0;
    while (first < length && g_ui_search_query[first] == ' ') ++first;
    while (length > first && g_ui_search_query[length - 1] == ' ') --length;
    if (first > 0) {
        for (int32_t i = first; i < length; ++i) {
            g_ui_search_query[i - first] = g_ui_search_query[i];
        }
        length -= first;
    }
    g_ui_search_query[length] = 0;
    g_ui_search_results_valid = 0;
    g_ui_screen_page_offsets[UI_SCREEN_LIST_MARKET] = 0;
    ui_rebuild_search_results(base);
    g_ui_player_index = ui_find_market_player(-1, 1);
    if (g_ui_player_index < 0 && g_ui_search_query[0]) {
        ui_show_notice(base, "Player Search", "No eligible market players matched that name. Change or clear the search.", UI_AFTER_CLUB_FILTER);
    } else {
        ui_show_market_player(base, g_ui_player_index);
    }
    return 1;
}

static void ui_show_offer_result(uint32_t base, MarketOffer *offer, int32_t after_notice) {
    static const char *const options[] = {"Continue"};
    MarketUiText builder;
    const char *title = "Negotiation Closed";
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    switch (g_neg_event) {
        case NEG_TALKS_ENDED:
            ui_text_append_ascii(&builder, "Talks ended after the seller or player rejected the terms. No transfer was made.");
            break;
        case NEG_RIVAL_SIGNED:
            ui_text_append_ascii(&builder, "You walked away. ");
            ui_append_team_name(&builder, base, g_neg_rival_team);
            ui_text_append_ascii(&builder, " signed ");
            if (offer) ui_append_player_name(&builder, base, offer->player_id);
            ui_text_append_ascii(&builder, " for ");
            ui_text_append_char(&builder, 0x0180);
            ui_text_append_char(&builder, ' ');
            ui_text_append_i32(&builder, g_neg_rival_fee);
            ui_text_append_ascii(&builder, " coins.");
            break;
        case NEG_BUYER_WALKED:
            ui_text_append_ascii(&builder, "The buyer ");
            if (offer) ui_append_team_name(&builder, base, offer->buyer_id);
            ui_text_append_ascii(&builder, " walked away. No transfer was made.");
            break;
        case NEG_SIGNED:
            title = "Transfer Deal";
            ui_text_append_ascii(&builder, "Signed ");
            if (offer) ui_append_player_name(&builder, base, offer->player_id);
            ui_text_append_ascii(&builder, ". The transfer and club finances were saved.");
            break;
        case NEG_SOLD:
            title = "Player Sold";
            ui_text_append_ascii(&builder, "Sold ");
            if (offer) ui_append_player_name(&builder, base, offer->player_id);
            ui_text_append_ascii(&builder, ". The transfer and club finances were saved.");
            break;
        default:
            ui_text_append_ascii(&builder, "The offer was rejected, expired, or could not be completed. No transfer was made.");
            break;
    }
    g_ui_after_notice = after_notice;
    ui_show_options(base, title, g_ui_description, options, 1, ui_callback_notice);
}

static int32_t ui_callback_offer(int32_t selection) {
    uint32_t base = g_ui_base;
    MarketOffer *offer = ui_find_offer_any(g_ui_offer_id);
    if (!offer || !offer_is_active(offer)) {
        if (g_ui_offer_origin == UI_OFFER_FROM_INBOX) ui_show_inbox(base);
        else ui_show_market_player(base, g_ui_player_index);
        return 1;
    }
    /* v31 buttons -> v30 indices. "Back" (and any unknown button) leaves the offer untouched:
     * the v30 code treated an unlisted index as a rejection. */
    int32_t back_index = g_ui_offer_layout == 3 ? 2 : 4;
    if (selection < 0 || selection >= back_index) {
        g_ui_amount_error = UI_AMOUNT_ERROR_NONE;
        if (g_ui_from_stock) g_ui_from_stock = 0;          /* started on the game's screen: close */
        else if (g_ui_offer_origin == UI_OFFER_FROM_INBOX) ui_show_club_hub(base);
        else ui_show_market_player(base, g_ui_player_index);
        return 1;
    }
    if (g_ui_offer_layout != 3) {
        static const int32_t v30_order[] = {0, 3, 1, 2};
        selection = v30_order[selection];
    }

    int32_t decision = CM_DECISION_REJECT;
    int32_t amount = 0;
    if (offer->status == CM_OFFER_WAIT_USER_FEE) {
        if (selection == 0) decision = CM_DECISION_ACCEPT;
        else if (selection == 1) {
            decision = CM_DECISION_COUNTER;
            amount = offer->fee + (offer->counter_fee - offer->fee) / 2;
        } else if (selection == 2) {
            ui_open_offer_counter_amount(base, offer);
            return 1;
        }
    } else if (offer->status == CM_OFFER_WAIT_USER_WAGE) {
        if (selection == 0) decision = CM_DECISION_ACCEPT;
        else if (offer->rounds == 0 && selection == 1) {
            decision = CM_DECISION_COUNTER;
            amount = offer->annual_wage +
                     (offer->counter_wage - offer->annual_wage) / 2;
        } else if (offer->rounds == 0 && selection == 2) {
            ui_open_offer_counter_amount(base, offer);
            return 1;
        }
    } else if (offer->seller_id == USER_TEAM_ID) {
        if (selection == 0) decision = CM_DECISION_ACCEPT;
        else if (selection == 1) {
            decision = CM_DECISION_COUNTER;
            int32_t current_fee = offer->status == CM_OFFER_WAIT_USER_SELLER_COUNTER
                ? offer->counter_fee : offer->fee;
            amount = clamp_add(current_fee, current_fee / 10 + 1);
        } else if (selection == 2) {
            ui_open_offer_counter_amount(base, offer);
            return 1;
        }
    }
    int32_t result = career_market_respond_to_offer(base, offer->offer_id,
                                                    decision, amount);
    (void)result;
    save_profile(base);
    if (offer_is_active(offer)) {
        ui_show_offer(base, offer);
    } else if (g_ui_offer_origin == UI_OFFER_FROM_INBOX) {
        ui_show_offer_result(base, offer, UI_AFTER_INBOX);
    } else {
        ui_show_offer_result(base, offer, UI_AFTER_BROWSER);
    }
    return 1;
}

static int32_t ui_callback_offer_counter_amount(int32_t selection) {
    uint32_t base = g_ui_base;
    MarketOffer *offer = ui_find_offer_any(g_ui_offer_id);
    void *box = g_ui_keyboard_box;
    const uint16_t *text = (const uint16_t *)0;
    int32_t amount = 0;
    int32_t error = UI_AMOUNT_ERROR_NONE;
    g_ui_keyboard_box = (void *)0;

    if (!offer || !offer_is_active(offer)) {
        if (g_ui_offer_origin == UI_OFFER_FROM_INBOX) ui_show_inbox(base);
        else ui_show_market_player(base, g_ui_player_index);
        return 1;
    }
    /* CFEMsgKeyboard flags 3: selection 0 = Cancel, selection != 0 (1) = OK. See ui_callback_exact_amount. */
    if (selection == 0) {
        ui_show_offer(base, offer);
        return 1;
    }

    if (box) {
        void *field = *(void **)((char *)box + 0xCE4);
        if (field) {
            TextFieldGetTextFn get_text = (TextFieldGetTextFn)(base + 0x241FB5);
            text = get_text(field);
        }
    }
    if (!ui_parse_positive_amount(text, &amount)) {
        error = UI_AMOUNT_ERROR_INVALID;
    } else if (offer->status == CM_OFFER_WAIT_USER_FEE) {
        sync_accounts(base);
        int32_t buyer_index = find_account(USER_TEAM_ID);
        if (buyer_index < 0) {
            error = UI_AMOUNT_ERROR_INVALID;
        } else if (amount > account_fee_room(&g_market.clubs[buyer_index])) {
            error = UI_AMOUNT_ERROR_FEE_BUDGET;
        }
    } else if (offer->status == CM_OFFER_WAIT_USER_WAGE) {
        sync_accounts(base);
        int32_t buyer_index = find_account(USER_TEAM_ID);
        if (buyer_index < 0) {
            error = UI_AMOUNT_ERROR_INVALID;
        } else if (amount > account_wage_room(&g_market.clubs[buyer_index])) {
            error = UI_AMOUNT_ERROR_WAGE_BUDGET;
        }
    }

    if (error != UI_AMOUNT_ERROR_NONE) {
        g_ui_amount_error = error;
        ui_show_offer(base, offer);
        return 1;
    }

    int32_t result = career_market_respond_to_offer(base, offer->offer_id,
                                                    CM_DECISION_COUNTER, amount);
    (void)result;
    save_profile(base);
    if (offer_is_active(offer)) {
        ui_show_offer(base, offer);
    } else if (g_ui_offer_origin == UI_OFFER_FROM_INBOX) {
        ui_show_offer_result(base, offer, UI_AFTER_INBOX);
    } else {
        ui_show_offer_result(base, offer, UI_AFTER_BROWSER);
    }
    return 1;
}

static int32_t ui_callback_history(int32_t selection) {
    int32_t count = career_market_get_history_count();
    if (selection == 0 && g_ui_history_index > 0) {
        ui_show_history(g_ui_base, g_ui_history_index - 1);
    } else if (selection == 1 && g_ui_history_index + 1 < count) {
        ui_show_history(g_ui_base, g_ui_history_index + 1);
    } else if (selection == 2 || selection < 0 || selection > 1) {
        ui_show_club_hub(g_ui_base);
    } else {
        ui_show_history(g_ui_base, g_ui_history_index);
    }
    return 1;
}


/*
 * The game's own transfer screens (hooked in build_mod.py) route here instead of the stock instant
 * buy/sell dialogs: the stock purchase charged a fixed coin price at any time, and the stock sale
 * paid 50% of value and sent the player to a random club (then back to his default club).
 *   team >= 0 : the user pressed buy on a player of that team (search screen or scouting results)
 *   team == -1: the user pressed sell on one of his players (squad screen)
 */
static int32_t ui_callback_stock_sale(int32_t selection);

/* Best bid an AI club would make right now for one of the user's players (-1 if nobody wants him). */
static int32_t best_ai_bid_for_user_player(int32_t player_index, int32_t *out_fee, int32_t *out_wage) {
    MarketPlayer *player = &g_players[player_index];
    int32_t user_index = player->owner_index;
    if (user_index < 0 || g_total_count[user_index] <= USER_SQUAD_MIN) return -1;
    if (player->position == 0 && g_group_count[user_index][0] <= 1) return -1;
    if (user_signing_locked(player->player_id)) return -1;
    int32_t best = -1, best_fee = 0, best_wage = 0;
    for (int32_t b = 0; b < MAX_CLUBS; ++b) {
        ClubAccount *buyer = &g_market.clubs[b];
        if (buyer->team_id < 0 || buyer->team_id == USER_TEAM_ID) continue;
        if (g_total_count[b] >= SQUAD_MAX || window_buys(buyer, g_market.window_id) >= ai_buy_limit(b)) continue;
        int32_t position = player->position;
        if (position < 0 || position > 3 || g_group_count[b][position] >= k_pos_max[position]) continue;
        int32_t need = position_need(b, position);
        int32_t improvement = starter_improvement(player, b);
        int32_t useful = improvement >= 2 || (need > 0 && player->rating + 10 >= buyer->strength);
        if (!useful || !player_will_join(player, b)) continue;
        int32_t fee_room = account_fee_room(buyer);
        int32_t fee = buyer_max_price(player, b);
        int32_t cap = mul_div(user_sale_value(player), USER_SALE_CEILING_PCT, 100);
        if (fee > cap) fee = cap;
        if (need == 0 && improvement < 2) fee = mul_div(fee, 70, 100);
        if (fee > fee_room) fee = fee_room;
        if (fee < mul_div(player->value, 50, 100)) continue;       /* no insulting bids */
        int32_t wage = transfer_wage_demand(player, b, user_index);
        if (wage > account_wage_room(buyer)) continue;
        if (fee > best_fee) {
            best = b;
            best_fee = fee;
            best_wage = wage;
        }
    }
    if (best >= 0) {
        *out_fee = best_fee;
        *out_wage = best_wage;
    }
    return best;
}

static void ui_show_stock_sale(uint32_t base) {
    static const char *const options[] = {"Get offers now", "List / Unlist", "Close"};
    if (g_ui_player_index < 0 || g_ui_player_index >= g_player_count) return;
    MarketPlayer *player = &g_players[g_ui_player_index];
    MarketUiText builder;
    ui_text_reset(&builder, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&builder, "Player: ");
    ui_append_player_name(&builder, base, player->player_id);
    ui_text_append_ascii(&builder, "\nRating: ");
    ui_text_append_i32(&builder, player->rating);
    ui_text_append_ascii(&builder, "  Value: ");
    ui_text_append_i32(&builder, player->value);
    ui_text_append_ascii(&builder, " coins\nSale status: ");
    ui_text_append_ascii(&builder, player->listed_for_sale ? "Listed" : "Not listed");
    ui_text_append_ascii(&builder, "\nGet offers now asks every club for its best bid today. Listed players also receive bids as matches are played while the window is open.");
    ui_show_options(base, "Sell Player", g_ui_description, options, 3, ui_callback_stock_sale);
}

static int32_t ui_callback_stock_sale(int32_t selection) {
    uint32_t base = g_ui_base;
    if (g_ui_player_index < 0 || g_ui_player_index >= g_player_count) return 1;
    MarketPlayer *player = &g_players[g_ui_player_index];
    if (selection == 0) {
        if (has_active_offer_for_player(player->player_id)) {
            ui_show_notice(base, "Sell Player", "An offer for this player is already waiting in your market inbox.", UI_AFTER_CLOSE);
            return 1;
        }
        int32_t fee = 0, wage = 0;
        int32_t buyer = best_ai_bid_for_user_player(g_ui_player_index, &fee, &wage);
        MarketOffer *offer = buyer >= 0
            ? create_ai_offer_for_user(buyer, g_ui_player_index, g_market.window_id,
                                       g_market.current_turn, g_market.season, fee, wage)
            : (MarketOffer *)0;
        if (!offer) {
            ui_show_notice(base, "No Offers", "No club wants to buy him right now. List him and bids may still arrive while the window is open.", UI_AFTER_CLOSE);
            return 1;
        }
        g_market_dirty = 1;
        save_profile(base);
        g_ui_offer_origin = UI_OFFER_FROM_INBOX;
        g_ui_offer_id = offer->offer_id;
        ui_show_offer(base, offer);
    } else if (selection == 1) {
        if (toggle_user_listing(player->player_id) < 0) {
            ui_show_notice(base, "Sell Player", "This player cannot be changed while an offer is active, or the listing limit has been reached.", UI_AFTER_CLOSE);
        } else {
            save_profile(base);
            ui_show_stock_sale(base);
        }
    } else {
        g_ui_from_stock = 0;
    }
    return 1;
}

__attribute__((visibility("default")))
void career_market_stock_action(void *info, int32_t team, uint32_t base) {
    if (!info || !base) return;
    pin_library(base);
    capture_pristine_links(base);
    repair_hacked_coins(base);
    apply_price_schedule(base);
    mod_boot(base, 0);
    g_ui_base = base;
    void *season = current_season(base);
    if (season) career_market_tick(season, base);
    if (!state_is_valid()) return;
    sync_accounts(base);
    build_player_cache(base);
    g_ui_from_stock = 1;
    if (g_market.window_id < 0) {
        ui_show_notice(base, "Transfer Window Closed",
                       "Transfers are only possible at the start of each season, until 6 league matches are played.",
                       UI_AFTER_CLOSE);
        return;
    }
    int32_t player_id = *(const uint16_t *)info;
    int32_t index = find_cached_player(player_id);
    int32_t user_index = find_account(USER_TEAM_ID);
    if (team >= 0) {
        if (index < 0 || user_index < 0 || g_players[index].owner_id == USER_TEAM_ID) {
            ui_show_notice(base, "Not Available", "This player is not registered with a club in the transfer market.", UI_AFTER_CLOSE);
            return;
        }
        MarketPlayer *player = &g_players[index];
        /* say no before the terms screen, not after the user has set them up */
        if (!player_will_join(player, user_index)) {
            ui_show_refusal(base, player, -11, UI_AFTER_CLOSE);
            return;
        }
        if (!starter_move_allowed(player, user_index)) {
            ui_show_refusal(base, player, -15, UI_AFTER_CLOSE);
            return;
        }
        g_ui_player_index = index;
        g_ui_bid_player_id = player_id;
        g_ui_bid_fee = user_bid_asking(player, user_index);
        g_ui_bid_wage = transfer_wage_demand(player, user_index, player->owner_index);
        g_ui_bid_contract_years = 3;
#if MARKET_TRANSFER_SCREEN_UI
        if (!g_ts_full_terms) {
            ts_buy_dialog(base);                /* v35b: the stock Sign Player dialog with the market price */
            return;
        }
        g_ts_full_terms = 0;
#endif
        ui_show_bid_terms(base);
        return;
    }
    if (index < 0 || g_players[index].owner_id != USER_TEAM_ID) {
        ui_show_notice(base, "Not Available", "Created players cannot be sold through the transfer market.", UI_AFTER_CLOSE);
        return;
    }
    g_ui_player_index = index;
#if MARKET_TRANSFER_SCREEN_UI
    if (!g_ts_full_terms) {
        ts_sell_dialog(base);                   /* v35b: the stock Sell Player dialog with the best bid */
        return;
    }
    g_ts_full_terms = 0;
#endif
    ui_show_stock_sale(base);
}

/* ---------------------------------------------------------------------------------------------
 * Transfer screen UI (v35). The stock transfer screen (CFESDreamLeagueTransfers) is the market's
 * browser: its player-card grid shows the market's lists, every buy card shows the market's asking
 * price, and the market's actions are buttons in the screen's free space. Browsing opens no box;
 * only Make Offer (the negotiation) and Club (records) open market boxes.
 *
 * Layout, in FE units (the screen's GetW/GetH, about 1024 x 640 on a 16:10 tablet):
 *  - top band, right of the card scroller (this+0x100) and above the filter tabs: one text line
 *    (window / squad status, or the selected player's terms) and [Make Offer] [Shortlist] [Sort].
 *    These are children of the screen, so the screen deletes them with itself.
 *  - footer gap between Free Scout (footer button 0x2a) and Sell Player (9): list tabs
 *    [Search] [Shortlist n] [Offers n] [For You] [Club]. The footer is its own render layer drawn
 *    after the screen (CFEEntityManager::RenderAll 0x2609e4), and its background would cover screen
 *    children there, so these are children of the footer menu. The footer outlives the screen:
 *    they are created once and moved off-screen with input disabled whenever the current screen is
 *    not the transfer screen (footer RenderPost hook). They are plain children, not CFEMenu options,
 *    so the footer's GetSelectedOption (+0xfc) never reports them.
 * A child's rect is its parent's origin plus its pixel rect (CFEEntity::CalculateRect 0x25edea with
 * zero anchors); GetX/GetY/GetW/GetH read the resulting absolute rect at +0x48.
 *
 * Hooks (modcore, build_mod.py RUNTIME_HOOKS):
 *  ts_init        CFESDreamLeagueTransfers::Init 0x2765bc           screen rebuilt: forget its widgets
 *  ts_process     CFESDreamLeagueTransfers::Process 0x276954        attach widgets, poll their taps
 *  ts_render_post CFESDreamLeagueTransfers::RenderPost 0x2770fc     status / selection line
 *  ts_setup       CFESDreamLeagueTransfers::SetupResults 0x276bb4   market lists and sorting
 *  ts_bid         CFESDreamLeagueTransfers::CurrentPlayerBid 0x276fb8  card tap = select
 *  ts_card_value  CFEPlayerCard::GetPlayerValue 0x2343ac            market asking price on buy cards
 *  ts_footer_post CFEFooterMenu::RenderPost 0x246656                hide the footer tabs elsewhere
 * SetupResults reads the result count through ms_pPlayerSearchInfo (= &CTransfers::
 * ms_tAsyncPlayerSearchInfo, set in Init 0x276606) and the entries (TPlayerSearchInfo, 0xb0 bytes:
 * +0 player id, +4 team id, +0xac available -> buy card with a price line) from that struct's +0
 * array. For a market list the hook points +0 / +0x2c at the market's own array only while the stock
 * body runs (called through the hook's resume block) and restores them before returning, so the
 * async search job never sees market memory.
 * ------------------------------------------------------------------------------------------- */
#define TS_ASYNC_SEARCH 0x75CDE4u          /* CTransfers::ms_tAsyncPlayerSearchInfo */
#define TS_SETUP_RESULTS_FLAG 0x763BA8u    /* CFESDreamLeagueTransfers::ms_bSetupResults */
#define TS_SCROLLER 0x100                  /* CFEScrollerX* (the card grid's scroller) */
#define TS_ENTRY 0xB0
#define TS_ROWS_MAX 96
#define TS_SORT_MAX 512
#define TS_FOOT_SCOUT 0x2A
#define TS_FOOT_SELL 9
#define TS_LABEL 32

enum { TS_LIST_SEARCH = 0, TS_LIST_SHORTLIST = 1, TS_LIST_OFFERS = 2, TS_LIST_FORYOU = 3, TS_LIST_COUNT = 4 };
#define TS_FOOT_TABS 4                     /* row 1: [Shortlist] [Offers] [For You] [Club]; search is the stock filter */
#define TS_FOOT_ACTIONS 3                  /* row 2: [+ / - <player>] [Negotiate] [Sort] */
#define TS_FOOT_BUTTONS (TS_FOOT_TABS + TS_FOOT_ACTIONS)
enum { TS_SORT_DEFAULT = 0, TS_SORT_RATING = 1, TS_SORT_PRICE = 2, TS_SORT_WAGE = 3, TS_SORT_COUNT = 4 };
enum { TS_ACT_LIST = 1, TS_ACT_CLUB = 2, TS_ACT_OFFER = 3, TS_ACT_SHORTLIST = 4, TS_ACT_SORT = 5 };

typedef struct {
    void *button;
    int32_t action;
    int32_t value;
    float rect[4];                         /* pixel rect relative to the parent */
    uint16_t label[TS_LABEL];
} TsButton;

typedef void *(*TsGetEntityFn)(void);
typedef void *(*TsFooterButtonFn)(void *, int32_t);
typedef void (*TsEnableInputFn)(void *, int32_t);
typedef void (*TsUpdateTextFn)(void *, const uint16_t *);
typedef void (*TsSetSchemeFn)(void *, int32_t);
typedef void (*TsScreenFn)(void *);

static void *g_ts_screen;                  /* transfer screen that owns g_ts_top */
static TsButton g_ts_top[3];
static int32_t g_ts_top_count;
static void *g_ts_footer;                  /* footer menu that owns g_ts_foot */
static TsButton g_ts_foot[TS_FOOT_BUTTONS];
static int32_t g_ts_foot_count;
static int32_t g_ts_foot_shown;
static int32_t g_ts_list;
static int32_t g_ts_sort;
static int32_t g_ts_sel_id = -1;
static int32_t g_ts_sel_team = -1;
static uint8_t g_ts_rows[TS_ROWS_MAX * TS_ENTRY] __attribute__((aligned(8)));
static int32_t g_ts_row_count;
static uint8_t g_ts_sort_buffer[TS_SORT_MAX * TS_ENTRY] __attribute__((aligned(8)));
static int32_t g_ts_sort_keys[TS_SORT_MAX];
static int32_t g_ts_sort_order[TS_SORT_MAX];
static int32_t g_ts_sort_tmp[TS_SORT_MAX];
static uint32_t g_ts_frame;
static int32_t g_ts_last_shortlist = -1;
static int32_t g_ts_last_offers = -1;
static int32_t g_ts_last_sel = -2;
static int32_t g_ts_last_sort = -1;
static struct { int32_t id; uint32_t epoch; int32_t price; } g_ts_price_cache[256];

static const char *const k_ts_list_names[TS_LIST_COUNT] = {"Search", "Shortlist", "Offers", "For You"};
static const char *const k_ts_sort_names[TS_SORT_COUNT] = {"Sort: Default", "Sort: Rating", "Sort: Price",
                                                           "Sort: Wage"};

static int32_t ts_enabled(void) {
    return MARKET_TRANSFER_SCREEN_UI && state_is_valid() && g_player_count > 0;
}

static int32_t ts_player_index(int32_t player_id) {
    if (player_id < 0 || player_id > 0xFFFF) return -1;
    int32_t index = (int32_t)g_player_slot_by_id[player_id] - 1;
    if (index >= 0 && index < g_player_count && g_players[index].player_id == player_id) return index;
    return find_cached_player(player_id);
}

/* Asking price the user pays, cached per player until the market changes (g_price_epoch). */
static int32_t ts_asking_price(int32_t player_id) {
    int32_t slot = player_id & 0xFF;
    if (g_ts_price_cache[slot].id == player_id && g_ts_price_cache[slot].epoch == g_price_epoch)
        return g_ts_price_cache[slot].price;
    int32_t index = ts_player_index(player_id);
    int32_t user_index = find_account(USER_TEAM_ID);
    int32_t price = -1;
    if (index >= 0 && user_index >= 0 && g_players[index].owner_id != USER_TEAM_ID &&
        g_players[index].owner_index >= 0)
        price = user_bid_asking(&g_players[index], user_index);
    g_ts_price_cache[slot].id = player_id;
    g_ts_price_cache[slot].epoch = g_price_epoch;
    g_ts_price_cache[slot].price = price;
    return price;
}

static MarketOffer *ts_user_offer_for(int32_t player_id) {
    if (player_id < 0) return (MarketOffer *)0;
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        MarketOffer *offer = &g_market.offers[i];
        if (offer_is_active(offer) && offer->player_id == player_id &&
            (offer->seller_id == USER_TEAM_ID || offer->buyer_id == USER_TEAM_ID)) return offer;
    }
    return (MarketOffer *)0;
}

static void ts_label(uint16_t *out, const char *text, int32_t count) {
    MarketUiText builder;
    ui_text_reset(&builder, out, TS_LABEL);
    ui_text_append_ascii(&builder, text);
    if (count > 0) {
        ui_text_append_ascii(&builder, " (");
        ui_text_append_i32(&builder, count);
        ui_text_append_char(&builder, ')');
    }
}

static void ts_rect_of(void *entity, float *out) {
    const float *rect = (const float *)((const uint8_t *)entity + 0x48);
    for (int32_t i = 0; i < 4; ++i) out[i] = rect[i];
}

static int32_t ts_released(void *button) {
    if (!button) return 0;
    uint32_t *vtable = *(uint32_t **)button;
    CfeIsReleasedMarketFn is_released = (CfeIsReleasedMarketFn)(uintptr_t)vtable[0x98 / 4];
    return is_released(button);
}

static void ts_set_text(uint32_t base, TsButton *button, const uint16_t *text) {
    int32_t same = 1;
    for (int32_t i = 0; i < TS_LABEL; ++i) {
        if (button->label[i] != text[i]) { same = 0; break; }
        if (!text[i]) break;
    }
    if (same) return;
    ui_wide_copy(button->label, text, TS_LABEL);
    if (button->button) ((TsUpdateTextFn)(base + 0x2260A7))(button->button, button->label);
}

static void *ts_new_button(uint32_t base, void *parent, TsButton *spec, int32_t scheme) {
    GameNewFn game_new = (GameNewFn)(base + 0x1C06AC);
    CfeTextButtonCtorMarketFn constructor = (CfeTextButtonCtorMarketFn)(base + 0x225F99);
    CfeSetPixelRectMarketFn set_rect = (CfeSetPixelRectMarketFn)(base + 0x25F3F5);
    void *button = game_new(0x11C, 0, 0);
    if (!button) return (void *)0;
    constructor(button, spec->label, 0, 0, scheme);
    set_rect(button, spec->rect[0], spec->rect[1], spec->rect[2], spec->rect[3]);
    uint32_t *vtable = *(uint32_t **)parent;
    CfeAddChildMarketFn add_child = (CfeAddChildMarketFn)(uintptr_t)vtable[0x6C / 4];
    add_child(parent, button, 0.0f, 0.0f, 0.0f, 0.0f, 0);
    return button;
}

/* Footer gap between Free Scout and Sell Player, absolute FE units. Fallback: the gap measured on the
 * 16:10 tablet (v30 screenshot) as a fraction of the screen. */
static void ts_footer_gap(uint32_t base, void *footer, float width, float height, float *out) {
    out[0] = width * 0.2225f;
    out[1] = height * 0.892f;
    out[2] = width * 0.5575f;
    out[3] = height * 0.096f;
    TsFooterButtonFn get_button = (TsFooterButtonFn)(base + 0x246211);
    void *scout = get_button(footer, TS_FOOT_SCOUT);
    void *sell = get_button(footer, TS_FOOT_SELL);
    if (!scout || !sell) return;
    float a[4], b[4];
    ts_rect_of(scout, a);
    ts_rect_of(sell, b);
    float gap = width * 0.010f;
    float left = a[0] + a[2] + gap, right = b[0] - gap;
    if (a[2] < 20.0f || b[2] < 20.0f || a[3] < 16.0f || right - left < width * 0.25f ||
        a[1] < height * 0.5f || a[1] + a[3] > height * 1.05f) return;
    out[0] = left;
    out[1] = a[1];
    out[2] = right - left;
    out[3] = a[3];
}

static void ts_footer_show(uint32_t base, int32_t show) {
    CfeSetPixelRectMarketFn set_rect = (CfeSetPixelRectMarketFn)(base + 0x25F3F5);
    TsEnableInputFn enable_input = (TsEnableInputFn)(base + 0x25FD09);
    for (int32_t i = 0; i < g_ts_foot_count; ++i) {
        TsButton *b = &g_ts_foot[i];
        if (!b->button) continue;
        if (show) set_rect(b->button, b->rect[0], b->rect[1], b->rect[2], b->rect[3]);
        else set_rect(b->button, -4000.0f, -4000.0f, b->rect[2], b->rect[3]);
        enable_input(b->button, show);
    }
    g_ts_foot_shown = show;
}

static void ts_attach_footer(uint32_t base, float width, float height) {
    void *footer = ((TsGetEntityFn)(base + 0x2609D5))();          /* CFEEntityManager::GetFooterMenu */
    if (!footer) return;
    if (footer != g_ts_footer) {           /* a new footer object: the old one deleted its children */
        g_ts_footer = footer;
        g_ts_foot_count = 0;
    }
    float gap_rect[4], origin[4];
    ts_footer_gap(base, footer, width, height, gap_rect);
    ts_rect_of(footer, origin);
    float gap = width * 0.008f;
    float row_gap = height * 0.006f;
    float row_h = (gap_rect[3] - row_gap) * 0.5f;
    static const int32_t actions[TS_FOOT_ACTIONS] = {TS_ACT_SHORTLIST, TS_ACT_OFFER, TS_ACT_SORT};
    for (int32_t i = 0; i < TS_FOOT_BUTTONS; ++i) {
        TsButton *b = &g_ts_foot[i];
        int32_t row = i < TS_FOOT_TABS ? 0 : 1;
        int32_t col = row ? i - TS_FOOT_TABS : i;
        int32_t cols = row ? TS_FOOT_ACTIONS : TS_FOOT_TABS;
        float w = (gap_rect[2] - gap * (float)(cols - 1)) / (float)cols;
        b->rect[0] = gap_rect[0] + (float)col * (w + gap) - origin[0];
        b->rect[1] = gap_rect[1] + (float)row * (row_h + row_gap) - origin[1];
        b->rect[2] = w;
        b->rect[3] = row_h;
        if (row == 0) {
            b->action = i + 1 < TS_LIST_COUNT ? TS_ACT_LIST : TS_ACT_CLUB;
            b->value = i + 1;              /* tab i shows list i + 1 (TS_LIST_SEARCH has no tab) */
        } else {
            b->action = actions[col];
            b->value = 0;
        }
    }
    if (g_ts_foot_count == 0) {
        for (int32_t i = 0; i < TS_FOOT_BUTTONS; ++i) {
            TsButton *b = &g_ts_foot[i];
            if (i < TS_FOOT_TABS) ts_label(b->label, i + 1 < TS_LIST_COUNT ? k_ts_list_names[i + 1] : "Club", 0);
            else ts_label(b->label, i == TS_FOOT_TABS ? "Shortlist +" : i == TS_FOOT_TABS + 1 ? "Negotiate"
                                    : k_ts_sort_names[g_ts_sort], 0);
            b->button = ts_new_button(base, footer, b, i < TS_FOOT_TABS && i + 1 == g_ts_list ? 2 : 0);
        }
        g_ts_foot_count = TS_FOOT_BUTTONS;
        g_ts_last_shortlist = g_ts_last_offers = g_ts_last_sel = g_ts_last_sort = -2;
    }
    ts_footer_show(base, 1);
}

static void ts_attach(uint32_t base, void *screen) {
    g_ts_screen = screen;
    g_ts_top_count = 0;                    /* v35c: the free-looking band above the filter tabs is the search bar */
    g_ts_last_shortlist = g_ts_last_offers = g_ts_last_sel = g_ts_last_sort = -2;
    CfeGetSizeMarketFn get_width = (CfeGetSizeMarketFn)(base + 0x25EFED);
    CfeGetSizeMarketFn get_height = (CfeGetSizeMarketFn)(base + 0x25EFF1);
    float width = get_width(screen), height = get_height(screen);
    if (width < 400.0f || height < 300.0f) return;
    ts_attach_footer(base, width, height);
}

/* ---- market lists fed to the stock card grid ---- */
static int32_t ts_sort_key(int32_t player_id) {
    int32_t index = ts_player_index(player_id);
    if (index < 0) return -0x7fffffff;
    MarketPlayer *player = &g_players[index];
    switch (g_ts_sort) {
        case TS_SORT_PRICE: {
            int32_t price = ts_asking_price(player_id);
            return -(price > 0 ? price : market_value(player));     /* cheapest first */
        }
        case TS_SORT_WAGE: return -player->wage;                      /* lowest wage first */
        default: return player->rating * 0x10000 + (0xFFFF - (market_value(player) & 0xFFFF));
    }
}

/* Stable merge sort of `count` entries (stride TS_ENTRY) by descending key; `count` <= TS_SORT_MAX. */
static void ts_sort_entries(uint8_t *entries, int32_t count) {
    if (count < 2 || count > TS_SORT_MAX) return;
    for (int32_t i = 0; i < count; ++i) {
        g_ts_sort_order[i] = i;
        g_ts_sort_keys[i] = ts_sort_key(*(int32_t *)(entries + i * TS_ENTRY));
    }
    for (int32_t width = 1; width < count; width *= 2) {
        for (int32_t start = 0; start < count; start += width * 2) {
            int32_t mid = start + width < count ? start + width : count;
            int32_t end = start + width * 2 < count ? start + width * 2 : count;
            int32_t l = start, r = mid, o = start;
            while (l < mid && r < end) {
                if (g_ts_sort_keys[g_ts_sort_order[r]] > g_ts_sort_keys[g_ts_sort_order[l]])
                    g_ts_sort_tmp[o++] = g_ts_sort_order[r++];
                else
                    g_ts_sort_tmp[o++] = g_ts_sort_order[l++];
            }
            while (l < mid) g_ts_sort_tmp[o++] = g_ts_sort_order[l++];
            while (r < end) g_ts_sort_tmp[o++] = g_ts_sort_order[r++];
        }
        for (int32_t i = 0; i < count; ++i) g_ts_sort_order[i] = g_ts_sort_tmp[i];
    }
    for (int32_t i = 0; i < count; ++i) {
        const uint8_t *src = entries + g_ts_sort_order[i] * TS_ENTRY;
        uint8_t *dst = g_ts_sort_buffer + i * TS_ENTRY;
        for (int32_t k = 0; k < TS_ENTRY; ++k) dst[k] = src[k];
    }
    for (int32_t i = 0; i < count * TS_ENTRY; ++i) entries[i] = g_ts_sort_buffer[i];
}

static int32_t ts_list_has(int32_t player_id) {
    for (int32_t i = 0; i < g_ts_row_count; ++i) {
        if (*(int32_t *)(g_ts_rows + i * TS_ENTRY) == player_id) return 1;
    }
    return 0;
}

static void ts_list_add(int32_t player_id) {
    if (g_ts_row_count >= TS_ROWS_MAX || player_id < 0 || ts_list_has(player_id)) return;
    int32_t index = ts_player_index(player_id);
    if (index < 0) return;
    uint8_t *entry = g_ts_rows + g_ts_row_count * TS_ENTRY;
    for (int32_t k = 0; k < TS_ENTRY; ++k) entry[k] = 0;
    *(int32_t *)(entry + 0) = player_id;
    *(int32_t *)(entry + 4) = g_players[index].owner_id;
    entry[0xAC] = g_players[index].owner_id != USER_TEAM_ID;     /* buy card: shows the price line */
    ++g_ts_row_count;
}

/* Players who would sign for the user now at a price and wage the user can pay. */
static int32_t ts_for_you(const MarketPlayer *player, int32_t user_index, int32_t coins, int32_t wage_room) {
    if (player->owner_id == USER_TEAM_ID || player->owner_index < 0) return 0;
    if (!player_will_join(player, user_index) || !starter_move_allowed(player, user_index)) return 0;
    int32_t price = ts_asking_price(player->player_id);
    if (price <= 0 || price > coins) return 0;
    return transfer_wage_demand(player, user_index, player->owner_index) <= wage_room;
}

static void ts_build_list(uint32_t base) {
    g_ts_row_count = 0;
    if (g_ts_list == TS_LIST_SHORTLIST) {
        for (int32_t i = 0; i < SHORTLIST_CAPACITY; ++i) ts_list_add(g_ext.shortlist[i]);
    } else if (g_ts_list == TS_LIST_OFFERS) {
        for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
            MarketOffer *offer = &g_market.offers[i];
            if (offer_is_active(offer) &&
                (offer->seller_id == USER_TEAM_ID || offer->buyer_id == USER_TEAM_ID)) ts_list_add(offer->player_id);
        }
    } else if (g_ts_list == TS_LIST_FORYOU) {
        int32_t user_index = find_account(USER_TEAM_ID);
        if (user_index < 0) return;
        ClubAccount *club = &g_market.clubs[user_index];
        int32_t coins = base ? user_coins(base) : club->cash;
        int32_t fee_room = account_fee_room(club);
        if (fee_room < coins) coins = fee_room;
        int32_t wage_room = account_wage_room(club);
        /* the strongest candidates first, so a full list keeps the best ones */
        for (int32_t rating = 99; rating >= 1 && g_ts_row_count < TS_ROWS_MAX; --rating) {
            for (int32_t i = 0; i < g_player_count && g_ts_row_count < TS_ROWS_MAX; ++i) {
                if (g_players[i].rating != rating) continue;
                if (ts_for_you(&g_players[i], user_index, coins, wage_room)) ts_list_add(g_players[i].player_id);
            }
        }
    }
    if (g_ts_list != TS_LIST_SEARCH) {
        int32_t sort = g_ts_sort;
        if (sort == TS_SORT_DEFAULT) g_ts_sort = TS_SORT_RATING;
        ts_sort_entries(g_ts_rows, g_ts_row_count);
        g_ts_sort = sort;
    }
}

static void ts_request_setup(uint32_t base) {
    if (base) *(volatile uint8_t *)(uintptr_t)(base + TS_SETUP_RESULTS_FLAG) = 1;
}

static void ts_set_list(uint32_t base, int32_t list) {
    if (list < 0 || list >= TS_LIST_COUNT) return;
    if (list == g_ts_list) list = TS_LIST_SEARCH;       /* the active tab again: back to the search results */
    g_ts_list = list;
    if (list != TS_LIST_SEARCH) ts_build_list(base);
    TsSetSchemeFn set_scheme = (TsSetSchemeFn)(base + 0x21A2FD);   /* CFEButton::SetScheme */
    for (int32_t i = 0; i < g_ts_foot_count; ++i) {
        if (g_ts_foot[i].button && g_ts_foot[i].action == TS_ACT_LIST)
            set_scheme(g_ts_foot[i].button, g_ts_foot[i].value == list ? 2 : 0);
    }
    ts_request_setup(base);
}

/* ---- actions ---- */
static int32_t ts_box_busy(uint32_t base) {
    if (g_ui_live_box || g_ui_pending_slot >= 0) return 1;
    void *queue = ui_queue_get(base);
    if (!queue) return 0;
    for (int32_t i = 0; i < UI_QUEUE_SLOT_COUNT; ++i) {
        if (*(const uint32_t *)((const uint8_t *)queue + UI_QUEUE_SLOTS + 4 * i)) return 1;
    }
    return 0;
}

/* Tap on a card: select the player and open the stock card dialog (buy or sell) through the market. */
static void ts_open_card(uint32_t base, int32_t full_terms) {
    g_ui_base = base;
    if (g_ts_sel_id < 0) {
        ui_show_notice(base, "Negotiate", "Tap a player card first.", UI_AFTER_CLOSE);
        return;
    }
    if (full_terms) {
        MarketOffer *active = ts_user_offer_for(g_ts_sel_id);
        if (active) {
            g_ui_offer_origin = UI_OFFER_FROM_INBOX;
            ui_show_offer(base, active);       /* counters, exact amounts */
            return;
        }
    }
    PlayerInfo info;
    if (!load_player_info(base, g_ts_sel_id, &info)) return;
    int32_t index = ts_player_index(g_ts_sel_id);
    int32_t own = index >= 0 && g_players[index].owner_id == USER_TEAM_ID;
    g_ts_full_terms = full_terms;
    career_market_stock_action(&info, own ? -1 : (g_ts_sel_team >= 0 ? g_ts_sel_team : 0), base);
    g_ts_full_terms = 0;
}

static void ts_make_offer(uint32_t base) {
    g_ui_base = base;
    if (g_ts_sel_id < 0) {
        ui_show_notice(base, "Negotiate", "Tap a player card first.", UI_AFTER_CLOSE);
        return;
    }
    MarketOffer *offer = ts_user_offer_for(g_ts_sel_id);
    if (offer) {
        g_ui_offer_origin = UI_OFFER_FROM_INBOX;
        ui_show_offer(base, offer);
        return;
    }
    PlayerInfo info;
    if (!load_player_info(base, g_ts_sel_id, &info)) return;
    int32_t index = ts_player_index(g_ts_sel_id);
    int32_t own = index >= 0 && g_players[index].owner_id == USER_TEAM_ID;
    career_market_stock_action(&info, own ? -1 : (g_ts_sel_team >= 0 ? g_ts_sel_team : 0), base);
}

static void ts_run_action(uint32_t base, const TsButton *button) {
    switch (button->action) {
        case TS_ACT_LIST: ts_set_list(base, button->value); break;
        case TS_ACT_CLUB: g_ui_base = base; g_ui_from_stock = 0; ui_show_club_hub(base); break;
        case TS_ACT_OFFER: ts_open_card(base, 1); break;          /* Negotiate: full terms / counters */
        case TS_ACT_SHORTLIST:
            if (g_ts_sel_id < 0) {
                ui_show_notice(base, "Shortlist", "Tap a player card to select him first.", UI_AFTER_CLOSE);
            } else if (shortlist_toggle(g_ts_sel_id) < 0) {
                ui_show_notice(base, "Shortlist Full", "The shortlist holds 16 players. Remove one first.",
                               UI_AFTER_CLOSE);
            } else if (g_ts_list == TS_LIST_SHORTLIST) {
                ts_build_list(base);
                ts_request_setup(base);
            }
            break;
        case TS_ACT_SORT:
            g_ts_sort = (g_ts_sort + 1) % TS_SORT_COUNT;
            if (g_ts_list != TS_LIST_SEARCH) ts_build_list(base);
            ts_request_setup(base);
            break;
        default: break;
    }
}

/* "+ Surname" / "- Surname" for the last tapped player, "Shortlist +" before any tap. */
static void ts_shortlist_label(uint32_t base, uint16_t *out, int32_t starred) {
    if (g_ts_sel_id < 0) {
        ts_label(out, "Shortlist +", 0);
        return;
    }
    uint16_t name[64];
    MarketUiText full;
    ui_text_reset(&full, name, 64);
    ui_append_player_name(&full, base, g_ts_sel_id);
    int32_t start = 0;
    for (int32_t i = 0; name[i]; ++i) if (name[i] == ' ' && name[i + 1]) start = i + 1;
    MarketUiText b;
    ui_text_reset(&b, out, TS_LABEL);
    ui_text_append_ascii(&b, starred ? "- " : "+ ");
    ui_text_append_wide(&b, name + start, 14);
}

static void ts_update_labels(uint32_t base) {
    int32_t shortlisted = shortlist_count();
    int32_t offers = ui_active_offer_count();
    uint16_t text[TS_LABEL];
    if (shortlisted != g_ts_last_shortlist && g_ts_foot_count > TS_LIST_SHORTLIST - 1) {
        ts_label(text, k_ts_list_names[TS_LIST_SHORTLIST], shortlisted);
        ts_set_text(base, &g_ts_foot[TS_LIST_SHORTLIST - 1], text);
        g_ts_last_shortlist = shortlisted;
    }
    if (offers != g_ts_last_offers && g_ts_foot_count > TS_LIST_OFFERS - 1) {
        ts_label(text, k_ts_list_names[TS_LIST_OFFERS], offers);
        ts_set_text(base, &g_ts_foot[TS_LIST_OFFERS - 1], text);
        g_ts_last_offers = offers;
    }
    if (g_ts_foot_count != TS_FOOT_BUTTONS) return;
    int32_t starred = g_ts_sel_id >= 0 && shortlist_slot(g_ts_sel_id) >= 0;
    int32_t index = ts_player_index(g_ts_sel_id);
    int32_t offer_state = g_ts_sel_id < 0 ? 0 : ts_user_offer_for(g_ts_sel_id) ? 2 :
                          (index >= 0 && g_players[index].owner_id == USER_TEAM_ID) ? 1 : 0;
    int32_t state = g_ts_sel_id * 8 + starred * 4 + offer_state;
    if (state != g_ts_last_sel) {
        ts_label(text, offer_state == 2 ? "View Offer" : offer_state == 1 ? "Sale Options" : "Negotiate", 0);
        ts_set_text(base, &g_ts_foot[TS_FOOT_TABS + 1], text);
        ts_shortlist_label(base, text, starred);
        ts_set_text(base, &g_ts_foot[TS_FOOT_TABS], text);
        g_ts_last_sel = state;
    }
    if (g_ts_sort != g_ts_last_sort) {
        ts_label(text, k_ts_sort_names[g_ts_sort], 0);
        ts_set_text(base, &g_ts_foot[TS_FOOT_TABS + 2], text);
        g_ts_last_sort = g_ts_sort;
    }
}

/* ---- hooks ---- */
static uint32_t ts_init_hook(ModCtx *ctx, uint32_t base) {
    (void)ctx;
    (void)base;
    g_ts_screen = (void *)0;               /* the screen is being (re)built: its children are new */
    g_ts_top_count = 0;
    g_ts_list = TS_LIST_SEARCH;            /* Init starts a fresh stock search */
    g_ts_sel_id = g_ts_sel_team = -1;
    return 0;
}

static uint32_t ts_process_hook(ModCtx *ctx, uint32_t base) {
    void *screen = (void *)(uintptr_t)ctx->r[0];
    if (!screen || !ts_enabled()) return 0;
    g_ui_base = base;
    ++g_ts_frame;
    if ((g_ts_frame & 127) == 0) ++g_price_epoch;         /* prices follow the market within ~2 s */
    if (g_ts_screen != screen) ts_attach(base, screen);
    if (!g_ts_foot_shown && g_ts_foot_count) ts_footer_show(base, 1);
    if (g_ts_list != TS_LIST_SEARCH && *(const int32_t *)(uintptr_t)(base + TS_ASYNC_SEARCH + 4) != 0)
        ts_set_list(base, TS_LIST_SEARCH);   /* the stock filter / name search started a search */
    ts_update_labels(base);
    if (ts_box_busy(base)) return 0;
    for (int32_t i = 0; i < g_ts_top_count; ++i) {
        if (ts_released(g_ts_top[i].button)) { ts_run_action(base, &g_ts_top[i]); return 0; }
    }
    for (int32_t i = 0; i < g_ts_foot_count; ++i) {
        if (g_ts_foot_shown && ts_released(g_ts_foot[i].button)) { ts_run_action(base, &g_ts_foot[i]); return 0; }
    }
    return 0;
}

static void ts_status_text(uint32_t base, MarketUiText *out) {
    int32_t user_index = find_account(USER_TEAM_ID);
    if (g_ts_sel_id >= 0) {
        int32_t index = ts_player_index(g_ts_sel_id);
        ui_append_player_name(out, base, g_ts_sel_id);
        if (index < 0) return;
        MarketPlayer *player = &g_players[index];
        ui_text_append_ascii(out, "  ");
        ui_text_append_i32(out, player->rating);
        if (player->owner_id != USER_TEAM_ID && user_index >= 0 && player->owner_index >= 0) {
            int32_t price = ts_asking_price(player->player_id);
            ui_text_append_ascii(out, "  Fee ");
            ui_text_append_char(out, 0x0180);
            ui_text_append_i32(out, price > 0 ? price : market_value(player));
            ui_text_append_ascii(out, "  Wage ");
            ui_text_append_i32(out, transfer_wage_demand(player, user_index, player->owner_index));
            if (!player_will_join(player, user_index)) ui_text_append_ascii(out, "  Not interested");
        } else if (player->owner_id == USER_TEAM_ID) {
            ui_text_append_ascii(out, "  Your player, wage ");
            ui_text_append_i32(out, player->wage);
        }
        return;
    }
    ui_text_append_ascii(out, g_market.window_id < 0 ? "Window closed" : "Window open");
    if (g_market.window_id >= 0) {
        int32_t left = window_matches_left();
        ui_text_append_ascii(out, " (");
        ui_text_append_i32(out, left > 0 ? left : 1);
        ui_text_append_ascii(out, left == 1 ? " match)" : " matches)");
    }
    if (user_index >= 0) {
        ui_text_append_ascii(out, "  Squad ");
        ui_text_append_i32(out, g_total_count[user_index]);
        ui_text_append_char(out, '/');
        ui_text_append_i32(out, g_user_squad_max);
        ui_text_append_ascii(out, "  Wage room ");
        ui_text_append_i32(out, account_wage_room(&g_market.clubs[user_index]));
    }
}

static uint32_t ts_render_post_hook(ModCtx *ctx, uint32_t base) {
    if (!g_ts_screen || ctx->r[0] != (uint32_t)(uintptr_t)g_ts_screen || !ts_enabled()) return 0;
    (void)base;                            /* v35c: no text over the screen (the band there is the search bar) */
    return 0;
}

static uint32_t ts_setup_hook(ModCtx *ctx, uint32_t base) {
    if (!ts_enabled()) return 0;
    uint8_t *async = (uint8_t *)(uintptr_t)(base + TS_ASYNC_SEARCH);
    if (g_ts_list == TS_LIST_SEARCH) {
        if (g_ts_sort != TS_SORT_DEFAULT && *(int32_t *)(async + 4) == 0) {
            uint8_t *entries = (uint8_t *)(uintptr_t)*(const uint32_t *)async;
            int32_t count = *(int32_t *)(async + 0x2C);
            if (entries && count > 1 && count <= TS_SORT_MAX) ts_sort_entries(entries, count);
        }
        return 0;
    }
    uint32_t resume = modcore_resume("ts_setup");
    if (!resume || *(int32_t *)(async + 4) != 0) return 0;         /* a search job owns the array */
    uint32_t saved_entries = *(uint32_t *)async;
    int32_t saved_count = *(int32_t *)(async + 0x2C);
    *(uint32_t *)async = (uint32_t)(uintptr_t)g_ts_rows;
    *(int32_t *)(async + 0x2C) = g_ts_row_count;
    ((TsScreenFn)(uintptr_t)resume)((void *)(uintptr_t)ctx->r[0]);
    *(uint32_t *)async = saved_entries;
    *(int32_t *)(async + 0x2C) = saved_count;
    return ctx->lr;
}

static void ts_select_card(int32_t player_id, int32_t team) {
    g_ts_sel_id = player_id;
    g_ts_sel_team = team;
}

static uint32_t ts_bid_hook(ModCtx *ctx, uint32_t base) {
    /* CurrentPlayerBid(CFEPlayerCard *) is static: the card is r0 (v35c read r1 = 1 and crashed on every tap) */
    const uint8_t *card = (const uint8_t *)(uintptr_t)ctx->r[0];
    if ((uintptr_t)card < 0x10000 || !ts_enabled() || !g_ts_screen) return 0;  /* stock path -> market stock action */
    int32_t player_id = *(const uint16_t *)(card + 0x294);
    if (player_id >= CREATED_PLAYER_FIRST_ID) return 0;           /* created players: stock create-player path */
    int32_t team = *(const int32_t *)(card + 0x344);
    ts_select_card(player_id, team);
    ts_open_card(base, 0);                 /* the game's own sign dialog, with the market price */
    ctx->r[0] = 0;
    return ctx->lr;
}

static uint32_t ts_card_value_hook(ModCtx *ctx, uint32_t base) {
    const uint8_t *card = (const uint8_t *)(uintptr_t)ctx->r[0];
    if (!card || !g_ts_screen || !ts_enabled()) return 0;
    if (!(*(const uint32_t *)(card + 0x288) & 0x10)) return 0;    /* buy cards only */
    void *current = ((TsGetEntityFn)(base + 0x297841))();         /* CFE::GetCurrentScreen */
    if (current != g_ts_screen) return 0;
    int32_t price = ts_asking_price(*(const uint16_t *)(card + 0x294));
    if (price <= 0) return 0;
    ctx->r[0] = (uint32_t)price;
    return ctx->lr;
}

static uint32_t ts_footer_post_hook(ModCtx *ctx, uint32_t base) {
    (void)ctx;
    if (!g_ts_foot_shown || !g_ts_foot_count) return 0;
    void *current = ((TsGetEntityFn)(base + 0x297841))();
    if (!g_ts_screen || current != g_ts_screen) ts_footer_show(base, 0);
    return 0;
}

/* ---- v35b: transfers through the game's own player-card dialogs ----
 * Buying: tap a card -> CFEMsgSignPlayer (card + green coin button = the market asking fee). The coin submits
 * the bid (asking fee, the player's wage demand, 3 years); a seller counter or the player's wage counter comes
 * back in the same dialog with the new amount on the coin. Selling: the stock Sell Player button -> squad in
 * sell mode -> tap a player -> CFEMsgSellPlayer (card + coin button = today's best club bid); the coin sells.
 * The cross always closes the dialog without deciding anything: a pending offer stays in the Offers tab, and
 * Negotiate (or View Offer) opens the full terms pages for counters and exact amounts. */
static int32_t g_ts_sell_buyer = -1;
static int32_t g_ts_sell_fee;
static int32_t g_ts_sell_wage;

static int32_t ui_show_card_dialog(uint32_t base, int32_t kind, int32_t player_id, int32_t team, int32_t price,
                                   const char *title, MarketUiCallback callback) {
    if (!base || !callback || price < 0) return 0;
    int32_t slot = ui_box_acquire();
    MarketUiBox *box = &g_ui_boxes[slot];
    box->kind = kind;
    box->page_count = 0;
    box->chunk = 0;
    box->button_count = 2;
    box->button_map[0] = 0;
    box->button_map[1] = 1;
    box->keyboard_max = 0;
    box->callback = callback;
    box->card_player = player_id;
    box->card_team = team;
    box->card_price = price;
    MarketUiText builder;
    ui_text_reset(&builder, box->title, 48);
    ui_text_append_ascii(&builder, title);
    ui_wide_copy(box->description, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    g_ui_base = base;
    return ui_box_submit(base, slot);
}

static void ts_after_change(uint32_t base) {
    ++g_price_epoch;
    if (g_ts_list != TS_LIST_SEARCH && g_ts_screen) {
        ts_build_list(base);
        ts_request_setup(base);
    }
}

static void ts_append_event(MarketUiText *out, uint32_t base) {
    switch (g_neg_event) {
        case NEG_SOFTENED: ui_text_append_ascii(out, "Your bid was low, but they lowered the price.\n"); break;
        case NEG_INSULT: ui_text_append_ascii(out, "That bid was far below their value (one strike).\n"); break;
        case NEG_AGREED: ui_text_append_ascii(out, "The clubs agreed the fee.\n"); break;
        case NEG_RIVAL_BID:
        case NEG_RIVAL_RAISED:
            ui_append_team_name(out, base, g_neg_rival_team);
            ui_text_append_ascii(out, " is bidding ");
            ui_text_append_i32(out, g_neg_rival_fee);
            ui_text_append_ascii(out, " too; this price beats them.\n");
            break;
        case NEG_BUYER_RAISED: ui_text_append_ascii(out, "They raised their bid.\n"); break;
        case NEG_BUYER_FINAL: ui_text_append_ascii(out, "This is their final bid.\n"); break;
        default: break;
    }
}

static int32_t ts_cb_offer(int32_t selection);

/* An active offer that involves the user, as the matching card dialog; other stages use the terms page. */
static void ts_offer_dialog(uint32_t base, MarketOffer *offer) {
    if (!offer || !offer_is_active(offer)) return;
    g_ui_offer_id = offer->offer_id;
    MarketUiText b;
    ui_text_reset(&b, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ts_append_event(&b, base);
    if (offer->buyer_id == USER_TEAM_ID && offer->status == CM_OFFER_WAIT_USER_FEE) {
        ui_append_team_name(&b, base, offer->seller_id);
        ui_text_append_ascii(&b, " want ");
        ui_text_append_i32(&b, offer->counter_fee);
        ui_text_append_ascii(&b, " (you bid ");
        ui_text_append_i32(&b, offer->fee);
        ui_text_append_ascii(&b, ").\nCoin: pay it. Cross: decide later (Offers tab, Negotiate to counter).");
        ui_show_card_dialog(base, UI_BOX_SIGN, offer->player_id, offer->seller_id, offer->counter_fee,
                            "Seller Counter", ts_cb_offer);
    } else if (offer->buyer_id == USER_TEAM_ID && offer->status == CM_OFFER_WAIT_USER_WAGE) {
        ui_text_append_ascii(&b, "Fee agreed: ");
        ui_text_append_i32(&b, offer->fee);
        ui_text_append_ascii(&b, ". He asks ");
        ui_text_append_i32(&b, offer->counter_wage);
        ui_text_append_ascii(&b, " a season for ");
        ui_text_append_i32(&b, offer->contract_years);
        ui_text_append_ascii(&b, offer->contract_years == 1 ? " year" : " years");
        ui_text_append_ascii(&b, " (you offered ");
        ui_text_append_i32(&b, offer->annual_wage);
        ui_text_append_ascii(&b, ").\nCoin: agree his wage and sign him.");
        ui_show_card_dialog(base, UI_BOX_SIGN, offer->player_id, offer->seller_id, offer->counter_wage,
                            "Player's Wage", ts_cb_offer);
    } else if (offer->seller_id == USER_TEAM_ID) {
        int32_t fee = offer->status == CM_OFFER_WAIT_USER_SELLER_COUNTER ? offer->counter_fee : offer->fee;
        ui_append_team_name(&b, base, offer->buyer_id);
        ui_text_append_ascii(&b, " bid ");
        ui_text_append_i32(&b, fee);
        ui_text_append_ascii(&b, " for him.\nCoin: sell. Cross: keep him for now (the bid stays in Offers).");
        ui_show_card_dialog(base, UI_BOX_SELL, offer->player_id, USER_TEAM_ID, fee, "Transfer Bid", ts_cb_offer);
    } else {
        ui_show_offer(base, offer);
    }
}

static int32_t ts_cb_offer(int32_t selection) {
    uint32_t base = g_ui_base;
    MarketOffer *offer = ui_find_offer_any(g_ui_offer_id);
    if (selection != 1 || !offer || !offer_is_active(offer)) {
        g_ui_from_stock = 0;               /* cross: nothing decided, the offer stays open */
        return 1;
    }
    career_market_respond_to_offer(base, offer->offer_id, CM_DECISION_ACCEPT, 0);
    save_profile(base);
    ts_after_change(base);
    if (offer_is_active(offer)) ts_offer_dialog(base, offer);
    else ui_show_offer_result(base, offer, UI_AFTER_CLOSE);
    return 1;
}

static int32_t ts_cb_buy(int32_t selection) {
    uint32_t base = g_ui_base;
    if (selection != 1 || g_ui_player_index < 0 || g_ui_player_index >= g_player_count ||
        g_ui_bid_player_id != g_players[g_ui_player_index].player_id) {
        g_ui_from_stock = 0;
        return 1;
    }
    MarketPlayer *player = &g_players[g_ui_player_index];
    int32_t offer_id = career_market_submit_user_bid(base, player->player_id, g_ui_bid_fee, g_ui_bid_wage,
                                                      g_ui_bid_contract_years);
    g_ui_bid_player_id = -1;
    save_profile(base);
    ts_after_change(base);
    if (offer_id == -12) {
        ui_show_notice(base, "Not Enough Coins",
                       "After this fee you must still have enough coins to pay a season of wages, including his.",
                       UI_AFTER_CLOSE);
        return 1;
    }
    if (offer_id == -11 || offer_id == -15 || offer_id == -16 || offer_id == -17) {
        ui_show_refusal(base, player, offer_id, UI_AFTER_CLOSE);
        return 1;
    }
    if (offer_id < 0) {
        ui_show_notice(base, "Offer Not Submitted",
                       "The player, seller, window, or your budgets no longer allow this offer.", UI_AFTER_CLOSE);
        return 1;
    }
    g_ui_offer_origin = UI_OFFER_FROM_BROWSER;
    g_ui_offer_id = offer_id;
    MarketOffer *offer = ui_find_offer_any(offer_id);
    if (offer && offer_is_active(offer)) ts_offer_dialog(base, offer);
    else if (offer && offer->status == CM_OFFER_COMPLETED) ui_show_offer_result(base, offer, UI_AFTER_CLOSE);
    else ui_show_notice(base, "Offer Declined",
                        "The seller or player rejected those terms, or your budget no longer covers them.",
                        UI_AFTER_CLOSE);
    return 1;
}

/* From career_market_stock_action (buy): g_ui_player_index and g_ui_bid_* are set and the checks passed. */
static void ts_buy_dialog(uint32_t base) {
    MarketPlayer *player = &g_players[g_ui_player_index];
    MarketOffer *offer = ts_user_offer_for(player->player_id);
    if (offer) {
        g_ui_offer_origin = UI_OFFER_FROM_INBOX;
        g_neg_event = NEG_NONE;
        ts_offer_dialog(base, offer);
        return;
    }
    MarketUiText b;
    ui_text_reset(&b, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&b, "Wage ");
    ui_text_append_i32(&b, g_ui_bid_wage);
    ui_text_append_ascii(&b, " a season, ");
    ui_text_append_i32(&b, g_ui_bid_contract_years);
    ui_text_append_ascii(&b, " years.\nCoin: offer the asking fee. The seller and the player may counter.");
    ui_show_card_dialog(base, UI_BOX_SIGN, player->player_id, player->owner_id, g_ui_bid_fee, "Make Offer", ts_cb_buy);
}

static int32_t ts_cb_sell(int32_t selection) {
    uint32_t base = g_ui_base;
    if (selection != 1 || g_ui_player_index < 0 || g_ui_player_index >= g_player_count ||
        g_players[g_ui_player_index].owner_id != USER_TEAM_ID || g_ts_sell_buyer < 0) {
        g_ui_from_stock = 0;
        return 1;
    }
    MarketPlayer *player = &g_players[g_ui_player_index];
    if (has_active_offer_for_player(player->player_id)) {
        MarketOffer *active = ts_user_offer_for(player->player_id);
        if (active) ts_offer_dialog(base, active);
        return 1;
    }
    MarketOffer *offer = create_ai_offer_for_user(g_ts_sell_buyer, g_ui_player_index, g_market.window_id,
                                                  g_market.current_turn, g_market.season,
                                                  g_ts_sell_fee, g_ts_sell_wage);
    if (!offer) {
        ui_show_notice(base, "No Sale", "That club can no longer make this bid.", UI_AFTER_CLOSE);
        return 1;
    }
    g_market_dirty = 1;
    g_ui_offer_origin = UI_OFFER_FROM_INBOX;
    g_ui_offer_id = offer->offer_id;
    career_market_respond_to_offer(base, offer->offer_id, CM_DECISION_ACCEPT, 0);
    save_profile(base);
    ts_after_change(base);
    if (offer_is_active(offer)) ts_offer_dialog(base, offer);
    else ui_show_offer_result(base, offer, UI_AFTER_CLOSE);
    return 1;
}

/* From career_market_stock_action (sell): g_ui_player_index is one of the user's players. */
static void ts_sell_dialog(uint32_t base) {
    MarketPlayer *player = &g_players[g_ui_player_index];
    MarketOffer *offer = ts_user_offer_for(player->player_id);
    if (offer) {
        g_ui_offer_origin = UI_OFFER_FROM_INBOX;
        g_neg_event = NEG_NONE;
        ts_offer_dialog(base, offer);
        return;
    }
    if (user_signing_locked(player->player_id)) {
        ui_show_notice(base, "Just Signed", "Players signed this season can be sold from next season.",
                       UI_AFTER_CLOSE);
        return;
    }
    int32_t fee = 0, wage = 0;
    g_ts_sell_buyer = best_ai_bid_for_user_player(g_ui_player_index, &fee, &wage);
    g_ts_sell_fee = fee;
    g_ts_sell_wage = wage;
    if (g_ts_sell_buyer < 0 || fee <= 0) {
        int32_t listed = player->listed_for_sale;
        if (!listed && toggle_user_listing(player->player_id) >= 0) {
            player->listed_for_sale = 1;
            save_profile(base);
            listed = 2;
        }
        ui_show_notice(base, "No Bids Today",
                       listed == 2 ? "No club bids for him today. He is now listed, so bids can arrive as matches are played."
                       : listed ? "No club bids for him today. He stays listed; bids can arrive as matches are played."
                                : "No club bids for him today.",
                       UI_AFTER_CLOSE);
        return;
    }
    MarketUiText b;
    ui_text_reset(&b, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_append_team_name(&b, base, g_market.clubs[g_ts_sell_buyer].team_id);
    ui_text_append_ascii(&b, " bid ");
    ui_text_append_i32(&b, fee);
    ui_text_append_ascii(&b, " today.\nCoin: sell him now. Cross: keep him (Negotiate lists him or asks for more).");
    ui_show_card_dialog(base, UI_BOX_SELL, player->player_id, USER_TEAM_ID, fee, "Sell Player?", ts_cb_sell);
}

static void ts_on_enter(uint32_t base) {
    g_ui_base = base;
    ++g_price_epoch;
    if (g_ts_list != TS_LIST_SEARCH) {     /* back from a sub-screen: the list may have changed */
        ts_build_list(base);
        ts_request_setup(base);
    }
}

static void ts_register_hooks(void) {
#if MARKET_TRANSFER_SCREEN_UI
    modcore_register("ts_init", ts_init_hook);
    modcore_register("ts_process", ts_process_hook);
    modcore_register("ts_render_post", ts_render_post_hook);
    modcore_register("ts_setup", ts_setup_hook);
    modcore_register("ts_bid", ts_bid_hook);
    modcore_register("ts_card_value", ts_card_value_hook);
    modcore_register("ts_footer_post", ts_footer_post_hook);
#endif
}

#include "tm_screen.c"
