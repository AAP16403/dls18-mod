/*
 * Host test harness for native_bridge.c (x86-64 Linux/WSL, non-PIE; see run_tests.sh). Game pointers
 * are 32-bit, so every address the bridge reads from game memory is kept below 4 GiB.
 *
 * The bridge calls game functions as `base + <libDLS18 offset>`. This harness maps a fake library
 * image at FAKE_BASE, writes x86 `jmp` thunks at every offset the bridge uses, and backs them with
 * mocks that model the game's roster mechanics as traced in data/ROSTER_MECHANICS.md:
 *   - a default link table (DB+0x24) and a live table (DB+0x28), 0x108-byte links sorted by team id;
 *   - SetOverrideLinks redirecting per-team lookups to the default table;
 *   - CalculateLinks rebuilding the live table from the default one, keeping only the user link and
 *     stripping user-owned players from their default clubs;
 *   - SignPlayer (always to the user) and SellPlayer (ignores the buyer), CanAdd/CanRemove limits;
 *   - the career save storing only the user link plus whatever the market serializes.
 * native_bridge.c is #included so tests can inspect its static state.
 *
 * Usage: harness.exe <dataset.txt> [seasons] [seed] [--quiet] [--csv out.csv]
 */
#include <sys/mman.h>
#include <malloc.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../native_bridge.c"

/* modcore.c is ARM-only (thunks in inline asm); the host harness has no extended library */
int32_t modcore_register(const char *name, ModHookFn fn) { (void)name; (void)fn; return 0; }
int32_t modcore_init(uint32_t base) { (void)base; return 0; }
static void h_fake_setup_results(void *screen);
/* ts_setup calls the hook's resume block as the stock SetupResults; the host stands in a recorder */
uint32_t modcore_resume(const char *name) {
    return strcmp(name, "ts_setup") ? 0u : (uint32_t)(uintptr_t)h_fake_setup_results;
}

#define FAKE_BASE 0x10000000u
#define FAKE_SIZE 0x00900000u
#define H_MAX_LINKS 300
#define USER_TEAM 0x102

typedef struct {
    int32_t known, pos, rating, age, value;
} HPlayer;

typedef struct {               /* CDataBase as seen by a 32-bit game: 4-byte pointers */
    uint8_t pad0[0x1C];
    uint32_t override_links;   /* +0x1C */
    int32_t override_count;    /* +0x20 */
    uint32_t default_links;    /* +0x24 */
    uint32_t live_links;       /* +0x28 */
    uint8_t pad1[0x38 - 0x2C];
    int32_t link_count;        /* +0x38 */
    uint8_t pad2[0x90 - 0x3C];
    uint32_t default_simple;   /* +0x90 */
    int32_t default_simple_count; /* +0x94 */
    uint8_t pad3[0x100 - 0x98];
} HDataBase;
#define P32(x) ((uint8_t *)(uintptr_t)(x))
#define U32(p) ((uint32_t)(uintptr_t)(p))

static HDataBase h_db;
static HPlayer h_players[0x10000];
static uint8_t h_pristine[H_MAX_LINKS * LINK_STRIDE];   /* the links file, as a fresh process sees it */
static uint8_t h_valid[0x10000];
static uint8_t h_kind[0x10000];          /* see make_dataset.py */
static int32_t h_saves, h_calc_links, h_dev_adds, h_sign_calls, h_sell_calls, h_verify_calls;
static int32_t h_ratings, h_simple_regens, h_violations, h_quiet;

typedef struct { int32_t season, turn, end_turn; } HSeason;
static HSeason *h_season(void) { return (HSeason *)(uintptr_t)(FAKE_BASE + 0x84A260 + 0x14); }

/* ---- link helpers ---- */
static uint8_t *link_in(uint8_t *table, int32_t team) {
    for (int32_t i = 0; i < h_db.link_count; ++i) {
        uint8_t *l = table + i * LINK_STRIDE;
        if (*(int32_t *)l == team) return l;
    }
    return NULL;
}
static uint8_t *cur_table(void) { return h_db.override_links ? P32(h_db.override_links) : P32(h_db.live_links); }
static int32_t *lcount(uint8_t *l) { return (int32_t *)(l + 4); }
static int32_t *lids(uint8_t *l) { return (int32_t *)(l + 0x88); }
static uint32_t *lspec(uint8_t *l) { return (uint32_t *)(l + 8); }
static int32_t lfind(uint8_t *l, int32_t pid) {
    for (int32_t i = 0; l && i < *lcount(l); ++i) if (lids(l)[i] == pid) return i;
    return -1;
}
static int lremove(uint8_t *l, int32_t pid) {
    int32_t i = lfind(l, pid);
    if (i < 0) return 0;
    for (int32_t j = i; j + 1 < *lcount(l); ++j) { lids(l)[j] = lids(l)[j + 1]; lspec(l)[j] = lspec(l)[j + 1]; }
    --(*lcount(l));
    return 1;
}
static int ladd(uint8_t *l, int32_t pid, uint32_t spec) {
    if (!l || *lcount(l) > 31 || lfind(l, pid) >= 0) return 0;
    lids(l)[*lcount(l)] = pid; lspec(l)[*lcount(l)] = spec; ++(*lcount(l));
    return 1;
}
static int32_t pid_of(const PlayerInfo *info) { return *(const uint16_t *)&info->bytes[0]; }

/* ---- mocks ---- */
static int m_link_count(void) { return h_db.link_count; }
static void *m_link_by_index(int32_t i) { return (i >= 0 && i < h_db.link_count) ? P32(h_db.live_links) + i * LINK_STRIDE : NULL; }
static int m_team_value(int32_t team) {
    uint8_t *l = link_in(P32(h_db.live_links), team); int64_t s = 0;
    for (int32_t i = 0; l && i < *lcount(l); ++i) s += h_players[lids(l)[i]].value;
    return s > 0x7fffffff ? 0x7fffffff : (int)s;
}
static int m_valid_team(int32_t team) { return team >= 0 && team < 0x10000 && h_valid[team]; }
static int m_international(int32_t team) { return team >= 0 && team < 0x10000 && (h_kind[team] & 7) == 2; }
static int m_miscellaneous(int32_t team) { return team >= 0 && team < 0x10000 && (h_kind[team] & 7) == 3; }
static int m_classic(int32_t team) { return team >= 0 && team < 0x10000 && (h_kind[team] & 8) != 0; }
static int is_real_club(int32_t team) { return team == USER_TEAM || (m_valid_team(team) && !m_international(team) && !m_miscellaneous(team) && !m_classic(team)); }
static int m_player_info(PlayerInfo *info, int32_t pid, int32_t a, int32_t b) {
    (void)a; (void)b;
    if (pid < 0 || pid > 0xFFFF || !h_players[pid].known) return 0;
    memset(info, 0, sizeof(*info));
    *(uint16_t *)&info->bytes[0] = (uint16_t)pid;
    info->bytes[0x7F] = (uint8_t)h_players[pid].pos;
    info->bytes[0x80] = (uint8_t)(h_players[pid].pos == 0 ? 0 : 5);
    return 1;
}
static int m_rating(PlayerInfo *info) { return h_players[pid_of(info)].rating; }
static int m_value(PlayerInfo *info, int32_t a, int32_t b, int32_t c, int32_t d) {
    (void)a; (void)b; (void)c; (void)d; return h_players[pid_of(info)].value;
}
static int m_turn(void *s) { return ((HSeason *)s)->turn; }
static int m_end_turn(void *s) { return ((HSeason *)s)->end_turn; }
static int m_season_count(void *s) { return ((HSeason *)s)->season; }
/* DLS schedule: season slots 17..56 (friendly at 17), league rounds on even slots from 18 */
static int m_start_turn(void *s) { (void)s; return 17; }
static int m_start_league_turn(void *s) { (void)s; return 18; }
static void *m_specific(int32_t team, int32_t pid) {
    uint8_t *l = link_in(cur_table(), team);
    int32_t i = lfind(l, pid);
    return i >= 0 ? (void *)&lspec(l)[i] : NULL;
}
static void m_add_to_link(int32_t dummy, int32_t team, const PlayerInfo *info, const void *spec, int32_t f, int32_t xi) {
    (void)dummy; (void)f; (void)xi;
    ladd(link_in(cur_table(), team), pid_of(info), spec ? *(const uint32_t *)spec : 0);
}
static void m_remove_from_link(int32_t dummy, int32_t team, int32_t pid, int32_t upd) {
    (void)dummy; (void)upd; lremove(link_in(cur_table(), team), pid);
}
static void m_set_override(void *links, int32_t count) { h_db.override_links = U32(links); h_db.override_count = count; }
static int m_can_remove(int32_t team, PlayerInfo *info) {
    if (team == -1) return 2;
    uint8_t *l = link_in(cur_table(), team);
    if (!l || *lcount(l) < 17) return 0;
    int32_t gk = 0;
    for (int32_t i = 0; i < *lcount(l); ++i) if (h_players[lids(l)[i]].pos == 0) ++gk;
    if (gk > 1) return 2;
    return h_players[pid_of(info)].pos == 0 ? 1 : 2;
}
static int m_can_add(int32_t team, const PlayerInfo *info, int32_t x) {
    (void)x;
    if (team == -1) return 2;
    uint8_t *l = link_in(cur_table(), team);
    if (!l || *lcount(l) > 31) return 0;
    return lfind(l, pid_of(info)) >= 0 ? 1 : 2;
}
static void m_verify_link(void *db, PlayerInfo *info, int32_t team, const void *spec, int32_t u,
                          int32_t a, void *b, int32_t c, int32_t d) {
    (void)db; (void)info; (void)spec; (void)u; (void)a; (void)b; (void)c; (void)d;
    ++h_verify_calls;
    uint8_t *l = link_in(cur_table(), team);
    if (l && *lcount(l) < 16) {
        fprintf(stderr, "  VIOLATION: VerifyLink on team %d with %d players (game would auto-sign)\n", team, *lcount(l));
        ++h_violations;
    }
}
static void *m_generate_simple(void *links, int32_t count, int32_t *out) {
    int32_t total = 0;
    for (int32_t i = 0; i < count; ++i) total += *lcount((uint8_t *)links + i * LINK_STRIDE);
    *out = total; ++h_simple_regens;
    return malloc((size_t)total * 8 + 8);
}
static void m_sort_simple(void *p, int32_t n, int32_t f) { (void)p; (void)n; (void)f; }
static void m_delete_array(void *p) { free(p); }
static void m_team_rating(void *db, int32_t team, void *tm) { (void)db; (void)team; (void)tm; ++h_ratings; }
static void calculate_links_impl(void) {
    ++h_calc_links;
    uint8_t user[LINK_STRIDE];
    memcpy(user, link_in(P32(h_db.live_links), USER_TEAM), LINK_STRIDE);
    memcpy(P32(h_db.live_links), P32(h_db.default_links), (size_t)h_db.link_count * LINK_STRIDE);
    memcpy(link_in(P32(h_db.live_links), USER_TEAM), user, LINK_STRIDE);
    for (int32_t k = 0; k < *lcount(user); ++k) {
        int32_t pid = lids(user)[k];
        for (int32_t i = 0; i < h_db.link_count; ++i) {
            uint8_t *l = P32(h_db.live_links) + i * LINK_STRIDE;
            if (*(int32_t *)l != USER_TEAM) lremove(l, pid);
        }
    }
}
static void m_calc_links(int32_t verify, int32_t upd, int32_t norecalc, int32_t unused) {
    (void)verify; (void)upd; (void)norecalc; (void)unused; calculate_links_impl();
}
static void m_sign(const PlayerInfo *info, int32_t from, const void *spec, int32_t calc, int32_t force, int32_t xi) {
    (void)force; (void)xi; ++h_sign_calls;
    int32_t pid = pid_of(info);
    ladd(link_in(P32(h_db.live_links), USER_TEAM), pid, spec ? *(const uint32_t *)spec : 0);
    lremove(link_in(P32(h_db.live_links), from), pid);
    if (calc) calculate_links_impl();
}
static void m_sell(PlayerInfo *info, int32_t buyer, const void *spec, int32_t calc) {
    (void)buyer; (void)spec; ++h_sell_calls;             /* the game ignores the buyer */
    lremove(link_in(P32(h_db.live_links), USER_TEAM), pid_of(info));
    if (calc) calculate_links_impl();
}
static void m_dev_add(int32_t pid, int32_t a) { (void)pid; (void)a; ++h_dev_adds; }
static void m_save_profile(void *p, int32_t mode) { (void)p; (void)mode; ++h_saves; }
static void *m_dlopen(const char *name, int32_t flags) { (void)name; (void)flags; return (void *)1; }
static int32_t h_set_credits_calls;
static int32_t *h_coins(void) { return (int32_t *)(uintptr_t)(FAKE_BASE + 0x84A260 + 0x2A7CC); }
static void m_set_credits(int32_t value) { ++h_set_credits_calls; *h_coins() = value; }
/* post-match award hook mocks */
static int32_t h_mock_division = 5, h_mock_tid = 5, h_match_credits = -1;
static uint8_t h_mock_tournament[16];
static int32_t m_user_league(void *season) { (void)season; return h_mock_division; }
static void *m_tournament_by_turn(void *season, int32_t turn) { (void)season; (void)turn; return h_mock_tournament; }
static int32_t m_tournament_id(void *t) { (void)t; return h_mock_tid; }
static int32_t m_is_league(int32_t tid) { return tid >= 0 && tid <= 6; }
static void m_set_match_credits(uint32_t profile, int32_t total) { (void)profile; h_match_credits = total; }
static void m_unexpected(void) { fprintf(stderr, "UNEXPECTED game UI call from bridge in test\n"); exit(3); }

/* ---- serializer: mirrors CFTTSerialize::SerializeInternal<u64> (+0x18 file version, +0x1C writing) ---- */
typedef struct {
    uint8_t pad0[0x18];
    int32_t version;
    uint8_t writing;
    uint8_t pad1[3];
    uint8_t *buf;
    size_t pos, cap;
} HSerializer;
static void m_serialize_u64(void *ser_v, uint64_t *value, int32_t min_version) {
    HSerializer *ser = (HSerializer *)ser_v;
    if (ser->writing) {
        if (ser->pos + 8 > ser->cap) { ser->cap = ser->cap ? ser->cap * 2 : 1 << 20; ser->buf = realloc(ser->buf, ser->cap); }
        memcpy(ser->buf + ser->pos, value, 8); ser->pos += 8; return;
    }
    if (ser->version < min_version) return;
    if (ser->pos + 8 > ser->cap) { fprintf(stderr, "serializer read past end\n"); exit(4); }
    memcpy(value, ser->buf + ser->pos, 8); ser->pos += 8;
}

static void thunk(uint32_t offset, void *target) {
    uint8_t *at = (uint8_t *)(uintptr_t)(FAKE_BASE + offset);
    int64_t rel = (int64_t)(intptr_t)target - (int64_t)(intptr_t)(at + 5);
    if (rel != (int32_t)rel) { fputs("thunk target out of range\n", stderr); exit(2); }
    at[0] = 0xE9;
    *(int32_t *)(at + 1) = (int32_t)rel;
}

static void install_fake_library(void) {
    mallopt(M_MMAP_MAX, 0);          /* keep heap blocks in the low brk heap (32-bit game pointers) */
    void *p = mmap((void *)(uintptr_t)FAKE_BASE, FAKE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != (void *)(uintptr_t)FAKE_BASE) { fprintf(stderr, "cannot map fake base\n"); exit(2); }
    memset(p, 0xCC, 0x500000);   /* int3 over the code range: any unmocked call traps */
    memset((uint8_t *)p + 0x500000, 0, FAKE_SIZE - 0x500000);
    thunk(0x20C029, m_link_count);      thunk(0x20BF6D, m_link_by_index);
    thunk(0x20C3A1, m_team_value);      thunk(0x210885, m_valid_team);
    thunk(0x20DA0D, m_player_info);     thunk(0x2B35D1, m_rating);
    thunk(0x212B3D, m_value);           thunk(0x36A67D, m_turn);
    thunk(0x36B031, m_end_turn);        thunk(0x36AA89, m_season_count);
    thunk(0x20A621, m_specific);        thunk(0x20A40D, m_add_to_link);
    thunk(0x209DD9, m_remove_from_link);thunk(0x20CBD5, m_sign);
    thunk(0x20CC69, m_sell);            thunk(0x20E131, m_dev_add);
    thunk(0x2093B1, m_calc_links);      thunk(0x375B89, m_save_profile);
    thunk(0x3FFAFD, m_serialize_u64);   thunk(0x1D90FC, m_dlopen);
    thunk(0x20BF85, m_set_override);    thunk(0x213071, m_can_remove);
    thunk(0x2102D5, m_can_add);         thunk(0x209E6D, m_verify_link);
    thunk(0x20A751, m_generate_simple); thunk(0x20A857, m_sort_simple);
    thunk(0x1C05E0, m_delete_array);    thunk(0x20B71D, m_team_rating);
    thunk(0x20C0A9, m_international);   thunk(0x20C08F, m_miscellaneous);
    thunk(0x20C049, m_classic);
    thunk(0x36CC41, m_user_league);     thunk(0x36A8FD, m_tournament_by_turn);
    thunk(0x360A31, m_tournament_id);   thunk(0x3646A1, m_is_league);
    thunk(0x3776A5, m_set_match_credits);
    thunk(0x36B02B, m_start_turn);      thunk(0x36B037, m_start_league_turn);         thunk(0x26348D, m_set_credits);
    uint32_t ui[] = {0x1C06AC, 0x20C0C9, 0x241FB5, 0x24D785, 0x24DD59, 0x2938CD, 0x298609};
    for (size_t i = 0; i < sizeof(ui) / sizeof(ui[0]); ++i) thunk(ui[i], m_unexpected);
    *(uint32_t *)(uintptr_t)(FAKE_BASE + DB_INSTANCE) = U32(&h_db);
}

static int cmp_team(const void *a, const void *b) { return *(const int32_t *)a - *(const int32_t *)b; }

static void load_dataset(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    static uint8_t links[H_MAX_LINKS * LINK_STRIDE];
    int32_t n = 0;
    char tag[4];
    while (fscanf(f, "%3s", tag) == 1) {
        if (tag[0] == 'T') {
            int32_t team, valid, tier, count;
            if (fscanf(f, "%d %d %d %d", &team, &valid, &tier, &count) != 4) exit(5);
            uint8_t *l = links + n++ * LINK_STRIDE;
            memset(l, 0, LINK_STRIDE);
            *(int32_t *)l = team;
            h_valid[team] = (uint8_t)valid;
            h_kind[team] = (uint8_t)tier;
            for (int32_t i = 0; i < count; ++i) {
                int32_t pid; if (fscanf(f, "%d", &pid) != 1) exit(5);
                if (i < 32) ladd(l, pid, (uint32_t)(i + 1) | ((i < 11) ? 0x10000u : 0));
            }
        } else if (tag[0] == 'P') {
            int32_t pid, pos, rating, age, value;
            if (fscanf(f, "%d %d %d %d %d", &pid, &pos, &rating, &age, &value) != 5) exit(5);
            HPlayer *p = &h_players[pid];
            p->known = 1; p->pos = pos; p->rating = rating; p->age = age; p->value = value;
        } else { int c; while ((c = fgetc(f)) != '\n' && c != EOF) {} }
    }
    fclose(f);
    qsort(links, (size_t)n, LINK_STRIDE, cmp_team);
    h_db.link_count = n;
    memcpy(h_pristine, links, (size_t)n * LINK_STRIDE);
    h_db.default_links = U32(malloc((size_t)n * LINK_STRIDE));
    h_db.live_links = U32(malloc((size_t)n * LINK_STRIDE));
    memcpy(P32(h_db.default_links), h_pristine, (size_t)n * LINK_STRIDE);
    memcpy(P32(h_db.live_links), h_pristine, (size_t)n * LINK_STRIDE);
}

/* ---- invariants ---- */
static void fail(const char *what, int32_t a, int32_t b) {
    if (!h_quiet && h_violations < 40) fprintf(stderr, "  VIOLATION: %s (%d, %d)\n", what, a, b);
    ++h_violations;
}
static int32_t live_owner(int32_t pid) {
    for (int32_t i = 0; i < h_db.link_count; ++i) {
        uint8_t *l = P32(h_db.live_links) + i * LINK_STRIDE;
        if (is_real_club(*(int32_t *)l) && lfind(l, pid) >= 0) return *(int32_t *)l;
    }
    return -1;
}
static void check_rosters(void) {
    static int32_t seen[0x10000];
    memset(seen, 0, sizeof(seen));
    for (int32_t i = 0; i < h_db.link_count; ++i) {
        uint8_t *l = P32(h_db.live_links) + i * LINK_STRIDE;
        int32_t team = *(int32_t *)l, n = *lcount(l), gk = 0;
        if (n < 0 || n > 32) fail("link count out of range", team, n);
        for (int32_t j = 0; j < n; ++j) {
            int32_t pid = lids(l)[j];
            if (is_real_club(team) && seen[pid]++) fail("player linked to two clubs", pid, team);
            if (h_players[pid].pos == 0) ++gk;
        }
        if (is_real_club(team)) {
            if (n < 16) fail("market club below 16 players", team, n);
            if (gk < 1) fail("market club without goalkeeper", team, n);
        }
    }
}
/* the market's cached ownership must match the live table after every tick */
static void check_cache_vs_live(void) {
    for (int32_t i = 0; i < g_player_count; ++i) {
        int32_t owner = live_owner(g_players[i].player_id);
        if (owner != g_players[i].owner_id) fail("market cache owner != live roster", g_players[i].player_id, owner);
    }
}
static void check_market(void) {
    if (!state_is_valid()) fail("state_is_valid() false", g_market.season, g_market.current_turn);
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        ClubAccount *c = &g_market.clubs[i];
        if (c->team_id < 0) continue;
        if (c->transfer_budget > c->cash) fail("transfer budget above cash", c->team_id, c->transfer_budget - c->cash);
    }
}

static void h_test_bid_term_controls(void) {
    int32_t old_fee = g_ui_bid_fee;
    int32_t old_wage = g_ui_bid_wage;
    int32_t old_years = g_ui_bid_contract_years;
    g_ui_bid_fee = 1000;
    g_ui_bid_wage = 500;
    g_ui_bid_contract_years = 3;

    if (!ui_adjust_bid_terms(0) || g_ui_bid_fee != 900 || g_ui_bid_wage != 500)
        fail("offer terms fee -10%", g_ui_bid_fee, 900);
    if (!ui_adjust_bid_terms(1) || g_ui_bid_fee != 990 || g_ui_bid_wage != 500)
        fail("offer terms fee +10%", g_ui_bid_fee, 990);
    if (!ui_adjust_bid_terms(2) || g_ui_bid_wage != 450 || g_ui_bid_fee != 990)
        fail("offer terms wage -10%", g_ui_bid_wage, 450);
    if (!ui_adjust_bid_terms(3) || g_ui_bid_wage != 495 || g_ui_bid_fee != 990)
        fail("offer terms wage +10%", g_ui_bid_wage, 495);

    g_ui_bid_fee = 1;
    if (!ui_adjust_bid_terms(0) || g_ui_bid_fee != 1)
        fail("offer terms positive fee floor", g_ui_bid_fee, 1);
    if (!ui_adjust_bid_terms(1) || g_ui_bid_fee != 2)
        fail("offer terms minimum fee step", g_ui_bid_fee, 2);
    g_ui_bid_contract_years = 1;
    if (!ui_adjust_bid_terms(4) || g_ui_bid_contract_years != 1)
        fail("offer terms minimum years", g_ui_bid_contract_years, 1);
    if (!ui_adjust_bid_terms(5) || g_ui_bid_contract_years != 2)
        fail("offer terms year increment", g_ui_bid_contract_years, 2);
    g_ui_bid_contract_years = 5;
    if (!ui_adjust_bid_terms(5) || g_ui_bid_contract_years != 5)
        fail("offer terms maximum years", g_ui_bid_contract_years, 5);
    if (!ui_adjust_bid_terms(4) || g_ui_bid_contract_years != 4)
        fail("offer terms year decrement", g_ui_bid_contract_years, 4);
    if (ui_adjust_bid_terms(6) || ui_adjust_bid_terms(7) ||
        ui_adjust_bid_terms(8) || ui_adjust_bid_terms(9))
        fail("offer terms non-step actions", 1, 0);

    g_ui_bid_fee = old_fee;
    g_ui_bid_wage = old_wage;
    g_ui_bid_contract_years = old_years;
    if (!h_quiet) puts("offer terms: fee/wage steps and 1-5 year limits pass");
}

/* ---- saves ---- */
typedef struct { uint8_t *buf; size_t size; uint8_t user_link[LINK_STRIDE]; } HSave;
static HSave save_game(void) {
    HSerializer w; memset(&w, 0, sizeof(w)); w.writing = 1; w.version = 0xB5;
    career_market_on_serialize(h_season(), &w, FAKE_BASE);
    HSave s; s.buf = w.buf; s.size = w.pos;
    memcpy(s.user_link, link_in(P32(h_db.live_links), USER_TEAM), LINK_STRIDE);   /* SerializeDreamTeam */
    return s;
}
/* A fresh process: links file reloaded, bridge statics reset, then the career save loaded. */
static void restart_and_load(const HSave *s, int32_t file_version) {
    memcpy(P32(h_db.default_links), h_pristine, (size_t)h_db.link_count * LINK_STRIDE);
    memcpy(P32(h_db.live_links), h_pristine, (size_t)h_db.link_count * LINK_STRIDE);
    memcpy(link_in(P32(h_db.live_links), USER_TEAM), s->user_link, LINK_STRIDE);
    calculate_links_impl();                                /* SerializeDreamTeam load */
    g_pristine_count = 0; g_links_dirty = 0; g_library_pinned = 0;
    g_player_count = 0; g_avg_revenue = 0; g_market_changed = 0; g_market_dirty = 0;
    memset(g_club_rep, 0, sizeof(g_club_rep)); memset(g_top_rating, 0, sizeof(g_top_rating));
    memset(g_group_count, 0, sizeof(g_group_count)); memset(g_group_rating_sum, 0, sizeof(g_group_rating_sum));
    memset(g_total_count, 0, sizeof(g_total_count)); memset(g_total_rating_sum, 0, sizeof(g_total_rating_sum));
    memset(g_players, 0, sizeof(g_players)); memset(g_rating_dirty, 0, sizeof(g_rating_dirty));
    HSerializer r; memset(&r, 0, sizeof(r)); r.version = file_version; r.buf = s->buf; r.cap = s->size;
    career_market_on_serialize(h_season(), &r, FAKE_BASE);
    if (file_version >= 0xB5 && r.pos != s->size) fail("load did not consume the whole market block", (int)r.pos, (int)s->size);
}
static int restart_roundtrip(void) {
    static uint8_t market_before[sizeof(g_market)];
    static uint8_t live_before[H_MAX_LINKS * LINK_STRIDE];
    static uint8_t ext_before[sizeof(g_ext)];
    static uint8_t ext2_before[sizeof(g_ext2)];
    memcpy(market_before, &g_market, sizeof(g_market));
    memcpy(ext_before, &g_ext, sizeof(g_ext));
    memcpy(ext2_before, &g_ext2, sizeof(g_ext2));
    memcpy(live_before, P32(h_db.live_links), (size_t)h_db.link_count * LINK_STRIDE);
    HSave s = save_game();
    restart_and_load(&s, 0xB5);
    int ok = 1;
    if (memcmp(market_before, &g_market, sizeof(g_market)) != 0) { fail("market state changed across save/restart", 0, 0); ok = 0; }
    if (memcmp(ext_before, &g_ext, sizeof(g_ext)) != 0) { fail("extension block changed across save/restart", 0, 0); ok = 0; }
    if (memcmp(ext2_before, &g_ext2, sizeof(g_ext2)) != 0) { fail("books block changed across save/restart", 0, 0); ok = 0; }
    for (int32_t i = 0; i < h_db.link_count; ++i) {
        uint8_t *a = live_before + i * LINK_STRIDE, *b = P32(h_db.live_links) + i * LINK_STRIDE;
        if (*lcount(a) != *lcount(b)) { fail("roster size changed across save/restart", *(int32_t *)a, *lcount(b) - *lcount(a)); ok = 0; continue; }
        for (int32_t j = 0; j < *lcount(a); ++j) if (lfind(b, lids(a)[j]) < 0) { fail("roster changed across save/restart", *(int32_t *)a, lids(a)[j]); ok = 0; }
    }
    free(s.buf);
    return ok;
}

/* ---- deterministic RNG for simulated user behaviour ---- */
static uint32_t h_rng = 12345;
static uint32_t rnd(void) { h_rng = h_rng * 1103515245u + 12345u; return h_rng >> 8; }

static int32_t h_user_buys, h_user_sales, h_user_bids;
/* A sensible manager: list the weakest surplus players when the squad is big, answer AI bids,
 * and each window turn go for the best-value upgrade the club can afford, haggling from 90% of the
 * asking price and walking away when a rival pushes the price past what it is worth. */
static int32_t h_insults, h_rival_events, h_rival_signed, h_counters, h_quick_sales;
static void simulate_user(void) {
    if (g_market.window_id < 0) return;
    int32_t user_index = find_account(USER_TEAM);
    if (user_index < 0) return;
    ClubAccount *user = &g_market.clubs[user_index];
    /* list surplus: squad above 24 -> list the weakest non-starters, at most two listed */
    int32_t listed = 0;
    for (int32_t i = 0; i < USER_LISTING_CAPACITY; ++i) if (g_user_listed_player_ids[i] >= 0) ++listed;
    while (listed < 2 && g_total_count[user_index] - listed > 24) {
        int32_t worst = -1;
        for (int32_t k = 0; k < g_player_count; ++k) {
            MarketPlayer *p = &g_players[k];
            if (p->owner_id != USER_TEAM || p->listed_for_sale || player_role(p) >= ROLE_STARTER) continue;
            if (p->position == 0 && g_group_count[user_index][0] <= 2) continue;
            if (worst < 0 || p->rating < g_players[worst].rating) worst = k;
        }
        if (worst < 0 || toggle_user_listing(g_players[worst].player_id) != 0) break;
        ++listed;
    }
    /* respond to AI offers: accept fair ones, push once on low ones */
    CareerMarketOfferView view;
    for (int32_t k = career_market_get_offer_count() - 1; k >= 0; --k) {
        if (!career_market_get_offer(k, &view) || view.seller_id != USER_TEAM) continue;
        int32_t pi = find_cached_player(view.player_id);
        int32_t value = pi >= 0 ? g_players[pi].value : view.fee;
        int32_t before = h_sell_calls;
        int32_t current = view.counter_fee > 0 ? view.counter_fee : view.fee;
        if (current >= mul_div(value, 95, 100) || (pi >= 0 && g_players[pi].listed_for_sale && current >= mul_div(value, 80, 100)))
            career_market_respond_to_offer(FAKE_BASE, view.offer_id, CM_DECISION_ACCEPT, 0);
        else if (view.counter_fee == 0)
            career_market_respond_to_offer(FAKE_BASE, view.offer_id, CM_DECISION_COUNTER, mul_div(value, 110, 100));
        else
            career_market_respond_to_offer(FAKE_BASE, view.offer_id, CM_DECISION_REJECT, 0);
        if (h_sell_calls != before) ++h_user_sales;
    }
    /* clear deadwood with "Get offers now" once the squad passes 25 */
    for (int32_t guard = 0; guard < 3 && g_total_count[user_index] > 25; ++guard) {
        int32_t worst = -1;
        for (int32_t k = 0; k < g_player_count; ++k) {
            MarketPlayer *p = &g_players[k];
            if (p->owner_id != USER_TEAM || player_role(p) >= ROLE_STARTER || has_active_offer_for_player(p->player_id)) continue;
            if (p->position == 0 && g_group_count[user_index][0] <= 2) continue;
            if (worst < 0 || p->rating < g_players[worst].rating) worst = k;
        }
        if (worst < 0) { if (getenv("H_DEBUG_BIDS")) fprintf(stderr, "noworst s%d\n", g_market.season); break; }
        int32_t fee = 0, wage = 0, before = h_sell_calls;
        int32_t buyer = best_ai_bid_for_user_player(worst, &fee, &wage);
        MarketOffer *offer = buyer >= 0 ? create_ai_offer_for_user(buyer, worst, g_market.window_id, g_market.current_turn,
                                                                  g_market.season, fee, wage) : 0;
        int32_t worst_id = g_players[worst].player_id;
        int32_t r1 = offer ? career_market_respond_to_offer(FAKE_BASE, offer->offer_id, CM_DECISION_ACCEPT, 0) : -99;
        int32_t r2 = -99;
        if (h_sell_calls == before) { r2 = career_market_quick_sale(FAKE_BASE, worst_id); if (r2 > 0) ++h_quick_sales; }
        if (getenv("H_DEBUG_BIDS")) fprintf(stderr, "sell fail%d s%d pid %d offer %d quick %d\n", g_commit_fail, g_market.season, worst_id, r1, r2);
        if (h_sell_calls == before) break;
        ++h_user_sales;
    }
    if (g_total_count[user_index] >= SQUAD_MAX - 1) {
        if (getenv("H_DEBUG_BIDS")) fprintf(stderr, "full s%d t%d size %d\n", g_market.season, g_market.current_turn, g_total_count[user_index]);
        return;
    }
    /* best upgrade per coin that the club can afford, keeping a season of wages */
    int32_t best = -1; int64_t best_score = 0; int32_t best_ask = 0;
    int32_t coins = user_coins(FAKE_BASE);
    for (int32_t k = 0; k < g_player_count; ++k) {
        MarketPlayer *p = &g_players[k];
        if (p->owner_id == USER_TEAM || p->owner_index < 0 || p->value <= 0) continue;
        int32_t improvement = starter_improvement(p, user_index);
        if (improvement < 2) continue;
        if (!player_will_join(p, user_index) || !starter_move_allowed(p, user_index)) continue;
        if (talk_strikes(p->player_id) >= TALK_STRIKE_LIMIT || has_active_offer_for_player(p->player_id)) continue;
        if (!club_can_sell_now(p)) continue;
        int32_t ask = user_bid_asking(p, user_index);
        int32_t wage = transfer_wage_demand(p, user_index, p->owner_index);
        if (coins - ask < clamp_add(user->payroll, wage) + 100) continue;
        int64_t score = (int64_t)improvement * improvement * 100000 / (ask + 50);
        if (best < 0 || score > best_score) { best = k; best_score = score; best_ask = ask; }
    }
    if (best < 0) {
        if (getenv("H_DEBUG_BIDS")) {
            int32_t c_imp = 0, c_will = 0, c_talk = 0, c_money = 0;
            for (int32_t k = 0; k < g_player_count; ++k) {
                MarketPlayer *q = &g_players[k];
                if (q->owner_id == USER_TEAM || q->owner_index < 0 || q->value <= 0) continue;
                if (starter_improvement(q, user_index) < 2) continue;
                ++c_imp;
                if (!player_will_join(q, user_index) || !starter_move_allowed(q, user_index)) continue;
                ++c_will;
                if (talk_strikes(q->player_id) >= TALK_STRIKE_LIMIT || has_active_offer_for_player(q->player_id)) continue;
                ++c_talk;
                int32_t a = user_bid_asking(q, user_index), w = transfer_wage_demand(q, user_index, q->owner_index);
                if (coins - a < clamp_add(user->payroll, w) + 100) continue;
                ++c_money;
            }
            fprintf(stderr, "nobid s%d t%d size %d coins %d payroll %d imp %d will %d talk %d money %d\n", g_market.season,
                    g_market.current_turn, g_total_count[user_index], coins, user->payroll, c_imp, c_will, c_talk, c_money);
        }
        return;
    }
    MarketPlayer *p = &g_players[best];
    int32_t before = h_sign_calls;
    int32_t wage = transfer_wage_demand(p, user_index, p->owner_index);
    if (getenv("H_DEBUG_BIDS")) fprintf(stderr, "pre role %d res %d ask %d top %d %d %d %d\n", player_role(p), seller_reservation(p, p->owner_index), user_bid_asking(p, user_index),
        g_top_rating[p->owner_index][p->position][0], g_top_rating[p->owner_index][p->position][1], g_top_rating[p->owner_index][p->position][2], g_top_rating[p->owner_index][p->position][3]);
    int32_t id = career_market_submit_user_bid(FAKE_BASE, p->player_id, mul_div(best_ask, 90, 100), wage, 3);
    ++h_user_bids;
    if (getenv("H_DEBUG_BIDS")) fprintf(stderr, "bid s%d t%d pid %d rating %d ask %d id %d event %d value %d role %d years %d index %d res %d now_ask %d\n", g_market.season, g_market.current_turn, p->player_id, p->rating, best_ask, id, g_neg_event, p->value, player_role(p), contract_seasons_to_expiry(p->player_id, p->owner_id, g_market.season), market_index_for(p), seller_reservation(p, p->owner_index), user_bid_asking(p, user_index));
    for (int32_t round = 0; id > 0 && round < 8; ++round) {
        MarketOffer *o = find_offer(id);
        if (!o) break;
        if (g_neg_event == NEG_INSULT) ++h_insults;
        if (g_neg_event == NEG_RIVAL_BID || g_neg_event == NEG_RIVAL_RAISED) ++h_rival_events;
        if (g_neg_event == NEG_COUNTER || g_neg_event == NEG_SOFTENED) ++h_counters;
        if (o->status == CM_OFFER_WAIT_USER_FEE) {
            /* do not chase a rival beyond 125% of the player's value or beyond the coins left */
            int32_t limit = mul_div(p->value, 125, 100);
            int32_t cap = user_coins(FAKE_BASE) - clamp_add(user->payroll, wage) - 100;
            if (o->counter_fee > limit || o->counter_fee > cap) {
                career_market_respond_to_offer(FAKE_BASE, id, CM_DECISION_REJECT, 0);
                if (g_neg_event == NEG_RIVAL_SIGNED) ++h_rival_signed;
                break;
            }
        }
        career_market_respond_to_offer(FAKE_BASE, id, CM_DECISION_ACCEPT, 0);
    }
    if (h_sign_calls != before) ++h_user_buys;
    else if (getenv("H_DEBUG_BIDS")) {
        MarketOffer *o = 0;
        for (int32_t i = 0; i < OFFER_CAPACITY; ++i) if (g_market.offers[i].offer_id == id) o = &g_market.offers[i];
        fprintf(stderr, "nosign commit_fail %d status %d rounds %d fee %d counter %d wage %d cwage %d event %d\n", g_commit_fail,
                o ? o->status : -1, o ? o->rounds : -1, o ? o->fee : -1, o ? o->counter_fee : -1, o ? o->annual_wage : -1,
                o ? o->counter_wage : -1, g_neg_event);
    }
}

/* ---- simulated user matches for the v6 economy ---- */
static const int32_t h_div_strength[6] = {82, 78, 74, 70, 66, 62};
static int32_t h_division = 5, h_points, h_in_cup = 1;
static uint32_t h_mrng = 777;
static uint32_t mrnd(void) { h_mrng = h_mrng * 1103515245u + 12345u; return h_mrng >> 8; }
static int64_t h_income_by_div[6], h_seasons_in_div[6];
static void h_sim_user_match(int32_t turn) {
    int32_t u = find_account(USER_TEAM);
    int32_t strength = u >= 0 ? g_market.clubs[u].strength : 60;
    int32_t league = turn >= 18 && turn <= 46 && (turn % 2) == 0;
    int32_t cup = turn >= 21 && turn <= 45 && ((turn - 21) % 4) == 0 && h_in_cup;
    if (turn == 18) { h_points = 0; h_in_cup = 1; }
    if (!league && !cup) return;
    int32_t p_win = 40 + (strength - h_div_strength[h_division]) * 3;
    if (p_win < 10) p_win = 10;
    if (p_win > 80) p_win = 80;
    int32_t roll = (int32_t)(mrnd() % 100);
    MatchFacts m; memset(&m, 0, sizeof(m));
    m.division = h_division; m.cup = cup; m.home = (turn / 2) % 2;
    m.capacity = mul_div(k_reference_capacity[h_division], REFERENCE_FILL_PCT, 100);   /* a typical crowd */
    if (roll < p_win) {
        m.goals_for = 1 + (int32_t)(mrnd() % 3);
        m.goals_against = m.goals_for - 1 - (int32_t)(mrnd() % 2);
        if (m.goals_against < 0) m.goals_against = 0;
    } else if (roll < p_win + 25) {
        m.goals_for = m.goals_against = (int32_t)(mrnd() % 3);
    } else {
        m.goals_against = 1 + (int32_t)(mrnd() % 3);
        m.goals_for = (int32_t)(mrnd() % (uint32_t)m.goals_against);
    }
    int32_t paid = user_match_payout(FAKE_BASE, &m, 1);
    h_income_by_div[h_division] += paid;
    if (league) h_points += m.goals_for > m.goals_against ? 3 : m.goals_for == m.goals_against ? 1 : 0;
    if (cup && m.goals_for <= m.goals_against) h_in_cup = 0;
    if (turn == 46) {
        /* 15 rounds, 45 points max: map points to a final position */
        int32_t position = 16 - (h_points * 15) / 40;
        if (position < 1) position = 1;
        if (position > 16) position = 16;
        int32_t promoted = position <= 2 && h_division > 0;
        int32_t relegated = position >= 14 && h_division < 5;
        int32_t end = user_season_end_payout(FAKE_BASE, h_division, position, promoted);
        h_income_by_div[h_division] += end;
        ++h_seasons_in_div[h_division];
        if (!h_quiet) printf("  user season: division %d position %d points %d strength %d season-end %d coins now %d\n",
                             h_division, position, h_points, strength, end, *h_coins());
        if (!h_quiet) {
            int32_t *t = g_ext.econ.this_season;
            printf("    books: prize %d gate %d bonus %d cup %d tv %d sponsor %d league %d promo %d sales %d purchases %d wages %d\n",
                   t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7], t[8], t[9], t[10]);
        }
        if (promoted) --h_division;
        else if (relegated) ++h_division;
    }
}

static uint64_t fnv(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}
static uint64_t state_digest(void) {
    uint64_t h = 1469598103934665603ull;
    h = fnv(h, &g_market, sizeof(g_market));
    h = fnv(h, &g_ext, sizeof(g_ext));
    h = fnv(h, &g_ext2, sizeof(g_ext2));
    h = fnv(h, g_contract_extension, sizeof(g_contract_extension));
    h = fnv(h, g_finance_ledger, sizeof(g_finance_ledger));
    h = fnv(h, g_user_listed_player_ids, sizeof(g_user_listed_player_ids));
    for (int32_t i = 0; i < h_db.link_count; ++i) {
        uint8_t *l = P32(h_db.live_links) + i * LINK_STRIDE;
        h = fnv(h, l, 8 + 4 * 32 * 0);          /* team id + count */
        h = fnv(h, lids(l), sizeof(int32_t) * (size_t)*lcount(l));
    }
    return h;
}

/* digest through the exported API only (the ARM build is stripped): clubs, history, live rosters */
static uint64_t api_digest(void) {
    uint64_t h = 1469598103934665603ull;
    for (int32_t i = 0; i < h_db.link_count; ++i) {
        int32_t team = *(int32_t *)(P32(h_db.live_links) + i * LINK_STRIDE);
        CareerMarketClubView v;
        if (career_market_get_club(team, &v)) h = fnv(h, &v, sizeof(v));
    }
    int32_t n = career_market_get_history_count();
    h = fnv(h, &n, sizeof(n));
    for (int32_t k = 0; k < n; ++k) {
        CareerMarketTransferView t;
        if (career_market_get_history(k, &t)) h = fnv(h, &t, sizeof(t));
    }
    for (int32_t i = 0; i < h_db.link_count; ++i) {
        uint8_t *l = P32(h_db.live_links) + i * LINK_STRIDE;
        h = fnv(h, l, 8);
        h = fnv(h, lids(l), sizeof(int32_t) * (size_t)*lcount(l));
    }
    return h;
}

static int cmp_int(const void *a, const void *b) { return *(const int32_t *)a - *(const int32_t *)b; }
static int32_t h_first_top_strength = -1, h_first_bottom_strength = -1;
/* squad sizes, thin positions, strength spread and squad-value concentration (Gini) */
static void season_extra_metrics(int32_t season) {
    int32_t sizes[MAX_CLUBS], strengths[MAX_CLUBS], n = 0, thin = 0, no_gk2 = 0;
    int64_t values[MAX_CLUBS];
    for (int32_t i = 0; i < MAX_CLUBS; ++i) {
        if (g_market.clubs[i].team_id < 0 || g_market.clubs[i].team_id == USER_TEAM) continue;
        sizes[n] = g_total_count[i]; strengths[n] = g_market.clubs[i].strength; values[n] = g_market.clubs[i].squad_value;
        for (int32_t p = 0; p < 4; ++p) if (g_group_count[i][p] < k_pos_min[p]) { ++thin; break; }
        if (g_group_count[i][0] < 2) ++no_gk2;
        ++n;
    }
    qsort(sizes, (size_t)n, sizeof(int32_t), cmp_int);
    qsort(strengths, (size_t)n, sizeof(int32_t), cmp_int);
    for (int32_t i = 1; i < n; ++i) for (int32_t j = i; j > 0 && values[j - 1] > values[j]; --j) { int64_t t = values[j]; values[j] = values[j - 1]; values[j - 1] = t; }
    int64_t sum = 0, weighted = 0;
    for (int32_t i = 0; i < n; ++i) { sum += values[i]; weighted += (int64_t)(i + 1) * values[i]; }
    int32_t gini = sum ? (int32_t)((2 * weighted * 1000) / (n * sum) - (1000L * (n + 1)) / n) : 0;
    int32_t top = 0, bottom = 0;
    for (int32_t i = 0; i < 10; ++i) { top += strengths[n - 1 - i]; bottom += strengths[i]; }
    if (h_first_top_strength < 0) { h_first_top_strength = top / 10; h_first_bottom_strength = bottom / 10; }
    printf("  season %d squads: size min/med/max=%d/%d/%d below_pos_min=%d one_gk=%d | strength top10=%d bottom10=%d "
           "(start %d/%d) median=%d | value gini=0.%03d\n", season, sizes[0], sizes[n / 2], sizes[n - 1], thin, no_gk2,
           top / 10, bottom / 10, h_first_top_strength, h_first_bottom_strength, strengths[n / 2], gini);
}

/* Which players a fresh user club may sign, by rating band (willingness rules only, and fee). */
static void eligibility_report(void) {
    build_player_cache(FAKE_BASE);
    int32_t u = find_account(USER_TEAM);
    ClubAccount *user = &g_market.clubs[u];
    printf("user rep=%d strength=%d value=%d\n", g_club_rep[u], user->strength, user->squad_value);
    for (int32_t lo = 55; lo <= 85; lo += 5) {
        int32_t total = 0, ok = 0, no_join = 0, no_sell = 0; int64_t ask = 0;
        for (int32_t k = 0; k < g_player_count; ++k) {
            MarketPlayer *p = &g_players[k];
            if (p->owner_id == USER_TEAM || p->owner_index < 0 || p->rating < lo || p->rating >= lo + 5) continue;
            ++total;
            int32_t j = player_will_join(p, u), s = starter_move_allowed(p, u);
            if (!j) ++no_join;
            if (!s) ++no_sell;
            if (j && s) { ++ok; ask += user_bid_asking(p, u); }
        }
        printf("  rating %d-%d: %4d players, signable %4d (refuse to join %4d, club won't sell %4d) avg ask %lld\n",
               lo, lo + 4, total, ok, no_join, no_sell, ok ? (long long)(ask / ok) : 0LL);
    }
}

/* Post-match award hook: a finished home match with stock rows already built. Checks the rows the
 * screen will show, the total handed to SetMatchCredits, and the books. */
static void h_stock_row(int32_t index, int32_t type, int32_t coins) {
    uint8_t *row = (uint8_t *)(uintptr_t)(FAKE_BASE + CREDIT_AWARD_ROWS) + index * CREDIT_AWARD_ROW_STRIDE;
    memset(row, 0, CREDIT_AWARD_ROW_STRIDE);
    *(int32_t *)row = type;
    ((uint16_t *)(row + 4))[0] = 'S';
    *(int32_t *)(row + 0x204) = coins;
}
static int32_t h_row_coins(int32_t index) {
    return *(int32_t *)((uint8_t *)(uintptr_t)(FAKE_BASE + CREDIT_AWARD_ROWS) + index * CREDIT_AWARD_ROW_STRIDE + 0x204);
}
static int32_t h_row_type(int32_t index) {
    return *(int32_t *)((uint8_t *)(uintptr_t)(FAKE_BASE + CREDIT_AWARD_ROWS) + index * CREDIT_AWARD_ROW_STRIDE);
}
static void h_test_match_awards(int32_t cup_final) {
    uint8_t *tinfo = (uint8_t *)(uintptr_t)(FAKE_BASE + MATCH_SETUP_INFO);
    uint8_t *tgame = (uint8_t *)(uintptr_t)(FAKE_BASE + TGAME_BASE);
    *(int32_t *)(tinfo + 0xfb0) = -1;           /* career match */
    *(int32_t *)(tinfo + 0x40) = 0;             /* user side 0 */
    *(int32_t *)(tinfo + 0xf6c) = USER_TEAM;    /* home */
    *(uint8_t *)(tinfo + 0x14) = 0;
    *(int32_t *)(tinfo + 0xf64) = 3600;         /* the Academy reference crowd */
    *(int32_t *)(tgame + 0x9ebc) = 0;
    *(int32_t *)(tgame + 0x9ed4) = 0;
    tgame[0x9edc] = 2; tgame[0x9edd] = 0;       /* won 2-0 */
    *(uint8_t *)(uintptr_t)(FAKE_BASE + MC_IN_POST_MATCH_CALLBACK) = 1;
    *(int32_t *)(uintptr_t)(FAKE_BASE + PROFILE_INSTANCE + PROFILE_PENDING_MATCH_CREDITS) = 7;
    h_mock_division = 5;
    h_mock_tid = cup_final ? 7 : 5;
    /* stock rows: result, goals, clean sheet, stadium, achievement 25, tournament award 50, total */
    h_stock_row(0, 0, 10); h_stock_row(1, 1, 2); h_stock_row(2, 2, 2); h_stock_row(3, 3, 3);
    h_stock_row(4, 4, 25); h_stock_row(5, 5, 50); h_stock_row(6, 8, 92);
    *(int32_t *)(uintptr_t)(FAKE_BASE + CREDIT_AWARD_COUNT) = 7;
    int32_t cup_before = book_total(ECON_CUP, 0), awards_before = book_total(ECON_AWARDS, 0);
    h_match_credits = -1;
    int32_t handled = career_market_on_match_awards(0, 0, FAKE_BASE);
    int32_t count = *(int32_t *)(uintptr_t)(FAKE_BASE + CREDIT_AWARD_COUNT);
    int32_t sum = 0;
    for (int32_t i = 0; i < count - 1; ++i) sum += h_row_coins(i);
    int32_t gate = econ_gate(5, 3600), win = cup_final ? 0 : econ_win_prize(5);
    if (!handled) fail("match awards not handled", cup_final, 0);
    if (count < 3 || count > 10 || h_row_type(count - 1) != 8) fail("award rows malformed", count, h_row_type(count - 1));
    if (h_row_coins(count - 1) != sum) fail("award total != rows", h_row_coins(count - 1), sum);
    if (h_match_credits != sum + 7) fail("SetMatchCredits total", h_match_credits, sum + 7);
    if (book_total(ECON_AWARDS, 0) - awards_before != 25) fail("achievement not booked", book_total(ECON_AWARDS, 0) - awards_before, 25);
    if (cup_final && book_total(ECON_CUP, 0) - cup_before < cup_winners_prize(5)) fail("cup win prize missing", book_total(ECON_CUP, 0) - cup_before, cup_winners_prize(5));
    if (!cup_final && sum != win + gate + 2 * econ_goal_bonus(5) + econ_clean_sheet_bonus(5) + 25)
        fail("league match rows", sum, win + gate + 2 * econ_goal_bonus(5) + econ_clean_sheet_bonus(5) + 25);
    if (!h_quiet) printf("match awards (%s): %d rows, total %d\n", cup_final ? "cup final" : "league", count, sum);
}

static void h_test_market_ui_pages(void) {
    int32_t offset = -1, count = -1;
    market_ui_page_window(10, -3, &offset, &count);
    if (offset != 0 || count != 4) fail("market page lower bound", offset, count);
    market_ui_page_window(10, 4, &offset, &count);
    if (offset != 4 || count != 4) fail("market page middle", offset, count);
    market_ui_page_window(10, 9, &offset, &count);
    if (offset != 8 || count != 2) fail("market page final partial", offset, count);
    market_ui_page_window(0, 7, &offset, &count);
    if (offset != 0 || count != 0) fail("market page empty", offset, count);
    if (market_ui_page_turn_offset(10, 0, 1) != 4 ||
        market_ui_page_turn_offset(10, 8, 1) != 8 ||
        market_ui_page_turn_offset(10, 8, -1) != 4 ||
        market_ui_page_turn_offset(10, 0, -1) != 0 ||
        market_ui_page_turn_offset(5, 0, 1) != 4 ||
        market_ui_page_turn_offset(5, 4, 1) != 4) {
        fail("market page turn bounds", market_ui_page_turn_offset(10, 8, 1), 8);
    }
    if (!h_quiet) printf("market UI pagination: 4-row pages, partial-page bounds and turn limits OK\n");
}

static void h_test_market_ui_action_layout(void) {
    const float heights[] = {768.0f, 300.0f};
    for (int32_t h = 0; h < (int32_t)(sizeof(heights) / sizeof(heights[0])); ++h) {
        float height = heights[h];
        float content_y = height * 0.28f;
        float content_h = height - content_y - height * 0.05f;
        for (int32_t options = 1; options <= 10; ++options) {
            int32_t columns = 0;
            float detail_h = 0.0f, option_top = 0.0f, option_height = 0.0f;
            market_ui_screen_action_layout(height, content_y, content_h, options, 1,
                                           &columns, &detail_h, &option_top, &option_height);
            int32_t rows = (options + columns - 1) / columns;
            float gap = height * 0.016f;
            float bottom = option_top + (float)rows * option_height + (float)(rows - 1) * gap;
            float detail_end = content_y + height * 0.095f + detail_h;
            float action_label_y = option_top - height * 0.040f;
            if (columns != (options >= 4 ? 2 : 1))
                fail("market UI action columns", columns, options);
            if (option_height + 0.1f < height * 0.055f)
                fail("market UI minimum button height", (int32_t)(option_height * 1000.0f),
                     (int32_t)(height * 55.0f));
            if (bottom > content_y + content_h - height * 0.05f + 0.1f)
                fail("market UI action bottom bound", (int32_t)(bottom * 100.0f),
                     (int32_t)((content_y + content_h - height * 0.05f) * 100.0f));
            if (detail_h > 0.0f && action_label_y < detail_end + height * 0.01f)
                fail("market UI detail/action separation", (int32_t)(action_label_y * 100.0f),
                     (int32_t)((detail_end + height * 0.01f) * 100.0f));
        }
        int32_t columns = 0;
        float detail_h = 0.0f, option_top = 0.0f, option_height = 0.0f;
        market_ui_screen_action_layout(height, content_y, content_h, 2, 1,
                                       &columns, &detail_h, &option_top, &option_height);
        if (detail_h <= 0.0f || columns != 1)
            fail("market UI empty-list detail area", (int32_t)(detail_h * 100.0f), columns);
    }
    if (!h_quiet) printf("market UI action layout: details and buttons fit across 1-10 options at tablet/minimum sizes OK\n");
}

static void h_test_market_ui_filter_toolbar(void) {
    const float heights[] = {768.0f, 300.0f};
    for (int32_t i = 0; i < (int32_t)(sizeof(heights) / sizeof(heights[0])); ++i) {
        float market = market_ui_screen_filter_height(heights[i], UI_SCREEN_LIST_MARKET);
        float sales = market_ui_screen_filter_height(heights[i], UI_SCREEN_LIST_SALES);
        float squad = market_ui_screen_filter_height(heights[i], UI_SCREEN_LIST_SQUAD);
        if (market < heights[i] * 0.089f || market > heights[i] * 0.091f)
            fail("market UI filter toolbar height", (int32_t)(market * 100.0f),
                 (int32_t)(heights[i] * 9.0f));
        if (sales != market)
            fail("sales UI filter toolbar height", (int32_t)(sales * 100.0f),
                 (int32_t)(market * 100.0f));
        if (squad != market)
            fail("squad UI club toolbar height", (int32_t)(squad * 100.0f),
                 (int32_t)(market * 100.0f));
        for (int32_t mode = UI_SCREEN_LIST_NONE; mode <= UI_SCREEN_LIST_SHORTLIST; ++mode) {
            if (mode != UI_SCREEN_LIST_MARKET && mode != UI_SCREEN_LIST_SALES &&
                mode != UI_SCREEN_LIST_SQUAD &&
                market_ui_screen_filter_height(heights[i], mode) != 0.0f)
                fail("market UI filter toolbar leaked", mode,
                     (int32_t)market_ui_screen_filter_height(heights[i], mode));
        }
    }
    if (!h_quiet) printf("market UI toolbars: reserved on Market, Sales and Squad at tablet/minimum sizes OK\n");
}

static void h_test_market_ui_action_schemes(void) {
    static const char *const primary[] = {
        "Continue", "Make Offer", "Confirm Sale", "Submit Offer", "Accept fee",
        "Accept counter", "Accept player wage", "Accept player counter",
        "Renew contract", "Submit renewal", "Get offers now"
    };
    for (int32_t i = 0; i < (int32_t)(sizeof(primary) / sizeof(primary[0])); ++i) {
        ui_set_option(0, primary[i]);
        if (market_ui_option_scheme(g_ui_options[0]) != 2)
            fail("market UI primary action scheme", i, market_ui_option_scheme(g_ui_options[0]));
    }
    static const char *const neutral[] = {
        "Back", "Walk away", "Reject", "Remove", "List / Unlist", "Position Filter"
    };
    for (int32_t i = 0; i < (int32_t)(sizeof(neutral) / sizeof(neutral[0])); ++i) {
        ui_set_option(0, neutral[i]);
        if (market_ui_option_scheme(g_ui_options[0]) != 0)
            fail("market UI neutral action scheme", i, market_ui_option_scheme(g_ui_options[0]));
    }
    if (!h_quiet) printf("market UI action schemes: 11 primary and 6 neutral labels classified OK\n");
}

static void h_test_market_ui_back_navigation(void) {
    static const char *const roots[] = {
        "Transfer Market", "Club Squad", "Player Sales", "Offers Inbox",
        "Club Finances", "Transfer History", "Shortlist"
    };
    g_ui_page_callback = (MarketUiCallback)0;
    g_ui_option_count = 2;
    for (int32_t i = 0; i < (int32_t)(sizeof(roots) / sizeof(roots[0])); ++i) {
        ui_set_title(roots[i]);
        if (!market_ui_screen_is_root_page()) fail("market UI root back page", i, 0);
    }
    static const char *const children[] = {
        "Offer Terms", "Quick Sale", "Club Finance Activity", "Season Books",
        "Contract Renewal", "Club Fee Negotiation"
    };
    for (int32_t i = 0; i < (int32_t)(sizeof(children) / sizeof(children[0])); ++i) {
        ui_set_title(children[i]);
        if (market_ui_screen_is_root_page()) fail("market UI child back page", i, 1);
    }
    ui_set_title("Player Sales");
    g_ui_page_callback = ui_callback_notice;
    g_ui_option_count = 1;
    if (market_ui_screen_is_root_page()) fail("market UI notice back page", 1, 0);
    g_ui_page_callback = (MarketUiCallback)0;
    g_ui_option_count = 0;
    if (!h_quiet) printf("market UI back navigation: 7 roots, 6 child pages and notice routing classified OK\n");
}

/* v31 option-box layer: chunking, one live box, per-box buffers, deferred pages. */
static uint8_t h_fake_queue[0x100];
static uint8_t h_fake_boxes[8][16];
static int32_t h_factory_calls;
static uint16_t h_factory_title[48];
static void *h_fake_factory(uint32_t base, MarketUiBox *box) {
    (void)base;
    void **slots = (void **)(h_fake_queue + UI_QUEUE_SLOTS);
    for (int32_t i = 0; i < UI_QUEUE_SLOT_COUNT; ++i) {
        if (slots[i]) continue;
        void *object = h_fake_boxes[h_factory_calls % 8];
        slots[i] = object;
        ui_wide_copy(h_factory_title, box->title, 48);
        ++h_factory_calls;
        return object;
    }
    return (void *)0;
}
static void h_fake_queue_delete(void *object) {
    void **slots = (void **)(h_fake_queue + UI_QUEUE_SLOTS);
    for (int32_t i = 0; i < UI_QUEUE_SLOT_COUNT; ++i) if (slots[i] == object) slots[i] = (void *)0;
}
static int32_t h_page_selection;
static int32_t h_page_callback(int32_t selection) {
    static const char *const c_options[] = {"C0", "C1", "Back"};
    h_page_selection = selection;
    if (selection == 3) {
        ui_show_notice(1, "Page B", "b", UI_AFTER_CLOSE);
        ui_show_options(1, "Page C", g_ui_description, c_options, 3, h_page_callback);
    }
    return 1;
}

static void h_test_market_ui_boxes(void) {
    for (int32_t count = 1; count <= MARKET_UI_OPTION_LIMIT; ++count) {
        int32_t seen[MARKET_UI_OPTION_LIMIT] = {0};
        int32_t chunks = ui_box_chunk_count(count);
        for (int32_t chunk = 0; chunk < chunks; ++chunk) {
            int32_t map[MARKET_UI_BOX_BUTTONS];
            int32_t n = ui_box_chunk_layout(count, chunk, map), more = 0;
            if (n < 1 || n > MARKET_UI_BOX_BUTTONS) fail("option box button count", n, count);
            for (int32_t b = 0; b < n; ++b) {
                if (map[b] == MARKET_UI_MORE) ++more;
                else if (map[b] < 0 || map[b] >= count || seen[map[b]]++) fail("option box chunk map", map[b], count);
            }
            if (chunk + 1 < chunks && more != 1) fail("option box More button", chunk, count);
            if (chunks == 1 && more) fail("option box needless More", chunk, count);
        }
        for (int32_t i = 0; i < count; ++i) if (seen[i] != 1) fail("option box option lost", i, count);
    }
    for (int32_t i = 0; i < (int32_t)sizeof(h_fake_queue); ++i) h_fake_queue[i] = 0;
    uint32_t saved_base = g_ui_base;
    g_ui_queue_override = h_fake_queue;
    g_ui_box_factory = h_fake_factory;
    g_ui_live_box = (void *)0; g_ui_live_slot = -1; g_ui_pending_slot = -1; g_ui_in_callback = 0;
    h_factory_calls = 0;
    g_ui_base = 1;
    static const char *const a_options[] = {"A0", "A1", "A2", "A3", "A4"};
    g_ui_description[0] = 0;
    ui_show_options(1, "Page A", g_ui_description, a_options, 5, h_page_callback);
    if (h_factory_calls != 1 || !g_ui_live_box) fail("option box shown immediately", h_factory_calls, 1);
    MarketUiBox *first = &g_ui_boxes[g_ui_live_slot];
    if (first->button_count != 3 || !ui_wide_equals_ascii(first->buttons[0], "A0") ||
        !ui_wide_equals_ascii(first->buttons[2], "More..."))
        fail("option box first group", first->button_count, 3);
    void *box1 = g_ui_live_box;
    ui_box_callback(2);                                   /* More... */
    if (h_factory_calls != 1 || g_ui_pending_slot < 0 || g_ui_pending_slot == g_ui_live_slot)
        fail("option box More deferred to its own slot", g_ui_pending_slot, g_ui_live_slot);
    ui_box_flush(h_fake_queue);
    if (h_factory_calls != 1) fail("option box second box while first alive", h_factory_calls, 1);
    h_fake_queue_delete(box1);
    ui_box_flush(h_fake_queue);
    if (h_factory_calls != 2 || g_ui_pending_slot != -1) fail("option box shown after close", h_factory_calls, 2);
    MarketUiBox *second = &g_ui_boxes[g_ui_live_slot];
    if (!ui_wide_equals_ascii(second->buttons[0], "A2") || !ui_wide_equals_ascii(second->buttons[2], "A4"))
        fail("option box second group", second->button_map[0], 2);
    void *box2 = g_ui_live_box;
    int32_t live_slot = g_ui_live_slot;
    ui_box_callback(1);                                   /* A3 -> page opens B then C */
    if (h_page_selection != 3) fail("option box page index", h_page_selection, 3);
    if (h_factory_calls != 2) fail("option box opened inside a callback", h_factory_calls, 2);
    if (!ui_wide_equals_ascii(g_ui_boxes[live_slot].buttons[0], "A2") ||
        !ui_wide_equals_ascii(g_ui_boxes[live_slot].title, "Page A"))
        fail("option box live strings rewritten", live_slot, 0);
    h_fake_queue_delete(box2);
    ui_box_flush(h_fake_queue);
    if (h_factory_calls != 3 || !ui_wide_equals_ascii(h_factory_title, "Page C"))
        fail("option box last request wins", h_factory_calls, 3);
    void *box3 = g_ui_live_box;
    ui_show_notice(1, "Notice", "n", UI_AFTER_CLOSE);     /* entry request waits too */
    if (h_factory_calls != 3) fail("option box entry request deferred", h_factory_calls, 3);
    h_fake_queue_delete(box3);
    ui_box_flush(h_fake_queue);
    if (h_factory_calls != 4 || !ui_wide_equals_ascii(h_factory_title, "Notice"))
        fail("option box entry request shown", h_factory_calls, 4);
    h_fake_queue_delete(g_ui_live_box);                   /* deleted behind our back */
    ui_set_title("Keyboard");
    g_ui_keyboard_box = (void *)0;
    if (!ui_show_keyboard(1, 10, h_page_callback) || h_factory_calls != 5 || g_ui_keyboard_box != g_ui_live_box)
        fail("option box keyboard", h_factory_calls, 5);
    if (ui_bid_stage_to_v30(UI_BID_TERMS, 0) != 8 || ui_bid_stage_to_v30(UI_BID_TERMS, 1) != -1 ||
        ui_bid_stage_to_v30(UI_BID_TERMS, 2) != 9 || ui_bid_stage_to_v30(UI_BID_FEE, 2) != 6 ||
        ui_bid_stage_to_v30(UI_BID_WAGE, 1) != 3 || ui_bid_stage_to_v30(UI_BID_YEARS, 1) != 5)
        fail("offer terms stage map", ui_bid_stage_to_v30(UI_BID_FEE, 2), 6);
    g_ui_queue_override = (void *)0; g_ui_queue_hooked = (void *)0;
    g_ui_box_factory = ui_box_create_native;
    g_ui_live_box = (void *)0; g_ui_live_slot = -1; g_ui_pending_slot = -1;
    g_ui_keyboard_box = (void *)0;
    g_ui_base = saved_base;
    if (!h_quiet) printf("market option boxes: <=3 buttons, one live box, per-box buffers, deferred pages OK\n");
}

static float h_outline_rects[4][4];
static int32_t h_outline_count;
static void h_capture_outline(float x, float y, float w, float h, uint32_t color) {
    if (h_outline_count < 4) {
        h_outline_rects[h_outline_count][0] = x;
        h_outline_rects[h_outline_count][1] = y;
        h_outline_rects[h_outline_count][2] = w;
        h_outline_rects[h_outline_count][3] = h;
    }
    if (color != 0xFF4FAE91u) fail("market UI outline colour", (int32_t)color, 0);
    ++h_outline_count;
}

static void h_test_market_ui_card_outline(void) {
    h_outline_count = 0;
    market_ui_draw_card_outline(10.0f, 20.0f, 100.0f, 50.0f, 3.0f,
                                0xFF4FAE91u, h_capture_outline);
    if (h_outline_count != 4) fail("market UI outline edge count", h_outline_count, 4);
    if (h_outline_rects[0][0] != 7.0f || h_outline_rects[0][1] != 17.0f ||
        h_outline_rects[0][2] != 106.0f || h_outline_rects[0][3] != 3.0f ||
        h_outline_rects[1][1] != 70.0f || h_outline_rects[2][0] != 7.0f ||
        h_outline_rects[2][1] != 20.0f || h_outline_rects[2][2] != 3.0f ||
        h_outline_rects[2][3] != 50.0f || h_outline_rects[3][0] != 110.0f)
        fail("market UI outline geometry", (int32_t)h_outline_rects[0][2], 106);
    market_ui_draw_card_outline(0.0f, 0.0f, 0.0f, 50.0f, 3.0f,
                                0xFF4FAE91u, h_capture_outline);
    if (h_outline_count != 4) fail("market UI invalid outline", h_outline_count, 4);
    if (!h_quiet) printf("market UI selected-card outline: four accent edges and invalid bounds OK\n");
}

static void h_test_market_ui_sorting(void) {
    g_player_count = 5;
    const int32_t ratings[5] = {70, 90, 80, 80, 60};
    const int32_t values[5] = {300, 500, 100, 200, 400};
    const int32_t wages[5] = {50, 40, 30, 20, 10};
    for (int32_t i = 0; i < 5; ++i) {
        g_players[i].player_id = 100 + i;
        g_players[i].rating = ratings[i];
        g_players[i].value = values[i];
        g_players[i].wage = wages[i];
        g_players[i].position = 0;
    }
    const int32_t expected[3][5] = {
        {1, 2, 3, 0, 4},
        {2, 3, 0, 4, 1},
        {4, 3, 2, 1, 0}
    };
    for (int32_t mode = 0; mode < 3; ++mode) {
        const int32_t shuffled[5] = {4, 0, 3, 1, 2};
        for (int32_t i = 0; i < 5; ++i) g_ui_screen_all_targets[i] = shuffled[i];
        g_ui_screen_total_rows = 5;
        g_ui_market_sort = mode;
        market_ui_sort_market_targets();
        for (int32_t i = 0; i < 5; ++i) {
            if (g_ui_screen_all_targets[i] != expected[mode][i])
                fail("market UI sort order", mode * 10 + i, g_ui_screen_all_targets[i]);
        }
    }
    g_player_count = 0;
    g_ui_screen_total_rows = 0;
    g_ui_market_sort = 0;
    if (!h_quiet) printf("market UI sorting: rating high, value low and wage low orders OK\n");
}

static void h_test_sales_position_filter(void) {
    g_player_count = 4;
    const int32_t positions[4] = {0, 1, 2, 1};
    for (int32_t i = 0; i < 4; ++i) {
        memset(&g_players[i], 0, sizeof(g_players[i]));
        g_players[i].player_id = 200 + i;
        g_players[i].owner_id = i == 3 ? USER_TEAM_ID + 1 : USER_TEAM_ID;
        g_players[i].position = positions[i];
        g_players[i].value = 100;
    }
    g_ui_position_filter = 2;
    g_ui_sales_position_filter = 1;
    if (ui_screen_sales_player_visible(0)) fail("sales filter rejects goalkeeper", 1, 0);
    if (!ui_screen_sales_player_visible(1)) fail("sales filter accepts defender", 0, 1);
    if (ui_screen_sales_player_visible(2)) fail("sales filter rejects midfielder", 1, 0);
    if (ui_screen_sales_player_visible(3)) fail("sales filter rejects other club", 1, 0);
    g_ui_screen_list_mode = UI_SCREEN_LIST_SALES;
    ui_screen_collect_targets(1);
    if (g_ui_screen_total_rows != 1 || g_ui_screen_all_targets[0] != 1)
        fail("sales filtered card collection", g_ui_screen_total_rows, 1);
    if (g_ui_position_filter != 2) fail("sales filter leaves market filter", g_ui_position_filter, 2);
    g_player_count = 0;
    g_ui_screen_total_rows = 0;
    g_ui_screen_list_mode = UI_SCREEN_LIST_NONE;
    g_ui_position_filter = -1;
    g_ui_sales_position_filter = -1;
    if (!h_quiet) printf("market UI sales filter: independent state and card collection OK\n");
}


/* v35 transfer screen: market lists fed to the stock card grid, sorting, the SetupResults swap and
 * card-tap selection. Runs on the initialised market and restores every piece of state it touches. */
static int32_t h_ts_seen_count;
static uint32_t h_ts_seen_array;
static void h_fake_setup_results(void *screen) {
    (void)screen;
    const uint8_t *async = (const uint8_t *)(uintptr_t)(FAKE_BASE + TS_ASYNC_SEARCH);
    h_ts_seen_array = *(const uint32_t *)async;
    h_ts_seen_count = *(const int32_t *)(async + 0x2C);
}

static int32_t h_ts_rating(const uint8_t *entry) {
    int32_t index = ts_player_index(*(const int32_t *)entry);
    return index >= 0 ? g_players[index].rating : -1;
}

/* v36: every buy-side curve is continuous. Sweep ratings / contract time / bids and fail on any jump
 * bigger than a small step between adjacent inputs, and on role weights that do not add to 1000. */
/* v37 Transfer Market v2: every tab builds a sorted list, the shown join chance matches the refusal curves, and a
 * low bid is answered without a transfer. Market and extension state are restored afterwards. */
static void m_tm_player_name(uint16_t *out, int32_t cap, PlayerInfo *info, float width, int32_t a, int32_t b) {
    (void)width; (void)a; (void)b;
    char text[24];
    snprintf(text, sizeof(text), "Player %u", (unsigned)*(uint16_t *)&info->bytes[0]);
    int32_t i = 0;
    while (text[i] && i < cap - 1) { out[i] = (uint8_t)text[i]; ++i; }
    out[i] = 0;
}

static const uint16_t *m_tm_team_name(int32_t team_id, int32_t a, int32_t b) {
    (void)a; (void)b;
    static uint16_t name[16];
    char text[16];
    snprintf(text, sizeof(text), "Club %d", team_id);
    int32_t i = 0;
    while (text[i] && i < 15) { name[i] = (uint8_t)text[i]; ++i; }
    name[i] = 0;
    return name;
}

/* Club Hub league mock: the user and every 12th club (up to 15 rivals) form one league, ordered by strength. */
static int32_t m_hub_league_pos(void *league, int32_t team_id) {
    (void)league;
    int32_t members[16], n = 0;
    for (int32_t c = 0; c < MAX_CLUBS && n < 16; ++c) {
        ClubAccount *club = &g_market.clubs[c];
        if (club->team_id < 0) continue;
        if (club->team_id == USER_TEAM_ID || (c % 12 == 0 && n < 15)) members[n++] = c;
    }
    int32_t me = -1;
    for (int32_t i = 0; i < n; ++i) if (g_market.clubs[members[i]].team_id == team_id) me = members[i];
    if (me < 0) return -1;
    int32_t above = 0;
    for (int32_t i = 0; i < n; ++i) {
        ClubAccount *o = &g_market.clubs[members[i]];
        if (members[i] != me && (o->strength > g_market.clubs[me].strength ||
                                 (o->strength == g_market.clubs[me].strength && members[i] < me))) ++above;
    }
    return above;
}

static void h_test_hub(void) {
    static CareerMarketState saved_market;
    static uint8_t saved_ext[sizeof(g_ext)];
    static PlayerContract saved_contracts[CONTRACT_EXTENSION_CAPACITY];
    uint32_t base = FAKE_BASE;
    thunk(0x2938CD, m_tm_player_name);
    thunk(0x20C0C9, m_tm_team_name);
    thunk(0x36202F, m_hub_league_pos);
    void **league_slot = (void **)(uintptr_t)(base + PROFILE_INSTANCE + 0x14 + 0x6AC);
    void *saved_league = *league_slot;
    *league_slot = (void *)&h_db;                 /* any non-null pointer: the mock ignores it */
    sync_accounts(base);
    build_player_cache(base);
    memcpy(&saved_market, &g_market, sizeof(g_market));
    memcpy(saved_ext, &g_ext, sizeof(g_ext));
    memcpy(saved_contracts, g_contract_extension, sizeof(g_contract_extension));
    int32_t user_index = find_account(USER_TEAM_ID);
    if (user_index < 0) fail("hub: no user club", 0, 0);
    g_tm.base = base;
    hub_build(base);
    /* board */
    if (g_hub.league_n < 2 || g_hub.league_n > 16) fail("hub: league not found", g_hub.league_n, 0);
    if (g_hub.league_pos < 1 || g_hub.league_pos > g_hub.league_n) fail("hub: league position out of range", g_hub.league_pos, g_hub.league_n);
    if (g_hub.expected_x10 < 10 || g_hub.expected_x10 > g_hub.league_n * 10) fail("hub: board target out of range", g_hub.expected_x10, g_hub.league_n);
    if (g_hub.confidence_pm < 30 || g_hub.confidence_pm > 970) fail("hub: confidence out of range", g_hub.confidence_pm, 0);
    /* the target is continuous: one more point of squad strength never moves it by a full place */
    {
        int32_t before = g_hub.expected_x10;
        g_market.clubs[user_index].strength += 1;
        hub_board_build(base);
        int32_t step = before - g_hub.expected_x10;
        g_market.clubs[user_index].strength -= 1;
        hub_board_build(base);
        if (step < 0 || step > 10) fail("hub: board target jumped with one strength point", before, step);
    }
    /* contracts: every user player, contracts ending first */
    if (g_hub.row_count != (g_total_count[user_index] < HUB_ROWS_MAX ? g_total_count[user_index] : HUB_ROWS_MAX))
        fail("hub: contract rows do not match the squad", g_hub.row_count, g_total_count[user_index]);
    for (int32_t i = 1; i < g_hub.row_count; ++i)
        if (g_hub.rows[i - 1].tenths > g_hub.rows[i].tenths) fail("hub: contracts not sorted", i, g_hub.rows[i].tenths);
    for (int32_t i = 0; i < g_hub.row_count; ++i) {
        const HubRow *row = &g_hub.rows[i];
        if (g_players[row->index].owner_id != USER_TEAM_ID) fail("hub: contract row not a user player", i, 0);
        if (row->ask <= 0 || row->pay <= 0) fail("hub: renewal ask or sale value missing", row->ask, row->pay);
    }
    /* renewal at his full demand signs and extends the deal */
    g_market.clubs[user_index].wage_budget = clamp_add(g_market.clubs[user_index].payroll, 100000);
    int32_t signed_id = -1, countered = 0, ended = 0, rounds = 0, accepted = 0;
    for (int32_t i = 0; i < g_hub.row_count && signed_id < 0; ++i) {
        const HubRow *row = &g_hub.rows[i];
        if (!row->renewable) continue;
        int32_t id = row->player_id, before = row->tenths, counter = 0;
        int32_t r = hub_renewal_offer(base, id, row->ask, 4, &counter);
        if (r != 1) fail("hub: renewal at the full demand refused", r, id);
        build_player_cache(base);
        int32_t index = find_cached_player(id);
        if (index < 0 || g_players[index].wage != row->ask) fail("hub: renewed wage not applied", index, row->ask);
        if (contract_tenths_left(id, USER_TEAM_ID) <= before) fail("hub: renewal did not extend the contract", before, 0);
        signed_id = id;
    }
    if (signed_id < 0) fail("hub: no renewable player", g_hub.row_count, 0);
    /* a lowball gets a counter at or above his hidden point; offers keep using patience until talks end */
    hub_build(base);
    for (int32_t i = 0; i < g_hub.row_count && !ended; ++i) {
        const HubRow *row = &g_hub.rows[i];
        if (!row->renewable || row->player_id == signed_id) continue;
        int32_t id = row->player_id, ask = row->ask;
        while (rounds < 12) {
            int32_t counter = 0, left = renewal_patience_left(id);
            int32_t r = hub_renewal_offer(base, id, mul_div(ask, 50, 100), 2, &counter);
            ++rounds;
            if (r == -4) { ended = 1; break; }
            if (r != 0) fail("hub: lowball renewal not countered", r, id);
            if (counter <= mul_div(ask, 50, 100) || counter > ask) fail("hub: counter out of range", counter, ask);
            if (renewal_patience_left(id) >= left) fail("hub: a short offer used no patience", left, 0);
            ++countered;
        }
        if (!ended) fail("hub: renewal talks never ended", rounds, 0);
        if (hub_renewal_offer(base, id, ask, 2, (int32_t *)0) != -4) fail("hub: talks reopened in the same season", id, 0);
        break;
    }
    /* accepting a counter signs */
    hub_build(base);
    for (int32_t i = 0; i < g_hub.row_count && !accepted; ++i) {
        const HubRow *row = &g_hub.rows[i];
        if (!row->renewable || row->player_id == signed_id || renewal_patience_left(row->player_id) < 3000) continue;
        int32_t counter = 0;
        int32_t r = hub_renewal_offer(base, row->player_id, mul_div(row->ask, 80, 100), 3, &counter);
        if (r == 1) { accepted = 1; break; }
        if (r != 0) fail("hub: 80% renewal offer gave no counter", r, row->player_id);
        if (hub_renewal_offer(base, row->player_id, counter, 3, (int32_t *)0) != 1) fail("hub: accepting the counter did not sign", counter, 0);
        accepted = 1;
    }
    /* keep: kept starters get no unsolicited AI bids; listing him releases the keep */
    build_player_cache(base);
    int32_t kept = 0;
    for (int32_t i = 0; i < g_player_count && kept < 16; ++i) {
        MarketPlayer *p = &g_players[i];
        if (p->owner_id != USER_TEAM_ID || p->listed_for_sale) continue;
        if (user_keep_toggle(p->player_id) != 1) fail("hub: keep failed", p->player_id, kept);
        ++kept;
    }
    for (int32_t b = 0; b < MAX_CLUBS; ++b) {
        ClubAccount *buyer = &g_market.clubs[b];
        if (buyer->team_id < 0 || buyer->team_id == USER_TEAM_ID) continue;
        int32_t fee = 0, wage = 0, max = 0;
        int32_t t = ai_find_target(b, 1 << 28, 1 << 28, 1, &fee, &wage, &max);
        if (t >= 0 && g_players[t].owner_id == USER_TEAM_ID && user_is_kept(g_players[t].player_id))
            fail("hub: an AI club targeted a kept player", g_players[t].player_id, b);
    }
    {
        int32_t id = -1;
        for (int32_t i = 0; i < g_player_count && id < 0; ++i)
            if (g_players[i].owner_id == USER_TEAM_ID && user_is_kept(g_players[i].player_id) &&
                !user_signing_locked(g_players[i].player_id) && !has_active_offer_for_player(g_players[i].player_id)) id = g_players[i].player_id;
        if (id >= 0) {
            if (toggle_user_listing(id) != 0) fail("hub: could not list a kept player", id, 0);
            if (user_is_kept(id)) fail("hub: listing did not release the keep", id, 0);
            toggle_user_listing(id);
        }
    }
    memcpy(&g_market, &saved_market, sizeof(g_market));
    memcpy(&g_ext, saved_ext, sizeof(g_ext));
    memcpy(g_contract_extension, saved_contracts, sizeof(g_contract_extension));
    rebuild_contract_index();
    *league_slot = saved_league;
    for (int32_t i = 0; i < USER_LISTING_CAPACITY; ++i) g_user_listed_player_ids[i] = -1;
    sync_accounts(base);
    build_player_cache(base);
    ++g_price_epoch;
    thunk(0x2938CD, m_unexpected);
    thunk(0x20C0C9, m_unexpected);
    thunk(0x36202F, m_unexpected);
    if (!h_quiet) printf("club hub: board target %d.%d of %d (now %d), %d contracts, renewal at demand signs, "
                         "%d counters before talks ended, counter accepted, %d kept players skipped by AI bids OK\n",
                         g_hub.expected_x10 / 10, g_hub.expected_x10 % 10, g_hub.league_n, g_hub.league_pos,
                         g_hub.row_count, countered, kept);
}

static void h_test_tm2(void) {
    static CareerMarketState saved_market;
    static uint8_t saved_ext[sizeof(g_ext)];
    uint32_t base = FAKE_BASE;
    thunk(0x2938CD, m_tm_player_name);
    thunk(0x20C0C9, m_tm_team_name);
    sync_accounts(base);
    build_player_cache(base);
    memcpy(&saved_market, &g_market, sizeof(g_market));
    memcpy(saved_ext, &g_ext, sizeof(g_ext));
    int32_t user_index = find_account(USER_TEAM_ID);
    if (user_index < 0) fail("tm2: no user club", 0, 0);
    tm_reset_state();
    g_tm.base = base;
    for (int32_t tab = 0; tab < TM_TAB_COUNT; ++tab) {
        for (int32_t sort = 0; sort < TM_SORT_COUNT; ++sort) {
            g_tm.tab = tab;
            g_tm.sort = sort;
            tm_build(base);
            if (g_tm.row_count < 0 || g_tm.row_count > TM_MAX_ROWS) fail("tm2: row count out of range", tab, g_tm.row_count);
            for (int32_t r = 0; r < g_tm.row_count; ++r) {
                const TmRow *row = &g_tm.rows[r];
                const MarketPlayer *p = &g_players[row->index];
                if (p->player_id != row->player_id) fail("tm2: row points at the wrong player", tab, r);
                if (tab == TM_TAB_SQUAD && p->owner_id != USER_TEAM_ID) fail("tm2: squad row not a user player", r, 0);
                if ((tab == TM_TAB_SCOUT || tab == TM_TAB_FORYOU) && p->owner_id == USER_TEAM_ID)
                    fail("tm2: market row is a user player", tab, r);
                if (tab == TM_TAB_SCOUT && r > 0) {
                    const MarketPlayer *q = &g_players[g_tm.rows[r - 1].index];
                    if (tm_row_key(q, g_tm.rows[r - 1].price) < tm_row_key(p, row->price)) fail("tm2: scout list not sorted", sort, r);
                }
            }
            if (tab == TM_TAB_SCOUT && sort == TM_SORT_RATING && g_tm.row_count == 0) fail("tm2: empty scout list", 0, 0);
        }
    }
    /* position filter */
    g_tm.tab = TM_TAB_SCOUT; g_tm.sort = TM_SORT_RATING; g_tm.pos_filter = 2;
    tm_build(base);
    for (int32_t r = 0; r < g_tm.row_count; ++r)
        if (g_tm.rows[r].position != 2) fail("tm2: position filter leaked", r, g_tm.rows[r].position);
    g_tm.pos_filter = -1;
    tm_build(base);
    /* join chance vs the draws: over many players the share that would join tracks the mean chance */
    int32_t sum = 0, joins = 0, n = 0;
    for (int32_t i = 0; i < g_player_count && n < 400; ++i) {
        MarketPlayer *p = &g_players[i];
        if (p->owner_id == USER_TEAM_ID || p->owner_index < 0) continue;
        int32_t pm = tm_join_pm(p, user_index);
        if (pm < 0 || pm > 1000) fail("tm2: join chance out of range", pm, 0);
        sum += pm;
        joins += player_will_join(p, user_index) && starter_move_allowed(p, user_index);
        ++n;
    }
    if (n && (joins * 1000 / n < sum / n - 150 || joins * 1000 / n > sum / n + 150))
        fail("tm2: shown join chance does not match the decisions", joins * 1000 / n, sum / n);
    /* one low bid on the cheapest scout row inside an open window: answered, nothing signed */
    g_market.current_turn = 0;
    g_market.window_id = g_market.season * 2;
    g_tm.sort = TM_SORT_PRICE;
    tm_build(base);
    int32_t signed_before = g_total_count[user_index];
    if (g_tm.row_count > 0) {
        tm_open_sheet(base, g_tm.rows[0].player_id);
        if (!g_tm.neg_open || g_tm.log_count != 1) fail("tm2: sheet did not open", g_tm.neg_open, g_tm.log_count);
        g_tm.neg_fee = round5(g_tm.neg_lo);
        tm_submit(base);
        if (g_tm.log_count < 2) fail("tm2: bid got no reply", g_tm.log_count, 0);
        if (!h_quiet) {
            for (int32_t i = 0; i < g_tm.log_count; ++i) {
                char line[TM_TEXT];
                int32_t k = 0;
                for (; g_tm.log[i][k] && k < TM_TEXT - 1; ++k) line[k] = (char)g_tm.log[i][k];
                line[k] = 0;
                printf("  tm2 log %d: %s\n", g_tm.log_kind[i], line);
            }
        }
        build_player_cache(base);
        user_index = find_account(USER_TEAM_ID);
        if (g_total_count[user_index] != signed_before) fail("tm2: a low bid signed the player", signed_before, 0);
    }
    g_tm.neg_open = 0;
    memcpy(&g_market, &saved_market, sizeof(g_market));
    memcpy(&g_ext, saved_ext, sizeof(g_ext));
    sync_accounts(base);
    build_player_cache(base);
    ++g_price_epoch;
    thunk(0x2938CD, m_unexpected);
    thunk(0x20C0C9, m_unexpected);
    if (!h_quiet) printf("transfer market v2: lists for 5 tabs x 3 sorts, position filter, join chance (%d%% shown, %d%% drawn) and a low bid OK\n",
                         n ? sum / n / 10 : 0, n ? joins * 100 / n : 0);
}

static void h_test_continuous(void) {
    uint32_t base = FAKE_BASE;
    sync_accounts(base);
    build_player_cache(base);
    int32_t user_index = find_account(USER_TEAM_ID);
    for (int32_t t = 1; t <= 60; ++t) {
        int32_t a = contract_price_pct(t - 1), b = contract_price_pct(t);
        if (b < a || b - a > 2) fail("contract price curve jumps", t, b - a);
        a = contract_floor_pct(t - 1), b = contract_floor_pct(t);
        if (b < a || b - a > 2) fail("contract floor curve jumps", t, b - a);
    }
    int32_t checked = 0;
    for (int32_t i = 0; i < g_player_count && checked < 40; ++i) {
        MarketPlayer *player = &g_players[i];
        if (player->owner_index < 0 || player->owner_id == USER_TEAM_ID || player->position < 0 ||
            player->position > 3 || player->value <= 0) continue;
        ++checked;
        MarketPlayer saved = *player;
        RoleMix prev = {0, 0, 0, 0};
        int32_t prev_index = 0, prev_markup = 0, prev_premium = 0, prev_wage = 0;
        for (int32_t rating = 40; rating <= 99; ++rating) {
            player->rating = rating;
            RoleMix m = role_mix(player);
            if (m.surplus < 0 || m.rotation < 0 || m.starter < 0 || m.key < 0 ||
                m.surplus + m.rotation + m.starter + m.key != 1000) fail("role weights do not add up", rating, m.rotation);
            int32_t index = market_index_for(player);
            int32_t markup = asking_markup_pct(player);
            int32_t premium = user_index >= 0 ? user_price_premium_pct(player, user_index) : 0;
            int32_t wage = user_index >= 0 ? transfer_wage_demand(player, user_index, player->owner_index) : 0;
            if (rating > 40) {
                int32_t dw = 0;
                dw += m.surplus > prev.surplus ? m.surplus - prev.surplus : prev.surplus - m.surplus;
                dw += m.starter > prev.starter ? m.starter - prev.starter : prev.starter - m.starter;
                dw += m.key > prev.key ? m.key - prev.key : prev.key - m.key;
                if (dw > 800) fail("role weights jump between adjacent ratings", rating, dw);
                if (index - prev_index > 7 || prev_index - index > 7) fail("market index jumps", rating, index - prev_index);
                if (markup - prev_markup > 4 || prev_markup - markup > 4) fail("asking markup jumps", rating, markup - prev_markup);
                if (premium - prev_premium > 12 || prev_premium - premium > 12) fail("user premium jumps", rating, premium);
                if (prev_wage > 0 && (wage > prev_wage * 120 / 100 + 2 || wage * 120 / 100 + 2 < prev_wage))
                    fail("wage demand jumps", rating, wage - prev_wage);
            }
            prev = m; prev_index = index; prev_markup = markup; prev_premium = premium; prev_wage = wage;
        }
        *player = saved;
    }
    if (!checked) fail("continuity sweep found no players", 0, 0);
    /* the negotiation: patience cost is 0 at the reservation, one strike at INSULT_PCT, proportional between */
    int32_t full = (100 - INSULT_PCT) * 10;
    if (full * 1000 / full != 1000) fail("patience scale broken", full, 0);
    if (!h_quiet) printf("continuity: contract, role, market index, markup, premium and wage curves have no jumps (%d players)\n", checked);
}

static void h_test_transfer_screen(void) {
    uint32_t base = FAKE_BASE;
    sync_accounts(base);                           /* as every market entry point does */
    build_player_cache(base);
    if (!ts_enabled()) fail("transfer screen UI not enabled after market init", state_is_valid(), g_player_count);
    int32_t saved_shortlist[SHORTLIST_CAPACITY];
    memcpy(saved_shortlist, g_ext.shortlist, sizeof(saved_shortlist));
    int32_t saved_list = g_ts_list, saved_sort = g_ts_sort, saved_sel = g_ts_sel_id, saved_team = g_ts_sel_team;
    void *saved_screen = g_ts_screen;

    /* id -> index table matches the cache */
    for (int32_t i = 0; i < g_player_count; i += 97)
        if (ts_player_index(g_players[i].player_id) != i) fail("player index table", i, ts_player_index(g_players[i].player_id));

    /* Shortlist list: exactly the shortlisted players, owner as team, buy flag, strongest first */
    for (int32_t i = 0; i < SHORTLIST_CAPACITY; ++i) g_ext.shortlist[i] = -1;
    int32_t added = 0;
    for (int32_t i = 0; i < g_player_count && added < 5; i += 37) {
        if (g_players[i].owner_id == USER_TEAM_ID) continue;
        g_ext.shortlist[added++] = g_players[i].player_id;
    }
    g_ts_list = TS_LIST_SHORTLIST;
    g_ts_sort = TS_SORT_DEFAULT;
    ts_build_list(base);
    if (g_ts_row_count != added) fail("shortlist list size", g_ts_row_count, added);
    for (int32_t r = 0; r < g_ts_row_count; ++r) {
        const uint8_t *e = g_ts_rows + r * TS_ENTRY;
        int32_t id = *(const int32_t *)e, index = ts_player_index(id);
        if (shortlist_slot(id) < 0 || index < 0) fail("shortlist list entry not shortlisted", id, r);
        if (*(const int32_t *)(e + 4) != g_players[index].owner_id || e[0xAC] != 1)
            fail("shortlist list entry team / buy flag", id, *(const int32_t *)(e + 4));
        if (r && h_ts_rating(e) > h_ts_rating(e - TS_ENTRY)) fail("shortlist list not strongest first", r, h_ts_rating(e));
    }

    /* For You: only players who would sign now and fit the user's coins and wage room */
    int32_t user_index = find_account(USER_TEAM_ID);
    ClubAccount *club = &g_market.clubs[user_index];
    ClubAccount saved_club = *club;
    int32_t saved_coins = *h_coins();
    *h_coins() = 20000;                            /* a user with money to spend */
    club->cash = club->transfer_budget = 20000;
    club->wage_budget = club->payroll + 5000;
    ++g_price_epoch;
    g_ts_list = TS_LIST_FORYOU;
    ts_build_list(base);
    int32_t coins = user_coins(base), room = account_fee_room(club);
    if (room < coins) coins = room;
    if (g_ts_row_count < 1 || g_ts_row_count > TS_ROWS_MAX) fail("for-you list size", g_ts_row_count, TS_ROWS_MAX);
    for (int32_t r = 0; r < g_ts_row_count; ++r) {
        const uint8_t *e = g_ts_rows + r * TS_ENTRY;
        int32_t index = ts_player_index(*(const int32_t *)e);
        if (index < 0 || !ts_for_you(&g_players[index], user_index, coins, account_wage_room(club)))
            fail("for-you list holds an unaffordable or unwilling player", *(const int32_t *)e, r);
        if (r && h_ts_rating(e) > h_ts_rating(e - TS_ENTRY)) fail("for-you list not strongest first", r, h_ts_rating(e));
    }

    /* price sort: cheapest asking price first */
    g_ts_sort = TS_SORT_PRICE;
    ts_build_list(base);
    for (int32_t r = 1; r < g_ts_row_count; ++r) {
        int32_t a = ts_asking_price(*(const int32_t *)(g_ts_rows + (r - 1) * TS_ENTRY));
        int32_t b = ts_asking_price(*(const int32_t *)(g_ts_rows + r * TS_ENTRY));
        if (b < a) fail("price sort not cheapest first", a, b);
    }

    /* SetupResults for a market list: the stock body sees the market array only while it runs */
    uint8_t *async = (uint8_t *)(uintptr_t)(base + TS_ASYNC_SEARCH);
    uint8_t saved_async[0x38];
    memcpy(saved_async, async, sizeof(saved_async));
    *(uint32_t *)async = 0x12345678u;
    *(int32_t *)(async + 4) = 0;
    *(int32_t *)(async + 0x2C) = 77;
    ModCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.r[0] = 0x5000;
    ctx.lr = 0xABCDu;
    h_ts_seen_count = -1;
    if (ts_setup_hook(&ctx, base) != 0xABCDu) fail("setup hook did not return to the caller", 0, 0);
    if (h_ts_seen_count != g_ts_row_count || h_ts_seen_array != (uint32_t)(uintptr_t)g_ts_rows)
        fail("setup hook did not feed the market list", h_ts_seen_count, g_ts_row_count);
    if (*(uint32_t *)async != 0x12345678u || *(int32_t *)(async + 0x2C) != 77)
        fail("setup hook did not restore the stock results", *(int32_t *)(async + 0x2C), 77);
    *(int32_t *)(async + 4) = 1;                       /* a search job running: leave everything alone */
    h_ts_seen_count = -1;
    if (ts_setup_hook(&ctx, base) != 0 || h_ts_seen_count != -1) fail("setup hook ran during a search job", h_ts_seen_count, 0);
    *(int32_t *)(async + 4) = 0;

    /* stock search results are sorted in place when a sort is chosen */
    static uint8_t search[3 * TS_ENTRY] __attribute__((aligned(8)));
    memset(search, 0, sizeof(search));
    int32_t picks[3] = {-1, -1, -1}, n = 0;
    for (int32_t i = 0; i < g_player_count && n < 3; i += 53) {
        int32_t dup = 0;
        for (int32_t k = 0; k < n; ++k) dup |= g_players[picks[k]].rating == g_players[i].rating;
        if (!dup) picks[n++] = i;
    }
    if (n < 3) fail("sort test needs three ratings", n, 3);
    for (int32_t k = 0; k < 3; ++k) *(int32_t *)(search + k * TS_ENTRY) = g_players[picks[k]].player_id;
    *(uint32_t *)async = (uint32_t)(uintptr_t)search;
    *(int32_t *)(async + 0x2C) = 3;
    g_ts_list = TS_LIST_SEARCH;
    g_ts_sort = TS_SORT_RATING;
    if (ts_setup_hook(&ctx, base) != 0) fail("search setup must run the stock body", 0, 0);
    if (h_ts_rating(search) < h_ts_rating(search + TS_ENTRY) ||
        h_ts_rating(search + TS_ENTRY) < h_ts_rating(search + 2 * TS_ENTRY))
        fail("search results not sorted by rating", h_ts_rating(search), h_ts_rating(search + 2 * TS_ENTRY));
    memcpy(async, saved_async, sizeof(saved_async));

    /* card tap selects; without the screen the stock path (market stock action) runs */
    static uint8_t card[0x400];
    memset(card, 0, sizeof(card));
    *(uint16_t *)(card + 0x294) = (uint16_t)g_players[picks[0]].player_id;
    *(int32_t *)(card + 0x344) = g_players[picks[0]].owner_id;
    ctx.r[0] = (uint32_t)(uintptr_t)card;  /* CurrentPlayerBid is static: card in r0 */
    ctx.r[1] = 1;                          /* what r1 held on the device when v35c crashed */
    g_ts_screen = (void *)0;
    if (ts_bid_hook(&ctx, base) != 0) fail("card tap intercepted without the transfer screen", 0, 0);
    /* the tap selects the player; the buy path then opens the stock Sign Player dialog at the asking fee
     * (ts_buy_dialog directly: career_market_stock_action would tick the market). The fake queue/factory
     * stand in for the game's message boxes. */
    ts_select_card(g_players[picks[0]].player_id, g_players[picks[0]].owner_id);
    if (g_ts_sel_id != g_players[picks[0]].player_id || g_ts_sel_team != g_players[picks[0]].owner_id)
        fail("card tap did not select the player", g_ts_sel_id, 0);
    for (int32_t i = 0; i < (int32_t)sizeof(h_fake_queue); ++i) h_fake_queue[i] = 0;
    uint32_t saved_ui_base = g_ui_base;
    int32_t saved_index = g_ui_player_index, saved_bid = g_ui_bid_player_id;
    int32_t saved_fee = g_ui_bid_fee, saved_wage = g_ui_bid_wage, saved_years = g_ui_bid_contract_years;
    g_ui_queue_override = h_fake_queue;
    g_ui_box_factory = h_fake_factory;
    g_ui_live_box = (void *)0; g_ui_live_slot = -1; g_ui_pending_slot = -1; g_ui_in_callback = 0;
    int32_t buy = picks[0];
    if (g_players[buy].owner_id == USER_TEAM_ID) buy = picks[1];
    g_ui_player_index = buy;
    g_ui_bid_player_id = g_players[buy].player_id;
    g_ui_bid_fee = user_bid_asking(&g_players[buy], user_index);
    g_ui_bid_wage = transfer_wage_demand(&g_players[buy], user_index, g_players[buy].owner_index);
    g_ui_bid_contract_years = 3;
    h_factory_calls = 0;
    ts_buy_dialog(base);
    if (h_factory_calls != 1 || g_ui_live_slot < 0 || g_ui_boxes[g_ui_live_slot].kind != UI_BOX_SIGN ||
        g_ui_boxes[g_ui_live_slot].card_player != g_players[buy].player_id ||
        g_ui_boxes[g_ui_live_slot].card_team != g_players[buy].owner_id ||
        g_ui_boxes[g_ui_live_slot].card_price != g_ui_bid_fee || g_ui_bid_fee <= 0)
        fail("buy did not open the sign dialog at the asking fee", h_factory_calls, g_ui_bid_fee);
    /* the cross closes it and decides nothing */
    int32_t offers_before = ui_active_offer_count();
    ui_box_callback(0);
    if (ui_active_offer_count() != offers_before) fail("sign dialog cross made an offer", offers_before, 0);
    g_ui_queue_override = (void *)0; g_ui_queue_hooked = (void *)0;
    g_ui_box_factory = ui_box_create_native;
    g_ui_live_box = (void *)0; g_ui_live_slot = -1; g_ui_pending_slot = -1;
    for (int32_t i = 0; i < MARKET_UI_BOX_SLOTS; ++i) g_ui_boxes[i].kind = UI_BOX_FREE;
    g_ui_player_index = saved_index; g_ui_bid_player_id = saved_bid;
    g_ui_bid_fee = saved_fee; g_ui_bid_wage = saved_wage; g_ui_bid_contract_years = saved_years;
    g_ui_from_stock = 0;
    g_ui_base = saved_ui_base;

    *club = saved_club;
    *h_coins() = saved_coins;
    ++g_price_epoch;
    memcpy(g_ext.shortlist, saved_shortlist, sizeof(saved_shortlist));
    g_ts_list = saved_list; g_ts_sort = saved_sort; g_ts_sel_id = saved_sel; g_ts_sel_team = saved_team;
    g_ts_screen = saved_screen;
    g_ts_row_count = 0;
    if (!h_quiet) printf("transfer screen: lists, sorting, SetupResults swap and card selection OK\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: harness dataset.txt [seasons] [seed] [--quiet] [--csv f]\n"); return 1; }
    int32_t seasons = argc > 2 ? atoi(argv[2]) : 6;
    const char *csv_path = NULL;
    int32_t restart_every = 0, no_user = 0, eligibility = 0;
    if (argc > 3) h_rng = (uint32_t)atoi(argv[3]);
    for (int i = 4; i < argc; ++i) {
        if (!strcmp(argv[i], "--quiet")) h_quiet = 1;
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc) csv_path = argv[++i];
        else if (!strcmp(argv[i], "--restart-every") && i + 1 < argc) restart_every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-user")) no_user = 1;
        else if (!strcmp(argv[i], "--eligibility")) eligibility = 1;
    }
    h_test_bid_term_controls();
    h_test_market_ui_pages();
    h_test_market_ui_action_layout();
    h_test_market_ui_filter_toolbar();
    h_test_market_ui_action_schemes();
    h_test_market_ui_back_navigation();
    h_test_market_ui_boxes();
    h_test_market_ui_card_outline();
    h_test_market_ui_sorting();
    h_test_sales_position_filter();
    install_fake_library();
    load_dataset(argv[1]);
    HSeason *s = h_season();
    s->season = 0; s->turn = 17; s->end_turn = 56;
    printf("links=%d\n", h_db.link_count);
    check_rosters();

    /* first load of a pre-market save initialises the market */
    { HSave empty; memset(&empty, 0, sizeof(empty)); memcpy(empty.user_link, link_in(P32(h_db.live_links), USER_TEAM), LINK_STRIDE);
      restart_and_load(&empty, 0xAE); }
    check_market();
    if (eligibility) { eligibility_report(); return 0; }
    /* shortlist entries are bounded, toggleable, and survive the real market save block */
    {
        int32_t original[SHORTLIST_CAPACITY];
        memcpy(original, g_ext.shortlist, sizeof(original));
        for (int32_t i = 0; i < SHORTLIST_CAPACITY; ++i) g_ext.shortlist[i] = -1;
        if (g_player_count < 1) fail("shortlist test has no cached players", g_player_count, 0);
        int32_t player_id = g_players[0].player_id;
        if (shortlist_toggle(player_id) != 1 || shortlist_slot(player_id) < 0 || shortlist_count() != 1)
            fail("shortlist add failed", player_id, shortlist_count());
        HSave saved_shortlist = save_game();
        restart_and_load(&saved_shortlist, 0xB5);
        free(saved_shortlist.buf);
        if (shortlist_slot(player_id) < 0) fail("shortlist did not survive save/reload", player_id, -1);
        if (shortlist_toggle(player_id) != 0 || shortlist_count() != 0)
            fail("shortlist remove failed", player_id, shortlist_count());
        for (int32_t i = 0; i < SHORTLIST_CAPACITY; ++i) {
            if (shortlist_toggle(0x7000 + i) != 1) fail("shortlist capacity add failed", i, shortlist_count());
        }
        if (shortlist_toggle(0x7100) != -1) fail("shortlist accepted an item past capacity", shortlist_count(), 0);
        memcpy(g_ext.shortlist, original, sizeof(original));
    }
    h_test_transfer_screen();
    h_test_continuous();
    h_test_tm2();
    h_test_hub();
    FILE *csv = csv_path ? fopen(csv_path, "w") : NULL;
    if (csv) fprintf(csv, "season,club,rep,tier_value,cash,budget,wage_budget,payroll,revenue,squad_value,strength,size\n");

    /* coin repair: the hack's exact balance is reset once, honest balances are never touched */
    *h_coins() = 0xFFFFFF;
    repair_hacked_coins(FAKE_BASE);                    /* runs at every market entry and save */
    if (*h_coins() != 1000) fail("hacked coin balance not repaired", *h_coins(), 1000);
    *h_coins() = 5000;
    repair_hacked_coins(FAKE_BASE);
    { HSave probe = save_game(); free(probe.buf); }    /* the save hook repairs too; 5000 must survive */
    if (*h_coins() != 5000 || h_set_credits_calls != 1) fail("honest coin balance was changed", *h_coins(), h_set_credits_calls);
    /* price schedule: the bundled config values, then the mod's schedule; applying it again or after a
     * config reload must give the same prices (percent rules never compound) */
    {
        int32_t *vars = config_vars(FAKE_BASE);
        vars[0x013] = 4; vars[0x014] = 1; vars[0x015] = 2;
        vars[0x01A] = 30; vars[0x01B] = 50; vars[0x04A] = 20; vars[0x04B] = 200;
        vars[0x177] = 50; vars[0x178] = 100; vars[0x179] = 20; vars[0x17B] = 10;
        vars[0x051] = 50; vars[0x052] = 500; vars[0x054] = 75; vars[0x055] = 1000; vars[0x057] = 125; vars[0x058] = 1200;
        apply_price_schedule(FAKE_BASE);
        apply_price_schedule(FAKE_BASE);
        if (vars[0x013] != 0 || vars[0x014] != 0 || vars[0x015] != 0)
            fail("dynamic difficulty schedule wrong", vars[0x013], vars[0x014] + vars[0x015]);
        if (vars[0x01A] != 40 || vars[0x01B] != 20 || vars[0x179] != 0 || vars[0x052] != 1250 || vars[0x058] != 3000)
            fail("price schedule wrong", vars[0x052], vars[0x058]);
        vars[0x013] = 4; vars[0x014] = 1; vars[0x015] = 2;
        vars[0x052] = 500; vars[0x058] = 1200;            /* the game reloads its config */
        apply_price_schedule(FAKE_BASE);
        if (vars[0x013] != 0 || vars[0x014] != 0 || vars[0x015] != 0)
            fail("dynamic difficulty returned after config reload", vars[0x013], vars[0x014] + vars[0x015]);
        if (vars[0x052] != 1250 || vars[0x058] != 3000) fail("price schedule compounded after reload", vars[0x052], vars[0x058]);
        book(ECON_MEDICAL, -40);
        if (book_total(ECON_MEDICAL, 0) != -40) fail("books did not record medical spend", book_total(ECON_MEDICAL, 0), -40);
        book(ECON_MEDICAL, 40);
    }
    h_test_match_awards(0);
    h_test_match_awards(1);
    /* routing of the game's own coin movements */
    {
        *h_coins() = 1000;
        int32_t med = book_total(ECON_MEDICAL, 0), scout = book_total(ECON_SCOUTING, 0), other = book_total(ECON_OTHER_SPEND, 0);
        int32_t awards = book_total(ECON_AWARDS, 0), prize = book_total(ECON_PRIZE, 0);
        if (career_market_on_spend(40, 0x23E6E2, FAKE_BASE) != ECON_MEDICAL) fail("heal not routed", 0, 0);
        if (career_market_on_spend(60, 0x2504AD, FAKE_BASE) != ECON_SCOUTING) fail("scouting not routed", 0, 0);
        if (career_market_on_spend(5, 0x123456, FAKE_BASE) != ECON_OTHER_SPEND) fail("unknown spend not routed", 0, 0);
        if (career_market_on_income(92, 0x298CBB, FAKE_BASE) != -1) fail("match credit booked twice", 0, 0);
        if (career_market_on_income(25, 0x111111, FAKE_BASE) != ECON_AWARDS) fail("award income not routed", 0, 0);
        if (book_total(ECON_MEDICAL, 0) - med != -40 || book_total(ECON_SCOUTING, 0) - scout != -60 ||
            book_total(ECON_OTHER_SPEND, 0) - other != -5 || book_total(ECON_AWARDS, 0) - awards != 25 ||
            book_total(ECON_PRIZE, 0) != prize) fail("routed amounts wrong", 0, 0);
        book(ECON_MEDICAL, 40); book(ECON_SCOUTING, 60); book(ECON_OTHER_SPEND, 5); book(ECON_AWARDS, -25);
    }
    *h_coins() = 1500;                                  /* a fresh career's coins */
    int32_t transfers_total = 0;
    for (int32_t season = 0; season < seasons; ++season) {
        int32_t season_transfers = 0; int64_t season_fees = 0, season_value = 0;
        int32_t buyers_seen[MAX_CLUBS] = {0};
        for (int32_t turn = 17; turn <= s->end_turn; ++turn) {
            s->season = season; s->turn = turn;
            int32_t head = g_market.history_head;
            uint32_t serial = g_transfer_serial;
            career_market_on_turn(s, NULL, FAKE_BASE);
            if (!no_user) simulate_user();
            /* the user's matches pay through the v6 economy (league on even slots 18..46, cup on
             * 21..45 every 4 slots while still in it); results follow strength vs the division */
            if (!no_user) h_sim_user_match(turn);
            int32_t added = (int32_t)(g_transfer_serial - serial);
            if (added > HISTORY_CAPACITY) added = HISTORY_CAPACITY;   /* only the newest are kept */
            head = (g_market.history_head - added + HISTORY_CAPACITY) % HISTORY_CAPACITY;
            for (int32_t k = 0; k < added; ++k) {
                TransferRecord *t = &g_market.history[(head + k) % HISTORY_CAPACITY];
                if (getenv("H_DEBUG_SALES") && (t->seller_id == USER_TEAM || t->buyer_id == USER_TEAM))
                    fprintf(stderr, "deal s%d t%d %s pid %d rating %d value %d fee %d\n", t->season, t->turn,
                            t->seller_id == USER_TEAM ? "SELL" : "BUY ", t->player_id, h_players[t->player_id].rating,
                            h_players[t->player_id].value, t->fee);
                ++season_transfers; season_fees += t->fee;
                int32_t pid = t->player_id; season_value += h_players[pid].value;
                int32_t b = find_account(t->buyer_id); if (b >= 0) buyers_seen[b] = 1;
            }
            check_market();
            /* the cache is maintained incrementally; it must already match the live rosters */
            check_cache_vs_live();
            if (restart_every > 0 && (turn % restart_every) == 0) restart_roundtrip();
            else if (restart_every == 0 && (turn == 18 || turn == 40)) {   /* default: two checks a season */
                /* compare only: restore the pre-save state so the run matches a no-restart run */
                restart_roundtrip();
            }
        }
        check_rosters();
        transfers_total += season_transfers;
        int32_t clubs = 0, buying = 0, broke = 0; int64_t cash = 0, budget = 0, payroll = 0, value = 0, revenue = 0;
        for (int32_t i = 0; i < MAX_CLUBS; ++i) {
            ClubAccount *c = &g_market.clubs[i];
            if (c->team_id < 0) continue;
            ++clubs; buying += buyers_seen[i];
            cash += c->cash; budget += c->transfer_budget; payroll += c->payroll; value += c->squad_value;
            revenue += annual_revenue(i);
            if (c->cash < cash_reserve(c)) ++broke;
            if (csv) fprintf(csv, "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", season, c->team_id, g_club_rep[i],
                             c->squad_value, c->cash, c->transfer_budget, c->wage_budget, c->payroll,
                             annual_revenue(i), c->squad_value, c->strength, g_total_count[i]);
        }
        season_extra_metrics(season);
        { int32_t u = find_account(USER_TEAM); if (u >= 0) { ClubAccount *c = &g_market.clubs[u];
          printf("  user: rep=%d size=%d strength=%d value=%d cash=%d budget=%d fee_room=%d wage_room=%d revenue=%d\n",
                 g_club_rep[u], g_total_count[u], c->strength, c->squad_value, c->cash, c->transfer_budget,
                 account_fee_room(c), account_wage_room(c), annual_revenue(u)); } }
        printf("season %d: transfers=%d buyers=%d/%d fee/value=%" PRId64 "%% avg_cash=%" PRId64 " avg_budget=%" PRId64 " "
               "wage/revenue=%" PRId64 "%% below_reserve=%d\n",
               season, season_transfers, buying, clubs,
               season_value ? season_fees * 100 / season_value : 0,
               clubs ? cash / clubs : 0, clubs ? budget / clubs : 0,
               revenue ? payroll * 100 / revenue : 0, broke);
    }
    if (csv) fclose(csv);
    printf("negotiation: counters=%d insults=%d rival_events=%d rival_signed=%d\n", h_counters, h_insults, h_rival_events, h_rival_signed);
    printf("transfers=%d user: bids=%d buys=%d sales=%d | calc_links=%d sign=%d sell=%d verify=%d simple_regen=%d saves=%d\n",
           transfers_total, h_user_bids, h_user_buys, h_user_sales, h_calc_links, h_sign_calls,
           h_sell_calls, h_verify_calls, h_simple_regens, h_saves);
    printf("digest=%016" PRIx64 "\n", state_digest());
    printf("api_digest=%016" PRIx64 "\n", api_digest());
    HSave final_save = save_game();
    printf("save block bytes=%u\n", (unsigned)final_save.size);
    /* a save from before the roster block must load with the pristine rosters */
    restart_and_load(&final_save, 0xB2);
    for (int32_t i = 0; i < h_db.link_count; ++i) {
        uint8_t *a = h_pristine + i * LINK_STRIDE, *b = P32(h_db.live_links) + i * LINK_STRIDE;
        if (*(int32_t *)a == USER_TEAM) continue;
        for (int32_t j = 0; j < *lcount(a); ++j) {
            int32_t pid = lids(a)[j];
            if (lfind(link_in(P32(h_db.live_links), USER_TEAM), pid) >= 0) continue;
            if (lfind(b, pid) < 0) { fail("old-version load did not restore pristine rosters", *(int32_t *)a, pid); break; }
        }
    }
    for (int32_t d = 0; d < 6; ++d) if (h_seasons_in_div[d])
        printf("division %d: %lld seasons, income/season %lld\n", d, (long long)h_seasons_in_div[d],
               (long long)(h_income_by_div[d] / h_seasons_in_div[d]));
    printf("violations=%d\n", h_violations);
    return h_violations ? 1 : 0;
}
