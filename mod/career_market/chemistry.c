/*
 * Team chemistry and match ratings (v39). Included from native_bridge.c.
 *
 * After every user match (career_market_on_match_awards) each player of the user squad gets a match rating from the
 * game's own post-match score, STAT_PlayerGetRatingOverall(team, lineup index) 0x2B730C: 50 + 0..100 from his match
 * stats + 0..10 for the goal difference, 0 when he did not play. It maps to 3.0..10.0 (rating_x10 30..100).
 *
 * Game data (verified in the disassembly of STAT_PlayerRating / STAT_PlayerGetRatingOverall):
 *   tGame + team*0x1018 + 0x3900   u8   players with a stat entry
 *   tGame + team*0x1018 + 0x47C4   ptr  match-day TPlayerInfo array (stride 0xB0, u16 player id at +0)
 *   team = CMatchSetup side ^ tGame+0x9ED4 (the same index the goal counts at tGame+0x9EDC use)
 *   CMatchSetup::ms_tInfo + 0xF74 + side*4   CTeamManagement* (CMatchSetup::GetPreGameTeamManagement)
 *   CTeamManagement + 0x184                  u8 formation 0..11 (CTeamTactics::SetFormation)
 *
 * Per player (saved, block version 0xB7): matches at the club, a recent-appearance share and a form average (both
 * exponential, recent matches heavier), and the last 10 ratings. Chemistry 0..1000 and the match boost are computed
 * from them on demand, every part a continuous curve:
 *   tenure 25%       1 - e^(-matches/6), as a table
 *   involvement 25%  appearance share, topped up by good form: share + (1 - share) * perf * 0.6
 *   performance 25%  form over 5.5..8.5 (form drifts towards 5.5 while he does not play)
 *   position fit 15% the XI slot against his own position (chem_update_fit)
 *   familiarity 10%  matches in the current formation
 *   boost            1% + 14% * smoothstep(chemistry), in tenths of a percent (10..150)
 */

#define CHEM_SAVE_VERSION 0xB7
#define CHEM_PLAYERS 64
#define CHEM_HISTORY 10
#define CHEM_STAT_ROWS 32

typedef struct {
    int32_t player_key;          /* player id + 1, 0 = free */
    int32_t joined;              /* g_ext3.matches when he joined (negative = settled before tracking) */
    int32_t apps_pm;             /* recent appearance share, permille */
    int32_t form_x100;           /* recent average rating x100 (600 = 6.0) */
    int32_t fit_pm;              /* position fit, permille */
    uint8_t ratings[CHEM_HISTORY];   /* rating x10, 0 = did not play; ring, newest at head - 1 */
    uint8_t head, count;
} ChemPlayer;

typedef struct {
    int32_t matches;             /* user matches recorded */
    int32_t formation;           /* formation of the last match, -1 unknown */
    int32_t formation_matches;   /* matches in a row in it */
    int32_t last_team_rating_x10;
    int32_t reserved[12];
    ChemPlayer players[CHEM_PLAYERS];
} MarketExt3;

static MarketExt3 g_ext3 __attribute__((aligned(8)));
static void clear_ext3(void);

/* The game writes every block and reads a block only from saves of at least its version, so the block is written
 * only into saves that declare 0xB7 (the v39 libDLS18). With an older libDLS18 the career saves as before and the
 * chemistry simply starts again on the next load, instead of leaving an unreadable block in a 0xB6 save. */
static void chem_serialize(void *serializer, void (*fn)(void *, uint64_t *, int32_t)) {
    int32_t version = *(int32_t *)((uint8_t *)serializer + 0x18);
    uint8_t writing = *((uint8_t *)serializer + 0x1C);
    if (version < CHEM_SAVE_VERSION) {
        if (!writing) clear_ext3();
        return;
    }
    uint64_t *words = (uint64_t *)&g_ext3;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_ext3) / sizeof(uint64_t)); ++i) fn(serializer, &words[i], CHEM_SAVE_VERSION);
}

static void clear_ext3(void) {
    uint64_t *words = (uint64_t *)&g_ext3;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(g_ext3) / sizeof(uint64_t)); ++i) words[i] = 0;
    g_ext3.formation = -1;
}

static ChemPlayer *chem_find(int32_t player_id) {
    if (player_id < 0) return (ChemPlayer *)0;
    for (int32_t i = 0; i < CHEM_PLAYERS; ++i)
        if (g_ext3.players[i].player_key == player_id + 1) return &g_ext3.players[i];
    return (ChemPlayer *)0;
}

/* Keep one entry per user squad player: drop players who left, add new ones. The first sync of a career (or of a
 * save from before v39) treats the existing squad as settled; later arrivals start as new signings. */
static void chem_sync(void) {
    int32_t first = 1;
    for (int32_t i = 0; i < CHEM_PLAYERS; ++i) if (g_ext3.players[i].player_key) first = 0;
    for (int32_t i = 0; i < CHEM_PLAYERS; ++i) {
        ChemPlayer *c = &g_ext3.players[i];
        if (!c->player_key) continue;
        int32_t index = find_cached_player(c->player_key - 1);
        if (index < 0 || g_players[index].owner_id != USER_TEAM_ID) {
            uint8_t *bytes = (uint8_t *)c;
            for (uint32_t k = 0; k < sizeof(*c); ++k) bytes[k] = 0;
        }
    }
    for (int32_t p = 0; p < g_player_count; ++p) {
        MarketPlayer *player = &g_players[p];
        if (player->owner_id != USER_TEAM_ID || chem_find(player->player_id)) continue;
        ChemPlayer *slot = (ChemPlayer *)0;
        for (int32_t i = 0; i < CHEM_PLAYERS && !slot; ++i) if (!g_ext3.players[i].player_key) slot = &g_ext3.players[i];
        if (!slot) break;
        slot->player_key = player->player_id + 1;
        slot->fit_pm = 1000;
        if (first) {
            slot->joined = g_ext3.matches - 20;
            slot->apps_pm = 600;
            slot->form_x100 = 650;
        } else {
            slot->joined = g_ext3.matches;
            slot->apps_pm = 0;
            slot->form_x100 = 600;
        }
        g_market_dirty = 1;
    }
}

static int32_t chem_rating_x10(int32_t overall) {
    if (overall <= 0) return 0;
    int32_t r = 30 + (overall - 50) * 70 / 110;
    return r < 30 ? 30 : r > 100 ? 100 : r;
}

/* One match: ids[] and overall[] for the players who were in the match-day squad (overall 0 = unused sub). */
static void chem_record(const int32_t *ids, const int32_t *overall, int32_t count, int32_t formation) {
    chem_sync();
    if (formation >= 0 && formation == g_ext3.formation) {
        if (g_ext3.formation_matches < 60) ++g_ext3.formation_matches;
    } else {
        g_ext3.formation = formation;
        g_ext3.formation_matches = formation >= 0 ? 1 : 0;
    }
    int32_t sum = 0, played_n = 0;
    for (int32_t i = 0; i < CHEM_PLAYERS; ++i) {
        ChemPlayer *c = &g_ext3.players[i];
        if (!c->player_key) continue;
        int32_t rating = 0;
        for (int32_t k = 0; k < count; ++k) if (ids[k] == c->player_key - 1) rating = chem_rating_x10(overall[k]);
        int32_t target = rating ? 1000 : 0;
        c->apps_pm += (target - c->apps_pm) / 6;
        if (rating) {
            int32_t played_before = 0;
            for (int32_t k = 0; k < c->count; ++k) if (c->ratings[k]) played_before = 1;
            if (!played_before && g_ext3.matches <= c->joined) c->form_x100 = rating * 10;
            else c->form_x100 += (rating * 10 - c->form_x100) / 4;
            sum += rating;
            ++played_n;
        } else {
            /* out of the side: sharpness fades, form drifts an eighth of the way towards 5.5 each match */
            c->form_x100 += (550 - c->form_x100) / 8;
        }
        c->ratings[c->head] = (uint8_t)rating;
        c->head = (uint8_t)((c->head + 1) % CHEM_HISTORY);
        if (c->count < CHEM_HISTORY) ++c->count;
    }
    g_ext3.last_team_rating_x10 = played_n ? sum / played_n : 0;
    ++g_ext3.matches;
    g_market_dirty = 1;
}

typedef struct { int32_t tenure, involvement, performance, fit, familiarity, chemistry, matches; } ChemParts;

static int32_t chem_familiarity_pm(void) {
    static const int32_t xs[4] = {0, 3, 6, 10}, ys[4] = {300, 650, 850, 1000};
    return g_ext3.formation >= 0 ? lerp_pts(g_ext3.formation_matches, xs, ys, 4) : 700;
}

/* 0 when the player is not tracked. */
static int32_t chem_parts(int32_t player_id, ChemParts *out) {
    ChemPlayer *c = chem_find(player_id);
    if (!c) return 0;
    static const int32_t tx[7] = {0, 3, 6, 10, 15, 20, 30}, ty[7] = {0, 393, 632, 811, 918, 964, 993};
    ChemParts p;
    p.matches = g_ext3.matches - c->joined;
    if (p.matches < 0) p.matches = 0;
    p.tenure = lerp_pts(p.matches, tx, ty, 7);
    p.performance = ramp_pm(c->form_x100, 550, 850);
    int32_t apps = c->apps_pm < 0 ? 0 : c->apps_pm > 1000 ? 1000 : c->apps_pm;
    p.involvement = apps + (1000 - apps) * p.performance / 1000 * 6 / 10;
    p.fit = c->fit_pm < 0 ? 0 : c->fit_pm > 1000 ? 1000 : c->fit_pm;
    p.familiarity = chem_familiarity_pm();
    p.chemistry = (250 * p.tenure + 250 * p.involvement + 250 * p.performance + 150 * p.fit + 100 * p.familiarity) / 1000;
    if (out) *out = p;
    return 1;
}

/* Match stat boost in tenths of a percent: 10 (1%) at chemistry 0 to 150 (15%) at 1000, smoothstep between. */
static int32_t chem_boost_x10(int32_t chemistry) {
    int32_t c = chemistry < 0 ? 0 : chemistry > 1000 ? 1000 : chemistry;
    int32_t s = c * c / 1000 * (3000 - 2 * c) / 1000;     /* permille */
    return 10 + 140 * s / 1000;
}

static int32_t chem_team_form_x10(void) { return g_ext3.last_team_rating_x10; }

static int32_t chem_form_x10(int32_t player_id) {
    ChemPlayer *c = chem_find(player_id);
    return c ? (c->form_x100 + 5) / 10 : 0;
}

/* Ratings oldest first into out[]; returns how many. */
static int32_t chem_history(int32_t player_id, uint8_t *out) {
    ChemPlayer *c = chem_find(player_id);
    if (!c) return 0;
    int32_t n = c->count;
    for (int32_t k = 0; k < n; ++k) out[k] = c->ratings[(c->head + CHEM_HISTORY - n + k) % CHEM_HISTORY];
    return n;
}

/* A user signing of this season in the transfer history (the market keeps the last 128 deals). */
static int32_t chem_signed_this_season(int32_t player_id) {
    for (int32_t i = 0; i < g_market.history_count && i < HISTORY_CAPACITY; ++i) {
        const TransferRecord *r = &g_market.history[(g_market.history_head + HISTORY_CAPACITY - 1 - i) % HISTORY_CAPACITY];
        if (r->player_id == player_id && r->buyer_id == USER_TEAM_ID) return r->season == g_market.season;
    }
    return 0;
}

/* First start of chemistry in a career that already has matches: seed from the game's own league stats
 * (CTournament at CSeason+0x6AC: u16 count at +0x30, 10-byte TTournamentPlayerStat rows at +0x34, u16 player id at
 * +0, appearances at +4, the counter CTournament::UpdatePlayerStats_UserMatch raises for everyone who played).
 * Appearances give the share of minutes against the squad's most used player; this season's signings start from
 * their appearances since they arrived, everyone else as settled. The game keeps no per-match ratings, so form
 * starts neutral (6.5) and moves from the next match on. */
static void chem_seed(uint32_t base) {
    if (!base) return;
    const uint8_t *league = *(const uint8_t *const *)((uint8_t *)(uintptr_t)(base + PROFILE_INSTANCE + 0x14) + 0x6AC);
    if (!league) return;
    int32_t rows = *(const uint16_t *)(league + 0x30);
    const uint8_t *stats = *(const uint8_t *const *)(league + 0x34);
    if (rows <= 0 || rows > 4096 || !stats) return;
    int32_t apps[CHEM_PLAYERS], team_max = 0;
    for (int32_t i = 0; i < CHEM_PLAYERS; ++i) {
        apps[i] = 0;
        ChemPlayer *c = &g_ext3.players[i];
        if (!c->player_key) continue;
        for (int32_t r = 0; r < rows; ++r) {
            if (*(const uint16_t *)(stats + r * 10) != c->player_key - 1) continue;
            apps[i] = stats[r * 10 + 4];
            break;
        }
        if (apps[i] > team_max) team_max = apps[i];
    }
    if (team_max <= 0) return;
    for (int32_t i = 0; i < CHEM_PLAYERS; ++i) {
        ChemPlayer *c = &g_ext3.players[i];
        if (!c->player_key) continue;
        c->apps_pm = apps[i] * 1000 / team_max;
        c->joined = chem_signed_this_season(c->player_key - 1) ? g_ext3.matches - apps[i]
                                                                  : g_ext3.matches - team_max - 20;
        c->form_x100 = 650;
    }
    g_market_dirty = 1;
}

/* Sync with the squad; on the very first start of a career's chemistry, seed it from the season so far. */
static void chem_prepare(uint32_t base) {
    int32_t empty = 1;
    for (int32_t i = 0; i < CHEM_PLAYERS; ++i) if (g_ext3.players[i].player_key) empty = 0;
    chem_sync();
    if (empty && g_ext3.matches == 0) chem_seed(base);
}

/*
 * Position fit for the starting XI. Career CTeamManagement = CSeason+0x6E0 (CSeason::GetTeamManagement); its
 * lineup ids at +0x140+2 (u16 x 32, index 0..10 = the formation slots, CTeamLineup::GetID), the CTeam at +0x194;
 * the menu formation at CTeam+0x12F (0..11, what CTeamLineup::SelectStartingEleven reads). Slot position =
 * FS_iFormationPlayerPos[formation][slot] 0x63A55C (int32 x 11 per formation, EPlayerPosition). The player's
 * own position = his club link entry (CDataBase::GetTeamLink(team) 0x20934C): count at +4, u32 ids at +0x88,
 * int8 position at +8+4i+1. CTeamLineup::PlayerPositionSuitability 0x2F0A2C gives the distance: 0 natural,
 * 1..10 further out of position, 50 a keeper outfield or the reverse; fit follows a smooth curve over it.
 * A player the 32-entry link table does not hold (squad-64 overflow) is scored by position group instead.
 */
typedef const uint8_t *(*ChemTeamLinkFn)(int32_t);
typedef int32_t (*ChemSuitabilityFn)(void *, int32_t, int32_t);

static int32_t chem_fit_from_distance(int32_t d) {
    static const int32_t xs[7] = {0, 1, 2, 3, 5, 10, 50}, ys[7] = {1000, 900, 800, 700, 520, 250, 0};
    return lerp_pts(d < 0 ? 0 : d, xs, ys, 7);
}

/* FS_iFormationFEPlayerPos groups: 0 GK, 1-3 defence, 4-8 midfield, 9-10 attack */
static int32_t chem_fit_by_group(int32_t player_group, int32_t fe_pos) {
    int32_t slot_group = fe_pos <= 0 ? 0 : fe_pos <= 3 ? 1 : fe_pos <= 8 ? 2 : 3;
    if ((player_group == 0) != (slot_group == 0)) return 0;
    int32_t gap = player_group > slot_group ? player_group - slot_group : slot_group - player_group;
    return gap == 0 ? 850 : gap == 1 ? 450 : 150;
}

static void chem_update_fit(uint32_t base) {
    if (!base) return;
    const uint8_t *tm = (const uint8_t *)(uintptr_t)(base + PROFILE_INSTANCE + 0x14 + 0x6E0);
    const uint8_t *cteam = *(const uint8_t *const *)(tm + 0x194);
    if (!cteam) return;
    int32_t formation = cteam[0x12F];
    if (formation < 0 || formation > 11) return;
    const int32_t *slot_pos = (const int32_t *)(uintptr_t)(base + 0x63A55C) + formation * 11;
    const int32_t *slot_fe = (const int32_t *)(uintptr_t)(base + 0x63A76C) + formation * 11;
    const uint8_t *link = ((ChemTeamLinkFn)(base + 0x20934D))(USER_TEAM_ID);
    int32_t link_count = link ? *(const int32_t *)(link + LINK_PLAYER_COUNT) : 0;
    if (link_count < 0 || link_count > 32) link_count = 0;
    ChemSuitabilityFn suitability = (ChemSuitabilityFn)(base + 0x2F0A2D);
    for (int32_t slot = 0; slot < 11; ++slot) {
        int32_t id = *(const uint16_t *)(tm + 0x140 + 2 + slot * 2);
        ChemPlayer *c = chem_find(id);
        if (!c) continue;
        int32_t fit = -1;
        for (int32_t i = 0; i < link_count; ++i) {
            if (*(const int32_t *)(link + LINK_PLAYER_IDS + 4 * i) != id) continue;
            int32_t own = (int8_t)link[LINK_TEAM_DATA + 4 * i + 1];
            int32_t want = slot_pos[slot];
            if (own >= 0 && own <= 0x16 && want >= 0 && want <= 0x16) fit = chem_fit_from_distance(suitability((void *)0, own, want));
            break;
        }
        if (fit < 0) {
            int32_t index = find_cached_player(id);
            if (index >= 0 && g_players[index].position >= 0 && g_players[index].position <= 3)
                fit = chem_fit_by_group(g_players[index].position, slot_fe[slot]);
        }
        if (fit >= 0 && fit != c->fit_pm) { c->fit_pm = fit; g_market_dirty = 1; }
    }
}

typedef int32_t (*ChemRatingFn)(int32_t, int32_t);

/* Device side: read the finished match from the game and record it. team = STAT team index of the user. */
static void chem_on_match(uint32_t base, int32_t team, int32_t side) {
    if (!base || team < 0 || team > 1) return;
    uint8_t *tgame = (uint8_t *)(uintptr_t)(base + TGAME_BASE) + team * 0x1018;
    int32_t count = *(uint8_t *)(tgame + 0x3900);
    const uint8_t *lineup = *(const uint8_t *const *)(tgame + 0x47C4);
    if (count <= 0 || !lineup) return;
    chem_prepare(base);
    if (count > CHEM_STAT_ROWS) count = CHEM_STAT_ROWS;
    int32_t ids[CHEM_STAT_ROWS], overall[CHEM_STAT_ROWS];
    ChemRatingFn rating = (ChemRatingFn)(base + 0x2B730D);
    for (int32_t i = 0; i < count; ++i) {
        ids[i] = *(const uint16_t *)(lineup + i * 0xB0);
        overall[i] = i < 18 ? rating(team, i) : 0;
    }
    int32_t formation = -1;
    if (side == 0 || side == 1) {
        const uint8_t *tm = *(const uint8_t *const *)((uint8_t *)(uintptr_t)(base + MATCH_SETUP_INFO) + 0xF74 + side * 4);
        if (tm) {
            formation = *(tm + 0x184);
            if (formation > 11) formation = -1;
        }
    }
    chem_update_fit(base);
    chem_record(ids, overall, count, formation);
}

/*
 * Match boost. CPlayer::SetupPlayer(team, index, TPlayerInfo*) 0x2DAC02 copies a player's stats into the match
 * object as bytes 0..99; the hook "chem_setup_player" sits on its last instruction before the tail call
 * (0x2DAD34 mov.w r1,#0x800, after every stat byte is stored; r0 = the CPlayer). SetupPlayer rewrites the bytes
 * from the career data every time it runs, so the boost never stacks, also on a resumed match.
 *   CPlayer+0x70 u16 player id;  +0x127 ball control, +0x129 passing, +0x128 crossing, +0x123 tackling,
 *   +0x12B shot stopping, +0x12C handling (GK)
 * Physical stats and the energy-scaled speed and acceleration (+0x125/+0x126) are left alone. Only players of the
 * user squad, and never in an online (DLO) match: CMatchSetup::ms_tInfo+0xFB0 != -1.
 */
static const uint16_t k_chem_boost_offsets[6] = {0x127, 0x129, 0x128, 0x123, 0x12B, 0x12C};

static int32_t chem_boost_player(uint8_t *cplayer, int32_t player_id) {
    if (!chem_find(player_id)) return 0;
    int32_t index = find_cached_player(player_id);
    if (index < 0 || g_players[index].owner_id != USER_TEAM_ID) return 0;
    ChemParts parts;
    if (!chem_parts(player_id, &parts)) return 0;
    int32_t boost = chem_boost_x10(parts.chemistry);             /* tenths of a percent */
    for (int32_t k = 0; k < 6; ++k) {
        int32_t v = cplayer[k_chem_boost_offsets[k]];
        if (v <= 0 || v > 99) continue;
        int32_t boosted = (v * (1000 + boost) + 500) / 1000;
        cplayer[k_chem_boost_offsets[k]] = (uint8_t)(boosted > 99 ? 99 : boosted);
    }
    return boost;
}

static uint32_t chem_setup_player_hook(ModCtx *ctx, uint32_t base) {
    if (!base || !state_is_valid()) return 0;
    if (*(int32_t *)((uint8_t *)(uintptr_t)(base + MATCH_SETUP_INFO) + 0xFB0) != -1) return 0;   /* online */
    uint8_t *cplayer = (uint8_t *)(uintptr_t)ctx->r[0];
    if (!cplayer) return 0;
    chem_boost_player(cplayer, *(const uint16_t *)(cplayer + 0x70));
    return 0;                                   /* run mov.w r1,#0x800 and the stock tail call */
}
