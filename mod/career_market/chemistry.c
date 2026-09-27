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
 *   position fit 15% (1000 until the Team Management screen supplies the slot)
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
    chem_record(ids, overall, count, formation);
}
