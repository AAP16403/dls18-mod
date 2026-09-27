/*
 * Team Management v2 (v40): replaces the career-menu Team Management screen (CFEScreenStack::NewScreen id 4,
 * CFESTeamManagement) with a third view of the Transfer Market v2 screen object (g_tm.view = 2). In a match
 * (CCore::InGame) and in online matches the stock screen stays: substitutions are never touched. A Classic button
 * opens the stock screen once on top (training, healing, roles, kits); Back returns here.
 *
 *   rail      Lineup, Squad, Chemistry; team chemistry and its average boost
 *   Lineup    the pitch in the current formation, a formation picker, tap a player then Swap then another
 *   Squad     every player: form, last 5 ratings, share of minutes, chemistry, boost
 *   Chemistry the XI's chemistry, the links on the pitch (defence, midfield, attack), who holds it back and why
 *   panel     the selected player: form, minutes, chemistry, his ratings, the parts of his chemistry, boost
 *
 * Game data (see chemistry.c for the addresses): career CTeamManagement = CSeason+0x6E0, lineup count u8 at +0x140,
 * ids u16 at +0x142 (index 0..10 = the formation slots), CTeam at +0x194; formation CTeam+0x12F. Edits happen only
 * when CTeam+0x1014 (CTeam::GetTeamMan) is that same CTeamManagement; otherwise the screen is read-only.
 * Swap = CTeamManagement::SwapPlayersByID(tm, a, b, false, -1, -1) 0x2F27D8 (the stock AttemptSwap call; it checks
 * the rules and keeps the CTeam in step), leaving saves through CTeamManagement::Save(true) 0x2F2C84.
 */

#define TMG_SCREEN_ID 4
#define TMG_ROW_H 50.0f
#define TMG_ROWS_MAX 64

enum { TMG_LINEUP, TMG_SQUAD, TMG_CHEM, TMG_VIEW_COUNT };

typedef int32_t (*TmgSwapFn)(void *, int32_t, int32_t, int32_t, int32_t, int32_t);
typedef void (*TmgSaveFn)(void *, int32_t);

static const char *const k_tmg_formations[12] = {
    "4-4-2", "4-1-2-1-2", "4-3-1-2", "4-5-1", "4-1-4-1", "4-4-1-1",
    "4-3-3", "4-1-2-3", "5-3-2", "5-2-1-2", "3-4-3", "3-5-2"
};

typedef struct {
    int32_t player_id, index, rating, position, slot;     /* slot 0..10 in the XI, -1 bench */
    int32_t chem, boost_x10, form_x10, apps_pm;
    uint8_t hist[CHEM_HISTORY];
    int32_t hist_n;
    uint16_t name[40];
} TmgRow;

static struct {
    int32_t view, dirty, sel_id, swap_from, edited, editable;
    int32_t formation, lineup_count;
    int32_t xi[11];
    TmgRow rows[TMG_ROWS_MAX];
    int32_t row_count;
    float scroll, scroll_max;
    int32_t team_chem, team_boost_x10;
    uint16_t note[TM_TEXT];
    int32_t note_kind;
} g_tmg;

static int32_t g_tmg_stock_once;          /* Classic: let NewScreen build the stock screen once */

static uint8_t *tmg_tm(uint32_t base) { return (uint8_t *)(uintptr_t)(base + PROFILE_INSTANCE + 0x14 + 0x6E0); }

static uint8_t *tmg_cteam(uint32_t base) {
    uint8_t *tm = tmg_tm(base);
    return *(uint8_t **)(tm + 0x194);
}

static int32_t tmg_lineup_id(uint32_t base, int32_t i) {
    uint8_t *tm = tmg_tm(base);
    return i >= 0 && i < 32 ? *(uint16_t *)(tm + 0x142 + i * 2) : -1;
}

static void tmg_note(int32_t kind, const char *text) {
    int32_t i = 0;
    while (text[i] && i < TM_TEXT - 1) { g_tmg.note[i] = (uint8_t)text[i]; ++i; }
    g_tmg.note[i] = 0;
    g_tmg.note_kind = kind;
}

static TmgRow *tmg_row(int32_t player_id) {
    for (int32_t i = 0; i < g_tmg.row_count; ++i) if (g_tmg.rows[i].player_id == player_id) return &g_tmg.rows[i];
    return (TmgRow *)0;
}

static void tmg_build(uint32_t base) {
    uint8_t *tm = tmg_tm(base);
    uint8_t *cteam = tmg_cteam(base);
    g_tmg.editable = cteam && *(uint8_t **)(cteam + 0x1014) == tm;
    g_tmg.formation = cteam ? cteam[0x12F] : 0;
    if (g_tmg.formation < 0 || g_tmg.formation > 11) g_tmg.formation = 0;
    g_tmg.lineup_count = tm[0x140] > 32 ? 32 : tm[0x140];
    chem_prepare(base);
    chem_update_fit(base);
    for (int32_t s = 0; s < 11; ++s) g_tmg.xi[s] = s < g_tmg.lineup_count ? tmg_lineup_id(base, s) : -1;
    g_tmg.row_count = 0;
    /* the lineup order first (XI, then the bench as the game orders it), then anyone else in the squad */
    for (int32_t pass = 0; pass < 2; ++pass) {
        for (int32_t i = 0; i < (pass == 0 ? g_tmg.lineup_count : g_player_count) && g_tmg.row_count < TMG_ROWS_MAX; ++i) {
            int32_t id = pass == 0 ? tmg_lineup_id(base, i) : g_players[i].player_id;
            if (pass == 1 && g_players[i].owner_id != USER_TEAM_ID) continue;
            if (tmg_row(id)) continue;
            int32_t index = find_cached_player(id);
            if (index < 0 || g_players[index].owner_id != USER_TEAM_ID) continue;
            TmgRow *r = &g_tmg.rows[g_tmg.row_count++];
            r->player_id = id;
            r->index = index;
            r->rating = g_players[index].rating;
            r->position = g_players[index].position;
            r->slot = pass == 0 && i < 11 ? i : -1;
            ChemParts parts;
            int32_t tracked = chem_parts(id, &parts);
            r->chem = tracked ? parts.chemistry : 0;
            r->boost_x10 = tracked ? chem_boost_x10(parts.chemistry) : 10;
            r->form_x10 = chem_form_x10(id);
            ChemPlayer *c = chem_find(id);
            r->apps_pm = c ? c->apps_pm : 0;
            r->hist_n = chem_history(id, r->hist);
            tm_player_name(base, id, r->name, 40);
        }
    }
    int32_t sum = 0, n = 0;
    for (int32_t s = 0; s < 11; ++s) {
        TmgRow *r = tmg_row(g_tmg.xi[s]);
        if (r) { sum += r->chem; ++n; }
    }
    g_tmg.team_chem = n ? sum / n : 0;
    g_tmg.team_boost_x10 = chem_boost_x10(g_tmg.team_chem);
    if (!tmg_row(g_tmg.sel_id)) g_tmg.sel_id = g_tmg.row_count ? g_tmg.rows[0].player_id : -1;
    g_tmg.dirty = 0;
}

static void tmg_open(uint32_t base) {
    g_tm.view = 2;
    g_tmg.dirty = 1;
    g_tmg.swap_from = -1;
    g_tmg.edited = 0;
    g_tmg.note[0] = 0;
    g_tm.toast_frames = 0;                 /* a market message does not follow into this screen */
    if (g_tmg.view < 0 || g_tmg.view >= TMG_VIEW_COUNT) g_tmg.view = TMG_LINEUP;
    tm_refresh(base);
}

/* Leaving: save the lineup once if anything changed (the stock screen saves in its Process on the way out). */
static void tmg_leave(uint32_t base) {
    if (g_tmg.edited && g_tmg.editable) {
        ((TmgSaveFn)(base + 0x2F2C85))(tmg_tm(base), 1);
        g_tmg.edited = 0;
    }
}

static void tmg_swap(uint32_t base, int32_t a, int32_t b) {
    g_tmg.swap_from = -1;
    if (a == b || a < 0 || b < 0) return;
    if (!g_tmg.editable) { tmg_note(TM_LOG_BAD, "This lineup cannot be changed here. Use Classic."); return; }
    int32_t r = ((TmgSwapFn)(base + 0x2F27D9))(tmg_tm(base), a, b, 0, -1, -1);
    if (r != 0) { tmg_note(TM_LOG_BAD, "Those two players cannot swap (injured, suspended or not allowed)."); return; }
    g_tmg.edited = 1;
    tmg_note(TM_LOG_GOOD, "Swapped. Chemistry updates for the new positions.");
    g_tmg.dirty = 1;
}

static void tmg_set_formation(uint32_t base, int32_t f) {
    if (f < 0 || f > 11) return;
    uint8_t *cteam = tmg_cteam(base);
    if (!g_tmg.editable || !cteam) { tmg_note(TM_LOG_BAD, "The formation cannot be changed here. Use Classic."); return; }
    if (cteam[0x12F] == f) return;
    cteam[0x12F] = (uint8_t)f;             /* CFETeamManagement::SetFormation's menu formation */
    g_tmg.edited = 1;
    g_tmg.dirty = 1;
    tmg_note(TM_LOG_CLUB, "Formation changed. Familiarity builds again over the next matches.");
}

/* ---------------------------------------------------------------------------------------------------------------
 * render
 * ------------------------------------------------------------------------------------------------------------- */
static uint32_t tmg_tone(int32_t chem) {
    return chem >= 700 ? TM_GOOD : chem >= 500 ? TM_SKY : chem >= 350 ? TM_AMBER : TM_BAD;
}

static uint32_t tmg_rating_tone(int32_t r10) {
    return r10 >= 75 ? TM_GOOD : r10 >= 65 ? TM_CHALK : r10 >= 60 ? TM_AMBER : TM_BAD;
}

static void tmg_append_x10(MarketUiText *t, int32_t v) {
    ui_text_append_i32(t, v / 10);
    ui_text_append_char(t, '.');
    ui_text_append_i32(t, v % 10);
}

static const uint16_t *tmg_boost_text(int32_t boost_x10) {
    static uint16_t buf[4][16];
    static int32_t next;
    uint16_t *out = buf[next];
    next = (next + 1) & 3;
    MarketUiText t;
    ui_text_reset(&t, out, 16);
    ui_text_append_char(&t, '+');
    tmg_append_x10(&t, boost_x10);
    ui_text_append_char(&t, '%');
    return out;
}

static void tmg_draw_top(uint32_t base) {
    float dw = g_tm.dw;
    tm_rect(0, 0, dw, TM_TOP_H, TM_GROUND);
    tm_rect(0, TM_TOP_H - 1.0f, dw, 1.0f, TM_LINE);
    tm_rect(10.0f, 9.0f, 88.0f, 38.0f, TM_TURF2);
    tm_rect(10.0f, 9.0f, 88.0f, 1.0f, TM_LINE);
    tm_rect(10.0f, 46.0f, 88.0f, 1.0f, TM_LINE);
    tm_triangle(20.0f, 28.0f, 30.0f, 20.0f, 30.0f, 36.0f, TM_CHALK);
    tm_rect(29.0f, 26.5f, 7.0f, 3.0f, TM_CHALK);
    tm_text(tm_w("Back"), 42.0f, 19.0f, 15.0f, TM_CHALK, TM_ALIGN_LEFT, 0, 1);
    tm_hit(4.0f, 2.0f, 104.0f, 52.0f, TM_HIT_BACK, 0);
    const uint16_t *title = tm_w("TEAM MANAGEMENT");
    tm_text(title, 116.0f, 15.0f, 24.0f, TM_CHALK, TM_ALIGN_LEFT, 0, 1);
    float x = 116.0f + tm_text_width(title, 24.0f) + 16.0f;
    MarketUiText t;
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, k_tmg_formations[g_tmg.formation]);
    ui_text_append_ascii(&t, "   Familiar ");
    ui_text_append_i32(&t, (chem_familiarity_pm() + 5) / 10);
    ui_text_append_char(&t, '%');
    float pw = tm_text_width(g_tm.line, 12.0f) + 24.0f;
    tm_rect(x, 15.0f, pw, 26.0f, TM_TURF2);
    tm_text(g_tm.line, x + 12.0f, 21.0f, 12.0f, TM_GOOD, TM_ALIGN_LEFT, 0, 1);
    tm_button(x + pw + 12.0f, 9.0f, 96.0f, 38.0f, tm_w("Classic"), 0, 1, TMG_HIT_CLASSIC, 0);
    float rx = dw - 16.0f;
    uint16_t chem[8];
    ui_text_reset(&t, chem, 8);
    ui_text_append_i32(&t, (g_tmg.team_chem + 5) / 10);
    ui_text_reset(&t, g_tm.line, 160);
    if (chem_team_form_x10() > 0) tmg_append_x10(&t, chem_team_form_x10());
    else ui_text_append_char(&t, '-');
    const char *labels[3] = {"FORM, LAST MATCH", "AVG BOOST", "TEAM CHEMISTRY"};
    const uint16_t *values[3] = {g_tm.line, tmg_boost_text(g_tmg.team_boost_x10), chem};
    uint32_t colours[3] = {TM_CHALK, TM_GOOD, tmg_tone(g_tmg.team_chem)};
    (void)base;
    for (int32_t i = 0; i < 3; ++i) {
        float w = tm_text_width(values[i], 21.0f);
        float lw = tm_text_width(tm_w(labels[i]), 10.0f);
        if (lw > w) w = lw;
        tm_text(tm_w(labels[i]), rx, 8.0f, 10.0f, TM_CHALK3, TM_ALIGN_RIGHT, 0, 0);
        tm_text(values[i], rx, 22.0f, 21.0f, colours[i], TM_ALIGN_RIGHT, 0, 1);
        rx -= w + 20.0f;
    }
}

static void tmg_draw_rail(void) {
    static const char *const names[TMG_VIEW_COUNT] = {"Lineup", "Squad", "Chemistry"};
    float h = g_tm.height / g_tm.s;
    tm_rect(0, TM_TOP_H, TM_RAIL_W, h - TM_TOP_H, TM_GROUND);
    tm_rect(TM_RAIL_W - 1.0f, TM_TOP_H, 1.0f, h - TM_TOP_H, TM_LINE);
    for (int32_t i = 0; i < TMG_VIEW_COUNT; ++i) {
        float y = TM_TOP_H + 10.0f + (float)i * 48.0f;
        int32_t active = g_tmg.view == i;
        if (active) { tm_rect(8.0f, y, TM_RAIL_W - 16.0f, 46.0f, TM_TURF2); tm_rect(8.0f, y, 3.0f, 46.0f, TM_AMBER); }
        tm_text(tm_w(names[i]), 22.0f, y + 15.0f, 15.0f, active ? TM_CHALK : TM_CHALK2, TM_ALIGN_LEFT, 0, active);
        tm_hit(8.0f, y, TM_RAIL_W - 16.0f, 46.0f, TMG_HIT_TAB, i);
    }
    float py = h - 132.0f;
    tm_rect(10.0f, py - 10.0f, TM_RAIL_W - 20.0f, 1.0f, TM_LINE);
    tm_text(tm_w("MATCHES TRACKED"), 14.0f, py, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    MarketUiText t;
    uint16_t buf[12];
    ui_text_reset(&t, buf, 12);
    ui_text_append_i32(&t, g_ext3.matches);
    tm_text(buf, 14.0f, py + 16.0f, 26.0f, TM_CHALK, TM_ALIGN_LEFT, 0, 1);
    tm_text_wrap(tm_w("Ratings and chemistry update after every match."), 14.0f, py + 54.0f, 12.0f, TM_CHALK2,
                 TM_RAIL_W - 28.0f, 3, 16.0f);
}

/* Pitch rows from the formation's menu positions (FS_iFormationFEPlayerPos): 0 GK, 1-3 defence, 4 holding,
 * 5 centre, 7/8 wide midfield, 6 attacking midfield, 9 wing, 10 striker. Slots in a row run right to left. */
static float tmg_row_level(int32_t fe) {
    switch (fe) {
    case 0: return 0.0f;
    case 1: case 2: case 3: return 1.0f;
    case 4: return 1.7f;
    case 5: case 7: case 8: return 2.2f;
    case 6: return 2.75f;
    case 9: return 3.1f;
    default: return 3.35f;
    }
}

static void tmg_slot_pos(uint32_t base, int32_t slot, float px, float py, float pw, float ph, float *ox, float *oy) {
    const int32_t *fe = (const int32_t *)(uintptr_t)(base + 0x63A76C) + g_tmg.formation * 11;
    float level = tmg_row_level(fe[slot]);
    int32_t count = 0, order = 0;
    for (int32_t s = 0; s < 11; ++s) {
        if (tmg_row_level(fe[s]) != level) continue;
        if (s < slot) ++order;
        ++count;
    }
    *ox = px + pw - pw * ((float)order + 0.5f) / (float)(count > 0 ? count : 1);
    *oy = py + ph - 52.0f - (ph - 108.0f) * level / 3.35f;
}

static void tmg_draw_disc(const TmgRow *r, float cx, float cy, int32_t selected, int32_t swap_src) {
    float d = 44.0f;
    uint32_t ring = tmg_tone(r->chem);
    tm_rect(cx - d * 0.5f - 3.0f, cy - d * 0.5f - 3.0f, d + 6.0f, d + 6.0f, swap_src ? TM_AMBER : selected ? TM_CHALK : TM_GROUND);
    tm_rect(cx - d * 0.5f, cy - d * 0.5f, d, d, TM_GROUND);
    tm_rect(cx - d * 0.5f, cy + d * 0.5f - 4.0f, d * (float)r->chem / 1000.0f, 4.0f, ring);
    MarketUiText t;
    uint16_t buf[8];
    ui_text_reset(&t, buf, 8);
    ui_text_append_i32(&t, r->rating);
    tm_text(buf, cx, cy - 11.0f, 18.0f, TM_CHALK, TM_ALIGN_CENTER, 0, 1);
    tm_rect(cx - 44.0f, cy + d * 0.5f + 4.0f, 88.0f, 18.0f, TM_GROUND);
    tm_text(r->name, cx, cy + d * 0.5f + 6.0f, 11.0f, TM_CHALK, TM_ALIGN_CENTER, 84.0f, 1);
    if (r->form_x10 > 0) {
        ui_text_reset(&t, buf, 8);
        tmg_append_x10(&t, r->form_x10);
        tm_rect(cx + d * 0.5f - 10.0f, cy - d * 0.5f - 8.0f, 30.0f, 16.0f, tmg_rating_tone(r->form_x10));
        tm_text(buf, cx + d * 0.5f + 5.0f, cy - d * 0.5f - 6.0f, 10.0f, TM_AMBER_INK, TM_ALIGN_CENTER, 0, 1);
    }
}

static void tmg_draw_lineup(uint32_t base, float x, float w) {
    float h = g_tm.height / g_tm.s;
    float top = TM_TOP_H;
    tm_rect(x, top, w, h - top, TM_TURF);
    /* formation picker: two rows of six */
    float fx = x + 12.0f, fw = (w - 24.0f - 5.0f * 4.0f) / 6.0f;
    for (int32_t f = 0; f < 12; ++f) {
        float bx = fx + (float)(f % 6) * (fw + 4.0f), by = top + 8.0f + (float)(f / 6) * 30.0f;
        int32_t on = f == g_tmg.formation;
        tm_rect(bx, by, fw, 26.0f, on ? TM_AMBER : TM_GROUND);
        tm_text(tm_w(k_tmg_formations[f]), bx + fw * 0.5f, by + 7.0f, 11.0f, on ? TM_AMBER_INK : TM_CHALK2, TM_ALIGN_CENTER, fw - 4.0f, 1);
        tm_hit(bx, by, fw, 26.0f, TMG_HIT_FORMATION, f);
    }
    /* pitch */
    float px = x + 16.0f, py = top + 74.0f, pw = w - 32.0f, ph = h - py - 12.0f;
    tm_rect(px, py, pw, ph, 0xFF16402Fu);
    for (int32_t k = 0; k < 6; k += 2) tm_rect(px, py + ph * (float)k / 6.0f, pw, ph / 6.0f, 0xFF1A4A37u);
    uint32_t chalk = tm_mix(TM_CHALK, 0xFF16402Fu, 250);
    tm_rect(px, py, pw, 2.0f, chalk); tm_rect(px, py + ph - 2.0f, pw, 2.0f, chalk);
    tm_rect(px, py, 2.0f, ph, chalk); tm_rect(px + pw - 2.0f, py, 2.0f, ph, chalk);
    tm_rect(px, py + ph * 0.5f - 1.0f, pw, 2.0f, chalk);
    tm_rect(px + pw * 0.3f, py + ph - ph * 0.14f, pw * 0.4f, 2.0f, chalk);
    tm_rect(px + pw * 0.3f, py + ph * 0.14f, pw * 0.4f, 2.0f, chalk);
    tm_hit(px, py, pw, ph, TM_HIT_NONE, 0);
    for (int32_t s = 0; s < 11; ++s) {
        TmgRow *r = tmg_row(g_tmg.xi[s]);
        if (!r) continue;
        float cx, cy;
        tmg_slot_pos(base, s, px, py, pw, ph, &cx, &cy);
        tmg_draw_disc(r, cx, cy, r->player_id == g_tmg.sel_id, r->player_id == g_tmg.swap_from);
        tm_hit(cx - 30.0f, cy - 28.0f, 60.0f, 70.0f, TMG_HIT_PLAYER, r->player_id);
    }
    if (g_tmg.swap_from >= 0) {
        tm_rect(px + 8.0f, py + 8.0f, pw - 16.0f, 30.0f, TM_AMBER);
        tm_text(tm_w("Pick the player to swap with, on the pitch or in Squad."), px + 20.0f, py + 15.0f, 12.0f, TM_AMBER_INK,
                TM_ALIGN_LEFT, pw - 40.0f, 1);
    }
}

static void tmg_draw_squad(float x, float w) {
    float h = g_tm.height / g_tm.s;
    float top = TM_TOP_H;
    float right = x + w - 16.0f;
    float boost_x = right, chem_x = boost_x - 70.0f, spark_r = chem_x - 96.0f, form_x = spark_r - 80.0f;
    float name_x = x + 72.0f;
    tm_rect(x, top, w, 28.0f, TM_TURF);
    tm_text(tm_w("PLAYER"), name_x, top + 9.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 0);
    tm_text(tm_w("FORM"), form_x, top + 9.0f, 10.0f, TM_CHALK3, TM_ALIGN_RIGHT, 0, 0);
    tm_text(tm_w("LAST 5"), spark_r, top + 9.0f, 10.0f, TM_CHALK3, TM_ALIGN_RIGHT, 0, 0);
    tm_text(tm_w("CHEMISTRY"), chem_x, top + 9.0f, 10.0f, TM_CHALK3, TM_ALIGN_RIGHT, 0, 0);
    tm_text(tm_w("BOOST"), boost_x, top + 9.0f, 10.0f, TM_CHALK3, TM_ALIGN_RIGHT, 0, 0);
    tm_rect(x, top + 27.0f, w, 1.0f, TM_LINE);
    float ly = top + 28.0f, lh = h - ly;
    tm_rect(x, ly, w, lh, TM_TURF);
    tm_hit(x, ly, w, lh, TMG_HIT_LIST, 0);
    g_tmg.scroll_max = (float)g_tmg.row_count * TMG_ROW_H + 8.0f - lh;
    if (g_tmg.scroll_max < 0.0f) g_tmg.scroll_max = 0.0f;
    if (g_tmg.scroll > g_tmg.scroll_max) g_tmg.scroll = g_tmg.scroll_max;
    if (g_tmg.scroll < 0.0f) g_tmg.scroll = 0.0f;
    MarketUiText t;
    for (int32_t i = 0; i < g_tmg.row_count; ++i) {
        float y = ly + 4.0f + (float)i * TMG_ROW_H - g_tmg.scroll;
        if (y < ly || y + TMG_ROW_H > ly + lh) continue;
        const TmgRow *r = &g_tmg.rows[i];
        if (r->player_id == g_tmg.sel_id) tm_rect(x + 8.0f, y, w - 16.0f, TMG_ROW_H - 2.0f, TM_TURF3);
        if (r->player_id == g_tmg.swap_from) tm_rect(x + 8.0f, y, 3.0f, TMG_ROW_H - 2.0f, TM_AMBER);
        tm_badge(x + 18.0f, y + 5.0f, 38.0f, r->rating, 19.0f);
        tm_text(r->name, name_x, y + 7.0f, 14.0f, TM_CHALK, TM_ALIGN_LEFT, form_x - name_x - 60.0f, 1);
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_ascii(&t, tm_pos_label(r->position));
        ui_text_append_ascii(&t, r->slot >= 0 ? "  XI  " : "  Bench  ");
        ui_text_append_i32(&t, (r->apps_pm + 5) / 10);
        ui_text_append_ascii(&t, "% mins");
        tm_text(g_tm.line, name_x, y + 27.0f, 11.0f, TM_CHALK2, TM_ALIGN_LEFT, form_x - name_x - 60.0f, 0);
        if (r->form_x10 > 0) {
            ui_text_reset(&t, g_tm.line, 160);
            tmg_append_x10(&t, r->form_x10);
            tm_text(g_tm.line, form_x, y + 14.0f, 15.0f, tmg_rating_tone(r->form_x10), TM_ALIGN_RIGHT, 0, 1);
        }
        /* last five ratings as bars (a gap where he did not play) */
        int32_t first = r->hist_n > 5 ? r->hist_n - 5 : 0;
        for (int32_t k = first; k < r->hist_n; ++k) {
            float bx = spark_r - (float)(r->hist_n - k) * 11.0f;
            int32_t v = r->hist[k];
            if (!v) { tm_rect(bx, y + 36.0f, 8.0f, 2.0f, TM_CHALK3); continue; }
            float bh = 26.0f * (float)(v - 30) / 70.0f + 2.0f;
            tm_rect(bx, y + 38.0f - bh, 8.0f, bh, tmg_rating_tone(v));
        }
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_i32(&t, (r->chem + 5) / 10);
        tm_text(g_tm.line, chem_x, y + 8.0f, 14.0f, tmg_tone(r->chem), TM_ALIGN_RIGHT, 0, 1);
        tm_rect(chem_x - 56.0f, y + 30.0f, 56.0f, 5.0f, TM_TURF2);
        tm_rect(chem_x - 56.0f, y + 30.0f, 56.0f * (float)r->chem / 1000.0f, 5.0f, tmg_tone(r->chem));
        tm_text(tmg_boost_text(r->boost_x10), boost_x, y + 14.0f, 14.0f, TM_GOOD, TM_ALIGN_RIGHT, 0, 1);
        tm_hit(x + 8.0f, y, w - 16.0f, TMG_ROW_H, TMG_HIT_PLAYER, r->player_id);
    }
}

/* Why a player holds the chemistry back: the weakest of his parts. */
static const char *tmg_reason(int32_t player_id) {
    ChemParts p;
    if (!chem_parts(player_id, &p)) return "not tracked yet";
    int32_t worst = p.tenure, which = 0;
    if (p.involvement < worst) { worst = p.involvement; which = 1; }
    if (p.performance < worst) { worst = p.performance; which = 2; }
    if (p.fit < worst) { worst = p.fit; which = 3; }
    static const char *const why[4] = {"new at the club", "rarely plays", "poor form", "out of position"};
    return why[which];
}

static void tmg_draw_chem(float x, float w) {
    float h = g_tm.height / g_tm.s;
    tm_rect(x, TM_TOP_H, w, h - TM_TOP_H, TM_TURF);
    float px = x + 18.0f, pw = (w - 54.0f) * 0.5f, y = TM_TOP_H + 16.0f;
    MarketUiText t;
    /* team card */
    tm_rect(px, y, pw, 150.0f, TM_TURF2);
    tm_text(tm_w("STARTING XI CHEMISTRY"), px + 14.0f, y + 12.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    uint16_t big[8];
    ui_text_reset(&t, big, 8);
    ui_text_append_i32(&t, (g_tmg.team_chem + 5) / 10);
    tm_text(big, px + 14.0f, y + 28.0f, 44.0f, tmg_tone(g_tmg.team_chem), TM_ALIGN_LEFT, 0, 1);
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, "Average boost ");
    ui_text_append_wide(&t, tmg_boost_text(g_tmg.team_boost_x10), 12);
    ui_text_append_ascii(&t, " to passing, control, crossing and tackling on the pitch.");
    tm_text_wrap(g_tm.line, px + 14.0f, y + 88.0f, 12.0f, TM_CHALK2, pw - 28.0f, 3, 16.0f);
    /* links: the XI by line */
    float lx = px + pw + 18.0f;
    tm_rect(lx, y, pw, 150.0f, TM_TURF2);
    tm_text(tm_w("LINKS ON THE PITCH"), lx + 14.0f, y + 12.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    const uint32_t tbl = g_tm.base + 0x63A76C;
    const int32_t *fe = (const int32_t *)(uintptr_t)tbl + g_tmg.formation * 11;
    static const char *const lines[3] = {"Defence", "Midfield", "Attack"};
    for (int32_t line = 0; line < 3; ++line) {
        int32_t sum = 0, n = 0;
        for (int32_t s = 1; s < 11; ++s) {
            int32_t g = fe[s] <= 3 ? 0 : fe[s] <= 8 ? 1 : 2;
            TmgRow *r = tmg_row(g_tmg.xi[s]);
            if (g == line && r) { sum += r->chem; ++n; }
        }
        int32_t v = n ? sum / n : 0;
        float ry = y + 34.0f + (float)line * 36.0f;
        tm_text(tm_w(lines[line]), lx + 14.0f, ry, 13.0f, TM_CHALK, TM_ALIGN_LEFT, 0, 0);
        uint16_t buf[8];
        ui_text_reset(&t, buf, 8);
        ui_text_append_i32(&t, (v + 5) / 10);
        tm_text(buf, lx + pw - 14.0f, ry, 13.0f, tmg_tone(v), TM_ALIGN_RIGHT, 0, 1);
        tm_rect(lx + 14.0f, ry + 20.0f, pw - 28.0f, 5.0f, TM_TURF);
        tm_rect(lx + 14.0f, ry + 20.0f, (pw - 28.0f) * (float)v / 1000.0f, 5.0f, tmg_tone(v));
    }
    /* who holds it back: the four lowest in the XI */
    y += 168.0f;
    float bw = w - 36.0f;
    tm_rect(px, y, bw, h - y - 16.0f, TM_TURF2);
    tm_text(tm_w("HOLDING THE XI BACK"), px + 14.0f, y + 12.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    int32_t used[11] = {0};
    for (int32_t k = 0; k < 4; ++k) {
        int32_t best = -1;
        for (int32_t s = 0; s < 11; ++s) {
            TmgRow *r = tmg_row(g_tmg.xi[s]);
            if (!r || used[s]) continue;
            if (best < 0 || r->chem < tmg_row(g_tmg.xi[best])->chem) best = s;
        }
        if (best < 0) break;
        used[best] = 1;
        TmgRow *r = tmg_row(g_tmg.xi[best]);
        float ry = y + 34.0f + (float)k * 34.0f;
        ui_text_reset(&t, g_tm.line, 160);
        ui_text_append_wide(&t, r->name, 40);
        ui_text_append_ascii(&t, "   ");
        ui_text_append_ascii(&t, tmg_reason(r->player_id));
        tm_text(g_tm.line, px + 14.0f, ry, 13.0f, TM_CHALK, TM_ALIGN_LEFT, bw - 90.0f, 0);
        uint16_t buf[8];
        ui_text_reset(&t, buf, 8);
        ui_text_append_i32(&t, (r->chem + 5) / 10);
        tm_text(buf, px + bw - 14.0f, ry, 13.0f, tmg_tone(r->chem), TM_ALIGN_RIGHT, 0, 1);
        tm_hit(px, ry - 6.0f, bw, 30.0f, TMG_HIT_PLAYER, r->player_id);
    }
}

static void tmg_draw_detail(float x) {
    float h = g_tm.height / g_tm.s;
    tm_rect(x, TM_TOP_H, TM_DETAIL_W, h - TM_TOP_H, TM_GROUND);
    tm_rect(x, TM_TOP_H, 1.0f, h - TM_TOP_H, TM_LINE);
    float px = x + 16.0f, pw = TM_DETAIL_W - 32.0f;
    TmgRow *r = tmg_row(g_tmg.sel_id);
    if (!r) {
        tm_text(tm_w("Pick a player."), px, TM_TOP_H + 24.0f, 13.0f, TM_CHALK2, TM_ALIGN_LEFT, pw, 0);
        return;
    }
    MarketUiText t;
    float y = TM_TOP_H + 14.0f;
    tm_badge(px, y, 52.0f, r->rating, 26.0f);
    tm_text(r->name, px + 64.0f, y + 6.0f, 17.0f, TM_CHALK, TM_ALIGN_LEFT, pw - 64.0f, 1);
    ui_text_reset(&t, g_tm.line, 160);
    ui_text_append_ascii(&t, tm_pos_label(r->position));
    ui_text_append_ascii(&t, r->slot >= 0 ? "  Starting XI" : "  Bench");
    tm_text(g_tm.line, px + 64.0f, y + 30.0f, 12.0f, TM_CHALK2, TM_ALIGN_LEFT, pw - 64.0f, 0);
    y += 64.0f;
    float fw = (pw - 12.0f) / 3.0f;
    uint16_t form[8], mins[8], chem[8];
    ui_text_reset(&t, form, 8);
    if (r->form_x10 > 0) tmg_append_x10(&t, r->form_x10); else ui_text_append_char(&t, '-');
    ui_text_reset(&t, mins, 8);
    ui_text_append_i32(&t, (r->apps_pm + 5) / 10);
    ui_text_append_char(&t, '%');
    ui_text_reset(&t, chem, 8);
    ui_text_append_i32(&t, (r->chem + 5) / 10);
    const char *labels[3] = {"FORM", "MINUTES", "CHEMISTRY"};
    const uint16_t *vals[3] = {form, mins, chem};
    uint32_t cols[3] = {tmg_rating_tone(r->form_x10), TM_CHALK, tmg_tone(r->chem)};
    for (int32_t i = 0; i < 3; ++i) {
        float fx = px + (float)i * (fw + 6.0f);
        tm_rect(fx, y, fw, 44.0f, TM_TURF);
        tm_text(tm_w(labels[i]), fx + 8.0f, y + 6.0f, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 0);
        tm_text(vals[i], fx + 8.0f, y + 21.0f, 15.0f, cols[i], TM_ALIGN_LEFT, fw - 12.0f, 1);
    }
    y += 56.0f;
    tm_text(tm_w("MATCH RATINGS, OLDEST FIRST"), px, y, 10.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 1);
    y += 16.0f;
    if (!r->hist_n) tm_text(tm_w("No match recorded yet."), px, y + 2.0f, 12.0f, TM_CHALK3, TM_ALIGN_LEFT, 0, 0);
    float cw = (pw - 9.0f * 3.0f) / 10.0f;
    for (int32_t k = 0; k < r->hist_n; ++k) {
        float cx = px + (float)k * (cw + 3.0f);
        int32_t v = r->hist[k];
        tm_rect(cx, y, cw, 20.0f, v ? tmg_rating_tone(v) : TM_TURF2);
        uint16_t buf[8];
        ui_text_reset(&t, buf, 8);
        if (v) tmg_append_x10(&t, v); else ui_text_append_char(&t, '-');
        tm_text(buf, cx + cw * 0.5f, y + 4.0f, 10.0f, v ? TM_AMBER_INK : TM_CHALK3, TM_ALIGN_CENTER, 0, 1);
    }
    y += 34.0f;
    ChemParts p;
    if (chem_parts(r->player_id, &p)) {
        const char *names[5] = {"Time at club", "How often", "How well", "Position fit", "Formation"};
        int32_t vals5[5] = {p.tenure, p.involvement, p.performance, p.fit, p.familiarity};
        for (int32_t i = 0; i < 5; ++i) {
            float ry = y + (float)i * 22.0f;
            tm_text(tm_w(names[i]), px, ry, 12.0f, TM_CHALK2, TM_ALIGN_LEFT, 100.0f, 0);
            tm_rect(px + 108.0f, ry + 5.0f, pw - 150.0f, 6.0f, TM_TURF2);
            tm_rect(px + 108.0f, ry + 5.0f, (pw - 150.0f) * (float)vals5[i] / 1000.0f, 6.0f, tmg_tone(vals5[i]));
            uint16_t buf[8];
            ui_text_reset(&t, buf, 8);
            ui_text_append_i32(&t, (vals5[i] + 5) / 10);
            tm_text(buf, px + pw, ry, 12.0f, TM_CHALK, TM_ALIGN_RIGHT, 0, 1);
        }
        y += 116.0f;
    }
    tm_rect(px, y, pw, 40.0f, TM_TURF2);
    tm_rect(px, y, 3.0f, 40.0f, TM_GOOD);
    tm_text(tm_w("Match stat boost"), px + 12.0f, y + 13.0f, 12.0f, TM_CHALK2, TM_ALIGN_LEFT, 0, 0);
    tm_text(tmg_boost_text(r->boost_x10), px + pw - 12.0f, y + 8.0f, 22.0f, TM_GOOD, TM_ALIGN_RIGHT, 0, 1);
    /* note and actions */
    float ay = h - 60.0f;
    if (g_tmg.note[0]) {
        uint32_t nc = g_tmg.note_kind == TM_LOG_GOOD ? TM_GOOD : g_tmg.note_kind == TM_LOG_BAD ? TM_BAD : TM_CHALK;
        tm_text_wrap(g_tmg.note, px, ay - 38.0f, 12.0f, nc, pw, 2, 15.0f);
    }
    int32_t swapping = g_tmg.swap_from >= 0;
    tm_button(px, ay + 8.0f, pw, 44.0f, tm_w(swapping ? "Cancel swap" : "Swap"), 1, g_tmg.editable, TMG_HIT_SWAP, r->player_id);
}

static void tmg_render(uint32_t base) {
    if (g_tmg.dirty) tmg_build(base);
    float main_x = TM_RAIL_W;
    float main_w = g_tm.dw - TM_RAIL_W - TM_DETAIL_W;
    tm_rect(0, 0, g_tm.dw, TM_DESIGN_H, TM_TURF);
    if (g_tmg.view == TMG_LINEUP) tmg_draw_lineup(base, main_x, main_w);
    else if (g_tmg.view == TMG_SQUAD) tmg_draw_squad(main_x, main_w);
    else tmg_draw_chem(main_x, main_w);
    tmg_draw_detail(main_x + main_w);
    tmg_draw_rail();
    tmg_draw_top(base);
}

static void tmg_activate(uint32_t base, const TmHit *hit) {
    switch (hit->kind) {
    case TMG_HIT_TAB: g_tmg.view = hit->value; g_tmg.scroll = 0.0f; break;
    case TMG_HIT_FORMATION: tmg_set_formation(base, hit->value); break;
    case TMG_HIT_PLAYER:
        if (g_tmg.swap_from >= 0) tmg_swap(base, g_tmg.swap_from, hit->value);
        g_tmg.sel_id = hit->value;
        break;
    case TMG_HIT_SWAP:
        if (g_tmg.swap_from >= 0) g_tmg.swap_from = -1;
        else { g_tmg.swap_from = hit->value; g_tmg.note[0] = 0; }
        break;
    case TMG_HIT_CLASSIC: {
        tmg_leave(base);
        g_tmg_stock_once = 1;
        CfeForwardMarketFn forward = (CfeForwardMarketFn)(base + 0x2984CD);
        forward(TMG_SCREEN_ID, 0, (void *)0, (void *)0, 1, 0);
        g_tmg.dirty = 1;
        break;
    }
    default: break;
    }
}
