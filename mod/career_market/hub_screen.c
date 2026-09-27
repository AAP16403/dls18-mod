/*
 * Club Hub (v38): the club's own window, opened from the Club Hub button in the Transfer Market v2 top bar
 * (tm_screen.c). It is a second view of the same screen object, so the screen stack, Back and the hidden stock
 * header and footer behave exactly as in the market; Back (the button or Android back) returns to the market.
 *
 *   top bar    Market (back), title, division and live league position, squad, wage room, coins
 *   rail       Finances, Board, Contracts; season net and the last match payout
 *   Finances   spendable coins and the wage reserve, wage bill against the wage budget, season net; income and
 *              spending by category, this season or the last
 *   Board      the board's target finish (from the squad strength of every club in the user's league, a smooth
 *              sum, not a band), confidence from the live position against it, and what the division pays:
 *              match prizes, bonuses, gate, TV and sponsor, league prize, what promotion is worth
 *   Contracts  depth per position against min / target / max; every player with role, contract left, wage,
 *              renewal ask and what clubs would pay; renew (continuous talks, hub_renewal_offer), list or keep
 *
 * Layout is in the market's 640-unit design space. Game functions beyond tm_screen.c's: CSeason::GetUserLeagueInTree
 * 0x36CC40 and CTournament::GetTeamLeaguePos 0x36202E (the league at CSeason+0x6AC).
 */

#define HUB_ROWS_MAX 64
#define HUB_ROW_H 50.0f
#define HUB_DEPTH_H 62.0f
#define HUB_HEAD_H 26.0f

enum { HUB_FIN, HUB_BOARD, HUB_CON, HUB_VIEW_COUNT };
enum { HUB_REPLY_NONE, HUB_REPLY_INFO, HUB_REPLY_GOOD, HUB_REPLY_BAD };

typedef struct {
    int32_t player_id, index;
    int32_t rating, position, tenths, wage, ask, pay;
    uint8_t listed, kept, renewable, bid;
    uint16_t name[40];
    uint16_t role[12];
    int32_t role_rank;          /* 3 key, 2 starter, 1 rotation, 0 surplus: the dominant weight */
} HubRow;

static struct {
    int32_t view, last_season, dirty;
    /* board */
    int32_t division, league_n, league_pos, expected_x10, stronger, confidence_pm, strength;
    /* contracts */
    HubRow rows[HUB_ROWS_MAX];
    int32_t row_count, sel_id;
    float scroll, scroll_max;
    int32_t ren_player, ren_wage, ren_years, ren_lo, ren_hi, ren_counter;
    uint16_t reply[TM_TEXT];
    int32_t reply_kind;
} g_hub;

static const char *hub_division_name(int32_t division) {
    static const char *const names[DIVISION_COUNT] = {"Elite", "Junior Elite", "Division 1", "Division 2", "Division 3", "Academy"};
    return division >= 0 && division < DIVISION_COUNT ? names[division] : "League";
}

static void hub_append_ordinal(MarketUiText *t, int32_t n) {
    ui_text_append_i32(t, n);
    int32_t teen = n % 100 >= 11 && n % 100 <= 13;
    ui_text_append_ascii(t, teen ? "th" : n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th");
}

static void hub_append_signed(MarketUiText *t, int32_t value) {
    if (value > 0) ui_text_append_char(t, '+');
    tm_append_money(t, value);
}

static const uint16_t *hub_signed(int32_t value) {
    static uint16_t ring[4][24];
    static int32_t next;
    uint16_t *out = ring[next];
    next = (next + 1) & 3;
    MarketUiText t;
    ui_text_reset(&t, out, 24);
    hub_append_signed(&t, value);
    return out;
}

static void hub_set_reply(int32_t kind, const uint16_t *text) {
    int32_t i = 0;
    while (text && text[i] && i < TM_TEXT - 1) { g_hub.reply[i] = text[i]; ++i; }
    g_hub.reply[i] = 0;
    g_hub.reply_kind = kind;
}

/* ---------------------------------------------------------------------------------------------------------------
 * data
 * ------------------------------------------------------------------------------------------------------------- */
typedef int32_t (*HubLeagueFn)(void *);
typedef int32_t (*HubLeaguePosFn)(void *, int32_t);

/* Division, the user's live league position and the board's target. The target is the expected finish from
 * squad strength: 1 + the sum over league rivals of how likely each one finishes above (a ramp over -6..+6 of
 * strength difference), so it slides with every point of strength instead of jumping between bands. */
static void hub_board_build(uint32_t base) {
    int32_t user_index = tm_user_index();
    g_hub.league_n = 0;
    g_hub.league_pos = 0;
    g_hub.expected_x10 = 0;
    g_hub.stronger = 0;
    g_hub.confidence_pm = 500;
    g_hub.strength = user_index >= 0 ? g_market.clubs[user_index].strength : 0;
    void *season = (void *)(uintptr_t)(base + PROFILE_INSTANCE + 0x14);
    int32_t division = ((HubLeagueFn)(base + 0x36CC41))(season);
    if (division < 0 || division >= DIVISION_COUNT) division = g_ext.econ.division;
    g_hub.division = division >= 0 && division < DIVISION_COUNT ? division : DIVISION_COUNT - 1;
    void *league = *(void **)((uint8_t *)season + 0x6AC);
    if (!league || user_index < 0) return;
    HubLeaguePosFn get_pos = (HubLeaguePosFn)(base + 0x36202F);
    int32_t pos = get_pos(league, USER_TEAM_ID);
    if (pos < 0) return;
    int32_t n = 1, expected_pm = 1000;
    for (int32_t c = 0; c < MAX_CLUBS; ++c) {
        ClubAccount *club = &g_market.clubs[c];
        if (club->team_id < 0 || club->team_id == USER_TEAM_ID) continue;
        if (get_pos(league, club->team_id) < 0) continue;
        ++n;
        expected_pm += ramp_pm(club->strength - g_hub.strength, -6, 6);
        if (club->strength > g_hub.strength) ++g_hub.stronger;
    }
    if (n < 2) return;
    g_hub.league_n = n;
    g_hub.league_pos = pos + 1;
    g_hub.expected_x10 = (expected_pm + 50) / 100;
    /* confidence: half at the target, 9 points of 100 for each place above or below it */
    int32_t conf = 500 + (g_hub.expected_x10 - g_hub.league_pos * 10) * 9;
    g_hub.confidence_pm = conf < 30 ? 30 : conf > 970 ? 970 : conf;
}

static void hub_contracts_build(uint32_t base) {
    g_hub.row_count = 0;
    for (int32_t i = 0; i < g_player_count && g_hub.row_count < HUB_ROWS_MAX; ++i) {
        MarketPlayer *player = &g_players[i];
        if (player->owner_id != USER_TEAM_ID) continue;
        HubRow *row = &g_hub.rows[g_hub.row_count++];
        row->player_id = player->player_id;
        row->index = i;
        row->rating = player->rating;
        row->position = player->position;
        row->tenths = contract_tenths_left(player->player_id, USER_TEAM_ID);
        row->wage = player->wage;
        row->ask = renewal_wage_demand(player, USER_TEAM_ID);
        row->pay = user_sale_value(player);
        row->listed = (uint8_t)user_player_is_listed(player->player_id);
        row->kept = (uint8_t)user_is_kept(player->player_id);
        row->renewable = (uint8_t)market_contract_available(player->player_id, USER_TEAM_ID);
        MarketOffer *offer = tm_user_offer_for(player->player_id);
        row->bid = (uint8_t)(offer && offer->seller_id == USER_TEAM_ID);
        RoleMix mix = role_mix(player);
        const char *role = "Rotation";
        int32_t best = mix.rotation, rank = 1;
        if (mix.surplus > best) { best = mix.surplus; role = "Surplus"; rank = 0; }
        if (mix.starter > best) { best = mix.starter; role = "Starter"; rank = 2; }
        if (mix.key > best) { role = "Key"; rank = 3; }
        row->role_rank = rank;
        MarketUiText t;
        ui_text_reset(&t, row->role, 12);
        ui_text_append_ascii(&t, role);
        tm_player_name(base, player->player_id, row->name, 40);
    }
    /* contracts ending first, then the better player */
    for (int32_t i = 1; i < g_hub.row_count; ++i) {
        HubRow tmp = g_hub.rows[i];
        int32_t j = i;
        while (j > 0 && (g_hub.rows[j - 1].tenths > tmp.tenths ||
                         (g_hub.rows[j - 1].tenths == tmp.tenths && g_hub.rows[j - 1].rating < tmp.rating))) {
            g_hub.rows[j] = g_hub.rows[j - 1];
            --j;
        }
        g_hub.rows[j] = tmp;
    }
    int32_t found = 0;
    for (int32_t i = 0; i < g_hub.row_count; ++i) if (g_hub.rows[i].player_id == g_hub.sel_id) found = 1;
    if (!found) g_hub.sel_id = g_hub.row_count ? g_hub.rows[0].player_id : -1;
}

static HubRow *hub_selected(void) {
    for (int32_t i = 0; i < g_hub.row_count; ++i) if (g_hub.rows[i].player_id == g_hub.sel_id) return &g_hub.rows[i];
    return (HubRow *)0;
}

/* Renewal terms follow the selection: a fresh player starts at 95% of his ask on a 3-year deal. */
static void hub_select(int32_t player_id) {
    g_hub.sel_id = player_id;
    HubRow *row = hub_selected();
    if (!row || g_hub.ren_player == player_id) return;
    g_hub.ren_player = player_id;
    g_hub.ren_wage = round5(mul_div(row->ask, 95, 100));
    if (g_hub.ren_wage < 1) g_hub.ren_wage = 1;
    g_hub.ren_years = 3;
    g_hub.ren_lo = round5(mul_div(row->ask, 60, 100));
    g_hub.ren_hi = round5(mul_div(row->ask, 130, 100));
    if (g_hub.ren_hi <= g_hub.ren_lo) g_hub.ren_hi = g_hub.ren_lo + 5;
    g_hub.ren_counter = 0;
    g_hub.reply[0] = 0;
    g_hub.reply_kind = HUB_REPLY_NONE;
}

static void hub_build(uint32_t base) {
    hub_board_build(base);
    hub_contracts_build(base);
    if (g_hub.sel_id >= 0) {
        int32_t keep = g_hub.ren_player;
        hub_select(g_hub.sel_id);
        (void)keep;
    }
    g_hub.dirty = 0;
}

static void hub_open(uint32_t base) {
    g_tm.view = 1;
    g_tm.neg_open = 0;
    g_hub.dirty = 1;
    g_hub.scroll = 0.0f;
    if (g_hub.view < 0 || g_hub.view >= HUB_VIEW_COUNT) g_hub.view = HUB_FIN;
    tm_refresh(base);
}

static void hub_back(uint32_t base) {
    (void)base;
    g_tm.view = 0;
    g_tm.dirty = 1;
}

/* ---------------------------------------------------------------------------------------------------------------
 * render
 * ------------------------------------------------------------------------------------------------------------- */
static void hub_draw_top(uint32_t base) {
    float dw = g_tm.dw;
    tm_rect(0, 0, dw, TM_TOP_H, TM_GROUND);
    tm_rect(0, TM_TOP_H - 1.0f, dw, 1.0f, TM_LINE);
    tm_rect(10.0f, 9.0f, 104.0f, 38.0f, TM_TURF2);
    tm_rect(10.0f, 9.0f, 104.0f, 1.0f, TM_LINE);
    tm_rect(10.0f, 46.0f, 104.0f, 1.0f, TM_LINE);
    tm_triangle(20.0f, 28.0f, 30.0f, 20.0f, 30.0f, 36.0f, TM_CHALK);
    tm_rect(29.0f, 26.5f, 7.0f, 3.0f, TM_CHALK);
    tm_text(tm_w("Market"), 42.0f, 19.0f, 15.0f, TM_CHALK, TM_ALIGN_LEFT, 0, 1);
    tm_hit(4.0f, 2.0f, 118.0f, 52.0f, HUB_HIT_BACK, 0);
    const uint16_t *title = tm_w("CLUB HUB");
    tm_text(title, 132.0f, 15.0f, 24.0f, TM_CHALK, TM_ALIGN_LEFT, 0, 1);
    float x = 132.0f + tm_text_width(title, 24.0f) + 16.0f;
    MarketUiText t;
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, hub_division_name(g_hub.division));
    if (g_hub.league_pos > 0) {
        ui_text_append_ascii(&t, "   ");
        hub_append_ordinal(&t, g_hub.league_pos);
        ui_text_append_ascii(&t, " of ");
        ui_text_append_i32(&t, g_hub.league_n);
    }
    float pw = tm_text_width(g_tm.line, 12.0f) + 24.0f;
    tm_rect(x, 15.0f, pw, 26.0f, TM_TURF2);
    tm_text(g_tm.line, x + 12.0f, 21.0f, 12.0f, TM_GOOD, TM_ALIGN_LEFT, 0, 1);
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

static int32_t hub_season_net(int32_t last) {
    int32_t income = books_sum(k_income_categories, (int32_t)(sizeof(k_income_categories) / sizeof(k_income_categories[0])), last);
    int32_t spend = books_sum(k_spend_categories, (int32_t)(sizeof(k_spend_categories) / sizeof(k_spend_categories[0])), last);
    return sat_add(income, spend);
}

static int32_t hub_expiring_count(void) {
    int32_t n = 0;
    for (int32_t i = 0; i < g_hub.row_count; ++i) if (g_hub.rows[i].tenths < 10) ++n;
    return n;
}

static void hub_draw_rail(void) {
    static const char *const names[HUB_VIEW_COUNT] = {"Finances", "Board", "Contracts"};
    float h = g_tm.height / g_tm.s;
    tm_rect(0, TM_TOP_H, TM_RAIL_W, h - TM_TOP_H, TM_GROUND);
    tm_rect(TM_RAIL_W - 1.0f, TM_TOP_H, 1.0f, h - TM_TOP_H, TM_LINE);
    for (int32_t i = 0; i < HUB_VIEW_COUNT; ++i) {
        float y = TM_TOP_H + 10.0f + (float)i * 48.0f;
        int32_t active = g_hub.view == i;
        if (active) {
            tm_rect(8.0f, y, TM_RAIL_W - 16.0f, 46.0f, TM_TURF2);
            tm_rect(8.0f, y, 3.0f, 46.0f, TM_AMBER);
        }
        tm_text(tm_w(names[i]), 22.0f, y + 15.0f, 15.0f, active ? TM_CHALK : TM_CHALK2, TM_ALIGN_LEFT, 0, active);
        if (i == HUB_CON) {
            int32_t n = hub_expiring_count();
            MarketUiText t;
            uint16_t buf[8];
            ui_text_reset(&t, buf, 8);
            ui_text_append_i32(&t, n);
            tm_rect(TM_RAIL_W - 50.0f, y + 13.0f, 32.0f, 20.0f, n ? TM_AMBER : TM_TURF3);
            tm_text(buf, TM_RAIL_W - 34.0f, y + 16.0f, 12.0f, n ? TM_AMBER_INK : TM_CHALK, TM_ALIGN_CENTER, 0, 1);
        }
        tm_hit(8.0f, y, TM_RAIL_W - 16.0f, 46.0f, HUB_HIT_TAB, i);
    }
    float py = h - 132.0f;
    tm_rect(10.0f, py - 10.0f, TM_RAIL_W - 20.0f, 1.0f, TM_LINE);
    tm_text(tm_w("SEASON NET"), 14.0f, py, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    int32_t net = hub_season_net(0);
    tm_text(hub_signed(net), 14.0f, py + 16.0f, 26.0f, net >= 0 ? TM_GOOD : TM_BAD, TM_ALIGN_LEFT, 0, 1);
    int32_t last_match = 0;
    for (int32_t i = 0; i < ECON_MATCH_ROWS; ++i) last_match = sat_add(last_match, g_ext.econ.last_match[i]);
    MarketUiText t;
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, "Last match paid ");
    tm_append_money(&t, last_match);
    tm_text_wrap(g_tm.line, 14.0f, py + 54.0f, 12.0f, TM_CHALK2, TM_RAIL_W - 28.0f, 2, 16.0f);
}

/* One books column: label, amount and a bar scaled to the column's largest line. Returns the column total. */
static int32_t hub_draw_books(const int32_t *cats, int32_t count, float x, float y, float w, uint32_t colour) {
    int32_t max = 1, total = 0;
    for (int32_t i = 0; i < count; ++i) {
        int32_t v = book_total(cats[i], g_hub.last_season);
        if (v < 0) v = -v;
        if (v > max) max = v;
    }
    int32_t shown = 0;
    for (int32_t i = 0; i < count; ++i) {
        int32_t v = book_total(cats[i], g_hub.last_season);
        total = sat_add(total, v);
        if (!v) continue;
        int32_t a = v < 0 ? -v : v;
        float ly = y + (float)shown * 31.0f;
        tm_text(tm_w(book_label(cats[i])), x, ly, 13.0f, TM_CHALK, TM_ALIGN_LEFT, w - 80.0f, 0);
        tm_text(tm_money(a), x + w, ly, 13.0f, TM_CHALK, TM_ALIGN_RIGHT, 0, 1);
        tm_rect(x, ly + 19.0f, w, 5.0f, TM_TURF2);
        tm_rect(x, ly + 19.0f, w * (float)a / (float)max, 5.0f, colour);
        ++shown;
    }
    if (!shown) tm_text(tm_w("Nothing yet."), x, y, 13.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 0);
    return total;
}

static void hub_draw_finances(uint32_t base, float x, float w) {
    int32_t user_index = tm_user_index();
    if (user_index < 0) return;
    ClubAccount *club = &g_market.clubs[user_index];
    float h = g_tm.height / g_tm.s;
    float y = TM_TOP_H;
    float tile_w = w / 3.0f;
    int32_t coins = user_coins(base);
    int32_t spendable = account_fee_room(club);
    if (spendable > coins) spendable = coins;
    MarketUiText t;
    for (int32_t i = 0; i < 3; ++i) {
        float tx = x + (float)i * tile_w;
        tm_rect(tx, y, tile_w - 1.0f, 98.0f, TM_TURF);
        tm_rect(tx + tile_w - 1.0f, y, 1.0f, 98.0f, TM_LINE);
    }
    tm_rect(x, y + 98.0f, w, 1.0f, TM_LINE);
    float px = x + 18.0f, tw = tile_w - 36.0f;
    /* spendable */
    tm_text(tm_w("SPENDABLE NOW"), px, y + 14.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    tm_text(tm_money(spendable), px, y + 30.0f, 32.0f, TM_AMBER, TM_ALIGN_LEFT, 0, 1);
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, "Coins after a reserve of ");
    tm_append_money(&t, coins - spendable > 0 ? coins - spendable : 0);
    ui_text_append_ascii(&t, " for wages");
    tm_text(g_tm.line, px, y + 72.0f, 12.0f, TM_CHALK2, TM_ALIGN_LEFT, tw, 0);
    /* wage bill */
    px += tile_w;
    tm_text(tm_w("WAGE BILL A SEASON"), px, y + 14.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    const uint16_t *bill = tm_money(club->payroll);
    tm_text(bill, px, y + 30.0f, 32.0f, TM_CHALK, TM_ALIGN_LEFT, 0, 1);
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, "/ ");
    tm_append_money(&t, club->wage_budget);
    tm_text(g_tm.line, px + tm_text_width(bill, 32.0f) + 8.0f, y + 44.0f, 15.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 0);
    float fill = club->wage_budget > 0 ? (float)club->payroll / (float)club->wage_budget : 1.0f;
    if (fill > 1.0f) fill = 1.0f;
    tm_rect(px, y + 70.0f, tw, 5.0f, TM_TURF2);
    tm_rect(px, y + 70.0f, tw * fill, 5.0f, fill > 0.9f ? TM_AMBER : TM_SKY);
    ui_text_reset(&t, g_tm.line, 160);
    tm_append_money(&t, account_wage_room(club));
    ui_text_append_ascii(&t, " room for new wages or renewals");
    tm_text(g_tm.line, px, y + 80.0f, 12.0f, TM_CHALK2, TM_ALIGN_LEFT, tw, 0);
    /* season */
    px += tile_w;
    int32_t income = books_sum(k_income_categories, (int32_t)(sizeof(k_income_categories) / sizeof(k_income_categories[0])), g_hub.last_season);
    int32_t spend = books_sum(k_spend_categories, (int32_t)(sizeof(k_spend_categories) / sizeof(k_spend_categories[0])), g_hub.last_season);
    int32_t net = sat_add(income, spend);
    tm_text(tm_w(g_hub.last_season ? "LAST SEASON" : "SEASON SO FAR"), px, y + 14.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    tm_text(hub_signed(net), px, y + 30.0f, 32.0f, net >= 0 ? TM_GOOD : TM_BAD, TM_ALIGN_LEFT, 0, 1);
    ui_text_reset(&t, g_tm.line, 160);
    tm_append_money(&t, income);
    ui_text_append_ascii(&t, " in, ");
    tm_append_money(&t, spend < 0 ? -spend : spend);
    ui_text_append_ascii(&t, " out");
    tm_text(g_tm.line, px, y + 72.0f, 12.0f, TM_CHALK2, TM_ALIGN_LEFT, tw, 0);
    /* books */
    float by = y + 99.0f;
    float cw = w * 0.5f;
    tm_rect(x + cw, by, 1.0f, h - by, TM_LINE);
    float cx = x + 18.0f, iw = cw - 36.0f;
    tm_text(tm_w("INCOME"), cx, by + 18.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    /* this season / last season */
    float sx = cx + iw - 200.0f;
    tm_rect(sx, by + 10.0f, 200.0f, 30.0f, TM_GROUND);
    for (int32_t k = 0; k < 2; ++k) {
        float bx = sx + 2.0f + (float)k * 99.0f;
        int32_t on = g_hub.last_season == k;
        if (on) tm_rect(bx, by + 12.0f, 97.0f, 26.0f, TM_TURF3);
        tm_text(tm_w(k ? "Last season" : "This season"), bx + 48.0f, by + 18.0f, 12.0f, on ? TM_CHALK : TM_CHALK2, TM_ALIGN_CENTER, 0, 1);
        tm_hit(bx, by + 10.0f, 97.0f, 30.0f, HUB_HIT_SEASON, k);
    }
    hub_draw_books(k_income_categories, (int32_t)(sizeof(k_income_categories) / sizeof(k_income_categories[0])),
                   cx, by + 52.0f, iw, TM_GOOD);
    float ox = x + cw + 18.0f;
    tm_text(tm_w("SPENDING"), ox, by + 18.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    tm_text(hub_signed(spend), ox + iw, by + 14.0f, 20.0f, spend < 0 ? TM_BAD : TM_CHALK2, TM_ALIGN_RIGHT, 0, 1);
    hub_draw_books(k_spend_categories, (int32_t)(sizeof(k_spend_categories) / sizeof(k_spend_categories[0])),
                   ox, by + 52.0f, iw, TM_BAD);
}

static void hub_pay_line(const char *label, const uint16_t *value, float x, float y, float w) {
    tm_text(tm_w(label), x, y, 13.0f, TM_CHALK2, TM_ALIGN_LEFT, w - 110.0f, 0);
    tm_text(value, x + w, y, 13.0f, TM_CHALK, TM_ALIGN_RIGHT, 0, 1);
}

static void hub_draw_board(float x, float w) {
    float h = g_tm.height / g_tm.s;
    float cw = w * 0.5f;
    int32_t d = g_hub.division;
    MarketUiText t;
    /* left: target and confidence */
    float px = x + 20.0f, pw = cw - 40.0f, y = TM_TOP_H + 18.0f;
    tm_rect(px, y, pw, 206.0f, TM_TURF2);
    tm_text(tm_w("BOARD TARGET THIS SEASON"), px + 16.0f, y + 14.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    int32_t n = g_hub.league_n;
    if (n >= 2) {
        int32_t target = (g_hub.expected_x10 + 5) / 10;
        if (target < 1) target = 1;
        if (target > n) target = n;
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, "Finish around ");
        hub_append_ordinal(&t, target);
        tm_text(g_tm.line, px + 16.0f, y + 32.0f, 34.0f, TM_CHALK, TM_ALIGN_LEFT, pw - 32.0f, 1);
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, "Squad strength ");
        ui_text_append_i32(&t, g_hub.strength);
        ui_text_append_ascii(&t, ": ");
        ui_text_append_i32(&t, g_hub.stronger);
        ui_text_append_ascii(&t, g_hub.stronger == 1 ? " club in your league is stronger. " : " clubs in your league are stronger. ");
        ui_text_append_ascii(&t, "The closer a rival is to you, the less it counts against the target.");
        tm_text_wrap(g_tm.line, px + 16.0f, y + 78.0f, 12.0f, TM_CHALK2, pw - 32.0f, 3, 16.0f);
        /* ladder: every place in the table, promotion (top 2) and relegation (bottom 2) tinted */
        float lx = px + 16.0f, lw = pw - 32.0f, ly = y + 138.0f;
        float cell = lw / (float)n;
        for (int32_t i = 0; i < n; ++i) {
            uint32_t c = i < 2 ? tm_mix(TM_GOOD, TM_TURF, 250) : i >= n - 2 ? tm_mix(TM_BAD, TM_TURF, 200) : TM_TURF;
            tm_rect(lx + (float)i * cell + 1.0f, ly, cell - 2.0f, 30.0f, c);
        }
        float tx = lx + ((float)g_hub.expected_x10 / 10.0f - 0.5f) * cell;
        tm_rect(tx - 2.0f, ly - 4.0f, 4.0f, 38.0f, TM_AMBER);
        float nx = lx + ((float)g_hub.league_pos - 0.5f) * cell;
        tm_rect(nx - 2.0f, ly - 4.0f, 4.0f, 38.0f, TM_CHALK);
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, "Now ");
        hub_append_ordinal(&t, g_hub.league_pos);
        int32_t right = nx > tx;
        tm_text(g_tm.line, nx + (right ? 6.0f : -6.0f), ly + 38.0f, 11.0f, TM_CHALK, right ? TM_ALIGN_LEFT : TM_ALIGN_RIGHT, 0, 1);
        tm_text(tm_w("Target"), tx + (right ? -6.0f : 6.0f), ly + 38.0f, 11.0f, TM_AMBER, right ? TM_ALIGN_RIGHT : TM_ALIGN_LEFT, 0, 1);
        y += 226.0f;
        const char *mood = g_hub.confidence_pm >= 750 ? "Delighted" : g_hub.confidence_pm >= 550 ? "Pleased"
                         : g_hub.confidence_pm >= 400 ? "Steady" : g_hub.confidence_pm >= 250 ? "Concerned" : "Worried";
        uint32_t mc = g_hub.confidence_pm >= 550 ? TM_GOOD : g_hub.confidence_pm >= 400 ? TM_CHALK : g_hub.confidence_pm >= 250 ? TM_AMBER : TM_BAD;
        tm_text(tm_w("Board confidence"), px, y, 13.0f, TM_CHALK, TM_ALIGN_LEFT, 0, 1);
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, mood);
        ui_text_append_ascii(&t, "  ");
        ui_text_append_i32(&t, (g_hub.confidence_pm + 5) / 10);
        ui_text_append_char(&t, '%');
        tm_text(g_tm.line, px + pw, y, 13.0f, mc, TM_ALIGN_RIGHT, 0, 1);
        tm_rect(px, y + 22.0f, pw, 8.0f, TM_TURF2);
        tm_rect(px, y + 22.0f, pw * (float)g_hub.confidence_pm / 1000.0f, 8.0f, mc);
        int32_t gap10 = g_hub.league_pos * 10 - g_hub.expected_x10;
        ui_text_reset(&t, g_tm.line, 160);
        if (gap10 > 5) ui_text_append_ascii(&t, "Behind the target. Every place you climb lifts confidence.");
        else if (gap10 < -5) ui_text_append_ascii(&t, "Ahead of the target. Keep it up and the board stays behind you.");
        else ui_text_append_ascii(&t, "On target. Confidence moves with every place you gain or lose.");
        tm_text_wrap(g_tm.line, px, y + 42.0f, 12.0f, TM_CHALK2, pw, 2, 16.0f);
    } else {
        tm_text_wrap(tm_w("The board sets a target once the league table exists. Play your first league match."),
                     px + 16.0f, y + 40.0f, 14.0f, TM_CHALK, pw - 32.0f, 3, 20.0f);
    }
    /* right: what the division pays */
    float rx = x + cw;
    tm_rect(rx, TM_TOP_H, w - cw, h - TM_TOP_H, TM_GROUND);
    tm_rect(rx, TM_TOP_H, 1.0f, h - TM_TOP_H, TM_LINE);
    px = rx + 20.0f;
    pw = w - cw - 40.0f;
    y = TM_TOP_H + 18.0f;
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, "WHAT ");
    for (const char *c = hub_division_name(d); *c; ++c) ui_text_append_char(&t, (uint16_t)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c));
    ui_text_append_ascii(&t, " PAYS");
    tm_text(g_tm.line, px, y, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    y += 26.0f;
    int32_t target = division_target(d);
    uint16_t value[40];
    ui_text_reset(&t, value, 40);
    tm_append_money(&t, econ_win_prize(d));
    ui_text_append_ascii(&t, " / ");
    tm_append_money(&t, econ_draw_prize(d));
    ui_text_append_ascii(&t, " / ");
    tm_append_money(&t, econ_loss_prize(d));
    hub_pay_line("Win / draw / loss", value, px, y, pw); y += 26.0f;
    hub_pay_line("Each goal (up to 6)", tm_money(econ_goal_bonus(d)), px, y, pw); y += 26.0f;
    hub_pay_line("Clean sheet", tm_money(econ_clean_sheet_bonus(d)), px, y, pw); y += 26.0f;
    hub_pay_line("Home gate, typical crowd", tm_money(econ_gate(d, 0)), px, y, pw); y += 26.0f;
    hub_pay_line("Cup winners' prize", tm_money(cup_winners_prize(d)), px, y, pw); y += 26.0f;
    hub_pay_line("TV rights and sponsor, season end", tm_money(mul_div(target, 40, 100)), px, y, pw); y += 26.0f;
    ui_text_reset(&t, value, 40);
    tm_append_money(&t, mul_div(target, 2000, 10000));
    ui_text_append_ascii(&t, " to ");
    tm_append_money(&t, mul_div(target, 200, 10000));
    hub_pay_line("League prize, champion to last", value, px, y, pw); y += 26.0f;
    if (g_hub.league_pos > 0) {
        int32_t pos = g_hub.league_pos > LEAGUE_TEAMS ? LEAGUE_TEAMS : g_hub.league_pos;
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, "League prize at ");
        hub_append_ordinal(&t, g_hub.league_pos);
        char label[40];
        int32_t k = 0;
        for (; g_tm.line[k] && k < 39; ++k) label[k] = (char)g_tm.line[k];
        label[k] = 0;
        hub_pay_line(label, tm_money(mul_div(target, 2000 - (1800 * (pos - 1)) / (LEAGUE_TEAMS - 1), 10000)), px, y, pw);
        y += 26.0f;
    }
    y += 10.0f;
    tm_rect(px, y, pw, 98.0f, TM_TURF2);
    tm_rect(px, y, 3.0f, 98.0f, TM_GOOD);
    if (d > 0) {
        int32_t bonus = mul_div(division_target(d - 1), 15, 100);
        int32_t gain = division_target(d - 1) - target;
        tm_text(tm_w("PROMOTION IS WORTH"), px + 16.0f, y + 12.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_char(&t, '+');
        tm_append_money(&t, bonus);
        ui_text_append_ascii(&t, " now, +");
        tm_append_money(&t, gain);
        ui_text_append_ascii(&t, " a season after");
        tm_text(g_tm.line, px + 16.0f, y + 28.0f, 22.0f, TM_GOOD, TM_ALIGN_LEFT, pw - 32.0f, 1);
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, "A promotion bonus, then ");
        ui_text_append_ascii(&t, hub_division_name(d - 1));
        ui_text_append_ascii(&t, " pays about ");
        tm_append_money(&t, division_target(d - 1));
        ui_text_append_ascii(&t, " a season against ");
        tm_append_money(&t, target);
        ui_text_append_ascii(&t, " here.");
        tm_text_wrap(g_tm.line, px + 16.0f, y + 58.0f, 12.0f, TM_CHALK2, pw - 32.0f, 2, 16.0f);
    } else {
        tm_text(tm_w("TOP DIVISION"), px + 16.0f, y + 12.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, "The title pays ");
        tm_append_money(&t, mul_div(target, 2000, 10000));
        tm_text(g_tm.line, px + 16.0f, y + 28.0f, 22.0f, TM_GOOD, TM_ALIGN_LEFT, pw - 32.0f, 1);
        tm_text_wrap(tm_w("There is no promotion from Elite. Every place higher in the table pays more at the season end."),
                     px + 16.0f, y + 58.0f, 12.0f, TM_CHALK2, pw - 32.0f, 2, 16.0f);
    }
}

static void hub_draw_depth(float x, float w) {
    int32_t user_index = tm_user_index();
    if (user_index < 0) return;
    float y = TM_TOP_H;
    float cw = w / 4.0f;
    for (int32_t p = 0; p < 4; ++p) {
        float cx = x + (float)p * cw;
        tm_rect(cx, y, cw - 1.0f, HUB_DEPTH_H, TM_TURF);
        tm_rect(cx + cw - 1.0f, y, 1.0f, HUB_DEPTH_H, TM_LINE);
        int32_t have = g_group_count[user_index][p];
        MarketUiText t;
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, tm_pos_label(p));
        ui_text_append_char(&t, ' ');
        ui_text_append_i32(&t, have);
        tm_text(g_tm.line, cx + 14.0f, y + 10.0f, 13.0f, TM_CHALK, TM_ALIGN_LEFT, 0, 1);
        const char *note = have < k_pos_min[p] ? "below minimum" : have < k_pos_target[p] ? "thin"
                         : have > k_pos_max[p] ? "over the limit" : have > k_pos_target[p] ? "surplus" : "on target";
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, note);
        tm_text(g_tm.line, cx + cw - 12.0f, y + 12.0f, 11.0f, have < k_pos_min[p] ? TM_BAD : TM_CHALK2, TM_ALIGN_RIGHT,
                cw - 76.0f, 0);
        int32_t max = k_pos_max[p] > have ? k_pos_max[p] : have;
        float pip = (cw - 28.0f - 3.0f * (float)(max - 1)) / (float)max;
        if (pip > 16.0f) pip = 16.0f;
        for (int32_t i = 0; i < max; ++i) {
            float bx = cx + 14.0f + (float)i * (pip + 3.0f);
            uint32_t c = i < have ? (i >= k_pos_max[p] ? TM_BAD : TM_CHALK2) : i < k_pos_min[p] ? TM_BAD : TM_TURF3;
            tm_rect(bx, y + 36.0f, pip, 10.0f, c);
            if (i == k_pos_target[p] - 1) tm_rect(bx, y + 48.0f, pip, 2.0f, TM_AMBER);
        }
    }
    tm_rect(x, y + HUB_DEPTH_H, w, 1.0f, TM_LINE);
}

/* list columns, right to left from the row's right edge */
#define HUB_COL_PAY 64.0f
#define HUB_COL_WAGE 92.0f
#define HUB_COL_CON 100.0f

static void hub_draw_contract_list(float x, float w) {
    float top = TM_TOP_H + HUB_DEPTH_H + 1.0f;
    float h = g_tm.height / g_tm.s;
    float right = x + w - 16.0f;
    float pay_x = right, wage_x = pay_x - HUB_COL_PAY - 10.0f, con_x = wage_x - HUB_COL_WAGE - 10.0f;
    float name_x = x + 72.0f;
    float name_w = con_x - HUB_COL_CON - 16.0f - name_x;
    tm_rect(x, top, w, HUB_HEAD_H, TM_TURF);
    tm_text(tm_w("PLAYER"), name_x, top + 8.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 0);
    tm_text(tm_w("CONTRACT"), con_x, top + 8.0f, 10.0f, TM_CHALK3, TM_ALIGN_RIGHT, 0, 0);
    tm_text(tm_w("WAGE / ASKS"), wage_x, top + 8.0f, 10.0f, TM_CHALK3, TM_ALIGN_RIGHT, 0, 0);
    tm_text(tm_w("CLUBS PAY"), pay_x, top + 8.0f, 10.0f, TM_CHALK3, TM_ALIGN_RIGHT, 0, 0);
    tm_rect(x, top + HUB_HEAD_H - 1.0f, w, 1.0f, TM_LINE);
    float ly = top + HUB_HEAD_H;
    float lh = h - ly;
    tm_rect(x, ly, w, lh, TM_TURF);
    tm_hit(x, ly, w, lh, HUB_HIT_LIST, 0);
    g_hub.scroll_max = (float)g_hub.row_count * HUB_ROW_H + 8.0f - lh;
    if (g_hub.scroll_max < 0.0f) g_hub.scroll_max = 0.0f;
    if (g_hub.scroll > g_hub.scroll_max) g_hub.scroll = g_hub.scroll_max;
    if (g_hub.scroll < 0.0f) g_hub.scroll = 0.0f;
    MarketUiText t;
    for (int32_t i = 0; i < g_hub.row_count; ++i) {
        float y = ly + 4.0f + (float)i * HUB_ROW_H - g_hub.scroll;
        if (y < ly || y + HUB_ROW_H > ly + lh) continue;
        const HubRow *row = &g_hub.rows[i];
        if (row->player_id == g_hub.sel_id) tm_rect(x + 8.0f, y, w - 16.0f, HUB_ROW_H - 2.0f, TM_TURF3);
        tm_badge(x + 18.0f, y + 5.0f, 38.0f, row->rating, 19.0f);
        tm_text(row->name, name_x, y + 6.0f, 14.0f, TM_CHALK, TM_ALIGN_LEFT, name_w, 1);
        /* second line: position, role chip, status */
        const uint16_t *pos = tm_w(tm_pos_label(row->position));
        tm_text(pos, name_x, y + 28.0f, 11.0f, TM_CHALK2, TM_ALIGN_LEFT, 0, 0);
        float chip_x = name_x + tm_text_width(pos, 11.0f) + 8.0f;
        uint32_t chip = row->role_rank == 3 ? TM_AMBER : row->role_rank == 2 ? TM_TURF3 : TM_TURF2;
        float chip_w = tm_text_width(row->role, 10.0f) + 10.0f;
        tm_rect(chip_x, y + 26.0f, chip_w, 16.0f, chip);
        tm_text(row->role, chip_x + 5.0f, y + 28.0f, 10.0f, row->role_rank == 3 ? TM_AMBER_INK : TM_CHALK, TM_ALIGN_LEFT, 0, 1);
        const char *status = row->bid ? "Bid in" : row->listed ? "Listed" : row->kept ? "Kept" : (const char *)0;
        if (status) tm_text(tm_w(status), chip_x + chip_w + 8.0f, y + 28.0f, 11.0f, row->bid ? TM_AMBER : TM_SKY,
                            TM_ALIGN_LEFT, 0, 0);
        /* contract left: text and a bar that runs out with the deal */
        ui_text_reset(&t, g_tm.line, 160);
        if (row->tenths < 10) ui_text_append_ascii(&t, "Ends this season");
        else {
            ui_text_append_i32(&t, row->tenths / 10);
            ui_text_append_char(&t, '.');
            ui_text_append_i32(&t, row->tenths % 10);
            ui_text_append_ascii(&t, " seasons");
        }
        tm_text(g_tm.line, con_x, y + 10.0f, 12.0f, row->tenths < 10 ? TM_AMBER : TM_CHALK, TM_ALIGN_RIGHT, HUB_COL_CON, 1);
        float frac = (float)(row->tenths > 40 ? 40 : row->tenths) / 40.0f;
        tm_rect(con_x - HUB_COL_CON, y + 31.0f, HUB_COL_CON, 4.0f, TM_TURF2);
        tm_rect(con_x - HUB_COL_CON, y + 31.0f, HUB_COL_CON * frac, 4.0f, row->tenths < 10 ? TM_AMBER : TM_SKY);
        tm_text(tm_money(row->wage), wage_x, y + 8.0f, 13.0f, TM_CHALK, TM_ALIGN_RIGHT, 0, 1);
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, "asks ");
        tm_append_money(&t, row->ask);
        tm_text(g_tm.line, wage_x, y + 27.0f, 11.0f, row->ask > row->wage ? TM_CHALK2 : TM_CHALK3, TM_ALIGN_RIGHT, 0, 0);
        tm_text(tm_money(row->pay), pay_x, y + 15.0f, 14.0f, TM_CHALK, TM_ALIGN_RIGHT, 0, 1);
        tm_hit(x + 8.0f, y, w - 16.0f, HUB_ROW_H, HUB_HIT_ROW, row->player_id);
    }
    if (g_hub.scroll_max > 0.0f) {
        float bar_h = lh * lh / (lh + g_hub.scroll_max);
        float bar_y = ly + (lh - bar_h) * g_hub.scroll / g_hub.scroll_max;
        tm_rect(x + w - 4.0f, bar_y, 3.0f, bar_h, TM_LINE);
    }
}

static void hub_draw_contract_detail(float x) {
    float h = g_tm.height / g_tm.s;
    tm_rect(x, TM_TOP_H, TM_DETAIL_W, h - TM_TOP_H, TM_GROUND);
    tm_rect(x, TM_TOP_H, 1.0f, h - TM_TOP_H, TM_LINE);
    float px = x + 16.0f, pw = TM_DETAIL_W - 32.0f;
    HubRow *row = hub_selected();
    if (!row) {
        tm_text(tm_w("Pick a player to see his contract."), px, TM_TOP_H + 24.0f, 13.0f, TM_CHALK2, TM_ALIGN_LEFT, pw, 0);
        return;
    }
    MarketUiText t;
    float y = TM_TOP_H + 16.0f;
    tm_badge(px, y, 54.0f, row->rating, 28.0f);
    tm_text(row->name, px + 66.0f, y + 7.0f, 17.0f, TM_CHALK, TM_ALIGN_LEFT, pw - 66.0f, 1);
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, tm_pos_label(row->position));
    ui_text_append_ascii(&t, "  ");
    ui_text_append_wide(&t, row->role, 12);
    ui_text_append_ascii(&t, row->role_rank == 3 ? " player" : "");
    tm_text(g_tm.line, px + 66.0f, y + 31.0f, 12.0f, TM_CHALK2, TM_ALIGN_LEFT, pw - 66.0f, 0);
    y += 68.0f;
    /* facts */
    float fw = (pw - 16.0f) / 3.0f;
    uint16_t contract[24];
    ui_text_reset(&t, contract, 24);
    if (row->tenths < 10) ui_text_append_ascii(&t, "Ends soon");
    else {
        ui_text_append_i32(&t, row->tenths / 10);
        ui_text_append_char(&t, '.');
        ui_text_append_i32(&t, row->tenths % 10);
        ui_text_append_ascii(&t, " seasons");
    }
    const char *labels[3] = {"CONTRACT", "WAGE", "CLUBS PAY"};
    const uint16_t *facts[3] = {contract, tm_money(row->wage), tm_money(row->pay)};
    for (int32_t i = 0; i < 3; ++i) {
        float fx = px + (float)i * (fw + 8.0f);
        tm_rect(fx, y, fw, 44.0f, TM_TURF);
        tm_text(tm_w(labels[i]), fx + 8.0f, y + 6.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 0);
        tm_text(facts[i], fx + 8.0f, y + 21.0f, 13.0f, i == 0 && row->tenths < 10 ? TM_AMBER : TM_CHALK, TM_ALIGN_LEFT, fw - 12.0f, 1);
    }
    y += 56.0f;
    /* renewal box */
    int32_t patience = renewal_patience_left(row->player_id);
    int32_t user_index = tm_user_index();
    int32_t max_wage = user_index >= 0 ? clamp_add(row->wage, account_wage_room(&g_market.clubs[user_index])) : 0;
    int32_t can = row->renewable && patience > 0;
    float box_h = 222.0f;
    tm_rect(px, y, pw, box_h, TM_TURF);
    float bx = px + 12.0f, bw = pw - 24.0f, by = y + 10.0f;
    tm_text(tm_w("YOUR WAGE OFFER"), bx, by, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    tm_text(tm_money(g_hub.ren_wage), bx, by + 14.0f, 28.0f, can ? TM_AMBER : TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, "He asks ");
    tm_append_money(&t, row->ask);
    ui_text_append_ascii(&t, "  (");
    ui_text_append_i32(&t, row->ask > 0 ? mul_div(g_hub.ren_wage, 100, row->ask) : 0);
    ui_text_append_ascii(&t, "%)");
    tm_text(g_tm.line, bx + bw, by + 24.0f, 12.0f, TM_CHALK2, TM_ALIGN_RIGHT, 0, 0);
    by += 52.0f;
    /* slider */
    float track_y = by + 8.0f;
    tm_rect(bx, track_y, bw, 6.0f, TM_TURF2);
    float t01 = g_hub.ren_hi > g_hub.ren_lo ? (float)(g_hub.ren_wage - g_hub.ren_lo) / (float)(g_hub.ren_hi - g_hub.ren_lo) : 0.0f;
    if (t01 < 0.0f) t01 = 0.0f;
    if (t01 > 1.0f) t01 = 1.0f;
    tm_rect(bx, track_y, bw * t01, 6.0f, TM_AMBER);
    float ask_t = g_hub.ren_hi > g_hub.ren_lo ? (float)(row->ask - g_hub.ren_lo) / (float)(g_hub.ren_hi - g_hub.ren_lo) : 0.5f;
    if (ask_t >= 0.0f && ask_t <= 1.0f) tm_rect(bx + bw * ask_t - 1.0f, track_y - 6.0f, 2.0f, 18.0f, TM_CHALK2);
    tm_rect(bx + bw * t01 - 8.0f, track_y - 7.0f, 16.0f, 20.0f, can ? TM_CHALK : TM_CHALK3);
    if (can) tm_hit(bx - 10.0f, by - 6.0f, bw + 20.0f, 34.0f, HUB_HIT_SLIDER, 0);
    by += 30.0f;
    float b3 = (bw - 12.0f) / 3.0f;
    tm_button(bx, by, b3, 30.0f, tm_w("- 5%"), 0, can, HUB_HIT_MINUS, 0);
    tm_button(bx + b3 + 6.0f, by, b3, 30.0f, tm_w("+ 5%"), 0, can, HUB_HIT_PLUS, 0);
    tm_button(bx + 2.0f * (b3 + 6.0f), by, b3, 30.0f, tm_w("Type wage"), 0, can, HUB_HIT_TYPE, 0);
    by += 40.0f;
    float yb = (bw - 16.0f) / 5.0f;
    for (int32_t k = 1; k <= 5; ++k) {
        float yx = bx + (float)(k - 1) * (yb + 4.0f);
        int32_t on = g_hub.ren_years == k;
        tm_rect(yx, by, yb, 28.0f, on ? TM_TURF3 : TM_TURF2);
        uint16_t buf[8];
        ui_text_reset(&t, buf, 8);
        ui_text_append_i32(&t, k);
        ui_text_append_ascii(&t, " yr");
        tm_text(buf, yx + yb * 0.5f, by + 8.0f, 12.0f, on ? TM_CHALK : TM_CHALK2, TM_ALIGN_CENTER, 0, 1);
        if (can) tm_hit(yx, by, yb, 28.0f, HUB_HIT_YEARS, k);
    }
    by += 38.0f;
    /* his patience, then the last reply */
    tm_text(tm_w("HIS PATIENCE"), bx, by, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    float sw = (bw - 90.0f - 8.0f) / 3.0f;
    for (int32_t k = 0; k < 3; ++k) {
        float sx = bx + 90.0f + (float)k * (sw + 4.0f);
        int32_t fill = patience - k * 1000;
        fill = fill < 0 ? 0 : fill > 1000 ? 1000 : fill;
        tm_rect(sx, by + 2.0f, sw, 7.0f, TM_TURF2);
        tm_rect(sx, by + 2.0f, sw * (float)fill / 1000.0f, 7.0f, TM_SKY);
    }
    by += 20.0f;
    const uint16_t *reply = g_hub.reply[0] ? g_hub.reply
        : !row->renewable ? tm_w("He has no contract the market can renew.")
        : patience <= 0 ? tm_w("He will not talk about a new deal until next season.")
        : g_hub.ren_wage > max_wage ? tm_w("That wage is more than your wage room allows.") : (const uint16_t *)0;
    uint32_t rc = g_hub.reply_kind == HUB_REPLY_GOOD ? TM_GOOD : g_hub.reply_kind == HUB_REPLY_BAD || !g_hub.reply[0] ? TM_BAD : TM_CHALK;
    if (reply) tm_text_wrap(reply, bx, by, 12.0f, rc, bw, 2, 15.0f);
    y += box_h + 10.0f;
    /* actions */
    float ay = h - 108.0f;
    if (g_hub.ren_counter > 0 && can) {
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, "Accept ");
        tm_append_money(&t, g_hub.ren_counter);
        tm_button(px, ay - 46.0f, pw, 38.0f, g_tm.line, 0, g_hub.ren_counter <= max_wage, HUB_HIT_ACCEPT, 0);
    }
    tm_button(px, ay, pw, 44.0f, tm_w(can ? "Offer renewal" : "Can't renew"), 1, can && g_hub.ren_wage <= max_wage,
              HUB_HIT_RENEW, row->player_id);
    float half = (pw - 8.0f) * 0.5f;
    tm_button(px, ay + 52.0f, half, 40.0f, tm_w(row->listed ? "Listed" : "List for sale"), 0, 1, HUB_HIT_LISTTOGGLE, row->player_id);
    tm_button(px + half + 8.0f, ay + 52.0f, half, 40.0f, tm_w(row->kept ? "Kept" : "Keep him"), 0, 1, HUB_HIT_KEEP, row->player_id);
}

static void hub_render(uint32_t base) {
    if (g_hub.dirty) hub_build(base);
    float main_x = TM_RAIL_W;
    float main_w = g_tm.dw - TM_RAIL_W;
    tm_rect(0, 0, g_tm.dw, TM_DESIGN_H, TM_TURF);
    if (g_hub.view == HUB_FIN) hub_draw_finances(base, main_x, main_w);
    else if (g_hub.view == HUB_BOARD) hub_draw_board(main_x, main_w);
    else {
        float list_w = main_w - TM_DETAIL_W;
        hub_draw_contract_list(main_x, list_w);
        hub_draw_depth(main_x, list_w);
        hub_draw_contract_detail(main_x + list_w);
    }
    hub_draw_rail();
    hub_draw_top(base);
}

/* ---------------------------------------------------------------------------------------------------------------
 * input
 * ------------------------------------------------------------------------------------------------------------- */
static void hub_set_wage_from_x(float x) {
    float bx = g_tm.dw - TM_DETAIL_W + 16.0f + 12.0f, bw = TM_DETAIL_W - 32.0f - 24.0f;
    float t = (x - bx) / bw;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    g_hub.ren_wage = round5(g_hub.ren_lo + (int32_t)((float)(g_hub.ren_hi - g_hub.ren_lo) * t));
    if (g_hub.ren_wage < 1) g_hub.ren_wage = 1;
}

static int32_t hub_cb_wage(int32_t selection) {
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
        tm_toast(tm_w("Type the wage in coins a season, digits only."));
        return 1;
    }
    if (value > g_hub.ren_hi) g_hub.ren_hi = value;
    if (value < g_hub.ren_lo) g_hub.ren_lo = value;
    g_hub.ren_wage = value;
    return 1;
}

static void hub_open_wage_keyboard(uint32_t base) {
    ui_set_title("Renewal Wage");
    MarketUiText t;
    ui_text_reset(&t, g_ui_keyboard_initial, 48);
    ui_text_append_i32(&t, g_hub.ren_wage);
    ui_text_reset(&t, g_ui_description, MARKET_UI_TEXT_CAPACITY);
    ui_text_append_ascii(&t, "Wage in coins a season");
    if (!ui_show_keyboard(base, 9, hub_cb_wage)) g_ui_keyboard_box = (void *)0;
    else g_tm.kb_wait = 120;
}

static void hub_renew(uint32_t base, int32_t wage) {
    HubRow *row = hub_selected();
    if (!row) return;
    int32_t player_id = row->player_id, years = g_hub.ren_years, counter = 0;
    int32_t result = hub_renewal_offer(base, player_id, wage, years, &counter);
    MarketUiText t;
    ui_text_reset(&t, g_tm.line, 160);
    int32_t kind = HUB_REPLY_INFO;
    if (result == 1) {
        kind = HUB_REPLY_GOOD;
        ui_text_append_ascii(&t, "He signs for ");
        ui_text_append_i32(&t, years);
        ui_text_append_ascii(&t, years == 1 ? " year at " : " years at ");
        tm_append_money(&t, wage);
        ui_text_append_ascii(&t, " a season.");
        g_hub.ren_counter = 0;
    } else if (result == 0) {
        ui_text_append_ascii(&t, "Not enough. He would sign for ");
        tm_append_money(&t, counter);
        ui_text_append_char(&t, '.');
        g_hub.ren_counter = counter;
    } else {
        kind = HUB_REPLY_BAD;
        g_hub.ren_counter = 0;
        ui_text_append_ascii(&t, result == -4 ? "He is tired of this. No new deal until next season."
                                 : result == -3 ? "That wage is more than your wage room allows."
                                 : result == -2 ? "He has no contract the market can renew." : "The offer could not be made.");
    }
    uint16_t reply[TM_TEXT];
    int32_t i = 0;
    while (g_tm.line[i] && i < TM_TEXT - 1) { reply[i] = g_tm.line[i]; ++i; }
    reply[i] = 0;
    tm_refresh(base);
    hub_build(base);
    g_hub.sel_id = player_id;
    hub_set_reply(kind, reply);
    save_profile(base);
}

static void hub_activate(uint32_t base, const TmHit *hit) {
    switch (hit->kind) {
    case HUB_HIT_BACK: hub_back(base); break;
    case HUB_HIT_TAB: g_hub.view = hit->value; g_hub.scroll = 0.0f; g_hub.dirty = 1; break;
    case HUB_HIT_SEASON: g_hub.last_season = hit->value; break;
    case HUB_HIT_ROW: hub_select(hit->value); break;
    case HUB_HIT_MINUS:
    case HUB_HIT_PLUS: {
        HubRow *row = hub_selected();
        int32_t step = round5(mul_div(row ? row->ask : g_hub.ren_wage, 5, 100));
        if (step < 5) step = 5;
        int32_t wage = g_hub.ren_wage + (hit->kind == HUB_HIT_PLUS ? step : -step);
        if (wage < 5) wage = 5;
        if (wage > g_hub.ren_hi) g_hub.ren_hi = wage;
        if (wage < g_hub.ren_lo) g_hub.ren_lo = wage;
        g_hub.ren_wage = wage;
        break;
    }
    case HUB_HIT_TYPE: hub_open_wage_keyboard(base); break;
    case HUB_HIT_YEARS: g_hub.ren_years = hit->value; break;
    case HUB_HIT_RENEW: hub_renew(base, g_hub.ren_wage); break;
    case HUB_HIT_ACCEPT:
        if (g_hub.ren_counter > 0) { g_hub.ren_wage = g_hub.ren_counter; hub_renew(base, g_hub.ren_counter); }
        break;
    case HUB_HIT_LISTTOGGLE: {
        int32_t r = toggle_user_listing(hit->value);
        tm_toast(tm_w(r == 0 ? "Listed for sale. Clubs can bid for him now."
                      : r == 1 ? "Taken off the sale list." : "He cannot be listed right now."));
        g_hub.dirty = 1;
        break;
    }
    case HUB_HIT_KEEP: {
        int32_t r = user_keep_toggle(hit->value);
        tm_toast(tm_w(r == 1 ? "Kept. Clubs will not bid for him unless you list him."
                      : r == 0 ? "No longer kept. Clubs may bid for him again."
                      : r == -1 ? "You can keep at most 16 players." : "He cannot be kept right now."));
        g_hub.dirty = 1;
        break;
    }
    default: break;
    }
}
