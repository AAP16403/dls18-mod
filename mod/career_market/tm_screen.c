/*
 * Transfer Market v2 (v37): a full replacement for the stock transfer screen (CFESDreamLeagueTransfers, id 0x19).
 *
 * When the game builds screen 0x19 (CFEScreenStack::NewScreen, hooked by career_market_new_screen), this file builds
 * a plain CFEScreen with a cloned vtable instead, keeps id 0x19 so the screen stack and Back behave as before, hides
 * the stock header and footer, and draws the whole screen itself every frame:
 *
 *   top bar      Back, title, window (matches left), coins, wage room, squad size
 *   left rail    Scout, Shortlist, For You, Offers, My Squad; market pulse (price trend per position)
 *   main         search, position filter, sort; player rows (rating badge, name, club, asking price, trend, status)
 *   right panel  selected player: role, contract, wage, stats, chance he joins, rivals, asking price and value
 *   sheet        negotiation: fee slider, contract years, the club's replies, patience, rival bids, wage terms
 *
 * Everything is laid out in design units on a 640-unit-high canvas (the prototype's 1024x640 tablet screen) and
 * scaled to the real screen height; extra width goes to the player list. Taps and drags are read from the game's
 * touch tracker (XCTRL_Touch*, track 1, the same one CFEEntity::IsTouchInRect uses) and hit-tested against the
 * rectangles registered while drawing. Negotiation goes through career_market_submit_user_bid /
 * career_market_respond_to_offer, i.e. the v36 continuous seller, rival and wage logic.
 *
 * Game functions (stock VAs, Thumb |1): FE2D_DrawRectCol 0x28AF84, FESU_SetupText 0x294944, FESU_DrawTextBold
 * 0x293E28, FTTFont_PrintUnicode 0x38E8C8, FTTFont_GetUnicodeTextWidth 0x38F3C8, FTTFont_GetUnicodeTextDimensions
 * 0x38F2A0, XCTRL_TouchGetPos 0x202B20, XCTRL_TouchGetDownPos 0x202B04, XCTRL_TouchIsTouching 0x202AE4,
 * XCTRL_TouchIsPressed 0x202B78, XCTRL_TouchIsReleased 0x202B8C, CFEScreen::DisplayFooter 0x23B74C / DisplayHeader
 * 0x23B75C, CFE::Back 0x29910C, PU_Get*Stat (TPlayerInfo*) 0x2B37D8..0x2B38C8.
 */

#define TM_SCREEN_ID 0x19
#define TM_DESIGN_H 640.0f
#define TM_TOP_H 56.0f
#define TM_RAIL_W 176.0f
#define TM_DETAIL_W 320.0f
#define TM_TOOLS_H 52.0f
#define TM_ROW_H 58.0f
#define TM_MAX_ROWS 200
#define TM_MAX_HITS 160
#define TM_LOG_LINES 10
#define TM_TEXT 96

/* palette (0xAARRGGBB), from the approved prototype */
#define TM_GROUND 0xFF0D221Bu
#define TM_TURF 0xFF12302Au
#define TM_TURF2 0xFF173A32u
#define TM_TURF3 0xFF1F483Eu
#define TM_LINE 0xFF2A5549u
#define TM_CHALK 0xFFEEF3ECu
#define TM_CHALK2 0xFFB3C8BEu
#define TM_CHALK3 0xFF86A397u
#define TM_AMBER 0xFFF2B233u
#define TM_AMBER_INK 0xFF231A05u
#define TM_SKY 0xFF7CC0FFu
#define TM_BAD 0xFFFF8F6Bu
#define TM_GOOD 0xFF8FE0A8u

enum { TM_TAB_SCOUT, TM_TAB_SHORTLIST, TM_TAB_FORYOU, TM_TAB_OFFERS, TM_TAB_SQUAD, TM_TAB_COUNT };
enum { TM_SORT_RATING, TM_SORT_PRICE, TM_SORT_VALUE, TM_SORT_COUNT };
enum { TM_ALIGN_LEFT, TM_ALIGN_CENTER, TM_ALIGN_RIGHT };
enum {
    TM_HIT_NONE, TM_HIT_BACK, TM_HIT_TAB, TM_HIT_SEARCH, TM_HIT_POS, TM_HIT_SORT, TM_HIT_ROW, TM_HIT_STAR,
    TM_HIT_LIST, TM_HIT_OFFER, TM_HIT_SHORTLIST, TM_HIT_SLIDER, TM_HIT_YEARS, TM_HIT_SUBMIT, TM_HIT_ACCEPT,
    TM_HIT_LEAVE, TM_HIT_WAGE_ACCEPT, TM_HIT_WAGE_LOWER, TM_HIT_INBID_ACCEPT, TM_HIT_INBID_COUNTER,
    TM_HIT_INBID_REJECT, TM_HIT_LISTTOGGLE, TM_HIT_SHEET, TM_HIT_FEE_MINUS, TM_HIT_FEE_PLUS, TM_HIT_FEE_TYPE,
    TM_HIT_CLEAR_SEARCH
};
enum { TM_LOG_CLUB, TM_LOG_YOU, TM_LOG_RIVAL, TM_LOG_GOOD, TM_LOG_BAD };

typedef void (*TmRectFn)(float, float, float, float, uint32_t);
typedef void (*TmSetupTextFn)(int32_t, uint32_t, float, float);
typedef void (*TmPrintFn)(float, float, const uint16_t *);
typedef void (*TmBoldFn)(const uint16_t *, float, float, uint32_t);
typedef float (*TmTextWidthFn)(const uint16_t *);
typedef void (*TmTextDimsFn)(float *, const uint16_t *);
typedef void (*TmTouchPosFn)(int32_t *, int32_t);
typedef int32_t (*TmTouchFlagFn)(int32_t);
typedef void (*TmDisplayFn)(void *, int32_t);
typedef int32_t (*TmStatFn)(PlayerInfo *);
typedef void (*TmAlignFn)(int32_t);

typedef struct {
    int32_t player_id;
    int32_t index;              /* g_players index */
    int32_t offer_id;           /* Offers tab */
    int32_t rating, position, price, value, trend, block;
    uint8_t starred, talks, rivals, listed, incoming;
    uint16_t name[40];
    uint16_t sub[64];
    uint16_t extra[64];
} TmRow;

typedef struct { float x, y, w, h; int32_t kind, value; } TmHit;

static void tm_reset_state(void);
static void tm_back(uint32_t base);
static void tm_stock_bars_input(uint32_t base, int32_t enabled);
static void tm_keyboard_poke(uint32_t base);
static void tm_open_fee_keyboard(uint32_t base);

typedef struct {
    int32_t loaded;
    int32_t player_id, index;
    int32_t rating, position, ask, value, trend, wage, tenths, join_pm, rivals, block, bid_fee;
    int32_t stats[8], stat_count;
    uint16_t name[40], club[48], role[16], rival_name[48];
    uint16_t stat_labels[8][14];
} TmDetail;

static struct {
    void *screen;
    uint32_t base;
    float s, dw, height, width;
    float base_h;               /* font height at scale 1 */
    int32_t tab, pos_filter, sort;
    uint16_t query[32];
    int32_t dirty;
    TmRow rows[TM_MAX_ROWS];
    int32_t row_count;
    float scroll, scroll_max;
    int32_t sel_id;
    TmDetail detail;
    /* negotiation sheet */
    int32_t neg_open, neg_player, neg_offer, neg_fee, neg_years, neg_lo, neg_hi, neg_ask;
    uint16_t neg_club[48];      /* the selling club and its ask when talks opened (he may be ours afterwards) */
    uint16_t log[TM_LOG_LINES][TM_TEXT];
    uint8_t log_kind[TM_LOG_LINES];
    int32_t log_count;
    /* input */
    TmHit hits[TM_MAX_HITS];
    int32_t hit_count;
    int32_t drag_kind, drag_moved;
    float drag_scroll0;
    /* toast */
    uint16_t toast[TM_TEXT];
    int32_t toast_frames;
    int32_t kb_wait;            /* frames left to open the Android keyboard for a new keyboard box */
    uint16_t line[160];
} g_tm;

/* ---------------------------------------------------------------------------------------------------------------
 * drawing
 * ------------------------------------------------------------------------------------------------------------- */
static void tm_rect(float x, float y, float w, float h, uint32_t colour) {
    if (w <= 0.0f || h <= 0.0f) return;
    ((TmRectFn)(g_tm.base + 0x28AF85))(x * g_tm.s, y * g_tm.s, w * g_tm.s, h * g_tm.s, colour);
}

/* FE2D_DrawTriangle(x1, y1, x2, y2, x3, y3, bool, colour) 0x28BA64 */
typedef void (*TmTriangleFn)(float, float, float, float, float, float, int32_t, uint32_t);
static void tm_triangle(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t colour) {
    float s = g_tm.s;
    ((TmTriangleFn)(g_tm.base + 0x28BA65))(x1 * s, y1 * s, x2 * s, y2 * s, x3 * s, y3 * s, 0, colour);
}

static void tm_hit(float x, float y, float w, float h, int32_t kind, int32_t value) {
    if (g_tm.hit_count >= TM_MAX_HITS) return;
    TmHit *hit = &g_tm.hits[g_tm.hit_count++];
    hit->x = x; hit->y = y; hit->w = w; hit->h = h; hit->kind = kind; hit->value = value;
}

static float tm_font_scale(float px) {
    return px * g_tm.s / (g_tm.base_h > 1.0f ? g_tm.base_h : 20.0f);
}

/* Width of text at px, in design units. */
static float tm_text_width(const uint16_t *text, float px) {
    if (!text || !text[0]) return 0.0f;
    ((TmSetupTextFn)(g_tm.base + 0x294945))(0, TM_CHALK, tm_font_scale(px), -1.0f);
    float w = ((TmTextWidthFn)(g_tm.base + 0x38F3C9))(text);
    return w / g_tm.s;
}

/* Text at (x, y) = top-left (or top-centre / top-right) in design units, clipped to max_w with "...". */
static void tm_text(const uint16_t *text, float x, float y, float px, uint32_t colour, int32_t align,
                    float max_w, int32_t bold) {
    if (!text || !text[0]) return;
    static uint16_t buf[160];
    int32_t n = 0;
    while (text[n] && n < 159) { buf[n] = text[n]; ++n; }
    buf[n] = 0;
    ((TmSetupTextFn)(g_tm.base + 0x294945))(0, colour, tm_font_scale(px), -1.0f);
    TmTextWidthFn width = (TmTextWidthFn)(g_tm.base + 0x38F3C9);
    float w = width(buf) / g_tm.s;
    if (max_w > 0.0f && w > max_w) {
        while (n > 1 && w > max_w) {
            --n;
            buf[n] = 0;
            if (n >= 3) { buf[n - 1] = '.'; buf[n - 2] = '.'; buf[n - 3] = '.'; }
            w = width(buf) / g_tm.s;
        }
    }
    float left = align == TM_ALIGN_CENTER ? x - w * 0.5f : align == TM_ALIGN_RIGHT ? x - w : x;
    /* FTTFont_SetAlign(left): the font keeps the last caller's alignment (the stock screens leave it centred,
     * which put every v37 label half a width to the left). We align ourselves from the measured width. */
    ((TmAlignFn)(g_tm.base + 0x38E301))(0);
    if (bold) ((TmBoldFn)(g_tm.base + 0x293E29))(buf, left * g_tm.s, y * g_tm.s, colour);
    else ((TmPrintFn)(g_tm.base + 0x38E8C9))(left * g_tm.s, y * g_tm.s, buf);
}

/* Word-wrapped text; returns the number of lines drawn (at most max_lines, the last one ellipsised). */
static int32_t tm_text_wrap(const uint16_t *text, float x, float y, float px, uint32_t colour, float max_w,
                            int32_t max_lines, float line_h) {
    static uint16_t line[160];
    int32_t pos = 0, lines = 0;
    while (text && text[pos] && lines < max_lines) {
        while (text[pos] == ' ') ++pos;
        int32_t n = 0, last_space = -1;
        while (text[pos + n] && n < 159) {
            line[n] = text[pos + n];
            line[n + 1] = 0;
            if (tm_text_width(line, px) > max_w && n > 0) break;
            if (text[pos + n] == ' ') last_space = n;
            ++n;
        }
        int32_t take = n;
        if (text[pos + n] && last_space > 0 && lines + 1 < max_lines) take = last_space;
        line[take] = 0;
        if (lines + 1 == max_lines && text[pos + take]) {
            /* the last allowed line: keep the rest and let tm_text ellipsise it */
            int32_t k = 0;
            while (text[pos + k] && k < 159) { line[k] = text[pos + k]; ++k; }
            line[k] = 0;
        }
        tm_text(line, x, y + (float)lines * line_h, px, colour, 0, max_w, 0);
        ++lines;
        pos += take;
    }
    return lines;
}

static const uint16_t *tm_w(const char *ascii) {
    static uint16_t ring[8][TM_TEXT];
    static int32_t next;
    uint16_t *out = ring[next];
    next = (next + 1) & 7;
    int32_t i = 0;
    while (ascii[i] && i < TM_TEXT - 1) { out[i] = (uint8_t)ascii[i]; ++i; }
    out[i] = 0;
    return out;
}

static void tm_append_money(MarketUiText *t, int32_t value) {
    if (value < 0) { ui_text_append_char(t, '-'); value = -value; }
    char digits[12];
    int32_t n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value && n < 11);
    for (int32_t i = n - 1; i >= 0; --i) {
        ui_text_append_char(t, (uint16_t)digits[i]);
        if (i && i % 3 == 0) ui_text_append_char(t, ',');
    }
}

static const uint16_t *tm_money(int32_t value) {
    static uint16_t ring[8][24];
    static int32_t next;
    uint16_t *out = ring[next];
    next = (next + 1) & 7;
    MarketUiText t;
    ui_text_reset(&t, out, 24);
    tm_append_money(&t, value);
    return out;
}

static uint32_t tm_mix(uint32_t a, uint32_t b, int32_t t_pm) {
    if (t_pm < 0) t_pm = 0;
    if (t_pm > 1000) t_pm = 1000;
    uint32_t out = 0xFF000000u;
    for (int32_t shift = 0; shift < 24; shift += 8) {
        int32_t ca = (int32_t)((a >> shift) & 0xFF), cb = (int32_t)((b >> shift) & 0xFF);
        out |= (uint32_t)(cb + (ca - cb) * t_pm / 1000) << shift;
    }
    return out;
}

/* Rating badge: brighter amber as the rating rises (continuous). */
static void tm_badge(float x, float y, float size, int32_t rating, float px) {
    int32_t t = (rating - 55) * 1000 / 40;
    tm_rect(x, y, size, size, tm_mix(TM_AMBER, TM_TURF3, t));
    MarketUiText b;
    uint16_t buf[8];
    ui_text_reset(&b, buf, 8);
    ui_text_append_i32(&b, rating);
    tm_text(buf, x + size * 0.5f, y + (size - px) * 0.5f, px, t > 550 ? TM_AMBER_INK : TM_CHALK, TM_ALIGN_CENTER, 0, 1);
}

static void tm_button(float x, float y, float w, float h, const uint16_t *label, int32_t primary, int32_t enabled,
                      int32_t kind, int32_t value) {
    uint32_t fill = primary ? (enabled ? TM_AMBER : tm_mix(TM_AMBER, TM_GROUND, 450)) : TM_TURF2;
    tm_rect(x, y, w, h, fill);
    if (!primary) {
        tm_rect(x, y, w, 1.0f, TM_LINE); tm_rect(x, y + h - 1.0f, w, 1.0f, TM_LINE);
        tm_rect(x, y, 1.0f, h, TM_LINE); tm_rect(x + w - 1.0f, y, 1.0f, h, TM_LINE);
    }
    float px = primary ? 17.0f : 13.0f;
    tm_text(label, x + w * 0.5f, y + (h - px) * 0.5f, px,
            primary ? TM_AMBER_INK : (enabled ? TM_CHALK : TM_CHALK3), TM_ALIGN_CENTER, w - 12.0f, primary);
    if (enabled) tm_hit(x, y, w, h, kind, value);
}

/* ---------------------------------------------------------------------------------------------------------------
 * data
 * ------------------------------------------------------------------------------------------------------------- */
static int32_t tm_user_index(void) { return find_account(USER_TEAM_ID); }

static void tm_player_name(uint32_t base, int32_t player_id, uint16_t *out, int32_t cap) {
    MarketUiText t;
    ui_text_reset(&t, out, cap);
    ui_append_player_name(&t, base, player_id);
}

static void tm_team_name(uint32_t base, int32_t team_id, uint16_t *out, int32_t cap) {
    MarketUiText t;
    ui_text_reset(&t, out, cap);
    ui_append_team_name(&t, base, team_id);
}

static const char *tm_pos_label(int32_t position) {
    static const char *const labels[] = {"GK", "DEF", "MID", "FWD"};
    return position >= 0 && position <= 3 ? labels[position] : "-";
}

static int32_t tm_is_starred(int32_t player_id) {
    for (int32_t i = 0; i < SHORTLIST_CAPACITY; ++i) if (g_ext.shortlist[i] == player_id) return 1;
    return 0;
}

/* An offer by id whatever its status (find_offer only returns running ones): the sheet shows how talks ended. */
static MarketOffer *tm_any_offer(int32_t offer_id) {
    if (offer_id < 0) return (MarketOffer *)0;
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i)
        if (g_market.offers[i].offer_id == offer_id && g_market.offers[i].status != CM_OFFER_FREE) return &g_market.offers[i];
    return (MarketOffer *)0;
}

static MarketOffer *tm_user_offer_for(int32_t player_id) {
    for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
        MarketOffer *offer = &g_market.offers[i];
        if (offer_is_active(offer) && offer->player_id == player_id &&
            (offer->buyer_id == USER_TEAM_ID || offer->seller_id == USER_TEAM_ID)) return offer;
    }
    return (MarketOffer *)0;
}

/* Chance (permille) that he would join and that his club would sell to the user: the same curves
 * player_will_join / starter_move_allowed draw against. */
static int32_t tm_join_pm(const MarketPlayer *player, int32_t user_index) {
    int32_t a = 1000 - join_refusal_pm(player, user_index);
    int32_t b = 1000 - move_refusal_pm(player, user_index);
    return a * b / 1000;
}

static int32_t tm_row_key(const MarketPlayer *player, int32_t price) {
    if (g_tm.sort == TM_SORT_PRICE) return -price;
    if (g_tm.sort == TM_SORT_VALUE) return price > 0 ? player->rating * 100000 / price : 0;
    return player->rating * 100000 - (price > 99999 ? 99999 : price);
}

static int32_t tm_query_matches(uint32_t base, const MarketPlayer *player) {
    if (!g_tm.query[0]) return 1;
    uint16_t name[64];
    tm_player_name(base, player->player_id, name, 64);
    if (ui_wide_contains(name, g_tm.query)) return 1;
    tm_team_name(base, player->owner_id, name, 64);
    return ui_wide_contains(name, g_tm.query);
}

static void tm_fill_row(uint32_t base, TmRow *row, int32_t index, int32_t user_index) {
    MarketPlayer *player = &g_players[index];
    row->player_id = player->player_id;
    row->index = index;
    row->rating = player->rating;
    row->position = player->position;
    row->value = market_value(player);
    row->trend = market_index_for(player) - 100;
    row->price = player->owner_id != USER_TEAM_ID && user_index >= 0 ? user_bid_asking(player, user_index) : row->value;
    row->starred = (uint8_t)tm_is_starred(player->player_id);
    MarketOffer *offer = tm_user_offer_for(player->player_id);
    row->talks = offer != 0;
    row->listed = (uint8_t)user_player_is_listed(player->player_id);
    row->block = player->owner_id != USER_TEAM_ID ? user_bid_block(base, player, user_index) : 0;
    row->rivals = 0;
    tm_player_name(base, player->player_id, row->name, 40);
    MarketUiText t;
    ui_text_reset(&t, row->sub, 64);
    ui_text_append_ascii(&t, tm_pos_label(player->position));
    ui_text_append_ascii(&t, "  ");
    ui_append_team_name(&t, base, player->owner_id);
    row->extra[0] = 0;
}

/* Top-TM_MAX_ROWS by key; only indices move here, names and prices are filled once at the end. */
static int32_t g_tm_pick[TM_MAX_ROWS];

static void tm_insert_row(int32_t index, int32_t key, int32_t *keys) {
    int32_t n = g_tm.row_count;
    if (n >= TM_MAX_ROWS && key <= keys[n - 1]) return;
    int32_t at = n < TM_MAX_ROWS ? n : TM_MAX_ROWS - 1;
    while (at > 0 && keys[at - 1] < key) {
        if (at < TM_MAX_ROWS) { g_tm_pick[at] = g_tm_pick[at - 1]; keys[at] = keys[at - 1]; }
        --at;
    }
    g_tm_pick[at] = index;
    keys[at] = key;
    if (n < TM_MAX_ROWS) ++g_tm.row_count;
}

static void tm_fill_picks(uint32_t base, int32_t user_index) {
    for (int32_t r = 0; r < g_tm.row_count; ++r) {
        tm_fill_row(base, &g_tm.rows[r], g_tm_pick[r], user_index);
        g_tm.rows[r].offer_id = -1;
        g_tm.rows[r].incoming = 0;
    }
}

static void tm_reason(TmRow *row, const MarketPlayer *player, int32_t user_index) {
    MarketUiText t;
    ui_text_reset(&t, row->extra, 64);
    int32_t improvement = starter_improvement(player, user_index);
    int32_t need = player->position >= 0 && player->position <= 3 ? position_need(user_index, player->position) : 0;
    if (improvement > 0) {
        ui_text_append_ascii(&t, "Starts for you: +");
        ui_text_append_i32(&t, improvement);
        ui_text_append_ascii(&t, " on your ");
        ui_text_append_ascii(&t, tm_pos_label(player->position));
    } else if (need > 0) {
        ui_text_append_ascii(&t, "Fills a thin position: ");
        ui_text_append_ascii(&t, tm_pos_label(player->position));
    } else {
        ui_text_append_ascii(&t, "Affordable squad depth");
    }
}

static void tm_build(uint32_t base) {
    static int32_t keys[TM_MAX_ROWS];
    g_tm.row_count = 0;
    int32_t user_index = tm_user_index();
    if (user_index < 0) return;
    if (g_tm.tab == TM_TAB_OFFERS) {
        for (int32_t i = 0; i < OFFER_CAPACITY && g_tm.row_count < TM_MAX_ROWS; ++i) {
            MarketOffer *offer = &g_market.offers[i];
            if (!offer_is_active(offer)) continue;
            if (offer->seller_id != USER_TEAM_ID && offer->buyer_id != USER_TEAM_ID) continue;
            int32_t index = find_cached_player(offer->player_id);
            if (index < 0) continue;
            TmRow *row = &g_tm.rows[g_tm.row_count++];
            tm_fill_row(base, row, index, user_index);
            row->offer_id = offer->offer_id;
            row->incoming = offer->seller_id == USER_TEAM_ID;
            MarketUiText t;
            ui_text_reset(&t, row->extra, 64);
            if (row->incoming) {
                int32_t fee = offer->status == CM_OFFER_WAIT_USER_SELLER_COUNTER ? offer->counter_fee : offer->fee;
                ui_append_team_name(&t, base, offer->buyer_id);
                ui_text_append_ascii(&t, " bids ");
                tm_append_money(&t, fee);
                ui_text_append_ascii(&t, " (");
                ui_text_append_i32(&t, row->value > 0 ? mul_div(fee, 100, row->value) : 0);
                ui_text_append_ascii(&t, "% of value)");
                row->price = fee;
            } else {
                ui_text_append_ascii(&t, offer->status == CM_OFFER_WAIT_USER_WAGE ? "Personal terms: he asks "
                                                                                  : "Your bid: ");
                tm_append_money(&t, offer->status == CM_OFFER_WAIT_USER_WAGE ? offer->counter_wage : offer->fee);
                if (offer->status == CM_OFFER_WAIT_USER_FEE && offer->counter_fee > 0) {
                    ui_text_append_ascii(&t, "  They want ");
                    tm_append_money(&t, offer->counter_fee);
                }
            }
        }
        return;
    }
    if (g_tm.tab == TM_TAB_SQUAD) {
        for (int32_t i = 0; i < g_player_count; ++i) {
            if (g_players[i].owner_id != USER_TEAM_ID) continue;
            tm_insert_row(i, g_players[i].rating * 100000 + (i & 0xFFFF), keys);
        }
        tm_fill_picks(base, user_index);
        for (int32_t r = 0; r < g_tm.row_count; ++r) {
            TmRow *row = &g_tm.rows[r];
            MarketOffer *offer = tm_user_offer_for(row->player_id);
            MarketUiText t;
            ui_text_reset(&t, row->extra, 64);
            ui_text_append_ascii(&t, tm_pos_label(row->position));
            ui_text_append_ascii(&t, "  Clubs pay ");
            tm_append_money(&t, user_sale_value(&g_players[row->index]));
            int32_t tenths = contract_tenths_left(row->player_id, USER_TEAM_ID);
            ui_text_append_ascii(&t, "  Contract ");
            ui_text_append_i32(&t, tenths / 10);
            ui_text_append_char(&t, '.');
            ui_text_append_i32(&t, tenths % 10);
            if (offer && offer->seller_id == USER_TEAM_ID) {
                ui_text_append_ascii(&t, "  Bid ");
                tm_append_money(&t, offer->status == CM_OFFER_WAIT_USER_SELLER_COUNTER ? offer->counter_fee : offer->fee);
            }
        }
        return;
    }
    ClubAccount *club = &g_market.clubs[user_index];
    int32_t coins = user_coins(base);
    int32_t fee_room = account_fee_room(club);
    if (fee_room < coins) coins = fee_room;
    int32_t wage_room = account_wage_room(club);
    for (int32_t i = 0; i < g_player_count; ++i) {
        MarketPlayer *player = &g_players[i];
        if (player->owner_id == USER_TEAM_ID || player->owner_index < 0 || player->value <= 0) continue;
        if (g_tm.pos_filter >= 0 && player->position != g_tm.pos_filter) continue;
        if (g_tm.tab == TM_TAB_SHORTLIST && !tm_is_starred(player->player_id)) continue;
        if (g_tm.tab == TM_TAB_SCOUT && !club_can_sell_now(player)) continue;
        int32_t price = user_bid_asking(player, user_index);
        if (g_tm.tab == TM_TAB_FORYOU) {
            if (price <= 0 || price > coins) continue;
            if (transfer_wage_demand(player, user_index, player->owner_index) > wage_room) continue;
            if (starter_improvement(player, user_index) <= 0 &&
                (player->position < 0 || player->position > 3 || position_need(user_index, player->position) == 0)) continue;
            if (tm_join_pm(player, user_index) < 300) continue;
        }
        if (!tm_query_matches(base, player)) continue;
        tm_insert_row(i, tm_row_key(player, price), keys);
    }
    tm_fill_picks(base, user_index);
    for (int32_t r = 0; r < g_tm.row_count; ++r) {
        TmRow *row = &g_tm.rows[r];
        if (g_tm.tab == TM_TAB_FORYOU) tm_reason(row, &g_players[row->index], user_index);
    }
}

static void tm_load_detail(uint32_t base) {
    TmDetail *d = &g_tm.detail;
    d->loaded = 0;
    int32_t index = find_cached_player(g_tm.sel_id);
    int32_t user_index = tm_user_index();
    if (index < 0 || user_index < 0) return;
    MarketPlayer *player = &g_players[index];
    d->player_id = player->player_id;
    d->index = index;
    d->rating = player->rating;
    d->position = player->position;
    d->value = market_value(player);
    d->trend = market_index_for(player) - 100;
    int32_t own = player->owner_id == USER_TEAM_ID;
    d->ask = own ? user_sale_value(player) : user_bid_asking(player, user_index);
    d->wage = own ? player->wage : transfer_wage_demand(player, user_index, player->owner_index);
    d->tenths = contract_tenths_left(player->player_id, player->owner_id);
    d->join_pm = own ? 1000 : tm_join_pm(player, user_index);
    d->block = own ? 0 : user_bid_block(base, player, user_index);
    tm_player_name(base, player->player_id, d->name, 40);
    tm_team_name(base, player->owner_id, d->club, 48);
    RoleMix mix = role_mix(player);
    const char *role = "Rotation";
    int32_t best = mix.rotation;
    if (mix.surplus > best) { best = mix.surplus; role = "Surplus"; }
    if (mix.starter > best) { best = mix.starter; role = "Starter"; }
    if (mix.key > best) { role = "Key player"; }
    MarketUiText t;
    ui_text_reset(&t, d->role, 16);
    ui_text_append_ascii(&t, role);
    d->rivals = 0;
    d->rival_name[0] = 0;
    if (!own) {
        int32_t rival_max = 0, count = 0;
        int32_t rival = find_rival(player, user_index, &rival_max, &count);
        d->rivals = count;
        if (rival >= 0) tm_team_name(base, g_market.clubs[rival].team_id, d->rival_name, 48);
    }
    d->bid_fee = 0;
    if (own) {
        /* the best bid a club would make for him now */
        int32_t fee = 0, wage = 0;
        int32_t buyer = best_ai_bid_for_user_player(index, &fee, &wage);
        if (buyer >= 0) {
            d->bid_fee = fee;
            tm_team_name(base, g_market.clubs[buyer].team_id, d->rival_name, 48);
        }
    }
    /* stats through the game's own PU_Get*Stat readers */
    static const uint32_t out_fns[8] = {0x2B3829, 0x2B383D, 0x2B38A1, 0x2B3851, 0x2B3815, 0x2B38C9, 0x2B3865, 0x2B38B5};
    static const char *const out_labels[8] = {"Speed", "Acceleration", "Stamina", "Strength", "Tackling", "Control",
                                              "Passing", "Shooting"};
    static const uint32_t gk_fns[6] = {0x2B3801, 0x2B37D9, 0x2B37ED, 0x2B3829, 0x2B3851, 0x2B3865};
    static const char *const gk_labels[6] = {"Shot stopping", "Handling", "Presence", "Speed", "Strength", "Passing"};
    PlayerInfo info;
    d->stat_count = 0;
    if (load_player_info(base, player->player_id, &info)) {
        int32_t gk = player->position == 0;
        int32_t count = gk ? 6 : 8;
        for (int32_t i = 0; i < count; ++i) {
            int32_t v = ((TmStatFn)(base + (gk ? gk_fns[i] : out_fns[i])))(&info);
            d->stats[i] = v < 0 ? 0 : v > 99 ? 99 : v;
            ui_text_reset(&t, d->stat_labels[i], 14);
            ui_text_append_ascii(&t, gk ? gk_labels[i] : out_labels[i]);
        }
        d->stat_count = count;
    }
    d->loaded = 1;
}

static void tm_toast(const uint16_t *text) {
    int32_t i = 0;
    while (text && text[i] && i < TM_TEXT - 1) { g_tm.toast[i] = text[i]; ++i; }
    g_tm.toast[i] = 0;
    g_tm.toast_frames = 200;
}

static void tm_refresh(uint32_t base) {
    sync_accounts(base);
    build_player_cache(base);
    ++g_price_epoch;
    g_tm.dirty = 1;
}

/* ---------------------------------------------------------------------------------------------------------------
 * negotiation
 * ------------------------------------------------------------------------------------------------------------- */
static void tm_log(int32_t kind, const uint16_t *text) {
    if (g_tm.log_count == TM_LOG_LINES) {
        for (int32_t i = 1; i < TM_LOG_LINES; ++i) {
            for (int32_t k = 0; k < TM_TEXT; ++k) g_tm.log[i - 1][k] = g_tm.log[i][k];
            g_tm.log_kind[i - 1] = g_tm.log_kind[i];
        }
        --g_tm.log_count;
    }
    uint16_t *line = g_tm.log[g_tm.log_count];
    int32_t i = 0;
    while (text[i] && i < TM_TEXT - 1) { line[i] = text[i]; ++i; }
    line[i] = 0;
    g_tm.log_kind[g_tm.log_count++] = (uint8_t)kind;
}

static void tm_open_sheet(uint32_t base, int32_t player_id) {
    int32_t index = find_cached_player(player_id);
    int32_t user_index = tm_user_index();
    if (index < 0 || user_index < 0) return;
    MarketPlayer *player = &g_players[index];
    int32_t ask = user_bid_asking(player, user_index);
    g_tm.neg_open = 1;
    g_tm.neg_player = player_id;
    g_tm.neg_years = 3;
    g_tm.neg_lo = round5(ask / 2);
    g_tm.neg_hi = round5(mul_div(ask, 130, 100));
    g_tm.neg_ask = ask;
    tm_team_name(base, player->owner_id, g_tm.neg_club, 48);
    g_tm.log_count = 0;
    MarketOffer *offer = tm_user_offer_for(player_id);
    g_tm.neg_offer = offer && offer->buyer_id == USER_TEAM_ID ? offer->offer_id : -1;
    MarketUiText t;
    ui_text_reset(&t, g_tm.line, 160);
    if (offer && offer->buyer_id == USER_TEAM_ID) {
        g_tm.neg_fee = offer->counter_fee > 0 ? offer->counter_fee : offer->fee;
        ui_text_append_ascii(&t, "Talks resume. Your last bid was ");
        tm_append_money(&t, offer->fee);
        if (offer->counter_fee > 0) {
            ui_text_append_ascii(&t, ", we want ");
            tm_append_money(&t, offer->counter_fee);
        }
        ui_text_append_char(&t, '.');
    } else {
        g_tm.neg_fee = round5(mul_div(ask, 85, 100));
        ui_text_append_ascii(&t, "Our asking price is ");
        tm_append_money(&t, ask);
        ui_text_append_char(&t, '.');
    }
    tm_log(TM_LOG_CLUB, g_tm.line);
}

static void tm_log_event(uint32_t base, MarketOffer *offer) {
    MarketUiText t;
    uint16_t rival[48];
    rival[0] = 0;
    if (g_neg_rival_team >= 0) tm_team_name(base, g_neg_rival_team, rival, 48);
    ui_text_reset(&t, g_tm.line, 160);
    int32_t kind = TM_LOG_CLUB;
    switch (g_neg_event) {
    case NEG_COUNTER:
        ui_text_append_ascii(&t, "Close. We can do ");
        tm_append_money(&t, offer->counter_fee);
        ui_text_append_char(&t, '.');
        break;
    case NEG_SOFTENED:
        ui_text_append_ascii(&t, "Too low. We could come down to ");
        tm_append_money(&t, offer->counter_fee);
        ui_text_append_char(&t, '.');
        break;
    case NEG_INSULT:
        ui_text_append_ascii(&t, "That is not a serious offer. Our price is ");
        tm_append_money(&t, offer->counter_fee);
        ui_text_append_char(&t, '.');
        break;
    case NEG_RIVAL_BID:
    case NEG_RIVAL_RAISED:
        kind = TM_LOG_RIVAL;
        ui_text_append_wide(&t, rival, 40);
        ui_text_append_ascii(&t, g_neg_event == NEG_RIVAL_RAISED ? " raised their bid to " : " bid ");
        tm_append_money(&t, g_neg_rival_fee);
        ui_text_append_ascii(&t, ". Match ");
        tm_append_money(&t, offer->counter_fee);
        ui_text_append_ascii(&t, " to stay ahead.");
        break;
    case NEG_RIVAL_WITHDREW:
        kind = TM_LOG_RIVAL;
        ui_text_append_wide(&t, rival, 40);
        ui_text_append_ascii(&t, " pulled out.");
        if (offer->status == CM_OFFER_WAIT_USER_FEE && offer->counter_fee > 0) {
            tm_log(kind, g_tm.line);
            ui_text_reset(&t, g_tm.line, 160);
            kind = TM_LOG_CLUB;
            ui_text_append_ascii(&t, "We can do ");
            tm_append_money(&t, offer->counter_fee);
            ui_text_append_char(&t, '.');
        }
        break;
    case NEG_AGREED:
    case NEG_WAGE_COUNTER:
        if (offer->status == CM_OFFER_WAIT_USER_WAGE) {
            ui_text_append_ascii(&t, "Fee agreed at ");
            tm_append_money(&t, offer->fee);
            ui_text_append_ascii(&t, ". He asks ");
            tm_append_money(&t, offer->counter_wage);
            ui_text_append_ascii(&t, " a season.");
        } else {
            ui_text_append_ascii(&t, "Deal agreed.");
        }
        break;
    case NEG_SIGNED:
        kind = TM_LOG_GOOD;
        ui_text_append_ascii(&t, "Signed for ");
        tm_append_money(&t, offer->fee);
        ui_text_append_ascii(&t, " on ");
        ui_text_append_i32(&t, offer->contract_years);
        ui_text_append_ascii(&t, offer->contract_years == 1 ? " year at " : " years at ");
        tm_append_money(&t, offer->annual_wage);
        ui_text_append_ascii(&t, " a season.");
        break;
    case NEG_TALKS_ENDED:
        kind = TM_LOG_BAD;
        ui_text_append_ascii(&t, "We are wasting each other's time. Talks are over for this window.");
        break;
    case NEG_RIVAL_SIGNED:
        kind = TM_LOG_BAD;
        ui_text_append_wide(&t, rival, 40);
        ui_text_append_ascii(&t, " signed him instead.");
        break;
    default:
        if (offer && offer->status == CM_OFFER_COMPLETED) {
            kind = TM_LOG_GOOD;
            ui_text_append_ascii(&t, "Transfer complete.");
        } else {
            return;
        }
    }
    tm_log(kind, g_tm.line);
}

static const char *tm_bid_error(int32_t code) {
    switch (code) {
    case -2: return "The transfer window is closed.";
    case -5: return "He is not for sale.";
    case -18: return "You have made all the signings allowed this window.";
    case -19: return "His club is busy with another deal. Try again after the next match.";
    case -20: return "You already have 3 bids running. Finish one in Offers first.";
    case -21: return "You are already in talks for him.";
    case -8: return "He cannot move right now.";
    case -7: return "That fee or wage is beyond your budget.";
    case -12: return "You would not have enough coins left for wages.";
    case -11: return "He does not want to join your club this window.";
    case -15: return "His club will not sell a key player to you this window.";
    case -16: return "Talks with this club are over for this window.";
    case -17: return "His club cannot let a player go right now.";
    case -10: return "They rejected the bid and talks are over.";
    default: return (const char *)0;
    }
}

/* Short status for a list row. */
static const char *tm_block_short(int32_t code) {
    switch (code) {
    case 0: return (const char *)0;
    case -21: return "In talks";
    case -11: return "Won't join";
    case -15: return "Club keeps him";
    case -16: return "Talks ended";
    case -17: case -8: case -5: return "Not for sale";
    case -19: return "Club busy";
    case -2: return "Window closed";
    default: return "Can't bid now";
    }
}

static void tm_submit(uint32_t base) {
    int32_t index = find_cached_player(g_tm.neg_player);
    int32_t user_index = tm_user_index();
    if (index < 0 || user_index < 0) return;
    MarketUiText t;
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, "We offer ");
    tm_append_money(&t, g_tm.neg_fee);
    ui_text_append_ascii(&t, " on a ");
    ui_text_append_i32(&t, g_tm.neg_years);
    ui_text_append_ascii(&t, "-year deal.");
    tm_log(TM_LOG_YOU, g_tm.line);
    MarketOffer *offer = g_tm.neg_offer >= 0 ? find_offer(g_tm.neg_offer) : (MarketOffer *)0;
    if (offer && offer_is_active(offer)) {
        career_market_respond_to_offer(base, offer->offer_id, CM_DECISION_COUNTER, g_tm.neg_fee);
    } else {
        MarketPlayer *player = &g_players[index];
        int32_t wage = transfer_wage_demand(player, user_index, player->owner_index);
        int32_t id = career_market_submit_user_bid(base, g_tm.neg_player, g_tm.neg_fee, wage, g_tm.neg_years);
        if (id < 0 && id != -10) {
            const char *why = tm_bid_error(id);
            ui_text_reset(&t, g_tm.line, 160);
            if (why) {
                ui_text_append_ascii(&t, why);
            } else {                                        /* unexpected: keep the code for bug reports */
                ui_text_append_ascii(&t, "The transfer could not be started (code ");
                ui_text_append_i32(&t, id);
                ui_text_append_ascii(&t, ", player ");
                ui_text_append_i32(&t, g_tm.neg_player);
                ui_text_append_ascii(&t, ").");
            }
            tm_log(TM_LOG_BAD, g_tm.line);
            return;
        }
        g_tm.neg_offer = id;
        offer = id >= 0 ? find_offer(id) : (MarketOffer *)0;
    }
    offer = tm_any_offer(g_tm.neg_offer);
    if (offer) tm_log_event(base, offer);
    else if (g_neg_event == NEG_TALKS_ENDED || g_neg_event == NEG_RIVAL_SIGNED) tm_log(TM_LOG_BAD, tm_w("Talks are over for this window."));
    tm_refresh(base);
}

static void tm_decide(uint32_t base, int32_t decision, int32_t amount) {
    MarketOffer *offer = g_tm.neg_offer >= 0 ? find_offer(g_tm.neg_offer) : (MarketOffer *)0;
    if (!offer) return;
    career_market_respond_to_offer(base, offer->offer_id, decision, amount);
    offer = tm_any_offer(g_tm.neg_offer);
    if (offer) tm_log_event(base, offer);
    tm_refresh(base);
}

static void tm_answer_bid(uint32_t base, int32_t offer_id, int32_t decision) {
    MarketOffer *offer = find_offer(offer_id);
    if (!offer) return;
    int32_t fee = offer->status == CM_OFFER_WAIT_USER_SELLER_COUNTER ? offer->counter_fee : offer->fee;
    uint16_t club[48];
    tm_team_name(base, offer->buyer_id, club, 48);
    int32_t amount = decision == CM_DECISION_COUNTER ? round5(mul_div(fee, 110, 100)) : 0;
    career_market_respond_to_offer(base, offer_id, decision, amount);
    offer = tm_any_offer(offer_id);
    MarketUiText t;
    ui_text_reset(&t, g_tm.line, 160);
    if (g_neg_event == NEG_SOLD) {
        ui_text_append_ascii(&t, "Sold to ");
        ui_text_append_wide(&t, club, 40);
    } else if (g_neg_event == NEG_BUYER_WALKED) {
        ui_text_append_wide(&t, club, 40);
        ui_text_append_ascii(&t, " walked away.");
    } else if (g_neg_event == NEG_BUYER_FINAL && offer) {
        ui_text_append_wide(&t, club, 40);
        ui_text_append_ascii(&t, " can only go to ");
        tm_append_money(&t, offer->counter_fee);
        ui_text_append_ascii(&t, ". Final offer.");
    } else if (offer && offer->status == CM_OFFER_WAIT_USER_SELLER_COUNTER) {
        ui_text_append_wide(&t, club, 40);
        ui_text_append_ascii(&t, " offers ");
        tm_append_money(&t, offer->counter_fee);
    } else if (decision == CM_DECISION_REJECT) {
        ui_text_append_ascii(&t, "You turned down ");
        ui_text_append_wide(&t, club, 40);
    } else {
        ui_text_append_ascii(&t, "Done.");
    }
    tm_toast(g_tm.line);
    tm_refresh(base);
}

static int32_t tm_cb_search(int32_t selection) {
    uint32_t base = g_ui_base;
    void *box = g_ui_keyboard_box;
    g_ui_keyboard_box = (void *)0;
    if (selection == 0 || !box) return 1;
    void *field = *(void **)((char *)box + 0xCE4);
    const uint16_t *text = field ? ((TextFieldGetTextFn)(base + 0x241FB5))(field) : (const uint16_t *)0;
    int32_t n = 0;
    for (int32_t i = 0; i < 32; ++i) g_tm.query[i] = 0;
    while (text && text[n] && n < 31) { g_tm.query[n] = text[n]; ++n; }
    while (n > 0 && g_tm.query[n - 1] == ' ') g_tm.query[--n] = 0;
    g_tm.tab = TM_TAB_SCOUT;
    g_tm.scroll = 0.0f;
    g_tm.dirty = 1;
    return 1;
}

static int32_t tm_cb_fee(int32_t selection) {
    uint32_t base = g_ui_base;
    void *box = g_ui_keyboard_box;
    g_ui_keyboard_box = (void *)0;
    if (selection == 0 || !box) return 1;
    void *field = *(void **)((char *)box + 0xCE4);
    const uint16_t *text = field ? ((TextFieldGetTextFn)(base + 0x241FB5))(field) : (const uint16_t *)0;
    int32_t value = 0, digits = 0;
    for (int32_t i = 0; text && text[i] && i < 16; ++i) {
        if (text[i] >= '0' && text[i] <= '9' && value < 100000000) { value = value * 10 + (text[i] - '0'); ++digits; }
    }
    if (!digits || value <= 0) {
        tm_toast(tm_w("Type the fee in coins, digits only."));
        return 1;
    }
    value = round5(value);
    if (value > g_tm.neg_hi) g_tm.neg_hi = value;
    if (value < g_tm.neg_lo) g_tm.neg_lo = value;
    g_tm.neg_fee = value;
    return 1;
}

static void tm_open_fee_keyboard(uint32_t base) {
    ui_set_title("Your Fee Offer");
    MarketUiText t;
    ui_text_reset(&t, g_ui_keyboard_initial, 48);
    ui_text_append_i32(&t, g_tm.neg_fee);
    ui_text_reset(&t, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&t, "Fee in coins");
    if (!ui_show_keyboard(base, 9, tm_cb_fee)) g_ui_keyboard_box = (void *)0;
    else g_tm.kb_wait = 120;
}

static void tm_open_search(uint32_t base) {
    ui_set_title("Search Players");
    MarketUiText t;
    ui_text_reset(&t, g_ui_keyboard_initial, 48);
    ui_text_append_wide(&t, g_tm.query, 31);
    ui_text_reset(&t, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&t, "Player or club name. Leave empty to show everyone.");
    if (!ui_show_keyboard(base, 31, tm_cb_search)) g_ui_keyboard_box = (void *)0;
    else g_tm.kb_wait = 120;
}

/* The keyboard box only showed its text field: the Android keyboard opens when the field is tapped, and
 * that tap did not reach it on the tablet. Open it ourselves (CFETextField::ShowKeyboard(true) 0x241664)
 * as soon as the box and its field exist. */
static void tm_keyboard_poke(uint32_t base) {
    if (g_tm.kb_wait <= 0) return;
    --g_tm.kb_wait;
    void *box = g_ui_keyboard_box;
    void *field = box ? *(void **)((char *)box + 0xCE4) : (void *)0;
    if (!field) return;
    typedef void (*TmShowKeyboardFn)(void *, int32_t);
    ((TmShowKeyboardFn)(base + 0x241665))(field, 1);
    g_tm.kb_wait = 0;
}

/* ---------------------------------------------------------------------------------------------------------------
 * render
 * ------------------------------------------------------------------------------------------------------------- */
static void tm_draw_top(uint32_t base) {
    float dw = g_tm.dw;
    tm_rect(0, 0, dw, TM_TOP_H, TM_GROUND);
    tm_rect(0, TM_TOP_H - 1.0f, dw, 1.0f, TM_LINE);
    /* Back: a real button (closes the negotiation sheet first, then leaves the screen) */
    tm_rect(10.0f, 9.0f, 88.0f, 38.0f, TM_TURF2);
    tm_rect(10.0f, 9.0f, 88.0f, 1.0f, TM_LINE);
    tm_rect(10.0f, 46.0f, 88.0f, 1.0f, TM_LINE);
    tm_triangle(20.0f, 28.0f, 30.0f, 20.0f, 30.0f, 36.0f, TM_CHALK);   /* arrow head + shaft */
    tm_rect(29.0f, 26.5f, 7.0f, 3.0f, TM_CHALK);
    tm_text(tm_w(g_tm.neg_open ? "Close" : "Back"), 42.0f, 19.0f, 15.0f, TM_CHALK, TM_ALIGN_LEFT, 0, 1);
    tm_hit(4.0f, 2.0f, 104.0f, 52.0f, TM_HIT_BACK, 0);
    const uint16_t *title = tm_w("TRANSFER MARKET");
    tm_text(title, 116.0f, 15.0f, 24.0f, TM_CHALK, TM_ALIGN_LEFT, 0, 1);
    float x = 116.0f + tm_text_width(title, 24.0f) + 16.0f;
    int32_t open = g_market.window_id >= 0;
    MarketUiText t;
    ui_text_reset(&t, g_tm.line, 160);
    if (open) {
        ui_text_append_ascii(&t, "WINDOW OPEN  ");
        ui_text_append_i32(&t, window_matches_left());
        ui_text_append_ascii(&t, window_matches_left() == 1 ? " match left" : " matches left");
    } else {
        ui_text_append_ascii(&t, "WINDOW CLOSED");
    }
    float pw = tm_text_width(g_tm.line, 12.0f) + 24.0f;
    tm_rect(x, 15.0f, pw, 26.0f, TM_TURF2);
    tm_text(g_tm.line, x + 12.0f, 21.0f, 12.0f, open ? TM_GOOD : TM_CHALK2, TM_ALIGN_LEFT, 0, 1);
    int32_t user_index = tm_user_index();
    if (user_index < 0) return;
    ClubAccount *club = &g_market.clubs[user_index];
    float rx = dw - 16.0f;
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_i32(&t, g_total_count[user_index]);
    ui_text_append_char(&t, '/');
    ui_text_append_i32(&t, g_user_squad_max);
    const char *labels[3] = {"SQUAD", "WAGE ROOM", "COINS"};
    const uint16_t *values[3] = {g_tm.line, tm_money(account_wage_room(club)), tm_money(user_coins(base))};
    for (int32_t i = 0; i < 3; ++i) {
        float w = tm_text_width(values[i], 21.0f);
        float lw = tm_text_width(tm_w(labels[i]), 10.0f);
        if (lw > w) w = lw;
        tm_text(tm_w(labels[i]), rx, 8.0f, 10.0f, TM_CHALK3, TM_ALIGN_RIGHT, 0, 0);
        tm_text(values[i], rx, 22.0f, 21.0f, i == 2 ? TM_AMBER : TM_CHALK, TM_ALIGN_RIGHT, 0, 1);
        rx -= w + 20.0f;
    }
}

static int32_t tm_tab_count(int32_t tab) {
    int32_t n = 0;
    if (tab == TM_TAB_SHORTLIST) return shortlist_count();
    if (tab == TM_TAB_OFFERS || tab == TM_TAB_SQUAD) {
        if (tab == TM_TAB_SQUAD) {
            int32_t user_index = tm_user_index();
            return user_index >= 0 ? g_total_count[user_index] : 0;
        }
        for (int32_t i = 0; i < OFFER_CAPACITY; ++i) {
            MarketOffer *offer = &g_market.offers[i];
            if (offer_is_active(offer) && (offer->seller_id == USER_TEAM_ID || offer->buyer_id == USER_TEAM_ID)) ++n;
        }
        return n;
    }
    return -1;
}

static void tm_draw_rail(void) {
    static const char *const names[TM_TAB_COUNT] = {"Scout", "Shortlist", "For You", "Offers", "My Squad"};
    float h = g_tm.height / g_tm.s;
    tm_rect(0, TM_TOP_H, TM_RAIL_W, h - TM_TOP_H, TM_GROUND);
    tm_rect(TM_RAIL_W - 1.0f, TM_TOP_H, 1.0f, h - TM_TOP_H, TM_LINE);
    for (int32_t i = 0; i < TM_TAB_COUNT; ++i) {
        float y = TM_TOP_H + 10.0f + (float)i * 48.0f;
        int32_t active = g_tm.tab == i;
        if (active) {
            tm_rect(8.0f, y, TM_RAIL_W - 16.0f, 46.0f, TM_TURF2);
            tm_rect(8.0f, y, 3.0f, 46.0f, TM_AMBER);
        }
        tm_text(tm_w(names[i]), 22.0f, y + 15.0f, 15.0f, active ? TM_CHALK : TM_CHALK2, TM_ALIGN_LEFT, 0, active);
        int32_t count = tm_tab_count(i);
        if (count >= 0) {
            MarketUiText t;
            uint16_t buf[8];
            ui_text_reset(&t, buf, 8);
            ui_text_append_i32(&t, count);
            int32_t hot = i == TM_TAB_OFFERS && count > 0;
            tm_rect(TM_RAIL_W - 50.0f, y + 13.0f, 32.0f, 20.0f, hot ? TM_AMBER : TM_TURF3);
            tm_text(buf, TM_RAIL_W - 34.0f, y + 16.0f, 12.0f, hot ? TM_AMBER_INK : TM_CHALK, TM_ALIGN_CENTER, 0, 1);
        }
        tm_hit(8.0f, y, TM_RAIL_W - 16.0f, 46.0f, TM_HIT_TAB, i);
    }
    /* market pulse: average market index per position */
    float py = h - 128.0f;
    tm_rect(10.0f, py - 10.0f, TM_RAIL_W - 20.0f, 1.0f, TM_LINE);
    tm_text(tm_w("MARKET PULSE"), 14.0f, py, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    for (int32_t p = 0; p < 4; ++p) {
        int32_t sum = 0, n = 0;
        for (int32_t b = 0; b < BAND_COUNT; ++b) {
            int32_t index = g_ext.market_index[p][b];
            if (index >= MARKET_INDEX_MIN && index <= MARKET_INDEX_MAX) { sum += index; ++n; }
        }
        int32_t d = n ? sum / n - 100 : 0;
        float y = py + 20.0f + (float)p * 22.0f;
        tm_text(tm_w(tm_pos_label(p)), 14.0f, y, 12.0f, TM_CHALK2, TM_ALIGN_LEFT, 0, 0);
        float bx = 50.0f, bw = 78.0f;
        tm_rect(bx, y + 4.0f, bw, 6.0f, TM_TURF2);
        float len = (float)(d < 0 ? -d : d) * 3.0f;
        if (len > bw * 0.5f) len = bw * 0.5f;
        if (d >= 0) tm_rect(bx + bw * 0.5f, y + 4.0f, len, 6.0f, TM_SKY);
        else tm_rect(bx + bw * 0.5f - len, y + 4.0f, len, 6.0f, TM_CHALK3);
        MarketUiText t;
        uint16_t buf[12];
        ui_text_reset(&t, buf, 12);
        if (d > 0) ui_text_append_char(&t, '+');
        ui_text_append_i32(&t, d);
        ui_text_append_char(&t, '%');
        tm_text(buf, TM_RAIL_W - 12.0f, y, 12.0f, d >= 0 ? TM_SKY : TM_CHALK3, TM_ALIGN_RIGHT, 0, 0);
    }
}

static void tm_draw_tools(float x, float w) {
    float y = TM_TOP_H;
    tm_rect(x, y, w, TM_TOOLS_H, TM_TURF);
    tm_rect(x, y + TM_TOOLS_H - 1.0f, w, 1.0f, TM_LINE);
    if (g_tm.tab == TM_TAB_OFFERS || g_tm.tab == TM_TAB_SQUAD) {
        MarketUiText t;
        ui_text_reset(&t, g_tm.line, 160);
        int32_t value = 0, listed = 0, bids = 0;
        for (int32_t r = 0; r < g_tm.row_count; ++r) {
            value += g_tm.rows[r].value;
            listed += g_tm.rows[r].listed;
            bids += g_tm.rows[r].incoming;
        }
        if (g_tm.tab == TM_TAB_SQUAD) {
            ui_text_append_i32(&t, g_tm.row_count);
            ui_text_append_ascii(&t, " players    Squad value ");
            tm_append_money(&t, value);
            ui_text_append_ascii(&t, "    Listed for sale ");
            ui_text_append_i32(&t, listed);
        } else {
            ui_text_append_i32(&t, bids);
            ui_text_append_ascii(&t, bids == 1 ? " bid for your players    " : " bids for your players    ");
            ui_text_append_i32(&t, g_tm.row_count - bids);
            ui_text_append_ascii(&t, " of your bids running (at most 3)");
        }
        tm_text(g_tm.line, x + 18.0f, y + 17.0f, 13.0f, TM_CHALK2, TM_ALIGN_LEFT, w - 36.0f, 0);
        return;
    }
    float sort_w = 104.0f, seg_w = 5.0f * 42.0f + 4.0f;
    float search_w = w - 24.0f - seg_w - sort_w - 16.0f;
    float sx = x + 12.0f, sy = y + 8.0f;
    tm_rect(sx, sy, search_w, 36.0f, TM_GROUND);
    tm_rect(sx, sy + 35.0f, search_w, 1.0f, TM_LINE);
    tm_text(g_tm.query[0] ? g_tm.query : tm_w("Search players or clubs"), sx + 12.0f, sy + 11.0f, 13.0f,
            g_tm.query[0] ? TM_CHALK : TM_CHALK3, TM_ALIGN_LEFT, search_w - (g_tm.query[0] ? 84.0f : 24.0f), 0);
    tm_hit(sx, sy, search_w, 36.0f, TM_HIT_SEARCH, 0);
    if (g_tm.query[0]) {                       /* clear the search (registered last: wins over the field) */
        float cx = sx + search_w - 56.0f;
        tm_rect(cx, sy + 6.0f, 50.0f, 24.0f, TM_TURF3);
        tm_text(tm_w("Clear"), cx + 25.0f, sy + 11.0f, 11.0f, TM_CHALK, TM_ALIGN_CENTER, 0, 1);
        tm_hit(cx - 4.0f, sy, 58.0f, 36.0f, TM_HIT_CLEAR_SEARCH, 0);
    }
    float gx = sx + search_w + 8.0f;
    tm_rect(gx, sy, seg_w, 36.0f, TM_GROUND);
    static const char *const labels[5] = {"All", "GK", "DEF", "MID", "FWD"};
    for (int32_t i = 0; i < 5; ++i) {
        float bx = gx + 2.0f + (float)i * 42.0f;
        int32_t on = g_tm.pos_filter == i - 1;
        if (on) tm_rect(bx, sy + 3.0f, 40.0f, 30.0f, TM_TURF3);
        tm_text(tm_w(labels[i]), bx + 20.0f, sy + 11.0f, 12.0f, on ? TM_CHALK : TM_CHALK2, TM_ALIGN_CENTER, 0, 1);
        tm_hit(bx, sy, 40.0f, 36.0f, TM_HIT_POS, i - 1);
    }
    static const char *const sorts[TM_SORT_COUNT] = {"Best rating", "Lowest price", "Best value"};
    float ox = gx + seg_w + 8.0f;
    tm_rect(ox, sy, sort_w, 36.0f, TM_GROUND);
    tm_text(tm_w(sorts[g_tm.sort]), ox + sort_w * 0.5f, sy + 11.0f, 12.0f, TM_CHALK, TM_ALIGN_CENTER, sort_w - 8.0f, 0);
    tm_hit(ox, sy, sort_w, 36.0f, TM_HIT_SORT, 0);
}

static void tm_draw_row(const TmRow *row, float x, float y, float w) {
    int32_t selected = row->player_id == g_tm.sel_id;
    if (selected) tm_rect(x + 8.0f, y, w - 16.0f, TM_ROW_H - 2.0f, TM_TURF3);
    tm_badge(x + 18.0f, y + 7.0f, 44.0f, row->rating, 22.0f);
    float name_x = x + 74.0f;
    int32_t actions = g_tm.tab == TM_TAB_OFFERS && row->incoming;
    int32_t squad = g_tm.tab == TM_TAB_SQUAD;
    float right = x + w - (actions ? 250.0f : squad ? 130.0f : 60.0f);
    float price_x = right - (actions || squad ? 0.0f : 80.0f);
    tm_text(row->name, name_x, y + 10.0f, 14.0f, TM_CHALK, TM_ALIGN_LEFT, price_x - name_x - 100.0f, 1);
    tm_text(row->extra[0] && g_tm.tab != TM_TAB_SCOUT && g_tm.tab != TM_TAB_SHORTLIST ? row->extra : row->sub,
            name_x, y + 31.0f, 12.0f, g_tm.tab == TM_TAB_FORYOU ? TM_SKY : TM_CHALK2, TM_ALIGN_LEFT,
            price_x - name_x - (actions || squad ? 10.0f : 100.0f), 0);
    if (!actions && !squad) {
        tm_text(tm_money(row->price), price_x, y + 9.0f, 18.0f, TM_CHALK, TM_ALIGN_RIGHT, 0, 1);
        MarketUiText t;
        uint16_t buf[32];
        ui_text_reset(&t, buf, 32);
        ui_text_append_ascii(&t, "value ");
        tm_append_money(&t, row->value);
        tm_text(buf, price_x, y + 32.0f, 11.0f, TM_CHALK3, TM_ALIGN_RIGHT, 0, 0);
        ui_text_reset(&t, buf, 32);
        ui_text_append_ascii(&t, row->trend >= 0 ? "+" : "");
        ui_text_append_i32(&t, row->trend);
        ui_text_append_char(&t, '%');
        tm_text(buf, right, y + 12.0f, 12.0f, row->trend >= 0 ? TM_SKY : TM_CHALK3, TM_ALIGN_RIGHT, 0, 0);
        const char *status = row->talks ? "In talks" : tm_block_short(row->block);
        if (status) tm_text(tm_w(status), right, y + 32.0f, 11.0f, row->talks ? TM_AMBER : TM_CHALK3, TM_ALIGN_RIGHT, 0, 1);
        /* shortlist toggle */
        float sx = x + w - 54.0f;
        tm_rect(sx, y + 17.0f, 46.0f, 24.0f, row->starred ? TM_AMBER : TM_GROUND);
        tm_text(tm_w(row->starred ? "Saved" : "Save"), sx + 23.0f, y + 22.0f, 11.0f,
                row->starred ? TM_AMBER_INK : TM_CHALK2, TM_ALIGN_CENTER, 0, 1);
        tm_hit(sx - 4.0f, y, 54.0f, TM_ROW_H, TM_HIT_STAR, row->player_id);
    } else if (actions) {
        float bx = x + w - 246.0f;
        tm_button(bx, y + 11.0f, 74.0f, 36.0f, tm_w("Accept"), 0, 1, TM_HIT_INBID_ACCEPT, row->offer_id);
        tm_button(bx + 80.0f, y + 11.0f, 86.0f, 36.0f, tm_w("Ask +10%"), 0, 1, TM_HIT_INBID_COUNTER, row->offer_id);
        tm_button(bx + 172.0f, y + 11.0f, 66.0f, 36.0f, tm_w("Reject"), 0, 1, TM_HIT_INBID_REJECT, row->offer_id);
    } else {
        tm_button(x + w - 126.0f, y + 11.0f, 116.0f, 36.0f, tm_w(row->listed ? "Listed" : "List for sale"), 0, 1,
                  TM_HIT_LISTTOGGLE, row->player_id);
    }
    tm_hit(x + 8.0f, y, (actions ? w - 256.0f : squad ? w - 136.0f : w - 60.0f), TM_ROW_H, TM_HIT_ROW, row->player_id);
}

static void tm_draw_list(float x, float w) {
    float top = TM_TOP_H + TM_TOOLS_H;
    float h = g_tm.height / g_tm.s - top;
    tm_rect(x, top, w, h, TM_TURF);
    tm_hit(x, top, w, h, TM_HIT_LIST, 0);
    g_tm.scroll_max = (float)g_tm.row_count * TM_ROW_H + 12.0f - h;
    if (g_tm.scroll_max < 0.0f) g_tm.scroll_max = 0.0f;
    if (g_tm.scroll > g_tm.scroll_max) g_tm.scroll = g_tm.scroll_max;
    if (g_tm.scroll < 0.0f) g_tm.scroll = 0.0f;
    if (!g_tm.row_count) {
        const char *empty = g_tm.tab == TM_TAB_SHORTLIST ? "Tap Save on a player to keep him here."
            : g_tm.tab == TM_TAB_OFFERS ? "No bids running. Bids for your players show up here."
            : g_tm.tab == TM_TAB_FORYOU ? "Nobody affordable improves your squad right now."
            : "No players match. Clear the search or pick another position.";
        tm_text(tm_w(empty), x + w * 0.5f, top + 60.0f, 13.0f, TM_CHALK3, TM_ALIGN_CENTER, w - 40.0f, 0);
        return;
    }
    /* whole rows only: nothing is drawn over the toolbar */
    for (int32_t i = 0; i < g_tm.row_count; ++i) {
        float y = top + 6.0f + (float)i * TM_ROW_H - g_tm.scroll;
        if (y < top || y + TM_ROW_H > top + h) continue;
        tm_draw_row(&g_tm.rows[i], x, y, w);
    }
    if (g_tm.scroll_max > 0.0f) {
        float bar_h = h * h / (h + g_tm.scroll_max);
        float bar_y = top + (h - bar_h) * g_tm.scroll / g_tm.scroll_max;
        tm_rect(x + w - 4.0f, bar_y, 3.0f, bar_h, TM_LINE);
    }
}

static void tm_draw_detail(float x) {
    float h = g_tm.height / g_tm.s;
    tm_rect(x, TM_TOP_H, TM_DETAIL_W, h - TM_TOP_H, TM_GROUND);
    tm_rect(x, TM_TOP_H, 1.0f, h - TM_TOP_H, TM_LINE);
    TmDetail *d = &g_tm.detail;
    float px = x + 16.0f, pw = TM_DETAIL_W - 32.0f;
    if (!d->loaded || d->player_id != g_tm.sel_id) {
        tm_text(tm_w("Pick a player to see his details."), px, TM_TOP_H + 24.0f, 13.0f, TM_CHALK2, TM_ALIGN_LEFT, pw, 0);
        return;
    }
    float y = TM_TOP_H + 16.0f;
    tm_badge(px, y, 58.0f, d->rating, 30.0f);
    tm_text(d->name, px + 70.0f, y + 8.0f, 17.0f, TM_CHALK, TM_ALIGN_LEFT, pw - 70.0f, 1);
    MarketUiText t;
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, tm_pos_label(d->position));
    ui_text_append_ascii(&t, "  ");
    ui_text_append_wide(&t, d->club, 40);
    tm_text(g_tm.line, px + 70.0f, y + 33.0f, 12.0f, TM_CHALK2, TM_ALIGN_LEFT, pw - 70.0f, 0);
    y += 72.0f;
    /* facts */
    float fw = (pw - 16.0f) / 3.0f;
    const char *fact_labels[3] = {"ROLE", "CONTRACT", "WAGE"};
    uint16_t contract[24];
    ui_text_reset(&t, contract, 24);
    if (d->tenths < 10) ui_text_append_ascii(&t, "Ends soon");
    else {
        ui_text_append_i32(&t, d->tenths / 10);
        ui_text_append_char(&t, '.');
        ui_text_append_i32(&t, d->tenths % 10);
        ui_text_append_ascii(&t, " seasons");
    }
    const uint16_t *facts[3] = {d->role, contract, tm_money(d->wage)};
    for (int32_t i = 0; i < 3; ++i) {
        float fx = px + (float)i * (fw + 8.0f);
        tm_rect(fx, y, fw, 46.0f, TM_TURF);
        tm_text(tm_w(fact_labels[i]), fx + 8.0f, y + 7.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 0);
        tm_text(facts[i], fx + 8.0f, y + 22.0f, 13.0f, TM_CHALK, TM_ALIGN_LEFT, fw - 12.0f, 1);
    }
    y += 58.0f;
    /* stats, two columns */
    float cw = (pw - 14.0f) * 0.5f;
    for (int32_t i = 0; i < d->stat_count; ++i) {
        float sx = px + (float)(i % 2) * (cw + 14.0f);
        float sy = y + (float)(i / 2) * 30.0f;
        tm_text(d->stat_labels[i], sx, sy, 11.0f, TM_CHALK2, TM_ALIGN_LEFT, cw - 30.0f, 0);
        uint16_t buf[8];
        ui_text_reset(&t, buf, 8);
        ui_text_append_i32(&t, d->stats[i]);
        tm_text(buf, sx + cw, sy, 12.0f, TM_CHALK, TM_ALIGN_RIGHT, 0, 1);
        tm_rect(sx, sy + 17.0f, cw, 4.0f, TM_TURF2);
        tm_rect(sx, sy + 17.0f, cw * (float)d->stats[i] / 99.0f, 4.0f, TM_CHALK2);
    }
    y += (float)((d->stat_count + 1) / 2) * 30.0f + 6.0f;
    /* chance he joins */
    int32_t own = g_players[d->index].owner_id == USER_TEAM_ID;
    if (!own) {
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, d->join_pm >= 900 ? "Keen to move" : d->join_pm >= 700 ? "Open to it"
                                 : d->join_pm >= 500 ? "Unsure" : "Reluctant");
        ui_text_append_ascii(&t, "  ");
        ui_text_append_i32(&t, (d->join_pm + 5) / 10);
        ui_text_append_char(&t, '%');
        tm_text(tm_w("Chance he joins"), px, y, 12.0f, TM_CHALK, TM_ALIGN_LEFT, 0, 1);
        tm_text(g_tm.line, px + pw, y, 12.0f, TM_CHALK2, TM_ALIGN_RIGHT, 0, 0);
        tm_rect(px, y + 18.0f, pw, 8.0f, TM_TURF2);
        uint32_t c = d->join_pm >= 700 ? TM_GOOD : d->join_pm >= 500 ? TM_AMBER : TM_BAD;
        tm_rect(px, y + 18.0f, pw * (float)d->join_pm / 1000.0f, 8.0f, c);
        y += 36.0f;
        if (d->rivals > 0) {
            ui_text_reset(&t, g_tm.line, 160);
            ui_text_append_ascii(&t, "Also interested: ");
            ui_text_append_wide(&t, d->rival_name, 40);
            if (d->rivals > 1) {
                ui_text_append_ascii(&t, " +");
                ui_text_append_i32(&t, d->rivals - 1);
            }
            tm_text(g_tm.line, px, y, 12.0f, TM_AMBER, TM_ALIGN_LEFT, pw, 0);
            y += 22.0f;
        }
    }
    if (own) {
        ui_text_reset(&t, g_tm.line, 160);
        if (d->bid_fee > 0) {
            ui_text_append_ascii(&t, "Best bid now: ");
            tm_append_money(&t, d->bid_fee);
            ui_text_append_ascii(&t, " from ");
            ui_text_append_wide(&t, d->rival_name, 40);
        } else {
            ui_text_append_ascii(&t, "No club would bid for him right now");
        }
        tm_text(g_tm.line, px, y, 12.0f, d->bid_fee > 0 ? TM_SKY : TM_CHALK3, TM_ALIGN_LEFT, pw, 0);
        y += 22.0f;
    }
    /* price box */
    tm_rect(px, y, pw, 58.0f, TM_TURF);
    tm_text(tm_w(own ? "CLUBS WOULD PAY" : "ASKING PRICE"), px + 12.0f, y + 8.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 0);
    tm_text(tm_money(d->ask), px + 12.0f, y + 24.0f, 24.0f, TM_AMBER, TM_ALIGN_LEFT, 0, 1);
    tm_text(tm_w("MARKET VALUE"), px + pw * 0.5f + 8.0f, y + 8.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 0);
    tm_text(tm_money(d->value), px + pw * 0.5f + 8.0f, y + 24.0f, 24.0f, TM_CHALK, TM_ALIGN_LEFT, 0, 1);
    /* actions */
    float ay = h - 60.0f;
    tm_rect(x, ay - 1.0f, TM_DETAIL_W, 1.0f, TM_LINE);
    if (own) {
        tm_button(px, ay + 8.0f, pw, 44.0f, tm_w(user_player_is_listed(d->player_id) ? "Remove from sale" : "List for sale"),
                  1, 1, TM_HIT_LISTTOGGLE, d->player_id);
        return;
    }
    MarketOffer *offer = tm_user_offer_for(d->player_id);
    int32_t talks = offer && offer->buyer_id == USER_TEAM_ID;
    int32_t can = talks || d->block == 0;
    if (!can) {
        const char *why = tm_bid_error(d->block);
        tm_text_wrap(tm_w(why ? why : "You cannot bid for him right now."), px, ay - 40.0f, 12.0f, TM_AMBER, pw, 2, 16.0f);
    }
    const char *label = talks ? "Continue talks" : can ? "Make offer" : "Can't bid";
    tm_button(px, ay + 8.0f, pw - 118.0f, 44.0f, tm_w(label), 1, can, TM_HIT_OFFER, d->player_id);
    tm_button(px + pw - 110.0f, ay + 8.0f, 110.0f, 44.0f, tm_w(tm_is_starred(d->player_id) ? "Shortlisted" : "Shortlist"),
              0, 1, TM_HIT_SHORTLIST, d->player_id);
}

static void tm_draw_sheet(float x, float w) {
    float h = g_tm.height / g_tm.s;
    float top = TM_TOP_H;
    tm_rect(x, top, w, h - top, TM_TURF);
    tm_hit(x, top, w, h - top, TM_HIT_SHEET, 0);
    int32_t index = find_cached_player(g_tm.neg_player);
    int32_t user_index = tm_user_index();
    if (index < 0 || user_index < 0) { g_tm.neg_open = 0; return; }
    MarketPlayer *player = &g_players[index];
    MarketOffer *offer = tm_any_offer(g_tm.neg_offer);
    int32_t active = offer && offer_is_active(offer);
    int32_t done = offer && offer->status == CM_OFFER_COMPLETED;
    int32_t wage_stage = active && offer->status == CM_OFFER_WAIT_USER_WAGE;
    float deal_w = 400.0f;
    float log_w = w - deal_w;
    /* header */
    uint16_t name[40];
    tm_player_name(g_tm.base, player->player_id, name, 40);
    tm_badge(x + 16.0f, top + 12.0f, 44.0f, player->rating, 22.0f);
    tm_text(name, x + 72.0f, top + 14.0f, 15.0f, TM_CHALK, TM_ALIGN_LEFT, log_w - 90.0f, 1);
    int32_t ask = g_tm.neg_ask;
    MarketUiText t;
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, done ? "Signed from " : "Talks with ");
    ui_text_append_wide(&t, g_tm.neg_club, 40);
    if (!done) {
        ui_text_append_ascii(&t, "  asking ");
        tm_append_money(&t, ask);
    }
    tm_text(g_tm.line, x + 72.0f, top + 36.0f, 12.0f, TM_CHALK2, TM_ALIGN_LEFT, log_w - 90.0f, 0);
    tm_rect(x, top + 68.0f, log_w, 1.0f, TM_LINE);
    /* log */
    float ly = top + 80.0f;
    for (int32_t i = 0; i < g_tm.log_count; ++i) {
        int32_t kind = g_tm.log_kind[i];
        float mw = log_w - 90.0f;
        float lw = tm_text_width(g_tm.log[i], 13.0f);
        int32_t lines = lw > mw ? 2 : 1;
        float bh = 20.0f + 16.0f * (float)lines;
        float bx = kind == TM_LOG_YOU ? x + log_w - mw - 32.0f : x + 16.0f;
        uint32_t fill = kind == TM_LOG_YOU ? TM_GROUND : kind == TM_LOG_CLUB ? TM_TURF3 : TM_GROUND;
        tm_rect(bx, ly, mw + 16.0f, bh, fill);
        if (kind == TM_LOG_RIVAL || kind == TM_LOG_GOOD || kind == TM_LOG_BAD) {
            uint32_t edge = kind == TM_LOG_RIVAL ? TM_AMBER : kind == TM_LOG_GOOD ? TM_GOOD : TM_BAD;
            tm_rect(bx, ly, 3.0f, bh, edge);
        }
        const char *who = kind == TM_LOG_YOU ? "YOU" : kind == TM_LOG_RIVAL ? "RIVAL" : kind == TM_LOG_CLUB ? "CLUB" : "RESULT";
        tm_text(tm_w(who), bx + 10.0f, ly + 5.0f, 9.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 0);
        if (lines == 1) {
            tm_text(g_tm.log[i], bx + 10.0f, ly + 18.0f, 13.0f, TM_CHALK, TM_ALIGN_LEFT, mw, 0);
        } else {
            /* split at the last space that fits */
            uint16_t first[TM_TEXT];
            int32_t n = 0, cut = 0;
            while (g_tm.log[i][n] && n < TM_TEXT - 1) { first[n] = g_tm.log[i][n]; ++n; }
            first[n] = 0;
            for (int32_t k = n - 1; k > 0; --k) {
                if (first[k] != ' ') continue;
                first[k] = 0;
                if (tm_text_width(first, 13.0f) <= mw) { cut = k; break; }
            }
            if (!cut) { cut = n / 2; first[cut] = 0; }
            tm_text(first, bx + 10.0f, ly + 18.0f, 13.0f, TM_CHALK, TM_ALIGN_LEFT, mw, 0);
            tm_text(g_tm.log[i] + cut + (g_tm.log[i][cut] == ' '), bx + 10.0f, ly + 34.0f, 13.0f, TM_CHALK, TM_ALIGN_LEFT, mw, 0);
        }
        ly += bh + 8.0f;
    }
    /* deal column */
    float dx = x + log_w;
    tm_rect(dx, top, deal_w, h - top, TM_GROUND);
    tm_rect(dx, top, 1.0f, h - top, TM_LINE);
    float px = dx + 18.0f, pw = deal_w - 36.0f;
    float y = top + 16.0f;
    int32_t coins = user_coins(g_tm.base);
    int32_t ended = offer && !active && !done;
    if (done || ended) {
        /* talks are over: the result instead of the controls */
        tm_text(tm_w(done ? "TRANSFER COMPLETE" : "TALKS ENDED"), px, y, 10.0f, done ? TM_GOOD : TM_BAD, TM_ALIGN_LEFT, 0, 1);
        if (done) {
            tm_text(tm_money(offer->fee), px, y + 18.0f, 36.0f, TM_AMBER, TM_ALIGN_LEFT, 0, 1);
            ui_text_reset(&t, g_tm.line, 160);
            ui_text_append_i32(&t, offer->contract_years);
            ui_text_append_ascii(&t, offer->contract_years == 1 ? " year at " : " years at ");
            tm_append_money(&t, offer->annual_wage);
            ui_text_append_ascii(&t, " a season");
            tm_text(g_tm.line, px, y + 66.0f, 13.0f, TM_CHALK, TM_ALIGN_LEFT, pw, 0);
            tm_text_wrap(tm_w("He is in your squad now. Pick him in Team Management for the next match."), px, y + 96.0f,
                         12.0f, TM_CHALK2, pw, 3, 17.0f);
        } else {
            tm_text_wrap(tm_w("No deal this time. You can try again for him in the next window."), px, y + 22.0f,
                         13.0f, TM_CHALK2, pw, 3, 18.0f);
        }
        tm_button(px, h - 78.0f, pw, 46.0f, tm_w("Done"), 1, 1, TM_HIT_LEAVE, 0);
        return;
    }
    if (wage_stage) {
        tm_text(tm_w("PERSONAL TERMS"), px, y, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
        tm_text(tm_money(offer->counter_wage), px, y + 18.0f, 36.0f, TM_AMBER, TM_ALIGN_LEFT, 0, 1);
        tm_text(tm_w("a season, the wage he asks"), px, y + 62.0f, 12.0f, TM_CHALK2, TM_ALIGN_LEFT, pw, 0);
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, "Fee agreed: ");
        tm_append_money(&t, offer->fee);
        ui_text_append_ascii(&t, ",  ");
        ui_text_append_i32(&t, offer->contract_years);
        ui_text_append_ascii(&t, " years");
        tm_text(g_tm.line, px, y + 86.0f, 12.0f, TM_CHALK, TM_ALIGN_LEFT, pw, 0);
        float by = h - 180.0f;
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, "Accept ");
        tm_append_money(&t, offer->counter_wage);
        tm_button(px, by, pw, 46.0f, g_tm.line, 1, 1, TM_HIT_WAGE_ACCEPT, 0);
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, "Offer ");
        tm_append_money(&t, round5(mul_div(offer->counter_wage, 92, 100)));
        tm_button(px, by + 56.0f, pw, 40.0f, g_tm.line, 0, 1, TM_HIT_WAGE_LOWER, 0);
        tm_button(px, by + 104.0f, pw, 40.0f, tm_w("Walk away"), 0, 1, TM_HIT_LEAVE, 1);
        return;
    }
    tm_text(tm_w("YOUR FEE OFFER"), px, y, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    tm_text(tm_money(g_tm.neg_fee), px, y + 16.0f, 36.0f, TM_AMBER, TM_ALIGN_LEFT, 0, 1);
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_i32(&t, ask > 0 ? mul_div(g_tm.neg_fee, 100, ask) : 0);
    ui_text_append_ascii(&t, "% of asking");
    tm_text(g_tm.line, px + pw, y + 30.0f, 13.0f, TM_CHALK2, TM_ALIGN_RIGHT, 0, 0);
    y += 66.0f;
    /* slider */
    int32_t can_bid = !done && (!offer || active) && g_market.window_id >= 0;
    float track_y = y + 12.0f;
    tm_rect(px, track_y, pw, 6.0f, TM_TURF2);
    float t01 = g_tm.neg_hi > g_tm.neg_lo ? (float)(g_tm.neg_fee - g_tm.neg_lo) / (float)(g_tm.neg_hi - g_tm.neg_lo) : 0.0f;
    if (t01 < 0.0f) t01 = 0.0f;
    if (t01 > 1.0f) t01 = 1.0f;
    tm_rect(px, track_y, pw * t01, 6.0f, TM_AMBER);
    float ask_t = g_tm.neg_hi > g_tm.neg_lo ? (float)(ask - g_tm.neg_lo) / (float)(g_tm.neg_hi - g_tm.neg_lo) : 0.5f;
    tm_rect(px + pw * ask_t - 1.0f, track_y - 6.0f, 2.0f, 18.0f, TM_CHALK2);
    tm_rect(px + pw * t01 - 9.0f, track_y - 7.0f, 18.0f, 20.0f, can_bid ? TM_CHALK : TM_CHALK3);
    if (can_bid) tm_hit(px - 10.0f, y - 6.0f, pw + 20.0f, 40.0f, TM_HIT_SLIDER, 0);
    tm_text(tm_money(g_tm.neg_lo), px, track_y + 16.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 0);
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, "asking ");
    tm_append_money(&t, ask);
    tm_text(g_tm.line, px + pw * ask_t, track_y + 16.0f, 10.0f, TM_CHALK2, TM_ALIGN_CENTER, 0, 0);
    tm_text(tm_money(g_tm.neg_hi), px + pw, track_y + 16.0f, 10.0f, TM_CHALK3, TM_ALIGN_RIGHT, 0, 0);
    y += 46.0f;
    /* fine steps and an exact amount */
    float bw3 = (pw - 12.0f) / 3.0f;
    tm_button(px, y, bw3, 34.0f, tm_w("- 5%"), 0, can_bid, TM_HIT_FEE_MINUS, 0);
    tm_button(px + bw3 + 6.0f, y, bw3, 34.0f, tm_w("+ 5%"), 0, can_bid, TM_HIT_FEE_PLUS, 0);
    tm_button(px + 2.0f * (bw3 + 6.0f), y, bw3, 34.0f, tm_w("Type fee"), 0, can_bid, TM_HIT_FEE_TYPE, 0);
    y += 46.0f;
    /* contract years */
    tm_text(tm_w("CONTRACT LENGTH"), px, y, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    float yb = (pw - 24.0f) / 5.0f;
    for (int32_t k = 1; k <= 5; ++k) {
        float bx = px + (float)(k - 1) * (yb + 6.0f);
        int32_t on = g_tm.neg_years == k;
        tm_rect(bx, y + 16.0f, yb, 36.0f, on ? TM_TURF3 : TM_TURF);
        uint16_t buf[8];
        ui_text_reset(&t, buf, 8);
        ui_text_append_i32(&t, k);
        ui_text_append_ascii(&t, " yr");
        tm_text(buf, bx + yb * 0.5f, y + 27.0f, 13.0f, on ? TM_CHALK : TM_CHALK2, TM_ALIGN_CENTER, 0, 1);
        if (can_bid && !offer) tm_hit(bx, y + 16.0f, yb, 36.0f, TM_HIT_YEARS, k);
    }
    y += 62.0f;
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, "He asks ");
    tm_append_money(&t, transfer_wage_demand(player, user_index, player->owner_index));
    ui_text_append_ascii(&t, " a season.");
    tm_text(g_tm.line, px, y, 12.0f, TM_CHALK2, TM_ALIGN_LEFT, pw, 0);
    y += 26.0f;
    /* patience */
    TalkMemory *talk = talk_memory(player->player_id, g_market.window_id, 0);
    int32_t left = 3000 - (talk ? talk->strikes : 0);
    tm_text(tm_w("CLUB'S PATIENCE"), px, y, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    float sw = (pw - 8.0f) / 3.0f;
    for (int32_t k = 0; k < 3; ++k) {
        float sx = px + (float)k * (sw + 4.0f);
        int32_t fill = left - k * 1000;
        if (fill < 0) fill = 0;
        if (fill > 1000) fill = 1000;
        tm_rect(sx, y + 16.0f, sw, 8.0f, TM_TURF2);
        tm_rect(sx, y + 16.0f, sw * (float)fill / 1000.0f, 8.0f, TM_SKY);
    }
    y += 34.0f;
    if (active) {
        OfferExt *ext = offer_ext(offer);
        if (ext->rival_team >= 0) {
            uint16_t rival[48];
            tm_team_name(g_tm.base, ext->rival_team, rival, 48);
            ui_text_reset(&t, g_tm.line, 160);
            ui_text_append_wide(&t, rival, 40);
            ui_text_append_ascii(&t, " bids ");
            tm_append_money(&t, ext->rival_fee);
            tm_rect(px, y, pw, 30.0f, TM_TURF);
            tm_rect(px, y, 3.0f, 30.0f, TM_AMBER);
            tm_text(g_tm.line, px + 12.0f, y + 8.0f, 12.0f, TM_AMBER, TM_ALIGN_LEFT, pw - 20.0f, 0);
        }
    }
    /* actions */
    float by = h - 128.0f;
    if (done) {
        tm_button(px, by + 60.0f, pw, 44.0f, tm_w("Done"), 1, 1, TM_HIT_LEAVE, 0);
        return;
    }
    if (active && offer->status == CM_OFFER_WAIT_USER_FEE && offer->counter_fee > 0) {
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, "Accept ");
        tm_append_money(&t, offer->counter_fee);
        tm_button(px, by, pw, 40.0f, g_tm.line, 0, offer->counter_fee <= coins, TM_HIT_ACCEPT, 0);
    }
    int32_t afford = g_tm.neg_fee <= coins;
    tm_button(px, by + 50.0f, pw - 110.0f, 46.0f, tm_w(!can_bid ? "Talks over" : afford ? "Submit offer" : "Not enough coins"),
              1, can_bid && afford, TM_HIT_SUBMIT, 0);
    tm_button(px + pw - 100.0f, by + 50.0f, 100.0f, 46.0f, tm_w(active ? "Walk away" : "Leave"), 0, 1, TM_HIT_LEAVE,
              active ? 1 : 0);
}

static void tm_render(void *screen) {
    if (!screen || screen != g_tm.screen) return;
    uint32_t base = g_tm.base;
    CfeGetSizeMarketFn get_width = (CfeGetSizeMarketFn)(base + 0x25EFED);
    CfeGetSizeMarketFn get_height = (CfeGetSizeMarketFn)(base + 0x25EFF1);
    g_tm.width = get_width(screen);
    g_tm.height = get_height(screen);
    if (g_tm.width < 100.0f || g_tm.height < 100.0f) return;
    g_tm.s = g_tm.height / TM_DESIGN_H;
    g_tm.dw = g_tm.width / g_tm.s;
    /* the screen stack re-enables the stock header and footer after Init (the tablet showed Scout Players /
     * Sell Player and the header's menu dots over v37): hide them every frame */
    ((TmDisplayFn)(base + 0x23B74D))(screen, 0);
    ((TmDisplayFn)(base + 0x23B75D))(screen, 0);
    tm_stock_bars_input(base, 0);
    tm_keyboard_poke(base);
    if (g_tm.base_h <= 0.0f) {
        float dims[2] = {0.0f, 0.0f};
        ((TmSetupTextFn)(base + 0x294945))(0, TM_CHALK, 1.0f, -1.0f);
        ((TmTextDimsFn)(base + 0x38F2A1))(dims, tm_w("Ag"));
        g_tm.base_h = dims[1] > 1.0f ? dims[1] : 20.0f;
    }
    if (g_tm.dirty) {
        tm_build(base);
        if (g_tm.sel_id < 0 && g_tm.row_count) g_tm.sel_id = g_tm.rows[0].player_id;
        if (g_tm.sel_id >= 0) tm_load_detail(base);
        g_tm.dirty = 0;
    }
    g_tm.hit_count = 0;
    tm_rect(0, 0, g_tm.dw, TM_DESIGN_H, TM_TURF);
    float main_x = TM_RAIL_W;
    float main_w = g_tm.dw - TM_RAIL_W - TM_DETAIL_W;
    if (g_tm.neg_open) {
        tm_draw_sheet(main_x, g_tm.dw - main_x);
    } else {
        tm_draw_list(main_x, main_w);
        tm_draw_tools(main_x, main_w);
        tm_draw_detail(main_x + main_w);
    }
    tm_draw_rail();
    tm_draw_top(base);
    if (g_tm.toast_frames > 0) {
        --g_tm.toast_frames;
        float tw = main_w - 24.0f;
        float ty = TM_DESIGN_H - 52.0f;
        tm_rect(main_x + 12.0f, ty, tw, 40.0f, TM_CHALK);
        tm_text(g_tm.toast, main_x + 26.0f, ty + 12.0f, 13.0f, TM_GROUND, TM_ALIGN_LEFT, tw - 28.0f, 1);
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * input
 * ------------------------------------------------------------------------------------------------------------- */
static int32_t tm_busy(uint32_t base) {
    if (g_ui_live_box || g_ui_pending_slot >= 0) return 1;
    void *queue = ui_queue_get(base);
    if (!queue) return 0;
    for (int32_t i = 0; i < UI_QUEUE_SLOT_COUNT; ++i) {
        if (*(const uint32_t *)((const uint8_t *)queue + UI_QUEUE_SLOTS + 4 * i)) return 1;
    }
    return 0;
}

static const TmHit *tm_hit_at(float x, float y) {
    /* last registered wins: buttons are registered after the areas under them */
    for (int32_t i = g_tm.hit_count - 1; i >= 0; --i) {
        const TmHit *hit = &g_tm.hits[i];
        if (x >= hit->x && x < hit->x + hit->w && y >= hit->y && y < hit->y + hit->h) return hit;
    }
    return (const TmHit *)0;
}

static void tm_set_fee_from_x(float x) {
    float px = TM_RAIL_W + (g_tm.dw - TM_RAIL_W) - 400.0f + 18.0f, pw = 400.0f - 36.0f;
    float t = (x - px) / pw;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    g_tm.neg_fee = round5(g_tm.neg_lo + (int32_t)((float)(g_tm.neg_hi - g_tm.neg_lo) * t));
}

static void tm_activate(uint32_t base, const TmHit *hit) {
    switch (hit->kind) {
    case TM_HIT_BACK: tm_back(base); break;
    case TM_HIT_TAB:
        g_tm.tab = hit->value;
        g_tm.neg_open = 0;
        g_tm.scroll = 0.0f;
        g_tm.sel_id = -1;                      /* the detail follows the tab: its first row */
        g_tm.dirty = 1;
        break;
    case TM_HIT_SEARCH: tm_open_search(base); break;
    case TM_HIT_POS: g_tm.pos_filter = hit->value; g_tm.scroll = 0.0f; g_tm.dirty = 1; break;
    case TM_HIT_SORT: g_tm.sort = (g_tm.sort + 1) % TM_SORT_COUNT; g_tm.scroll = 0.0f; g_tm.dirty = 1; break;
    case TM_HIT_ROW: g_tm.sel_id = hit->value; tm_load_detail(base); break;
    case TM_HIT_STAR:
    case TM_HIT_SHORTLIST:
        shortlist_toggle(hit->value);
        g_market_dirty = 1;
        for (int32_t i = 0; i < g_tm.row_count; ++i)
            if (g_tm.rows[i].player_id == hit->value) g_tm.rows[i].starred = (uint8_t)tm_is_starred(hit->value);
        if (g_tm.tab == TM_TAB_SHORTLIST) g_tm.dirty = 1;
        break;
    case TM_HIT_OFFER: tm_open_sheet(base, hit->value); break;
    case TM_HIT_YEARS: g_tm.neg_years = hit->value; break;
    case TM_HIT_FEE_MINUS:
    case TM_HIT_FEE_PLUS: {
        int32_t step = round5(mul_div(g_tm.neg_ask, 5, 100));
        if (step < 5) step = 5;
        int32_t fee = g_tm.neg_fee + (hit->kind == TM_HIT_FEE_PLUS ? step : -step);
        if (fee < 5) fee = 5;
        if (fee > g_tm.neg_hi) g_tm.neg_hi = round5(fee);
        if (fee < g_tm.neg_lo) g_tm.neg_lo = round5(fee);
        g_tm.neg_fee = fee;
        break;
    }
    case TM_HIT_FEE_TYPE: tm_open_fee_keyboard(base); break;
    case TM_HIT_CLEAR_SEARCH:
        for (int32_t i = 0; i < 32; ++i) g_tm.query[i] = 0;
        g_tm.scroll = 0.0f;
        g_tm.dirty = 1;
        break;
    case TM_HIT_SUBMIT: tm_submit(base); break;
    case TM_HIT_ACCEPT: tm_decide(base, CM_DECISION_ACCEPT, 0); break;
    case TM_HIT_WAGE_ACCEPT: tm_decide(base, CM_DECISION_ACCEPT, 0); break;
    case TM_HIT_WAGE_LOWER: {
        MarketOffer *offer = g_tm.neg_offer >= 0 ? find_offer(g_tm.neg_offer) : (MarketOffer *)0;
        if (offer) tm_decide(base, CM_DECISION_COUNTER, round5(mul_div(offer->counter_wage, 92, 100)));
        break;
    }
    case TM_HIT_LEAVE:
        if (hit->value) tm_decide(base, CM_DECISION_REJECT, 0);
        g_tm.neg_open = 0;
        g_tm.dirty = 1;
        break;
    case TM_HIT_INBID_ACCEPT: tm_answer_bid(base, hit->value, CM_DECISION_ACCEPT); break;
    case TM_HIT_INBID_COUNTER: tm_answer_bid(base, hit->value, CM_DECISION_COUNTER); break;
    case TM_HIT_INBID_REJECT: tm_answer_bid(base, hit->value, CM_DECISION_REJECT); break;
    case TM_HIT_LISTTOGGLE: {
        int32_t r = toggle_user_listing(hit->value);
        tm_toast(tm_w(r == 0 ? "Listed for sale. Clubs can bid for him now."
                      : r == 1 ? "Taken off the sale list." : "He cannot be listed right now."));
        g_tm.dirty = 1;
        break;
    }
    default: break;
    }
}

static void tm_back(uint32_t base) {
    if (g_tm.neg_open) { g_tm.neg_open = 0; g_tm.dirty = 1; }
    else ((CfeBackMarketFn)(base + 0x29910D))(1);
}

static void tm_input(uint32_t base) {
    if (tm_busy(base)) { g_tm.drag_kind = TM_HIT_NONE; return; }
    /* Android back: CFEEntityManager::ProcessPhysicalBackButton presses the header's back button
     * (CFEHeaderMenu+0x310 = 1). The header is hidden here, so nothing else answers it. */
    void *header = ((TsGetEntityFn)(base + 0x2609C5))();          /* CFEEntityManager::GetHeaderMenu */
    if (header && *(volatile int32_t *)((uint8_t *)header + 0x310) == 1) {
        *(volatile int32_t *)((uint8_t *)header + 0x310) = -1;
        tm_back(base);
        return;
    }
    TmTouchFlagFn touching = (TmTouchFlagFn)(base + 0x202AE5);
    TmTouchFlagFn pressed = (TmTouchFlagFn)(base + 0x202B79);
    TmTouchFlagFn released = (TmTouchFlagFn)(base + 0x202B8D);
    int32_t pos[2] = {0, 0}, down[2] = {0, 0};
    ((TmTouchPosFn)(base + 0x202B21))(pos, 1);
    ((TmTouchPosFn)(base + 0x202B05))(down, 1);
    float s = g_tm.s > 0.0f ? g_tm.s : 1.0f;
    float px = (float)pos[0] / s, py = (float)pos[1] / s;
    float dx = (float)down[0] / s, dy = (float)down[1] / s;
    if (pressed(1)) {
        const TmHit *hit = tm_hit_at(dx, dy);
        g_tm.drag_kind = hit ? (hit->kind == TM_HIT_SLIDER ? TM_HIT_SLIDER
                                : hit->kind == TM_HIT_ROW || hit->kind == TM_HIT_LIST || hit->kind == TM_HIT_STAR
                                  || hit->kind == TM_HIT_INBID_ACCEPT || hit->kind == TM_HIT_INBID_COUNTER
                                  || hit->kind == TM_HIT_INBID_REJECT || hit->kind == TM_HIT_LISTTOGGLE
                                  ? TM_HIT_LIST : hit->kind) : TM_HIT_NONE;
        g_tm.drag_moved = 0;
        g_tm.drag_scroll0 = g_tm.scroll;
    }
    if (touching(1)) {
        if (g_tm.drag_kind == TM_HIT_LIST && !g_tm.neg_open) {
            float delta = py - dy;
            if (delta > 10.0f || delta < -10.0f) g_tm.drag_moved = 1;
            if (g_tm.drag_moved) g_tm.scroll = g_tm.drag_scroll0 - delta;
        } else if (g_tm.drag_kind == TM_HIT_SLIDER) {
            g_tm.drag_moved = 1;
            tm_set_fee_from_x(px);
        }
    }
    if (released(1)) {
        if (!g_tm.drag_moved) {
            const TmHit *hit = tm_hit_at(px, py);
            const TmHit *start = tm_hit_at(dx, dy);
            if (hit && start && hit->kind == start->kind && hit->value == start->value) {
                if (hit->kind == TM_HIT_SLIDER) tm_set_fee_from_x(px);
                else tm_activate(base, hit);
            }
        }
        g_tm.drag_kind = TM_HIT_NONE;
        g_tm.drag_moved = 0;
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * screen
 * ------------------------------------------------------------------------------------------------------------- */
static uint32_t g_tm_vtable[MARKET_SCREEN_VTABLE_ENTRIES];

/* Hidden is not inert: DisplayFooter/DisplayHeader(false) only stop drawing, and the stock Scout Players /
 * Sell Player buttons (and the header's) kept taking the taps at the edges of the screen. CFEEntity::EnableInput
 * 0x25FD08 sets the input flag (+5) on the entity and all its children. */
static void tm_stock_bars_input(uint32_t base, int32_t enabled) {
    TsEnableInputFn enable_input = (TsEnableInputFn)(base + 0x25FD09);
    void *footer = ((TsGetEntityFn)(base + 0x2609D5))();          /* CFEEntityManager::GetFooterMenu */
    void *header = ((TsGetEntityFn)(base + 0x2609C5))();          /* CFEEntityManager::GetHeaderMenu */
    if (footer) enable_input(footer, enabled);
    if (header) enable_input(header, enabled);
    if (!enabled && footer) {
        /* The footer still drew Scout Players / Sell Player on top of the screen (its visible flag +0x318 does
         * not cover the buttons). CFEFooterMenu::SetButtons(screen id) builds them from the 64-bit mask at
         * +0x108/+0x10c; remove every one with CFEFooterMenu::RemoveButton 0x24607C. The next screen's
         * CFEEntityManager::SetupHeaderAndFooter rebuilds its own footer. */
        typedef void (*TmRemoveButtonFn)(void *, int32_t);
        TmRemoveButtonFn remove_button = (TmRemoveButtonFn)(base + 0x24607D);
        volatile uint32_t *mask = (volatile uint32_t *)((uint8_t *)footer + 0x108);
        for (int32_t id = 0; id < 0x2D && (mask[0] | mask[1]); ++id) {
            uint32_t bit = id < 32 ? mask[0] & (1u << id) : mask[1] & (1u << (id - 32));
            if (bit) remove_button(footer, id);
        }
    }
}

static void tm_screen_init(void *screen) {
    uint32_t base = g_tm.base;
    g_tm.screen = screen;
    ((TmDisplayFn)(base + 0x23B74D))(screen, 0);   /* CFEScreen::DisplayFooter(false) */
    ((TmDisplayFn)(base + 0x23B75D))(screen, 0);   /* CFEScreen::DisplayHeader(false) */
    tm_refresh(base);
    g_tm.neg_open = 0;
    g_tm.drag_kind = TM_HIT_NONE;
    g_tm.hit_count = 0;
    if (g_tm.pos_filter < -1 || g_tm.pos_filter > 3) g_tm.pos_filter = -1;
}

static void tm_screen_exit(void *screen) {
    uint32_t base = g_tm.base;
    if (screen == g_tm.screen) {
        tm_stock_bars_input(base, 1);
        ((TmDisplayFn)(base + 0x23B74D))(screen, 1);
        ((TmDisplayFn)(base + 0x23B75D))(screen, 1);
        g_tm.screen = (void *)0;
        g_tm.hit_count = 0;
    }
}

static int32_t tm_screen_process(void *screen) {
    if (!screen || screen != g_tm.screen || !g_tm.base) return 0;
    tm_stock_bars_input(g_tm.base, 0);
    tm_keyboard_poke(g_tm.base);
    tm_input(g_tm.base);
    return 0;
}

/* Vtable slot 1, the deleting destructor. CFEScreen is abstract: its own slot 1 is a trap instruction
 * (0x23B5B6), so Back (CFEScreenStack::DeleteTopScreen) crashed with SIGILL. Run the complete destructor
 * CFEScreen::~CFEScreen 0x23B5B2, then free the object through the game's operator delete (the veneer
 * 0x5C15C8 the stock deleting destructors tail-call; the screen came from the game's operator new). */
typedef void (*TmObjFn)(void *);
static void tm_screen_delete(void *screen) {
    uint32_t base = g_tm.base;
    tm_screen_exit(screen);
    ((TmObjFn)(base + 0x23B5B3))(screen);
    ((TmObjFn)(base + 0x5C15C9))(screen);
}

static void tm_screen_render(void *screen) { (void)screen; }
static void tm_screen_render_pre(void *screen) { (void)screen; }
static void tm_screen_render_post(void *screen) { tm_render(screen); }
static int32_t tm_screen_is_fullscreen(void *screen) { (void)screen; return 1; }

/* Called from career_market_new_screen_hook for id 0x19. Returns the new screen or 0 (stock screen). */
static void *tm_build_screen(uint32_t base) {
    GameNewFn game_new = (GameNewFn)(base + 0x1C06AC);
    CfeScreenCtorMarketFn constructor = (CfeScreenCtorMarketFn)(base + 0x23B52D);
    CfeScreenSetIdMarketFn set_id = (CfeScreenSetIdMarketFn)(base + 0x23B5AD);
    void *screen = game_new(0xF4, 0, 0);
    if (!screen) return (void *)0;
    constructor(screen);
    const uint32_t *source_vtable = (const uint32_t *)(base + 0x71ABD0);
    for (int32_t i = 0; i < MARKET_SCREEN_VTABLE_ENTRIES; ++i) g_tm_vtable[i] = source_vtable[i];
    g_tm_vtable[1] = (uint32_t)(uintptr_t)tm_screen_delete | 1u;
    g_tm_vtable[3] = (uint32_t)(uintptr_t)tm_screen_init | 1u;
    g_tm_vtable[4] = (uint32_t)(uintptr_t)tm_screen_exit | 1u;
    g_tm_vtable[5] = (uint32_t)(uintptr_t)tm_screen_process | 1u;
    g_tm_vtable[6] = (uint32_t)(uintptr_t)tm_screen_render | 1u;
    g_tm_vtable[32] = (uint32_t)(uintptr_t)tm_screen_render_pre | 1u;
    g_tm_vtable[36] = (uint32_t)(uintptr_t)tm_screen_render_post | 1u;
    g_tm_vtable[44] = (uint32_t)(uintptr_t)tm_screen_is_fullscreen | 1u;
    *(uint32_t *)screen = (uint32_t)(uintptr_t)g_tm_vtable;
    set_id(screen, TM_SCREEN_ID);
    g_tm.base = base;
    g_ui_base = base;
    g_tm.screen = screen;
    g_tm.dirty = 1;
    static int32_t initialised;
    if (!initialised) { tm_reset_state(); initialised = 1; }
    if (g_tm.tab < 0 || g_tm.tab >= TM_TAB_COUNT) g_tm.tab = TM_TAB_SCOUT;
    return screen;
}

static void tm_reset_state(void) {
    g_tm.pos_filter = -1;
    g_tm.sel_id = -1;
    g_tm.neg_offer = -1;
    g_tm.tab = TM_TAB_SCOUT;
}
