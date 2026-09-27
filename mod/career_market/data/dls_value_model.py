"""Python re-implementation of DLS18 v5.064 (armeabi-v7a) player rating / value code.

Reverse-engineered from unpacked/apk/lib/armeabi-v7a/libDLS18.so (ELF VAs below).
Run `python dls_value_model.py` for the self-test (pure-Python checks, plus a Unicorn
cross-check against the native functions when unicorn + the stock lib are available).

Native functions covered
------------------------
* PU_GetPlayerFEPos            0x2b3388  det. position (0..22) -> FE rating row (table @0x63b1c0)
* PU_GetGeneralPosFromPos      0x2b3930  det. position -> general group 0..3 (table @0x63b280)
* PU_GetPlayerRating           0x2b35d0  OVR
* CTransfers::GetPlayerValue   0x212b3c  (TPlayerInfo*, int genPos, int rating, bool bRandom, bool bApplySecretDiscount)
* CDataBase::GetTeamValueTotal 0x20c3a0  sum of GetPlayerValue(info,-1,-1,false,true) over the team link
* XMATH_RoundToNearest         0x384530
"""
from __future__ import annotations

import math
import struct

# --------------------------------------------------------------------------------------------
# Tables copied from .rodata
# --------------------------------------------------------------------------------------------
# PU_GetPlayerFEPos table @0x63b1c0 (23 entries, index = detailed position TPlayerInfo+0x80)
FE_POS = [0, 1, 2, 1, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 6, 6, 8, 7, 6, 10, 9, 9, 10]
# PU_GetGeneralPosFromPos table @0x63b280 (0=GK 1=DEF 2=MID 3=ATT); stored at TPlayerInfo+0x7f
GEN_POS = [0, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3]

# Stat order used by PU_GetPlayerRating (it builds a pointer list in this order):
#   idx: 0 strength(+0x8e) 1 stamina(+0x8c) 2 speed(+0x8a) 3 acceleration(+0x88) 4 ball control(+0x92)
#        5 passing(+0x98) 6 crossing(+0x96) 7 shooting(+0x94) 8 heading(+0x9a) 9 tackling(+0x90)
#        10 GK shot stopping(+0x9c) 11 GK handling(+0x9e) 12 presence(+0xa0)
RATING_STAT_INFO_OFFSETS = [0x8E, 0x8C, 0x8A, 0x88, 0x92, 0x98, 0x96, 0x94, 0x9A, 0x90, 0x9C, 0x9E, 0xA0]
RATING_STAT_NAMES = ["strength", "stamina", "speed", "acceleration", "ball_control", "passing",
                     "crossing", "shooting", "heading", "tackling", "gk_shot_stopping",
                     "gk_handling", "presence"]
# Weight matrix @0x63af50: 13 int32 per FE row (row sums = 1000). Rows 0..10 used.
RATING_WEIGHTS = [
    [5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 494, 228, 228],          # 0 GK
    [10, 124, 66, 10, 95, 76, 114, 9, 48, 427, 7, 7, 7],     # 1 full back (det 1,3)
    [10, 124, 66, 10, 95, 76, 114, 9, 48, 427, 7, 7, 7],     # 2 full back (det 2,4)
    [114, 6, 7, 7, 57, 57, 6, 6, 114, 608, 6, 6, 6],         # 3 centre back (det 5,6,7)
    [66, 66, 10, 9, 105, 190, 162, 7, 6, 361, 6, 6, 6],      # 4 def. mid (det 8,9,10)
    [10, 67, 8, 8, 218, 238, 218, 57, 6, 152, 6, 6, 6],      # 5 (det 11,12,13)
    [6, 6, 7, 47, 333, 285, 95, 190, 8, 8, 5, 5, 5],         # 6 (det 14,15,18)
    [9, 57, 57, 57, 314, 161, 266, 38, 10, 10, 7, 7, 7],     # 7 (det 17)
    [9, 57, 57, 57, 314, 161, 266, 38, 10, 10, 7, 7, 7],     # 8 (det 16)
    [6, 10, 67, 67, 380, 142, 209, 85, 8, 8, 6, 6, 6],       # 9 (det 20,21)
    [28, 10, 48, 48, 256, 123, 9, 390, 57, 10, 7, 7, 7],     # 10 striker (det 19,22)
]

# --------------------------------------------------------------------------------------------
# CConfig "PlayerValues" variables (EConfigGameVariables 0x15b..0x16c)
# --------------------------------------------------------------------------------------------
# Effective values = bundled assets/data/x_android/dls_config.dat (XOR 0x53d392af + zlib XML),
# <Config><GameVariables><PlayerValues>. Loaded by CConfig::LoadVars -> LoadNodeVars(0x15b..0x16d).
CONFIG_BUNDLED = {
    # group: (min_rating, max_rating, min_value, max_value)
    0: (51, 100, 50, 3125),    # Gk
    1: (51, 100, 75, 3375),    # Def
    2: (52, 100, 100, 3875),   # Mid
    3: (52, 100, 150, 4125),   # Att
    "exp_whole": 2, "exp_tenths": 3,          # ExpFactorWhole / ExpFactorTenths -> 2.3
    "scout_price_percent": 20,                # Scouting/PricePercentIncrement (var 0x179)
}
# Compiled-in defaults (s_tConfigVarInfo @0x61b7d8, used only if a var is missing from the XML).
CONFIG_DEFAULTS = {
    0: (40, 100, 45, 450), 1: (40, 100, 60, 600), 2: (40, 100, 90, 900), 3: (40, 100, 120, 1200),
    "exp_whole": 1, "exp_tenths": 5, "scout_price_percent": 20,
}


def f32(x: float) -> float:
    return struct.unpack("<f", struct.pack("<f", x))[0]


def trunc_div(a: int, b: int) -> int:
    """C integer division (truncates toward zero)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def c_mod(a: int, b: int) -> int:
    return a - trunc_div(a, b) * b


def xmath_clamp(v: int, lo: int, hi: int) -> int:
    # XMATH_Clamp 0x383c48: r = (v<=hi ? v : hi); if v<lo r=lo
    r = v if v <= hi else hi
    if v < lo:
        r = lo
    return r


def round_to_nearest(v: int, n: int) -> int:
    # XMATH_RoundToNearest 0x384530: rem = v % n (C semantics); v -= rem; if rem >= n/2: v += n
    rem = c_mod(v, n)
    out = v - rem
    if rem >= trunc_div(n, 2):
        out += n
    return out


# --------------------------------------------------------------------------------------------
# TPlayerInfo helpers
# --------------------------------------------------------------------------------------------
def player_info_from_rom(rom: bytes) -> bytearray:
    """Minimal CDataBase::PlayerROMtoInfoSimple (0x20dabc) for the fields rating/value use.

    rom = one 0xB4-byte record of players.dat (decoded), i.e. TPlayerROM.
    Returns a 0xB0-byte TPlayerInfo with id, det/general position, stats, nationality etc.
    (No CPlayerDevelopment deltas: stock/new-career state.)
    """
    info = bytearray(0xB0)
    info[0:2] = rom[0:2]                                   # id
    info[0x02:0x24] = rom[0x02:0x24]                       # first name (wchar)
    info[0x26:0x4E] = rom[0x24:0x4C]                       # surname
    info[0x50:0x72] = rom[0x4C:0x6E]                       # common/display name
    info[0x74] = 30
    info[0x76:0x78] = rom[0x6E:0x70]                       # nationality (ECountry, s16)
    info[0x78] = rom[0x72]; info[0x79] = rom[0x74]; info[0x7A] = rom[0x70]; info[0x7B] = rom[0x76]
    info[0x7C] = rom[0x78]; info[0x7D] = rom[0x7A]         # height cm / weight kg (observed)
    det = struct.unpack_from("<b", rom, 0x7C)[0]
    info[0x80] = rom[0x7C]; info[0x81] = rom[0x7E]
    info[0x7F] = (GEN_POS[det] if 0 <= det <= 22 else -1) & 0xFF
    info[0x84] = rom[0xA0]; info[0x85] = rom[0xA2]; info[0x86] = rom[0x84]
    info[0xA2] = rom[0xA6]; info[0xA3] = rom[0xA7]; info[0xA4:0xA6] = rom[0xA4:0xA6]
    info[0xAD] = rom[0x80]; info[0xAE] = 0xFF
    # stats: ROM 0x86..0x9e -> info 0x88..0xa0 (same order, shifted +2)
    info[0x88:0xA2] = rom[0x86:0xA0]
    return info


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def pu_get_player_rating(info: bytes) -> int:
    """PU_GetPlayerRating(TPlayerInfo*) @0x2b35d0.

    row = FE_POS[det_pos(+0x80)]; acc = sum(W[row][i] * raw_u16_stat_i)  (raw stats are x10, 0..1000)
    rating = clamp( trunc((acc + 35000) / 10000), 0, 100 )
    i.e. OVR = weighted mean of the 13 stats (weights /1000) + 3.5, truncated.
    """
    det = struct.unpack_from("<b", info, 0x80)[0]
    if not 0 <= det <= 22:
        raise ValueError("detailed position out of range (native would index garbage)")
    w = RATING_WEIGHTS[FE_POS[det]]
    acc = 0
    for wi, off in zip(w, RATING_STAT_INFO_OFFSETS):
        acc += wi * u16(info, off)
    acc = (acc + 35000) & 0xFFFFFFFF
    if acc & 0x80000000:
        acc -= 1 << 32
    return xmath_clamp(trunc_div(acc, 10000), 0, 100)


def get_player_value(info: bytes, gen_pos: int = -1, rating: int = -1, b_random: bool = False,
                     b_secret_discount: bool = True, config=CONFIG_BUNDLED, scouted: bool = False,
                     secret_player_turn: bool = False, secret_discount_pct: int = 0,
                     random_fn=None, season_count: int = 0, matches_played: int = 0) -> int:
    """CTransfers::GetPlayerValue(TPlayerInfo*, int genPos, int rating, bool bRandom, bool bDisc) @0x212b3c.

    rating  = PU_GetPlayerRating(info) if rating == -1
    genPos  = (s8)info[+0x7f] if genPos == -1
    (minR,maxR,minV,maxV) = config vars 0x15b+4g..0x15e+4g  (all 0 if genPos > 3 unsigned)
    r       = clamp(rating, minR, maxR)
    e       = float(ExpFactorWhole) + float(ExpFactorTenths)/10.0          (float32)
    t       = float(r-minR) / float(maxR-minR)                             (float32)
    v       = minV + (int)(powf(t, e) * float(maxV-minV))
    if CSeason::IsPlayerScouted(id): v = (int)((PricePercentIncrement/100 + 1) * v)
    if bRandom: seed = id + matchesPlayed*100 + seasonCount;  v += -(v/100) + XSYS_Random(v/50)
            (the v/100, v/50 terms use the pre-scout value)
    v       = RoundToNearest(v, 5)
    if CTransfers::ms_bSecretPlayerTurn && bDisc: v = RoundToNearest(v - v*ms_tSecretPlayerInfo[+0x14]/100, 5)
    """
    if rating == -1:
        rating = pu_get_player_rating(info)
    if gen_pos == -1:
        gen_pos = struct.unpack_from("<b", info, 0x7F)[0]
    if 0 <= gen_pos <= 3:
        min_r, max_r, min_v, max_v = config[gen_pos]
    else:
        min_r = max_r = min_v = max_v = 0
    r = xmath_clamp(rating, min_r, max_r)
    e = f32(f32(float(config["exp_tenths"])) / f32(10.0))
    e = f32(e + f32(float(config["exp_whole"])))
    t = f32(f32(float(r - min_r)) / f32(float(max_r - min_r))) if max_r != min_r else float("nan")
    p = f32(math.pow(t, e)) if t == t else float("nan")
    prod = f32(p * f32(float(max_v - min_v)))
    base = min_v + (int(prod) if prod == prod else 0)
    v = base
    if scouted:
        v = int(f32(f32(f32(float(config["scout_price_percent"])) / f32(100.0)) + f32(1.0)) * f32(float(base)))
    if b_random:
        if random_fn is None:
            raise ValueError("b_random needs random_fn(seed, n)")
        seed = u16(info, 0) + matches_played * 100 + season_count
        v = trunc_div(base * -1, 100) + v + random_fn(seed, trunc_div(base, 50))
    v = round_to_nearest(v, 5)
    if secret_player_turn and b_secret_discount:
        v = round_to_nearest(v - trunc_div(v * secret_discount_pct, 100), 5)
    return v


def market_value(info: bytes, config=CONFIG_BUNDLED) -> int:
    """Exactly what native_bridge.c::native_player_value computes (GetPlayerValue(info,-1,-1,0,1), >=0)."""
    v = get_player_value(info, -1, -1, False, True, config)
    return v if v > 0 else 0


def team_value_total(infos, config=CONFIG_BUNDLED) -> int:
    """CDataBase::GetTeamValueTotal(teamId) @0x20c3a0 = sum GetPlayerValue(info,-1,-1,false,true)
    over every id in the team link (info from CDataBase::GetPlayerInfo(..., team, true, NULL, 0, -1))."""
    return sum(get_player_value(i, -1, -1, False, True, config) for i in infos)


# --------------------------------------------------------------------------------------------
# Self-test
# --------------------------------------------------------------------------------------------
def _selftest():
    import os
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, "..", "..", ".."))
    assert all(sum(r) == 1000 for r in RATING_WEIGHTS)
    # note n/2 truncates (5/2 == 2), so a remainder of 2 already rounds up: 11->10, 12->15
    assert round_to_nearest(11, 5) == 10 and round_to_nearest(12, 5) == 15 and round_to_nearest(15, 5) == 15
    # synthetic: all stats 70.0 -> weighted mean 700 -> (700000+35000)/10000 = 73
    info = bytearray(0xB0)
    for off in RATING_STAT_INFO_OFFSETS:
        struct.pack_into("<H", info, off, 700)
    for det in range(23):
        info[0x80] = det
        info[0x7F] = GEN_POS[det]
        assert pu_get_player_rating(info) == 73
    # value endpoints (bundled config)
    info[0x80] = 19; info[0x7F] = 3
    assert get_player_value(info, 3, 100) == 4125
    assert get_player_value(info, 3, 52) == 150
    assert get_player_value(info, 0, 30) == 50
    print("pure-python checks OK")
    players = os.path.join(root, "unpacked", "db_decoded", "players.dat.decoded.bin")
    if os.path.exists(players):
        d = open(players, "rb").read()
        n = struct.unpack_from("<I", d, 8)[0]
        rom = d[12:12 + 0xB4]
        pi = player_info_from_rom(rom)
        print("player", u16(pi, 0), "rating", pu_get_player_rating(pi), "value", market_value(pi))
    try:
        import dls_native_check  # Unicorn harness written alongside (optional)
    except ImportError:
        dls_native_check = None
    if dls_native_check is not None and os.path.exists(players):
        dls_native_check.run_crosscheck(verbose=True)


if __name__ == "__main__":
    _selftest()
