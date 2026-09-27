"""Build dls18_dataset.json + distribution summary from the stock DLS18 v5.064 data files.

Inputs (read-only): unpacked/db_decoded/{players,teams,teamplayerlinks_0}.dat.decoded.bin,
unpacked/apk/assets/data/text/ftsteamnames.xlc
Outputs: data/dls18_dataset.json, data/dataset_summary.json (numbers used in NOTES_dataset.md)
"""
from __future__ import annotations

import datetime
import json
import os
import random
import statistics
import struct
from collections import Counter, defaultdict

import dls_value_model as M

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
DB = os.path.join(ROOT, "unpacked", "db_decoded")
TEXT = os.path.join(ROOT, "unpacked", "apk", "assets", "data", "text")
USER_TEAM_ID = 0x102
AGE_REFERENCE = datetime.date(2018, 7, 1)   # the game itself never computes age (no age in TPlayerInfo)

DIVISIONS = ["Elite Division", "Junior Elite Division", "Division 1", "Division 2", "Division 3",
             "Academy Division"]  # tournament ids 0..5 (MC_tSeasonInfo tree = [0,1,2,3,4,5])

LEAGUE_CODE_NOTES = {
    -1: "unassigned (TTeamROM+4 = -1 -> CTeam+5 = 0xff): excluded from market search and DL pool",
    0: "England top flight", 1: "England 2nd tier", 2: "France", 3: "Italy", 5: "Spain", 7: "Scotland",
    9: "International (Europe)", 10: "International (Asia/Oceania)", 11: "International (S. America)",
    12: "International (N/C America)", 13: "International (Africa)",
    17: "Miscellaneous (Dream FC user team, all-star/legend XIs)",
    18: "relegation league (England)", 19: "relegation league (France)", 20: "relegation league (Italy)",
    22: "relegation league (Spain)", 23: "relegation league (Scotland)", 24: "Netherlands",
    25: "relegation league (Netherlands)", 27: "Portugal", 28: "relegation league (Portugal)",
}


def wstr(b: bytes) -> str:
    s = b.decode("utf-16le", "replace")
    return s.split("\0", 1)[0]


def load_xlc(path, lang_block=0):
    D = open(path, "rb").read()
    nl, nk = struct.unpack_from("<II", D, 0x0C)
    # header: 'FTTL', 0, 0, u32 14, u32 n_keys, u32 file_size, u32 values_offset, u32 6, then 13 x (block_size, lang_id)
    langs = [struct.unpack_from("<II", D, 0x20 + 8 * i) for i in range(nl - 1)]  # (block_size, lang_id); block 0 = lang 0 (English)
    p = 0x20 + 8 * (nl - 1)

    def rd(p):
        e = p
        while D[e:e + 2] != b"\0\0":
            e += 2
        return D[p:e].decode("utf-16le", "replace"), e + 2
    keys = []
    for _ in range(nk):
        s, p = rd(p)
        keys.append(s)
    for _ in range(lang_block):
        for _ in range(nk):
            _, p = rd(p)
    vals = []
    for _ in range(nk):
        s, p = rd(p)
        vals.append(s)
    return dict(zip(keys, vals)), langs


def team_category(lg):
    if lg == -1:
        return "unassigned"
    if 9 <= lg <= 13:
        return "international"
    if lg == 17:
        return "miscellaneous"
    if 18 <= lg <= 28 and lg in (18, 19, 20, 21, 22, 23, 25, 26, 28):
        return "relegation_league"
    return "club_league"


def is_valid_search_team(tid, lg):
    # CTransfers::IsValidSearchTeam 0x210884 (user team excluded; mod adds the user team back separately)
    if tid == USER_TEAM_ID or tid in (0x15C, 0x1F8):
        return False
    return (lg & 0xFF) != 0xFF


def cseason_is_valid_team(tid, lg, classic):
    # CSeason::IsValidTeam 0x369ab6
    if classic or 9 <= lg <= 13 or lg == 17:
        return False
    if lg in (18, 19, 20, 21, 22, 23, 25, 26, 28):
        return False
    return (lg & 0xFF) != 0xFF


def main():
    # ---------------- players ----------------
    pd = open(os.path.join(DB, "players.dat.decoded.bin"), "rb").read()
    n_players = struct.unpack_from("<I", pd, 8)[0]
    players = {}
    infos = {}
    for i in range(n_players):
        rom = pd[12 + i * 0xB4:12 + (i + 1) * 0xB4]
        info = M.player_info_from_rom(rom)
        pid = M.u16(info, 0)
        det = struct.unpack_from("<b", rom, 0x7C)[0]
        rating = M.pu_get_player_rating(info)
        value = M.market_value(info)
        day, month, year = struct.unpack_from("<III", rom, 0xA8)
        try:
            if year <= 1900:
                raise ValueError("placeholder birth date")
            bd = datetime.date(year, month, day)
            age = AGE_REFERENCE.year - bd.year - ((AGE_REFERENCE.month, AGE_REFERENCE.day) < (bd.month, bd.day))
            bds = bd.isoformat()
        except ValueError:
            bds, age = None, None
        stats = {n: M.u16(info, o) / 10 for n, o in zip(M.RATING_STAT_NAMES, M.RATING_STAT_INFO_OFFSETS)}
        players[pid] = {
            "id": pid,
            "first_name": wstr(rom[0x02:0x24]), "last_name": wstr(rom[0x24:0x4C]),
            "common_name": wstr(rom[0x4C:0x6E]) or None,
            "gen_pos": M.GEN_POS[det], "det_pos": det, "fe_rating_row": M.FE_POS[det],
            "rating": rating, "value": value,
            "nationality_code": struct.unpack_from("<h", rom, 0x6E)[0],
            "height_cm": rom[0x78], "weight_kg": rom[0x7A],
            "birth_date": bds, "age_2018_07_01": age,
            "stats": stats,
            "teams": [],
        }
        infos[pid] = info

    # ---------------- teams ----------------
    names, langs = load_xlc(os.path.join(TEXT, "ftsteamnames.xlc"), lang_block=0)
    td = open(os.path.join(DB, "teams.dat.decoded.bin"), "rb").read()
    n_teams = struct.unpack_from("<I", td, 8)[0]
    teams = {}
    team_order = []
    for i in range(n_teams):
        r = td[12 + i * 4092:12 + (i + 1) * 4092]
        tid, lg = struct.unpack_from("<Ii", r, 0)
        classic = r[0x139] != 0
        teams[tid] = {
            "team_id": tid, "db_index": i,
            "name": names.get(f"TXT_TEAMNAMELONG_{tid}"),
            "name_med": names.get(f"TXT_TEAMNAMEMED_{tid}"),
            "name_short": names.get(f"TXT_TEAMNAMESHORT_{tid}"),
            "league_code": lg, "league_note": LEAGUE_CODE_NOTES.get(lg),
            "category": team_category(lg), "classic": classic,
            "rom_ratings_cde": [r[0xC], r[0xD], r[0xE]],
            "market_search_team": is_valid_search_team(tid, lg),
            "native_bridge_market_club": is_valid_search_team(tid, lg) or tid == USER_TEAM_ID,
            "dream_league_pool": cseason_is_valid_team(tid, lg, classic),
            "is_user_team": tid == USER_TEAM_ID,
        }
        team_order.append(tid)

    # ---------------- links ----------------
    ld = open(os.path.join(DB, "teamplayerlinks_0.dat.decoded.bin"), "rb").read()
    ver, cnt, _, extra = struct.unpack_from("<IIiI", ld, 0)
    p = 0x10 + extra * 8
    for _ in range(cnt - 1):     # PopulateDefaultLinksArray 0x20a984 reads count-1 links
        tid, n = struct.unpack_from("<Ii", ld, p)
        p += 8
        ids, tsd = [], []
        if n > 0:
            tsd = [list(ld[p + 4 * j:p + 4 * j + 4]) for j in range(n)]
            ids = list(struct.unpack_from(f"<{n}I", ld, p + 0x80))
            p += 0x100
        t = teams[tid]
        t["roster"] = ids
        t["team_specific_data"] = tsd   # 4 bytes/player: byte0 -> TPlayerInfo+0x83, byte1 -> +0x82
        for pid in ids:
            players[pid]["teams"].append(tid)

    # ---------------- team derived values ----------------
    for tid, t in teams.items():
        ids = t.get("roster", [])
        rs = [players[pid]["rating"] for pid in ids]
        t["squad_size"] = len(ids)
        t["team_value_total"] = M.team_value_total([infos[pid] for pid in ids])
        # CDataBase::CalculateTeamRating 0x20b71c: top min(n,18) by rating (stable desc), floor(sum/count)
        top = sorted(range(len(rs)), key=lambda k: (-rs[k], k))[:18]
        t["team_rating"] = (sum(rs[k] for k in top) // len(top)) if top else 0
        grp = Counter(players[pid]["gen_pos"] for pid in ids)
        t["gen_pos_counts"] = [grp.get(g, 0) for g in range(4)]

    # ---------------- Dream League pyramid (CSeason::InitGeneratedTournamentInfo 0x369af4) -----------
    pool = [tid for tid in team_order if teams[tid]["dream_league_pool"]]
    # MCU_InsertionSortTeamRating 0x3646b8: stable, descending by CDataBase::GetTeamRating (CTeam+8)
    pool_sorted = sorted(pool, key=lambda tid: -teams[tid]["team_rating"])
    for rank, tid in enumerate(pool_sorted):
        teams[tid]["dream_league_pool_rank"] = rank
    rank_of = {tid: k for k, tid in enumerate(pool_sorted)}
    top24, rest = pool_sorted[:24], pool_sorted[24:]
    sims = 20000
    rng = random.Random(18)
    div_hits = defaultdict(lambda: [0] * 6)
    pooled = defaultdict(list)      # division -> list of team ids per sim (sampled for stats)
    for s in range(sims):
        chosen = top24 + rng.sample(rest, 72)
        chosen = sorted(chosen, key=lambda tid: (-teams[tid]["team_rating"], rank_of[tid]))
        for d in range(6):
            members = chosen[16 * d:16 * d + 16]
            if d == 5:
                members = members[:]
                members[rng.randrange(16)] = USER_TEAM_ID   # TTournamentGeneratedInfo::Init: user replaces a random slot
            for tid in members:
                div_hits[tid][d] += 1
            if s < 2000:
                pooled[d].extend(members)
    for tid, t in teams.items():
        if tid in div_hits:
            h = div_hits[tid]
            t["dream_league_division_probability"] = {DIVISIONS[d]: round(h[d] / sims, 4) for d in range(6) if h[d]}
            t["dream_league_in_career_probability"] = round(sum(h) / sims, 4)
            t["dream_league_likely_division"] = DIVISIONS[max(range(6), key=lambda d: h[d])]
        else:
            t["dream_league_division_probability"] = {}
            t["dream_league_in_career_probability"] = 0.0
            t["dream_league_likely_division"] = None

    out = {
        "meta": {
            "game": "Dream League Soccer 2018 v5.064 armeabi-v7a",
            "sources": ["unpacked/db_decoded/players.dat.decoded.bin", "unpacked/db_decoded/teams.dat.decoded.bin",
                        "unpacked/db_decoded/teamplayerlinks_0.dat.decoded.bin",
                        "unpacked/apk/assets/data/text/ftsteamnames.xlc (first language block)"],
            "rating": "PU_GetPlayerRating 0x2b35d0 (Unicorn-verified for all players)",
            "value": "CTransfers::GetPlayerValue(info,-1,-1,false,true) 0x212b3c with bundled dls_config.dat PlayerValues",
            "team_value_total": "CDataBase::GetTeamValueTotal 0x20c3a0 (sum of player values over stock link)",
            "team_rating": "CDataBase::CalculateTeamRating 0x20b71c (floor mean of top-18 player ratings)",
            "gen_pos": "0=GK 1=DEF 2=MID 3=ATT (TPlayerInfo+0x7f)",
            "age_note": "age_2018_07_01 is derived from TPlayerROM birth date (+0xa8/+0xac/+0xb0); the game never uses age",
            "dream_league_note": ("New-career pyramid = CSeason::InitGeneratedTournamentInfo: pool of CSeason::IsValidTeam teams "
                                  "sorted by team rating desc; top 24 always in, 72 random from the rest; 96 re-sorted; "
                                  "tournament d (0=Elite..5=Academy) gets ranks 16d..16d+15 (shuffled); the user team "
                                  "replaces a random slot in tournament 5. Probabilities from a 20000-run Monte Carlo "
                                  "(python RNG, not the game RNG). Division names from ftslang CS_DREAMTEAMLEAGUE_* "
                                  "(mapping of names to tournament ids inferred by rating order)."),
            "divisions": DIVISIONS,
            "user_team_id": USER_TEAM_ID,
            "value_config": {str(k): v for k, v in M.CONFIG_BUNDLED.items()},
            "xlc_languages": langs,
        },
        "teams": [teams[t] for t in team_order],
        "players": [players[p] for p in sorted(players)],
    }
    with open(os.path.join(HERE, "dls18_dataset.json"), "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)

    # ---------------- summary ----------------
    def mmm(xs):
        xs = sorted(xs)
        if not xs:
            return None
        return {"n": len(xs), "min": xs[0], "p25": xs[len(xs) // 4], "median": statistics.median(xs),
                "p75": xs[(3 * len(xs)) // 4], "max": xs[-1], "mean": round(sum(xs) / len(xs), 1)}
    summ = {}
    summ["counts"] = {"teams": len(teams), "players": len(players),
                      "players_on_rosters": sum(1 for p in players.values() if p["teams"]),
                      "players_on_multiple_rosters": sum(1 for p in players.values() if len(p["teams"]) > 1),
                      "players_without_team": sum(1 for p in players.values() if not p["teams"]),
                      "market_search_teams": sum(t["market_search_team"] for t in teams.values()),
                      "dream_league_pool": len(pool)}
    summ["by_category"] = {}
    for cat in sorted({t["category"] for t in teams.values()}):
        ts = [t for t in teams.values() if t["category"] == cat]
        pr = [players[p]["rating"] for t in ts for p in t["roster"]]
        pv = [players[p]["value"] for t in ts for p in t["roster"]]
        summ["by_category"][cat] = {"teams": len(ts), "squad_size": mmm([t["squad_size"] for t in ts]),
                                    "team_rating": mmm([t["team_rating"] for t in ts]),
                                    "player_rating": mmm(pr), "player_value": mmm(pv),
                                    "team_value_total": mmm([t["team_value_total"] for t in ts])}
    summ["by_league_code"] = {}
    for lg in sorted({t["league_code"] for t in teams.values()}):
        ts = [t for t in teams.values() if t["league_code"] == lg]
        summ["by_league_code"][str(lg)] = {"note": LEAGUE_CODE_NOTES.get(lg), "teams": len(ts),
                                           "team_rating": mmm([t["team_rating"] for t in ts]),
                                           "team_value_total": mmm([t["team_value_total"] for t in ts])}
    summ["dream_league_divisions_montecarlo_2000_pooled"] = {}
    for d in range(6):
        tids = pooled[d]
        pr = [players[p]["rating"] for t in tids for p in teams[t]["roster"]]
        pv = [players[p]["value"] for t in tids for p in teams[t]["roster"]]
        summ["dream_league_divisions_montecarlo_2000_pooled"][DIVISIONS[d]] = {
            "team_slots": len(tids), "distinct_teams": len(set(tids)),
            "squad_size": mmm([teams[t]["squad_size"] for t in tids]),
            "team_rating": mmm([teams[t]["team_rating"] for t in tids]),
            "team_value_total": mmm([teams[t]["team_value_total"] for t in tids]),
            "player_rating": mmm(pr), "player_value": mmm(pv)}
    summ["dream_league_fixed"] = {
        "always_elite": [teams[t]["name"] for t in pool_sorted[:16]],
        "always_junior_elite_or_elite_ranks_16_23": [teams[t]["name"] for t in pool_sorted[16:24]],
        "pool_rank_24_plus_selection_probability": round(72 / len(rest), 4) if rest else None,
    }
    rostered = [p for p in players.values() if p["teams"]]
    summ["players_all"] = {"rating": mmm([p["rating"] for p in players.values()]),
                           "value": mmm([p["value"] for p in players.values()]),
                           "age": mmm([p["age_2018_07_01"] for p in players.values() if p["age_2018_07_01"] is not None])}
    summ["players_rostered"] = {"rating": mmm([p["rating"] for p in rostered]),
                                "value": mmm([p["value"] for p in rostered]),
                                "age": mmm([p["age_2018_07_01"] for p in rostered if p["age_2018_07_01"] is not None])}
    summ["by_gen_pos"] = {g: {"players": sum(1 for p in players.values() if p["gen_pos"] == g),
                              "rating": mmm([p["rating"] for p in players.values() if p["gen_pos"] == g]),
                              "value": mmm([p["value"] for p in players.values() if p["gen_pos"] == g])}
                          for g in range(4)}
    summ["age_histogram_all"] = dict(sorted(Counter(p["age_2018_07_01"] for p in players.values()).items(),
                                            key=lambda kv: (kv[0] is None, kv[0] or 0)))
    summ["rating_histogram_all"] = dict(sorted(Counter(p["rating"] for p in players.values()).items()))
    summ["user_team"] = {k: teams[USER_TEAM_ID][k] for k in ("name", "squad_size", "team_rating", "team_value_total")}
    with open(os.path.join(HERE, "dataset_summary.json"), "w", encoding="utf-8") as f:
        json.dump(summ, f, ensure_ascii=False, indent=1)
    print(json.dumps(summ["counts"]), "user", summ["user_team"])


if __name__ == "__main__":
    main()
