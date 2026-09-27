"""DLS18 gameplay and animation patcher for libDLS18.so (v5.064, armeabi-v7a).

What it does
------------
* P1  CTRL_ControllerGetInput  : replaces the "hold direction ~1.5 s -> auto sprint" logic with
                                 DLS15-style analog sprint: sprint flag (TController+0x54) is set
                                 when the stick is pushed near full (power >= SPRINT_ON, drops out
                                 below SPRINT_OFF), the player has stamina, it's open play and
                                 close control isn't held.
* P2  GC_DribblingControl      : on-ball urgency = sprint 0x1000 / close control CC_URG / jog 0x800.
* P3  GC_MovementOffBall       : off-ball urgency = sprint 0x1000 / close control CC_OFF_URG /
                                 otherwise the game's own jog logic.
* P4  GPM_DribbleTouch         : when a dribbler is well below jog pace, scale the touch strength
                                 down (factor (2*urgency+0x400)/0x1000 below 0x600) so slow dribbling keeps the ball close.
* A1  CPlayer::Animate         : transition timing depends on the average acceleration/control rating.
* A2  CPlayer::SetAnimFromStateLoco: bias every forward-jog, forward-sprint, and angled-jog dribble
                                    candidate toward the player's low, mid, or high control tier.
                                    Direction fit remains part of the engine's choice.
* A3  GC_SpecialMoveDribbling  : deek style pick. By default anim_fit.py's style_fit replaces it with a
                                continuous pick (style probabilities ramp linearly with control); only
                                with --no-anim-fit / --no-deek-style-fit / --no-extend are the stock
                                thresholds (SM_DRIBBLE_HIGH/LOW) set to --dribble-skill-high/low instead.

Close control = a finger held on the right-hand touch track (XCTRL game touch) that is not on
any HUD button. Walk/jog/sprint animations are chosen by the engine from urgency, so the stock
animation sets (incl. SPRINT_DRIBBLE / JOG_DRIBBLE clips) are used automatically.

New code lives in the body of the unused debug function FTTCollectionsTest().

usage: python build_mod.py [--sprint-on 0x3800] [--sprint-off 0x3200] [--cc-urg 0x380]
                           [--cc-off-urg 0x500] [--dribble-skill-low 60]
                           [--dribble-skill-high 85] [--dribble-style-bias 0xC000]
                           [--out build/libDLS18.so]
"""
import argparse
import hashlib
import math
import re
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs
from keystone import KS_ARCH_ARM, KS_MODE_THUMB, Ks

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "analysis"))
from armdis import D as ORIG, find, va2off  # noqa: E402
from stat_audit import resolve  # noqa: E402


def stat_input_curve(stat, knee=7.0):
    """Softplus below 48, smoothly meeting the identity map at stat 56."""
    stat = float(stat)
    softplus = 40.0 + knee * math.log1p(math.exp((stat - 40.0) / knee))
    if stat <= 48.0:
        return softplus
    if stat >= 56.0:
        return stat
    t = (stat - 48.0) / 8.0
    blend = t * t * (3.0 - 2.0 * t)
    return softplus + blend * (stat - softplus)


def kick_error_decay(stat):
    """Strictly falling error curve over valid raw stats 0..99."""
    stat = float(stat)
    if stat < 25.0:
        u = stat - 25.0
        # Continue below 25 without a clamp; match value and slope at 25.
        x = u / 74.0 + (13.0 / 92500.0) * u * u
    else:
        x = (stat - 25.0) / 74.0
    return (1.0 - x) ** 2.6


def kick_error_degrees(stat, worst, best):
    return best + (worst - best) * kick_error_decay(stat)


def kick_error_units(stat, worst, best):
    """Encode degrees in DLS18's turn-angle units (0x80 is 2.8 degrees)."""
    return round(kick_error_degrees(stat, worst, best) * 128.0 / 2.8)


def duel_sigmoid(z, scale=7.0):
    return 1.0 / (1.0 + math.exp(-float(z) / scale))


def duel_probability(z, slide=False, scale=7.0, slide_shift=3.0):
    if slide:
        return 4.0 + 84.0 * duel_sigmoid(float(z) - slide_shift, scale)
    return 5.0 + 90.0 * duel_sigmoid(z, scale)


def _softplus_scaled(value, softness):
    x = value / softness
    return softness * (max(x, 0.0) + math.log1p(math.exp(-abs(x))))


def foul_factor(stat, softness=0.1):
    """Smoothly map the raw foul multiplier into the open interval (0.6, 1.4)."""
    raw = (160.0 - float(stat)) / 90.0
    return 0.6 + _softplus_scaled(raw - 0.6, softness) - _softplus_scaled(raw - 1.4, softness)


def _u16_table(values):
    values = tuple(int(v) for v in values)
    if any(v < 0 or v > 0xFFFF for v in values):
        raise ValueError("CAVE2 table value does not fit u16")
    return struct.pack("<" + "H" * len(values), *values)


def stat_table_q8(knee=7.0):
    return tuple(round(stat_input_curve(s, knee) * 256.0) for s in range(0, 57, 4))


def error_decay_table_q15():
    values = [round(kick_error_decay(s) * 32768.0) for s in range(100)]
    for index in range(len(values) - 2, -1, -1):
        values[index] = max(values[index], values[index + 1] + 1)
    return tuple(values)


def duel_sigmoid_table_q16(scale=7.0):
    raw = [round(duel_sigmoid(z, scale) * 65535.0) for z in range(-160, 161, 4)]
    out = []
    for index, value in enumerate(raw):
        low = 4 if not out else out[-1] + 1
        high = 65534 - (len(raw) - index - 1)
        out.append(min(max(value, low), high))
    return tuple(out)


def foul_table_q12(softness=0.1):
    return tuple(round(foul_factor(s, softness) * 4096.0) for s in range(100))

# Stat -> value retune: call site of AttributeInterpolate_Internal -> (stock lo, stock hi, new lo, new hi)
# lo = value at stat <= 40, hi = value at stat >= 99 (after the contrast curve).
RETUNE = {
    # jog / average run speed: the wider output range carries the rating gap.
    0x2E133E: (3204, 3738, 2950, 4600), 0x2E1390: (3204, 3738, 2950, 4600),
    0x2E14AC: (3204, 3738, 2950, 4600), 0x2E14EA: (3204, 3738, 2950, 4600),
    # sprint speed: linear input now gives about 1.22x at 62 versus 85.
    0x2E1410: (3738, 4539, 2950, 5000), 0x2E1502: (3738, 4539, 2950, 5000),
    # walk speed was identical for everyone
    0x2E13D0: (801, 801, 760, 840), 0x2E1494: (801, 801, 760, 840),
    # acceleration: a wider range gives low-rated players time to build pace.
    0x2E1798: (13, 19, 7, 27),
    # control: speed with the ball (x/1024), about 1.3x the previous width.
    0x2E159A: (870, 990, 768, 1042),
    # control: first touch / ball take radius, about 1.3x the previous width.
    0x2E1E92: (947, 1178, 820, 1320), 0x2E2058: (947, 1178, 820, 1320),
    0x2E233E: (947, 1178, 820, 1320), 0x2E26F4: (947, 1178, 820, 1320),
    0x2E2356: (8544, 13350, 6500, 15600),
    # control: dribble touch strength, jog / sprint; about 1.3x the previous width.
    0x2E4CC6: (1869, 1602, 2075, 1425), 0x2E4CDE: (2670, 2136, 2950, 1845),
    # tackling: tackle reach / action scaling
    0x2DB164: (1638, 2048, 1400, 2150), 0x2EA60C: (1638, 2048, 1400, 2150),
    0x2DB146: (512, 1024, 400, 1100),
    # shooting: shot-placement chance (%) and shot assist strength
    0x2E72F2: (33, 66, 20, 80), 0x2E8610: (-60, 80, -80, 96),
}


# Direct (non-AttributeInterpolate) stat uses: address -> (stock instruction, new instruction)
DIRECT = {
    # These call XMATH_InterpolateClamp directly (not through AttributeInterpolate, so the softplus stat
    # curve does not apply). Each line starts at stat 1 so there is no flat region below the stock
    # window; the endpoints are the old line extrapolated, so the values above the old window are unchanged.
    0x29F734: ("movs r1, #0x32", "movs r1, #1"),    # GAI_GetReactionTime: GK reaction stat 50..99 ->
    0x29F738: ("movs r3, #9", "movs r3, #25"),      #   delay 9..0 frames  ==>  1..99 -> 25..0 (45 -> 13.8)
    # CPlayer::UpdateSprint (stamina stat 40..99): sprint drain 0x500..0x300 -> 0x600..0x280 at 40..99,
    # recovery 0x300..0x500 -> 0x280..0x600 (fit players can keep running, unfit ones fade), both lines
    # continued down to stamina 1 (drain 0x850, recovery 0x30) instead of flat below 40
    0x2DA22C: ("mov.w r1, #0x300", "movw r1, #0x280"),
    0x2DA232: ("movs r1, #0x28", "movs r1, #1"),
    0x2DA236: ("mov.w r3, #0x500", "movw r3, #0x850"),
    0x2DA268: ("mov.w r1, #0x500", "movw r1, #0x600"),
    0x2DA26E: ("movs r1, #0x28", "movs r1, #1"),
    0x2DA272: ("mov.w r3, #0x300", "movw r3, #0x30"),
}


def retune_patches():
    """Re-encode the constant loads feeding each retuned call, keeping instruction sizes."""
    out = {}
    for call, (olo, ohi, nlo, nhi) in RETUNE.items():
        vals, sites = resolve(call)
        if (vals.get("r2"), vals.get("r3")) != (olo, ohi):
            sys.exit(f"retune {call:#x}: expected {olo},{ohi} got {vals.get('r2')},{vals.get('r3')}")
        for reg, new in (("r2", nlo), ("r3", nhi)):
            addr, mnem, size = sites[reg]
            if size == 2:
                if not 0 <= new <= 255:
                    sys.exit(f"{call:#x} {reg}: {new} does not fit movs")
                code = asm(f"movs {reg}, #{new}", addr)
            elif new < 0:
                code = asm(f"mvn {reg}, #{~new}", addr)
            else:
                code = asm(f"movw {reg}, #{new}", addr)
            if len(code) != size:
                sys.exit(f"{call:#x} {reg}: size mismatch")
            out[addr] = code
    for addr, (old, new) in DIRECT.items():
        o, n = asm(old, addr), asm(new, addr)
        if bytes(ORIG[va2off(addr):va2off(addr) + len(o)]) != o or len(n) != len(o):
            sys.exit(f"direct patch {addr:#x}: original is not '{old}' or size differs")
        out[addr] = n
    return out

CAVE = find("_Z18FTTCollectionsTestv")
CAVE_SIZE = 1676

# Strings are stored in the unused tail of the cave. Keeping them out of the
# assembled instruction stream lets the branch guard inspect every instruction.
MARKET_LIBRARY_NAME = b"libCareerMarket.so\0"
MARKET_SERIALIZE_NAME = b"career_market_on_serialize\0"
MARKET_TURN_NAME = b"career_market_on_turn\0"
MARKET_STOCK_NAME = b"career_market_stock_action\0"
MARKET_BASE_RODATA = MARKET_LIBRARY_NAME + MARKET_SERIALIZE_NAME + MARKET_TURN_NAME + MARKET_STOCK_NAME
# The code before the strings is padded with 2-byte NOPs, so the string block must have an even length:
# an odd length silently shifted every string one byte early ("ibCareerMarket.so").
if len(MARKET_BASE_RODATA) % 2:
    MARKET_BASE_RODATA += b"\0"
MARKET_ECON_MATCH_NAME = b"career_market_on_match_awards\0"
MARKET_ECON_SEASON_NAME = b"career_market_on_season\0"
MARKET_ECON_SPEND_NAME = b"career_market_on_spend\0"
MARKET_ECON_INCOME_NAME = b"career_market_on_income\0"
MARKET_ECON_NAMES_OFFSET = len(MARKET_BASE_RODATA)
MARKET_RODATA = (MARKET_BASE_RODATA + MARKET_ECON_MATCH_NAME + MARKET_ECON_SEASON_NAME
                 + MARKET_ECON_SPEND_NAME + MARKET_ECON_INCOME_NAME)
if len(MARKET_RODATA) % 2:
    MARKET_RODATA += b"\0"
MARKET_RODATA_OFFSET = CAVE_SIZE - len(MARKET_RODATA)
MARKET_SERIALIZE_HOOK = 0x36BC42
MARKET_TURN_HOOK = 0x36A078
MARKET_SCREEN_HOOK = 0x276490
MARKET_SCREEN_REPLAY_CALL = 0x1C35F8
# The game's own transfer screens route through the market (data/STOCK_TRANSFER_FLOW.md 3.1, 3.3): the stock
# buy dialog (search screen, scouting results) and the stock sell dialog are skipped, and
# career_market_stock_action(TPlayerInfo*, team or -1, base) shows the market's window check and negotiation.
STOCK_BUY_SEARCH_HOOK = 0x277006        # CFESDreamLeagueTransfers::CurrentPlayerBid: r4 = card
STOCK_BUY_SEARCH_SKIP = 0x2770E0        #   stock epilogue (skips the sign dialog)
STOCK_BUY_SCOUT_HOOK = 0x250A3C         # CFEMsgScoutResults::Process: r6 = card, r7 = TPlayerInfo*
STOCK_BUY_SCOUT_SKIP = 0x250AC2         #   continue the card loop without the sign dialog
STOCK_SELL_HOOK = 0x23E404              # CFETeamManagement::Process: r4 = this, card = [r4+0x100]
STOCK_SELL_SKIP = 0x23E426              #   skip the stock sell dialog
# Economy hooks (v6, data/ECONOMY_HOOKS.md 1.6 and 3.2): per-match income and season-end payouts.
ECON_MATCH_HOOK = 0x23A274              # CFEPostMatchCreditAwards::SetupCreditAwardInfo tail: blx SetMatchCredits
ECON_SET_MATCH_CREDITS = 0x3776A4       # CMyProfile::SetMatchCredits body (fallback tail-call target)
ECON_SEASON_HOOK = 0x29900A             # CFlow::Process step 6: blx CSeason::NextSeason
ECON_SEASON_NEXT_SEASON = 0x36AA08      # CSeason::NextSeason (in bl range of the cave; called directly)

# Second cave: the v6 economy hooks don't fit in the ~12 free bytes left in CAVE (see git history /
# HANDOFF_V6.md for the measurement). DEBUGCHARACTER_RenderPlayerData and
# DEBUGCHARACTER_RenderPlayerPitch are two adjacent, never-called free functions (verified with
# analysis/xref.py: zero direct bl/blx callers; not C++ methods, so no vtable entry; zero raw-address
# hits outside their own body when scanning the whole file, and zero .rel.dyn relocations into their
# range). Together they give 1,112 bytes of room for this hook set and future ones.
CAVE2 = find("_Z31DEBUGCHARACTER_RenderPlayerDatav")
CAVE2_SIZE = 1112
SAVE_VERSION_SETUP = 0x376AC6
# The base APK is a pre-hacked copy: CMyProfile::GetCredits was replaced by "mvn r0, #0xff000000; bx lr"
# (always 16,777,215 coins). Restore the stock getter (credits live at profile+0x2A7CC; the untouched
# tail "ldr r0, [r0, r1]; bx lr" and the sibling getters GetCreditsSpent/Earned/Purchased confirm it).
GET_CREDITS = 0x3769C0
ECON_SPEND_HOOK = 0x377A18             # CMyProfile::SubtractCredits entry (leaf): movw r2,#0xa7cc
ECON_SPEND_RESUME = 0x377A1C
ECON_INCOME_HOOK = 0x377984            # CMyProfile::AddCredits entry: push {r7,lr}; movw ip,#0xa7cc (6 bytes)
ECON_INCOME_RESUME = 0x37798A
CCREDITS_SUB_RETURN = 0x2633D9         # lr inside CCredits::SubtractCredits (blx at 0x2633d4) | Thumb
CCREDITS_ADD_RETURN = 0x2634E7         # lr inside CCredits::AddCredits (blx at 0x2634e2) | Thumb
SHOP_CTOR_BUTTONS = 0x254AB4           # CFEShopDialog ctor: button flags -> add an OK button
SHOP_CTOR_TEXT = 0x254AC4              # ... pass the caller's text (not NULL)
SHOP_CTOR_KIND = 0x254AD6              # ... keep a plain message box (no shop)
SHOP_CTOR_TITLE = 0x254AD8             # ... title LOC 0x6a1 "Coins" instead of "Shop"
SHOP_CTOR_SETUP = 0x254AE8             # ... skip the shop-only setup
HEADER_COIN_TAP = 0x246C32             # header coin button: taps no longer open the shop
REWARD_VIDEO_GRANT = 0x203AE6          # rewarded-video coin grant: skip (no coins for adverts)
VIDEO_DOUBLER = 0x201678               # CConfig::GetMaxDoubler: 0 hides the post-match 'watch a video' coin doubler
DEV_RECORDS_CAP = 0x20E1B4             # CPlayerDevelopment::AddPlayer: cmp r0,#0x3f (64 records; storage is dynamic)
CUSTOM_IMAGE_MAX = 0x206DA4            # CCustomData::GetImageMinMaxDimensions: mov.w r4,#0x200 (512 px)
# Runtime C hooks (v8+, modcore): {name, site, length}. The displaced `length` bytes (4 + 2n) must be
# whole, position-independent instructions (modcore_build checks). Handlers are registered by name in
# libCareerMarket (modcore_register). An unregistered hook runs the stock code.
RUNTIME_HOOKS = [
    {"name": "career_market_new_screen", "site": 0x23BC14, "length": 4},
    # v35 market inside the stock transfer screen (native_bridge.c "Transfer screen UI"); every site is a
    # function entry whose first 4 bytes are push/sub (position independent).
    {"name": "ts_init", "site": 0x2765BC, "length": 4},          # CFESDreamLeagueTransfers::Init
    {"name": "ts_process", "site": 0x276954, "length": 4},       # CFESDreamLeagueTransfers::Process
    {"name": "ts_render_post", "site": 0x2770FC, "length": 4},   # CFESDreamLeagueTransfers::RenderPost
    {"name": "ts_setup", "site": 0x276BB4, "length": 4},         # CFESDreamLeagueTransfers::SetupResults
    {"name": "ts_bid", "site": 0x276FB8, "length": 4},           # CFESDreamLeagueTransfers::CurrentPlayerBid
    {"name": "ts_card_value", "site": 0x2343AC, "length": 4},    # CFEPlayerCard::GetPlayerValue
    {"name": "ts_footer_post", "site": 0x246656, "length": 4},   # CFEFooterMenu::RenderPost
]
SAVE_VERSION_BOOT = 0x376B72
DL_OPEN_PLT = 0x1D90FC
DL_SYM_PLT = 0x1D855C
DL_CLOSE_PLT = 0x1D9138

SYM = {k: find(v) for k, v in {
    "GetButtonDown": "_Z19XCTRL_GetButtonDowni7EButton",
    "GameTouchTouching": "_Z26XCTRL_GetGameTouchTouchingi",
    "InOpenPlay": "_Z17AIGAME_InOpenPlayv",
    "SetUrgency": "_ZN7CPlayer10SetUrgencyEi",
    "InterpClamp": "_Z22XMATH_InterpolateClampiiiii",
    "Random": "_Z11XSYS_Randomi",
    "Trip": "_ZN7CPlayer4TripEii",
    "GetRotPoint": "_ZN7CPlayer11GetRotPointE6TPoint",
    "IsFacing": "_ZN7CPlayer8IsFacingEii",
}.items()}

# (site, expected original bytes length, description)
P1 = 0x2B17BE          # CTRL_ControllerGetInput: start of auto-sprint block
P1_RET = 0x2B1A8E      # its epilogue (add sp,#0x1c; pop {r4-r11,pc})
P2 = 0x2E4F44          # GC_DribblingControl: urgency select block (0x32 bytes)
P2_RET = 0x2E4F76      # blx SetUrgency
P3 = 0x2E94AE          # GC_MovementOffBall: sprint urgency block (0x22 bytes)
P3_RET = 0x2E94D0      # epilogue
P4 = 0x2E4CF8          # GPM_DribbleTouch: ldr r4,[pc,#0x130] ; mov r5,r0
P4_LIT = 0x2E4E2C      # literal that ldr loaded

S1 = 0x2DAEBC          # AttributeInterpolate_Internal: blx XMATH_InterpolateClamp (stat -> value)
S2 = 0x2E5B34          # ACT_KickErrorAccuracyGetRange entry: push {r4-r7,lr}; sub sp,#0xc
S2_CONT = 0x2E5B38
S3 = 0x2E5C02          # ...final error-range mapping: movs r0,#0x22; movs r1,#1
T1 = 0x2DB860          # UpdateActionConservativeTackle: contact block start (before ball SetVel + GL_SetTouch)
T1_LIT = 0x329C43      #   literal its 'ldr r1,[pc]' loads (used by the following 'add r1, pc')
T1_EPI = 0x2DB884      #   function epilogue
T2 = 0x2DB430          # UpdateActionSlideTackleX: contact block start
T2_LIT = 0x32A093
T2_SKIP = 0x2DB49C     #   first instruction after the contact block (expects r0 = team)
T3 = 0x2DBA18          # ...foul chance: mov r6,r0 ; movs r0,#100
C1 = 0x2DF2AC          # COL_PlayerAllCollisionProcess: all-or-nothing push split by strength
A1 = 0x2DC5BE          # CPlayer::Animate: add.w r0,r0,#0xfc00  (anim crossfade weight -= 0x400 per tick)
S4 = 0x2E6686          # GL_SetTouch: old possession-level duel site (no longer patched: broke tackle reactions)
A2 = 0x2E0F74          # CPlayer::SetAnimFromStateLoco: candidate score before best-match selection
SM_DRIBBLE_HIGH = 0x2E4156  # GC_SpecialMoveDribbling: 3-way style pool starts at this control rating
SM_DRIBBLE_LOW = 0x2E416E   # ...2-way style pool starts at this control rating

EXPECT = {
    S1: bytes.fromhex("e7f6b8e9"),
    S2: bytes.fromhex("f0b583b0"),
    S3: bytes.fromhex("22200121"),
    S4: bytes.fromhex("90f88420"),
    A1: bytes.fromhex("00f57c40"),
    A2: bytes.fromhex("5d4507da"),
    SM_DRIBBLE_HIGH: bytes.fromhex("5528"),
    SM_DRIBBLE_LOW: bytes.fromhex("4b28"),
    T1: bytes.fromhex("b6490620"),
    T2: bytes.fromhex("ad4a0c23"),
    T3: bytes.fromhex("06466420"),
    C1: bytes.fromhex("814294bf4ff400784ff40074"),
    P1: bytes.fromhex("b548c8f1"),      # ldr r0,[pc,#0x2d4] ; rsb.w ...
    P2: bytes.fromhex("9af85400"),      # ldrb.w r0,[sl,#0x54]
    P3: bytes.fromhex("424849f6"),      # ldr r0,[pc,#0x108] ; movw ...
    P4: bytes.fromhex("4c4c0546"),      # ldr r4,[pc,#0x130] ; mov r5,r0
    MARKET_SERIALIZE_HOOK: bytes.fromhex("03480121"),  # ldr r0,[pc,#0xc] ; movs r1,#1
    MARKET_TURN_HOOK: bytes.fromhex("012008b0"),       # movs r0,#1 ; add sp,#0x20
    MARKET_SCREEN_HOOK: bytes.fromhex("4df7b2e8"),      # blx CFE::GetLastFlowDirection
    STOCK_BUY_SEARCH_HOOK: bytes.fromhex("4ff49c60"),   # mov.w r0,#0x4e0 (sign dialog size)
    STOCK_BUY_SCOUT_HOOK: bytes.fromhex("4ff49c60"),    # mov.w r0,#0x4e0
    STOCK_SELL_HOOK: bytes.fromhex("4ff49c60"),         # mov.w r0,#0x4e0 (sell dialog size)
    ECON_MATCH_HOOK: bytes.fromhex("8cf70ae8"),         # blx CMyProfile::SetMatchCredits (PLT)
    ECON_SEASON_HOOK: bytes.fromhex("33f776ef"),        # blx CSeason::NextSeason (PLT)
    CAVE2: bytes.fromhex("2de9f04381b02ded"),           # DEBUGCHARACTER_RenderPlayerData entry (push.w/sub sp/vpush)
    SAVE_VERSION_SETUP: bytes.fromhex("ae21"),
    GET_CREDITS: bytes.fromhex("6ff07f4070470201"),        # hacked: mvn r0,#0xff000000; bx lr; (movt tail)
    SAVE_VERSION_BOOT: bytes.fromhex("ae21"),
    VIDEO_DOUBLER: bytes.fromhex("80b50848"),           # push {r7,lr}; ldr r0,[pc,#0x20]
    DEV_RECORDS_CAP: bytes.fromhex("3f28"),
    CUSTOM_IMAGE_MAX: bytes.fromhex("4ff40074"),
    ECON_SPEND_HOOK: bytes.fromhex("4af2cc72"),         # movw r2,#0xa7cc
    ECON_INCOME_HOOK: bytes.fromhex("80b54af2cc7c"),    # push {r7,lr}; movw ip,#0xa7cc
    SHOP_CTOR_BUTTONS: bytes.fromhex("cde90061"),
    SHOP_CTOR_TEXT: bytes.fromhex("0022"),
    SHOP_CTOR_KIND: bytes.fromhex("2060"),
    SHOP_CTOR_TITLE: bytes.fromhex("40f20a30"),
    SHOP_CTOR_SETUP: bytes.fromhex("0db3"),
    HEADER_COIN_TAP: bytes.fromhex("98b9"),
    REWARD_VIDEO_GRANT: bytes.fromhex("10db"),
}


def market_asm(market_base):
    library_rel = MARKET_RODATA_OFFSET - (market_base - CAVE)
    serialize_rel = library_rel + len(MARKET_LIBRARY_NAME)
    turn_rel = serialize_rel + len(MARKET_SERIALIZE_NAME)
    stock_rel = turn_rel + len(MARKET_TURN_NAME)
    return f"""
cave_market_dl_open:
    push    {{r4, lr}}
    adr     r12, cave_market_dl_open
    movw    r3, #{market_base & 0xffff:#x}
    movt    r3, #{market_base >> 16:#x}
    sub.w   r12, r12, r3
    movw    r3, #{DL_OPEN_PLT & 0xffff:#x}
    movt    r3, #{DL_OPEN_PLT >> 16:#x}
    add.w   r12, r12, r3
    blx     r12
    pop     {{r4, pc}}

cave_market_dl_sym:
    push    {{r4, lr}}
    adr     r12, cave_market_dl_open
    movw    r3, #{market_base & 0xffff:#x}
    movt    r3, #{market_base >> 16:#x}
    sub.w   r12, r12, r3
    movw    r3, #{DL_SYM_PLT & 0xffff:#x}
    movt    r3, #{DL_SYM_PLT >> 16:#x}
    add.w   r12, r12, r3
    blx     r12
    pop     {{r4, pc}}

cave_market_dl_close:
    push    {{r4, lr}}
    adr     r12, cave_market_dl_open
    movw    r3, #{market_base & 0xffff:#x}
    movt    r3, #{market_base >> 16:#x}
    sub.w   r12, r12, r3
    movw    r3, #{DL_CLOSE_PLT & 0xffff:#x}
    movt    r3, #{DL_CLOSE_PLT >> 16:#x}
    add.w   r12, r12, r3
    blx     r12
    pop     {{r4, pc}}

cave_market_dispatch:
    push    {{r4, r5, r6, r7, lr}}
    sub     sp, #4
    mov     r4, r0
    mov     r5, r1
    mov     r6, r2
    movs    r7, #0
    adr     r0, cave_market_dl_open
    addw    r0, r0, #{library_rel}
    movs    r1, #2
    bl      cave_market_dl_open
    cbz     r0, market_dispatch_exit
    mov     r7, r0
    mov     r0, r7
    mov     r1, r4
    bl      cave_market_dl_sym
    movs    r4, #0
    cbz     r0, market_dispatch_close
    mov     ip, r0
    adr     r3, cave_market_dl_open
    movw    r0, #{market_base & 0xffff:#x}
    movt    r0, #{market_base >> 16:#x}
    sub.w   r2, r3, r0
    mov     r0, r5
    mov     r1, r6
    blx     ip
    mov     r4, r0
market_dispatch_close:
    mov     r0, r7
    bl      cave_market_dl_close
    mov     r0, r4
market_dispatch_exit:
    add     sp, #4
    pop     {{r4, r5, r6, r7, pc}}

cave_market_serialize:
    adr     r0, cave_market_dl_open
    addw    r0, r0, #{serialize_rel}
    mov     r1, r5
    movw    r3, #0x3604
    sub.w   r1, r1, r3
    mov     r2, r4
    bl      cave_market_dispatch
    movw    r0, #0x4b0e
    movt    r0, #0x3c
    movs    r1, #1
    b.w     {MARKET_SERIALIZE_HOOK + 4:#x}

cave_market_turn:
    adr     r0, cave_market_dl_open
    addw    r0, r0, #{turn_rel}
    mov     r1, r4
    movs    r2, #0
    bl      cave_market_dispatch
    movs    r0, #1
    add     sp, #0x20
    b.w     {MARKET_TURN_HOOK + 4:#x}

cave_market_screen:
    adr     r0, cave_market_dl_open
    addw    r0, r0, #{turn_rel}
    movs    r1, #0
    movs    r2, #0
    bl      cave_market_dispatch
    blx     0x1c35f8
    b.w     {MARKET_SCREEN_HOOK + 4:#x}

cave_market_buy_search:
    adr     r0, cave_market_dl_open
    addw    r0, r0, #{stock_rel}
    add.w   r1, r4, #0x294
    ldr.w   r2, [r4, #0x344]
    bl      cave_market_dispatch
    b.w     {STOCK_BUY_SEARCH_SKIP:#x}

cave_market_buy_scout:
    adr     r0, cave_market_dl_open
    addw    r0, r0, #{stock_rel}
    mov     r1, r7
    ldr.w   r2, [r6, #0x344]
    bl      cave_market_dispatch
    b.w     {STOCK_BUY_SCOUT_SKIP:#x}

cave_market_sell:
    adr     r0, cave_market_dl_open
    addw    r0, r0, #{stock_rel}
    ldr.w   r1, [r4, #0x100]
    addw    r1, r1, #0x294
    mov.w   r2, #-1
    bl      cave_market_dispatch
    b.w     {STOCK_SELL_SKIP:#x}
"""


# The old economy hooks move into the newly freed CAVE1 space so CAVE2 can hold the stat tables
# and routines without overflowing its 1,112-byte reservation.
CAVE2_TABLE_COUNTS = {"stat": 15, "error": 100, "duel": 81, "foul": 100}
CAVE2_TABLE_LOCAL_OFFSETS = {}
_cave2_data_cursor = 0
for _table_name, _count in CAVE2_TABLE_COUNTS.items():
    _cave2_data_cursor = (_cave2_data_cursor + 3) & ~3
    CAVE2_TABLE_LOCAL_OFFSETS[_table_name] = _cave2_data_cursor
    _cave2_data_cursor += _count * 2
CAVE2_RODATA_SIZE = (_cave2_data_cursor + 3) & ~3
CAVE2_RODATA_OFFSET = CAVE2_SIZE - CAVE2_RODATA_SIZE
CAVE2_STAT_TABLE_REL = CAVE2_RODATA_OFFSET + CAVE2_TABLE_LOCAL_OFFSETS["stat"]
CAVE2_ERROR_TABLE_REL = CAVE2_RODATA_OFFSET + CAVE2_TABLE_LOCAL_OFFSETS["error"]
CAVE2_DUEL_TABLE_REL = CAVE2_RODATA_OFFSET + CAVE2_TABLE_LOCAL_OFFSETS["duel"]
CAVE2_FOUL_TABLE_REL = CAVE2_RODATA_OFFSET + CAVE2_TABLE_LOCAL_OFFSETS["foul"]


def cave2_rodata(cfg):
    """Aligned fixed-point lookup tables shared by the CAVE2 routines."""
    tables = {
        "stat": stat_table_q8(cfg.stat_knee),
        "error": error_decay_table_q15(),
        "duel": duel_sigmoid_table_q16(cfg.duel_scale),
        "foul": foul_table_q12(cfg.foul_softness),
    }
    data = bytearray()
    for name, values in tables.items():
        target = CAVE2_TABLE_LOCAL_OFFSETS[name]
        if len(data) > target:
            raise ValueError(f"CAVE2 {name} table overlaps previous data")
        data.extend(b"\0" * (target - len(data)))
        data.extend(_u16_table(values))
    data.extend(b"\0" * (CAVE2_RODATA_SIZE - len(data)))
    if len(data) != CAVE2_RODATA_SIZE or CAVE2_RODATA_OFFSET % 4:
        raise ValueError("CAVE2 rodata layout is not 4-byte aligned")
    return bytes(data)


def cave2_asm(cfg):
    """CAVE2 contains the v8 stat, error and duel routines plus their lookup tables."""
    shot_worst = round(cfg.shot_error_worst_deg * 128.0 / 2.8)
    shot_best = round(cfg.shot_error_best_deg * 128.0 / 2.8)
    pass_worst = round(cfg.pass_error_worst_deg * 128.0 / 2.8)
    pass_best = round(cfg.pass_error_best_deg * 128.0 / 2.8)
    return f"""
cave2_anchor:
cave2_stat:
    push    {{r4-r7}}
    cmp     r0, #56
    bhs     c2_stat_done
    mov     r4, r0
    lsrs    r5, r4, #2
    and.w   r4, r4, #3
    adr     r12, cave2_anchor
    addw    r12, r12, #{CAVE2_STAT_TABLE_REL}
    ldrh.w  r6, [r12, r5, lsl #1]
    adds    r5, #1
    ldrh.w  r7, [r12, r5, lsl #1]
    subs    r7, r7, r6
    muls    r7, r4, r7
    lsrs    r7, r7, #2
    adds    r0, r6, r7
    addw    r0, r0, #128
    lsrs    r0, r0, #8
c2_stat_done:
    pop     {{r4-r7}}
    b.w     {SYM['InterpClamp']:#x}

cave2_err_entry:
    push    {{r4, r5, r6, r7, lr}}
    sub     sp, #0x14
    str     r1, [sp, #0xc]
    b.w     {S2_CONT:#x}

cave2_err_exit:
    ldr     r0, [sp, #0xc]
    cmp     r0, #2
    beq     c2_error_type2
    cmp     r0, #4
    beq     c2_error_shot
    subs    r0, #2
    cmp     r0, #4
    bls     c2_error_pass
c2_error_shot:
    ldrb.w  r5, [r4, #0x12a]
    movw    r6, #{shot_worst}
    movw    r7, #{shot_best}
    b       c2_error_curve
c2_error_pass:
    ldrb.w  r5, [r4, #0x128]
    movw    r6, #{pass_worst}
    movw    r7, #{pass_best}
c2_error_curve:
    adr     r12, cave2_anchor
    addw    r12, r12, #{CAVE2_ERROR_TABLE_REL}
    ldrh.w  r5, [r12, r5, lsl #1]
    subs    r6, r6, r7
    muls    r6, r5, r6
    movw    r0, #0x4000
    adds    r6, r6, r0
    asrs    r6, r6, #15
    adds    r0, r7, r6
    b       c2_error_return
c2_error_type2:
    movs    r0, #0x20
c2_error_return:
    add     sp, #0x14
    pop     {{r4, r5, r6, r7, pc}}

cave2_duel_probability:
    push    {{r4-r7, lr}}
    mov     r4, r0
    tst     r1, #1
    beq     c2_prob_standing_z
    subs    r4, #{round(cfg.slide_shift * 2)}
c2_prob_standing_z:
    addw    r4, r4, #320
    cmp     r4, #0
    bge     c2_prob_low_ok
    movs    r4, #0
c2_prob_low_ok:
    cmp     r4, #640
    ble     c2_prob_high_ok
    movw    r4, #640
c2_prob_high_ok:
    lsrs    r5, r4, #3
    adr     r12, cave2_anchor
    addw    r12, r12, #{CAVE2_DUEL_TABLE_REL}
    ldrh.w  r6, [r12, r5, lsl #1]
    cmp     r5, #80
    beq     c2_prob_value_ready
    adds    r5, #1
    ldrh.w  r3, [r12, r5, lsl #1]
    subs    r3, r3, r6
    and.w   r7, r4, #7
    muls    r3, r7, r3
    lsrs    r3, r3, #3
    adds    r6, r6, r3
c2_prob_value_ready:
    tst     r1, #1
    beq     c2_prob_standing_range
    movw    r5, #21504
    movw    r3, #1024
    b       c2_prob_scale
c2_prob_standing_range:
    movw    r5, #23040
    movw    r3, #1280
c2_prob_scale:
    muls    r5, r6, r5
    lsrs    r5, r5, #16
    adds    r0, r5, r3
    pop     {{r4-r7, pc}}

cave2_duel_roll:
    push    {{r4, r5, r6, r7, lr}}
    sub     sp, #4
    mov     r6, r0
    mov     r5, r1
    mov     r7, r3
    ldrb.w  r4, [r6, #0x123]
    ldrb.w  r3, [r5, #0x127]
    subs    r4, r4, r3
    lsls    r4, r4, #1
    tst     r7, #1
    beq     c2_duel_strength_done
    ldrb.w  r3, [r6, #0x122]
    ldrb.w  r2, [r5, #0x122]
    subs    r3, r3, r2
    adds    r4, r4, r3
c2_duel_strength_done:
    tst     r7, #2
    beq     c2_duel_facing_done
    mov     r0, r6
    ldrd    r1, r2, [r5, #4]
    bl      {SYM['GetRotPoint']:#x}
    mov     r1, r0
    mov     r0, r6
    movw    r2, #{cfg.head_on_angle:#x}
    bl      {SYM['IsFacing']:#x}
    cbz     r0, c2_duel_facing_done
    movw    r3, #{cfg.head_on_bonus * 2}
    adds    r4, r4, r3
c2_duel_facing_done:
    mov     r0, r4
    movs    r1, #0
    tst     r7, #4
    it      ne
    movne   r1, #1
    bl      cave2_duel_probability
    mov     r4, r0
    movw    r0, #10000
    bl      {SYM['Random']:#x}
    lsls    r0, r0, #8
    movs    r1, #100
    muls    r4, r1, r4
    cmp     r0, r4
    ite     lo
    movlo   r0, #1
    movhs   r0, #0
    add     sp, #4
    pop     {{r4, r5, r6, r7, pc}}

cave2_tduel:
    push    {{r4, lr}}
    ldrb.w  r0, [sb, #0x84]
    cmp     r0, #0
    beq     c2_td_win
    mov     r0, r4
    mov     r1, sb
    movs    r2, #0
    movs    r3, #3
    bl      cave2_duel_roll
    cmp     r0, #0
    beq     c2_td_lose
c2_td_win:
    pop     {{r4, lr}}
    movw    r1, #{T1_LIT & 0xffff:#x}
    movt    r1, #{T1_LIT >> 16:#x}
    movs    r0, #6
    bx      lr
c2_td_lose:
    pop     {{r4, lr}}
    mov     r0, r4
    movs    r1, #0
    ldrh    r2, [r4, #2]
    bl      {SYM['Trip']:#x}
    cmp     r0, #0
    bne     c2_td_out
    mov.w   r0, #-1
    str.w   r0, [r4, #0x144]
c2_td_out:
    b.w     {T1_EPI:#x}

cave2_sduel:
    push    {{r0, lr}}
    ldrb.w  r1, [r8, #0x84]
    cmp     r1, #0
    beq     c2_sd_win
    mov     r0, r5
    mov     r1, r8
    movs    r2, #0
    movs    r3, #7
    bl      cave2_duel_roll
    cmp     r0, #0
    beq     c2_sd_lose
c2_sd_win:
    pop     {{r0, lr}}
    movw    r2, #{T2_LIT & 0xffff:#x}
    movt    r2, #{T2_LIT >> 16:#x}
    movs    r3, #0xc
    bx      lr
c2_sd_lose:
    pop     {{r0, lr}}
    movs    r1, #1
    strh.w  r1, [r5, #0x158]
    ldrb.w  r0, [r5, #0x48]
    b.w     {T2_SKIP:#x}

cave2_foul:
    ldrb.w  r1, [r4, #0x123]
    adr     r12, cave2_anchor
    addw    r12, r12, #{CAVE2_FOUL_TABLE_REL}
    ldrh.w  r1, [r12, r1, lsl #1]
    muls    r0, r1, r0
    lsrs    r0, r0, #12
    mov     r6, r0
    movs    r0, #100
    bx      lr
"""


def cave1_econ_asm(market_base):
    """v6 economy hooks share CAVE1's nearby dispatcher and string block."""
    match_rel = MARKET_RODATA_OFFSET + MARKET_ECON_NAMES_OFFSET - (market_base - CAVE)
    season_rel = match_rel + len(MARKET_ECON_MATCH_NAME)
    spend_rel = season_rel + len(MARKET_ECON_SEASON_NAME)
    income_rel = spend_rel + len(MARKET_ECON_SPEND_NAME)
    return f"""
cave_econ_match:
    push    {{r0, r1, r4, lr}}
    adr     r0, cave_market_dl_open
    addw    r0, r0, #{match_rel}
    movs    r1, #0
    movs    r2, #0
    bl      cave_market_dispatch
    cmp     r0, #0
    pop     {{r0, r1, r4, lr}}
    bne     econ_match_done
    b.w     {ECON_SET_MATCH_CREDITS:#x}
econ_match_done:
    bx      lr

cave_econ_season:
    push    {{r4, r5, r6, lr}}
    mov     r5, r0
    mov     r6, r1
    adr     r0, cave_market_dl_open
    addw    r0, r0, #{season_rel}
    mov     r1, r5
    mov     r2, r6
    bl      cave_market_dispatch
    mov     r1, r0
    mov     r0, r5
    bl      {ECON_SEASON_NEXT_SEASON:#x}
    adr     r0, cave_market_dl_open
    addw    r0, r0, #{season_rel}
    movs    r1, #0
    movs    r2, #0
    bl      cave_market_dispatch
    pop     {{r4, r5, r6, pc}}

cave_econ_spend:
    push    {{r0, r1, r2, r3, r4, lr}}
    adr     r4, cave_market_dl_open
    movw    r3, #{market_base & 0xffff:#x}
    movt    r3, #{market_base >> 16:#x}
    subs    r4, r4, r3
    ldr     r2, [sp, #20]
    subs    r2, r2, r4
    movw    r3, #{CCREDITS_SUB_RETURN & 0xffff:#x}
    movt    r3, #{CCREDITS_SUB_RETURN >> 16:#x}
    cmp     r2, r3
    bne     econ_spend_call
    ldr     r2, [sp, #28]
    subs    r2, r2, r4
econ_spend_call:
    adr     r0, cave_market_dl_open
    addw    r0, r0, #{spend_rel}
    bl      cave_market_dispatch
    pop     {{r0, r1, r2, r3, r4, lr}}
    movw    r2, #0xa7cc
    b.w     {ECON_SPEND_RESUME:#x}

cave_econ_income:
    push    {{r0, r1, r2, r3, r4, lr}}
    adr     r4, cave_market_dl_open
    movw    r3, #{market_base & 0xffff:#x}
    movt    r3, #{market_base >> 16:#x}
    subs    r4, r4, r3
    ldr     r2, [sp, #20]
    subs    r2, r2, r4
    movw    r3, #{CCREDITS_ADD_RETURN & 0xffff:#x}
    movt    r3, #{CCREDITS_ADD_RETURN >> 16:#x}
    cmp     r2, r3
    bne     econ_income_call
    ldr     r2, [sp, #28]
    subs    r2, r2, r4
econ_income_call:
    adr     r0, cave_market_dl_open
    addw    r0, r0, #{income_rel}
    bl      cave_market_dispatch
    pop     {{r0, r1, r2, r3, r4, lr}}
    push    {{r7, lr}}
    movw    ip, #0xa7cc
    b.w     {ECON_INCOME_RESUME:#x}
"""


# Control-tier pools. The A2 cave (SetAnimFromStateLoco) applies them, but that matcher only scores the
# state 0/4 lists, so these category-1 clips never reach it; anim_fit.py applies the same pools in
# CPlayer::SetAnimControl, the selector that actually picks control/dribble touches.
DRIBBLE_STYLE_POOLS = {
    # Cover every authored locomotion dribble candidate, including the
    # alternate takes that previously kept an unweighted direction score.
    "low": ((2245, 2248), (2257, 2258), (2265, 2284)),
    "mid": ((2249, 2252), (2261, 2264), (2285, 2288)),
    "high": ((2253, 2256), (2259, 2260), (2289, 2290)),
}


def cave_asm(cfg):
    lit_touch = struct.unpack_from("<I", ORIG, va2off(P4_LIT))[0]
    s = SYM
    style_pools = DRIBBLE_STYLE_POOLS

    def style_range_checks(ranges, target, prefix):
        lines = []
        for index, (first, last) in enumerate(ranges):
            next_label = f"{prefix}_next_{index}"
            lines.extend((
                f"    movw    r0, #{first}",
                "    subs.w  r1, sl, r0",
                f"    cmp     r1, #{last - first}",
                f"    bhi     {next_label}",
                f"    b.n     {target}",
                f"{next_label}:",
            ))
        return "\n".join(lines)

    tier_style_asm = f"""
drib_style_low:
{style_range_checks(style_pools['low'], 'drib_style_preferred', 'low_preferred')}
{style_range_checks(style_pools['mid'] + style_pools['high'], 'drib_style_secondary', 'low_secondary')}
    b       drib_style_score
drib_style_mid:
{style_range_checks(style_pools['mid'], 'drib_style_preferred', 'mid_preferred')}
{style_range_checks(style_pools['low'] + style_pools['high'], 'drib_style_secondary', 'mid_secondary')}
    b       drib_style_score
drib_style_high:
{style_range_checks(style_pools['high'], 'drib_style_preferred', 'high_preferred')}
{style_range_checks(style_pools['low'] + style_pools['mid'], 'drib_style_secondary', 'high_secondary')}
    b       drib_style_score
drib_style_preferred:
    movw    r0, #{cfg.dribble_style_bias:#x}
    subs.w  r5, r5, r0
    subs.w  r5, r5, r0
    b       drib_style_score
drib_style_secondary:
    movw    r0, #{cfg.dribble_style_bias // 2:#x}
    add.w   r5, r5, r0
"""
    return f"""
cave_sprint:
    ldr     r5, [r4, #8]
    movs    r6, #0
    cmp     r5, #0
    beq     sp_store
    ldrh    r0, [r4, #0x7c]
    movw    r1, #0xffff
    cmp     r0, r1
    beq     sp_store
    ldr.w   r0, [r5, #0x114]
    cmp     r0, #0
    beq     sp_store
    ldrb.w  r1, [r4, #0x54]
    cmp     r1, #0
    bne     sp_stam_ok
    cmp.w   r0, #{cfg.sprint_min_stamina:#x}
    ble     sp_store
sp_stam_ok:
    bl      {s['InOpenPlay']:#x}
    cmp     r0, #0
    beq     sp_store
    ldrb    r0, [r4, #5]
    bl      is_cc
    cmp     r0, #0
    bne     sp_store
    ldr.w   r0, [r4, #0x80]
    ldrb.w  r1, [r4, #0x54]
    movw    r2, #{cfg.sprint_on:#x}
    cmp     r1, #0
    beq     sp_cmp
    movw    r2, #{cfg.sprint_off:#x}
sp_cmp:
    cmp     r0, r2
    it      ge
    movge   r6, #1
sp_store:
    strb.w  r6, [r4, #0x54]
    movs    r0, #0
    strh.w  r0, [r4, #0x70]
    b.w     {P1_RET:#x}

is_cc:
    push    {{r4, r5, r6, lr}}
    mov     r4, r0
    bl      {s['GameTouchTouching']:#x}
    cmp     r0, #0
    beq     cc_out
    movs    r5, #0
cc_loop:
    mov     r0, r4
    mov     r1, r5
    bl      {s['GetButtonDown']:#x}
    cmp     r0, #0
    bne     cc_no
    adds    r5, #1
    cmp     r5, #9
    blt     cc_loop
    movs    r0, #1
    b       cc_out
cc_no:
    movs    r0, #0
cc_out:
    pop     {{r4, r5, r6, pc}}

cave_drib_urg:
    push    {{r4, r5, r6, lr}}
    mov     r4, r0
    mov     r5, r1
    ldrb.w  r0, [r4, #0x54]
    cmp     r0, #0
    beq     du_cc
    mov.w   r0, #0x1000
    b       du_out
du_cc:
    ldrb    r0, [r4, #5]
    bl      is_cc
    cmp     r0, #0
    beq     du_jog
    ldrb.w  r0, [r5, #0x127]
    cmp     r0, #99
    it      gt
    movgt   r0, #99
    cmp     r0, #40
    ble     du_cc_low
    subs    r0, #40
    movw    r2, #{cfg.cc_urg_best - cfg.cc_urg_worst - 40}
    muls    r0, r2, r0
    movw    r2, #1111
    muls    r0, r2, r0
    lsrs    r0, r0, #16
    addw    r0, r0, #{cfg.cc_urg_worst + 40}
    b       du_out
du_cc_low:
    addw    r0, r0, #{cfg.cc_urg_worst}
    b       du_out
du_jog:
    mov.w   r0, #0x800
du_out:
    pop     {{r4, r5, r6, pc}}

cave_offball:
    push    {{r4, r5, r6, lr}}
    mov     r4, r0
    mov     r5, r1
    ldrb.w  r0, [r4, #0x54]
    cmp     r0, #0
    beq     ob_cc
    mov.w   r1, #0x1000
    b       ob_set
ob_cc:
    ldrb    r0, [r4, #5]
    bl      is_cc
    cmp     r0, #0
    beq     ob_out
    movw    r1, #{cfg.cc_off_urg:#x}
ob_set:
    mov     r0, r5
    bl      {s['SetUrgency']:#x}
ob_out:
    pop     {{r4, r5, r6, pc}}

cave_touch:
    ldrsh.w r1, [r5, #0x7c]
    cmp.w   r1, #0x600
    bge     tc_done
    cmp     r1, #0
    it      lt
    movlt   r1, #0
    lsls    r1, r1, #1
    add.w   r1, r1, #0x400
    muls    r0, r1, r0
    asrs    r0, r0, #12
tc_done:
    mov     r5, r0
    movw    r4, #{lit_touch & 0xffff:#x}
    movt    r4, #{lit_touch >> 16:#x}
    bx      lr

cave_blend:
    push    {{r1, lr}}
    ldrb.w  r2, [r4, #0x125]
    ldrb.w  r3, [r4, #0x127]
    add     r2, r3
    lsrs    r2, r2, #1
    cmp     r2, #99
    it      gt
    movgt   r2, #99
    subs    r2, #40
    movw    r3, #{cfg.blend_step_best - cfg.blend_step_worst}
    muls    r2, r3, r2
    movw    r3, #1111
    muls    r2, r3, r2
    asrs    r2, r2, #16
    addw    r2, r2, #{cfg.blend_step_worst}
    subs    r0, r0, r2
    it      lt
    movlt   r0, #0
    pop     {{r1, pc}}
    nop
    nop
    nop

cave_dribble_style:
    movw    r0, #2245
    cmp.w   sl, r0
    bhs     drib_style_id_min_ok
    b       drib_style_score
drib_style_id_min_ok:
    movw    r0, #2290
    cmp.w   sl, r0
    bls     drib_style_id_max_ok
    b       drib_style_score
drib_style_id_max_ok:
    ldr     r0, [sp, #0x18]
    ldrb.w  r0, [r0, #0x127]
    cmp     r0, #{cfg.dribble_skill_low}
    bhs     drib_style_control_ge_low
    b       drib_style_low
drib_style_control_ge_low:
    cmp     r0, #{cfg.dribble_skill_high}
    blo     drib_style_mid
    b       drib_style_high
{tier_style_asm}
drib_style_score:
    cmp     r5, fp
    blt     drib_style_better
    b.w     0x2e0f88
drib_style_better:
    b.w     0x2e0f78
"""


KS = Ks(KS_ARCH_ARM, KS_MODE_THUMB)
CS = Cs(CS_ARCH_ARM, CS_MODE_THUMB)


def asm(src, addr):
    # an unformatted f-string placeholder (e.g. doubled braces) made keystone silently drop the
    # dlclose stub's movw/movt lines, which crashed the game at boot
    if re.search(r"#\s*\{", src):
        sys.exit(f"unformatted placeholder in assembly at {addr:#x}: {re.search(r'.*#\s*\{.*', src).group().strip()}")
    enc, count = KS.asm(src, addr)
    # keystone counts every statement: each line (labels and blanks too) plus each extra ';' on it
    stmts = sum(1 + ln.count(";") for ln in src.splitlines())
    if count != stmts:
        sys.exit(f"keystone assembled {count} of {stmts} statements at {addr:#x} (a line was silently skipped)")
    return bytes(enc)


def label_addr(src, label, addr):
    """Address of a label = size of code assembled before it."""
    pre = src.split(label + ":")[0]
    return addr + len(asm(pre, addr)) if pre.strip() else addr


def branch_to(target, addr, mnemonic="b.w"):
    """Encode a branch, then verify Keystone's actual decoded destination."""
    for candidate in range(target - 0x20, target + 0x22, 2):
        try:
            blob = asm(f"{mnemonic} {candidate:#x}", addr)
        except Exception:
            continue
        ins = next(CS.disasm(blob, addr), None)
        if ins is None or not ins.mnemonic.startswith("b") or not ins.op_str.startswith("#0x"):
            continue
        if int(ins.op_str[1:], 16) == target:
            return blob
    sys.exit(f"could not encode {mnemonic} to {target:#x} from {addr:#x}")


def correct_market_resume_branches(code, targets):
    """Correct far exits where Keystone misaligns B.W targets."""
    for target in targets:
        insns = [ins for ins in CS.disasm(code, CAVE)
                 if ins.mnemonic in ("b.w", "bl") and ins.op_str.startswith("#0x")
                 and abs(int(ins.op_str[1:], 16) - target) <= 0x20]
        if len(insns) != 1:
            sys.exit(f"expected one cave exit near {target:#x}; found {len(insns)}")
        ins = insns[0]
        encoded = branch_to(target, ins.address, ins.mnemonic)
        start = ins.address - CAVE
        code[start:start + len(encoded)] = encoded
    call_sites = [ins for ins in CS.disasm(code, CAVE)
                  if ins.mnemonic == "blx" and ins.op_str.startswith("#0x")
                  and abs(int(ins.op_str[1:], 16) - MARKET_SCREEN_REPLAY_CALL) <= 0x20]
    if len(call_sites) != 1:
        sys.exit(f"expected one transfer-screen replay call; found {len(call_sites)}")
    call = call_sites[0]
    encoded = branch_to(MARKET_SCREEN_REPLAY_CALL, call.address, "blx")
    start = call.address - CAVE
    code[start:start + len(encoded)] = encoded


def build_parser():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sprint-on", type=lambda x: int(x, 0), default=0x3800)
    ap.add_argument("--sprint-off", type=lambda x: int(x, 0), default=0x3200)
    ap.add_argument("--sprint-min-stamina", type=lambda x: int(x, 0), default=0x21C00,
                    help="stamina needed to START a sprint (game's own value; max is 0x87000)")
    ap.add_argument("--cc-urg-worst", type=lambda x: int(x, 0), default=0x280,
                    help="close-control pace at control 0; ratings 0..40 add 1 urgency each")
    ap.add_argument("--cc-urg-best", type=lambda x: int(x, 0), default=0x500,
                    help="close-control pace for control>=99 (jog is 0x800)")
    ap.add_argument("--cc-off-urg", type=lambda x: int(x, 0), default=0x500)
    ap.add_argument("--stat-knee", type=float, default=7.0,
                    help="softplus knee width for raw stats below 56")
    ap.add_argument("--shot-error-worst-deg", type=float, default=12.0)
    ap.add_argument("--shot-error-best-deg", type=float, default=0.8)
    ap.add_argument("--pass-error-worst-deg", type=float, default=5.0)
    ap.add_argument("--pass-error-best-deg", type=float, default=0.6)
    ap.add_argument("--duel-scale", type=float, default=7.0,
                    help="logistic scale for standing and slide duels")
    ap.add_argument("--slide-shift", type=float, default=3.0,
                    help="z penalty for slide duels")
    ap.add_argument("--foul-softness", type=float, default=0.1,
                    help="soft clipping width for the tackling foul factor")
    ap.add_argument("--head-on-bonus", type=int, default=8,
                    help="z points added to a standing duel when the defender faces the carrier")
    ap.add_argument("--head-on-angle", type=lambda x: int(x, 0), default=0x800,
                    help="half-angle for the standing-tackle facing check (0x800 = 45 degrees)")
    ap.add_argument("--blend-step-worst", type=lambda x: int(x, 0), default=0x266,
                    help="anim crossfade decay per tick at (agility+control)/2 = 40 (0x2000 total; stock 0x400 = "
                         "8 ticks); the line continues below 40 (no clamp)")
    ap.add_argument("--blend-step-best", type=lambda x: int(x, 0), default=0x3A0,
                    help="anim crossfade decay per tick for agility>=99")
    ap.add_argument("--dribble-skill-low", type=int, default=60,
                    help="control rating where the low and mid 2245-2290 pools cross (both bias 0): low-pool bias "
                         "rises linearly with control from here; also the A2 cave / --no-deek-style-fit threshold")
    ap.add_argument("--dribble-skill-high", type=int, default=85,
                    help="control rating where the mid and high 2245-2290 pools cross: high-pool bias falls "
                         "linearly with control through here; also the A2 cave / --no-deek-style-fit threshold")
    ap.add_argument("--dribble-style-bias", type=lambda x: int(x, 0), default=0xC000,
                    help="style score bias separating standard, CONTROL, and DRIB tiers (A2 cave)")
    ap.add_argument("--no-kick-fit", action="store_true",
                    help="keep the stock kick-selection flag ladder (control 75 / 80 / 90)")
    ap.add_argument("--no-deek-roll-fit", action="store_true",
                    help="keep the stock deek success roll (control-30 / control-40, 0 %% below 30 / 40)")
    ap.add_argument("--no-anim-fit", action="store_true",
                    help="do not distribute appended DLS26 clips by ability (anim_fit.py)")
    ap.add_argument("--ctrl-fit-tier-step", type=lambda x: int(x, 0), default=0x28,
                    help="SetAnimControl score per control point for the stock 2245-2290 pools: low + step*(c-low), "
                         "mid + step*(|c-(low+high)/2|-(high-low)/2), high + step*(high-c) (random term 0x400)")
    ap.add_argument("--ctrl-fit-close-mid", type=int, default=75,
                    help="close-control clips 2553-2558: control with no bias (preferred above, penalised below)")
    ap.add_argument("--ctrl-fit-close-step", type=lambda x: int(x, 0), default=0x28,
                    help="close-control clips 2553-2558: score + step * (close-mid - control)")
    ap.add_argument("--ctrl-fit-aerial-mid", type=int, default=85,
                    help="aerial control clips 2559/2560: control with no bias")
    ap.add_argument("--ctrl-fit-aerial-step", type=lambda x: int(x, 0), default=0x40,
                    help="aerial control clips 2559/2560: score + step * (aerial-mid - control)")
    ap.add_argument("--no-ctrl-fit-stock", action="store_true",
                    help="apply only the appended-clip rules in SetAnimControl, not the stock 2245-2290 pools")
    ap.add_argument("--deek-fit-high-lo", type=int, default=25,
                    help="deeks 2547-2550 kept with probability ramping linearly from 1/64 at this control ...")
    ap.add_argument("--deek-fit-high-hi", type=int, default=99, help="... to 1 at this control")
    ap.add_argument("--deek-fit-mid-lo", type=int, default=5,
                    help="deeks 2551/2552 kept with probability ramping from 1/64 at this control ...")
    ap.add_argument("--deek-fit-mid-hi", type=int, default=80, help="... to 1 at this control")
    ap.add_argument("--no-deek-style-fit", action="store_true",
                    help="keep the stock deek style pick (steps at --dribble-skill-low/high) instead of anim_fit's "
                         "continuous one")
    ap.add_argument("--deek-style1-lo", type=int, default=25,
                    help="deek style 1 (most skilful) probability ramps from 1%% at this control to 34%% at 99")
    ap.add_argument("--deek-style0-lo", type=int, default=10,
                    help="deek style 0 probability ramps from 2%% at this control to 33%% at 99 (style 2 = rest)")
    ap.add_argument("--lunge-tackling-lo", type=int, default=30,
                    help="lunge tackle 2535/2536 allowed with probability ramping from 1/64 at this tackling ...")
    ap.add_argument("--lunge-tackling-hi", type=int, default=95,
                    help="... to 1 at this tackling (else +0x40000: used only when no stock clip fits)")
    ap.add_argument("--stumble-strength-mid", type=int, default=60,
                    help="strength at which appended stumbles split evenly with their templates")
    ap.add_argument("--stumble-strength-span", type=int, default=40,
                    help="strength distance from the mid where the stumble bias reaches the full +-32 tie-break")
    ap.add_argument("--no-retune", action="store_true", help="keep stock stat->value ranges")
    ap.add_argument("--no-extend", action="store_true", help="do not add CAVE3/MODDATA segments (v7 layout)")
    ap.add_argument("--anim-capacity", type=int, default=3840,
                    help="CAnimLib clip capacity (stock 2535, max 3854)")
    ap.add_argument("--anim-count", type=int, default=2535,
                    help="clips that exist in anims.pak (0000..count-1.sat); loaded at boot")
    ap.add_argument("--dev-records-cap", type=int, default=256, choices=range(65, 257), metavar="65..256",
                    help="player-development records kept (stock 64)")
    ap.add_argument("--custom-image-max", type=int, default=512, choices=(512, 1024, 2048),
                    help="max custom logo/kit image size in px (stock 512; larger is untested on device)")
    ap.add_argument("--potw-max", type=int, default=64, choices=range(16, 256), metavar="16..255",
                    help="POTW roster entries (stock 16; array moved to MODDATA, see limits_caps.py)")
    ap.add_argument("--min-squad", type=int, default=16, choices=range(11, 32), metavar="11..31",
                    help="minimum squad a club may sell down to (stock 16; CTransfers::CanRemovePlayer)")
    ap.add_argument("--stock-ai", action="store_true",
                    help="keep the CPU penalty keeper's shot read and the difficulty rubber band (see ai_fair.py)")
    ap.add_argument("--dd-step", type=int, default=2, choices=range(0, 256), metavar="0..255",
                    help="post-match dynamic difficulty per goal of margin (stock 5; ai_fair.py)")
    ap.add_argument("--created-max", type=int, default=255, choices=range(32, 256), metavar="32..255",
                    help="created-player id slots (stock 32; ids 0xFFDE-N..0xFFDD, see created_caps.py)")
    ap.add_argument("--squad-max", type=int, default=64, choices=range(32, 65), metavar="32..64",
                    help="user squad size (stock 32; overflow tables in MODDATA, save version 0xB6, see squad_caps.py)")
    ap.add_argument("--out", default=str(Path(__file__).parent / "build/lib/armeabi-v7a/libDLS18.so"))
    ap.add_argument("--list", action="store_true", help="print disassembly of all patches")
    import anim_motion
    anim_motion.add_args(ap)
    return ap


def main():
    ap = build_parser()
    cfg = ap.parse_args()

    if cfg.created_max < cfg.squad_max:
        ap.error("--created-max must be at least --squad-max (live created players are bounded by the squad)")
    if cfg.no_extend and cfg.squad_max > 32:
        ap.error("--squad-max above 32 needs the extended library (drop --no-extend)")
    if cfg.stat_knee <= 0 or cfg.duel_scale <= 0 or cfg.foul_softness <= 0:
        ap.error("stat knee, duel scale, and foul softness must be positive")
    if cfg.slide_shift < 0:
        ap.error("--slide-shift cannot be negative")
    if cfg.slide_shift > 127.5:
        ap.error("--slide-shift must fit the half-stat z scale")
    if (cfg.cc_urg_worst < 0 or cfg.cc_urg_worst + 40 > 0xFFF or
            cfg.cc_urg_best - cfg.cc_urg_worst < 99 or cfg.cc_urg_best > 0xFFFF):
        ap.error("close-control urgency must fit the ARM immediates and rise by at least 99 across control 0..99")
    if not 0 <= cfg.head_on_bonus <= 0x7FFF:
        ap.error("--head-on-bonus must be between 0 and 32767")
    if not 1 <= cfg.head_on_angle <= 0x2000:
        ap.error("--head-on-angle must be between 1 and 0x2000 (180 degrees)")
    if not 0 < cfg.shot_error_best_deg < cfg.shot_error_worst_deg:
        ap.error("shot error degrees must satisfy 0 < best < worst")
    if not 0 < cfg.pass_error_best_deg < cfg.pass_error_worst_deg:
        ap.error("pass error degrees must satisfy 0 < best < worst")
    if not 40 <= cfg.dribble_skill_low < cfg.dribble_skill_high <= 99:
        ap.error("dribble skill thresholds must satisfy 40 <= low < high <= 99")
    if not 0 <= cfg.dribble_style_bias <= 0xFFFF:
        ap.error("--dribble-style-bias must fit in 16 bits")
    if cfg.blend_step_worst * 59 - 40 * (cfg.blend_step_best - cfg.blend_step_worst) < 0x100 * 59:
        ap.error("--blend-step-worst/best: the crossfade line continued to (agility+control)/2 = 0 must stay >= 0x100")
    import anim_fit
    anim_fit.params(cfg)          # validates the --ctrl-fit-*, --deek-*, --lunge-*, --stumble-* curves

    lib = bytearray(ORIG)
    for site, exp in EXPECT.items():
        got = bytes(lib[va2off(site):va2off(site) + len(exp)])
        if got != exp:
            sys.exit(f"unexpected bytes at {site:#x}: {got.hex()} (wrong game version?)")

    core_src = cave_asm(cfg)
    core_code = asm(core_src, CAVE)
    market_base = CAVE + len(core_code)
    ext_src = market_asm(market_base) + cave1_econ_asm(market_base)
    src = core_src + ext_src
    code = bytearray(core_code + asm(ext_src, market_base))
    if len(code) > MARKET_RODATA_OFFSET:
        sys.exit(f"cave code overlaps career strings: {len(code)} > {MARKET_RODATA_OFFSET}")
    if (MARKET_RODATA_OFFSET - len(code)) % 2:
        sys.exit("career strings would not be aligned after the NOP padding")
    correct_market_resume_branches(code, (MARKET_SERIALIZE_HOOK + 4,
                                          MARKET_TURN_HOOK + 4,
                                          MARKET_SCREEN_HOOK + 4,
                                          STOCK_BUY_SEARCH_SKIP,
                                          STOCK_BUY_SCOUT_SKIP,
                                          STOCK_SELL_SKIP,
                                          ECON_SET_MATCH_CREDITS,
                                          ECON_SEASON_NEXT_SEASON,
                                          ECON_SPEND_RESUME,
                                          ECON_INCOME_RESUME))
    # keystone silently mis-encodes far conditional branches (bge.w to the game crashed it), so every
    # branch that leaves the cave must land on an address literally named in the source
    wanted = {int(t, 16) for t in re.findall(r"^\s*b\S*\s+(0x[0-9a-fA-F]+)\s*$", src, re.M)}
    for ins in CS.disasm(code, CAVE):
        if ins.mnemonic.startswith("b") and ins.op_str.startswith("#0x"):
            tgt = int(ins.op_str[1:], 16)
            if not CAVE <= tgt < CAVE + CAVE_SIZE and tgt not in wanted:
                sys.exit(f"cave branch at {ins.address:#x} ({ins.mnemonic} {ins.op_str}) has an unexpected target")
    # the library is position-independent (random load base): never bx/blx to a movw/movt-built address
    const_regs = set()
    for ins in CS.disasm(code, CAVE):
        reg = ins.op_str.split(",")[0].strip()
        if ins.mnemonic in ("movw", "movt"):
            const_regs.add(reg)
        elif ins.mnemonic in ("bx", "blx") and reg in const_regs:
            sys.exit(f"cave {ins.mnemonic} {reg} at {ins.address:#x} jumps to an absolute address; use b.w/bl")
        elif reg in const_regs:
            const_regs.discard(reg)
    lab = {n: label_addr(src, n, CAVE) for n in ("cave_sprint", "cave_drib_urg", "cave_offball", "cave_touch",
                                                   "cave_blend",
                                                   "cave_dribble_style", "cave_market_dispatch",
                                                   "cave_market_serialize",
                                                   "cave_market_turn", "cave_market_screen",
                                                   "cave_market_buy_search", "cave_market_buy_scout",
                                                   "cave_market_sell", "cave_econ_match", "cave_econ_season",
                                                   "cave_econ_spend", "cave_econ_income")}

    # CAVE2: the stat, error and duel routines use the reserved second cave; the newly freed CAVE1
    # tail holds its existing economy hooks and nearby strings.
    cave2_src = cave2_asm(cfg)
    cave2_code = bytearray(asm(cave2_src, CAVE2))
    if len(cave2_code) > CAVE2_RODATA_OFFSET:
        sys.exit(f"CAVE2 code overlaps its rodata: {len(cave2_code)} > {CAVE2_RODATA_OFFSET}")
    if (CAVE2_RODATA_OFFSET - len(cave2_code)) % 2:
        sys.exit("CAVE2 rodata would not be aligned after the NOP padding")
    # Every far bl/b.w in cave2_src names its numeric target literally (game routines and CAVE1 /
    # career-market routines). Re-encode each one and verify Keystone's
    # actual decoded destination exactly, the same way correct_market_resume_branches does for CAVE -
    # this caught Keystone misencoding the CSeason::NextSeason bl by +4 bytes during development.
    wanted2 = {int(t, 16) for t in re.findall(r"^\s*b\S*\s+(0x[0-9a-fA-F]+)\s*$", cave2_src, re.M)}
    for target in wanted2:
        insns = [ins for ins in CS.disasm(cave2_code, CAVE2)
                 if ins.mnemonic in ("bl", "b.w") and ins.op_str.startswith("#0x")
                 and abs(int(ins.op_str[1:], 16) - target) <= 0x20]
        if not insns:
            sys.exit(f"expected at least one CAVE2 branch near {target:#x}; found none")
        for ins in insns:
            encoded = branch_to(target, ins.address, ins.mnemonic)
            start = ins.address - CAVE2
            cave2_code[start:start + len(encoded)] = encoded
    # every branch leaving CAVE2 must land inside CAVE2 or on one of the now-corrected far targets
    for ins in CS.disasm(cave2_code, CAVE2):
        if ins.mnemonic.startswith("b") and ins.op_str.startswith("#0x"):
            tgt = int(ins.op_str[1:], 16)
            if not (CAVE2 <= tgt < CAVE2 + CAVE2_SIZE or tgt in wanted2):
                sys.exit(f"CAVE2 branch at {ins.address:#x} ({ins.mnemonic} {ins.op_str}) has an unexpected target")
    const_regs2 = set()
    for ins in CS.disasm(cave2_code, CAVE2):
        reg = ins.op_str.split(",")[0].strip()
        if ins.mnemonic in ("movw", "movt"):
            const_regs2.add(reg)
        elif ins.mnemonic in ("bx", "blx") and reg in const_regs2:
            sys.exit(f"CAVE2 {ins.mnemonic} {reg} at {ins.address:#x} jumps to an absolute address; use b.w/bl")
        elif reg in const_regs2:
            const_regs2.discard(reg)
    lab2 = {n: label_addr(cave2_src, n, CAVE2)
            for n in ("cave2_anchor", "cave2_stat", "cave2_err_entry", "cave2_err_exit",
                      "cave2_duel_probability", "cave2_duel_roll", "cave2_tduel", "cave2_sduel",
                      "cave2_foul")}
    cave2_data = cave2_rodata(cfg)

    patches = {
        CAVE: (code + b"\x00\xbf" * ((MARKET_RODATA_OFFSET - len(code)) // 2)
               + MARKET_RODATA),
        P1: asm(f"b.w {lab['cave_sprint']:#x}", P1),
        P2: asm(f"mov r0, sl\nmov r1, r4\nbl {lab['cave_drib_urg']:#x}\nmov r1, r0\nmov r0, r4\n"
                f"b.w {P2_RET:#x}", P2),
        P3: asm(f"mov r0, fp\nmov r1, r4\nbl {lab['cave_offball']:#x}\nb.w {P3_RET:#x}", P3),
        P4: asm(f"bl {lab['cave_touch']:#x}", P4),
        S1: asm(f"bl {lab2['cave2_stat']:#x}", S1),
        S2: asm(f"b.w {lab2['cave2_err_entry']:#x}", S2),
        S3: asm(f"b.w {lab2['cave2_err_exit']:#x}", S3),
        T1: asm(f"bl {lab2['cave2_tduel']:#x}", T1),
        T2: asm(f"bl {lab2['cave2_sduel']:#x}", T2),
        T3: asm(f"bl {lab2['cave2_foul']:#x}", T3),
        C1: asm("mul r4, r1, r1; mul r8, r0, r0; nop.w", C1),
        A1: asm(f"bl {lab['cave_blend']:#x}", A1),
        A2: asm(f"b.w {lab['cave_dribble_style']:#x}", A2),
        MARKET_SERIALIZE_HOOK: branch_to(lab["cave_market_serialize"], MARKET_SERIALIZE_HOOK),
        MARKET_TURN_HOOK: branch_to(lab["cave_market_turn"], MARKET_TURN_HOOK),
        STOCK_BUY_SEARCH_HOOK: branch_to(lab["cave_market_buy_search"], STOCK_BUY_SEARCH_HOOK),
        STOCK_BUY_SCOUT_HOOK: branch_to(lab["cave_market_buy_scout"], STOCK_BUY_SCOUT_HOOK),
        STOCK_SELL_HOOK: branch_to(lab["cave_market_sell"], STOCK_SELL_HOOK),
        MARKET_SCREEN_HOOK: branch_to(lab["cave_market_screen"], MARKET_SCREEN_HOOK),
        CAVE2: (bytes(cave2_code) + b"\x00\xbf" * ((CAVE2_RODATA_OFFSET - len(cave2_code)) // 2)
                + cave2_data),
        ECON_MATCH_HOOK: branch_to(lab["cave_econ_match"], ECON_MATCH_HOOK, "bl"),
        ECON_SEASON_HOOK: branch_to(lab["cave_econ_season"], ECON_SEASON_HOOK, "bl"),
        SAVE_VERSION_SETUP: asm("movs r1, #0xb7", SAVE_VERSION_SETUP),   # 0xb5 v7 books, 0xb6 squad64, 0xb7 v39 chemistry
        GET_CREDITS: asm("movw r1, #0xa7cc; movt r1, #2", GET_CREDITS),     # stock: real coin balance
        SAVE_VERSION_BOOT: asm("movs r1, #0xb7", SAVE_VERSION_BOOT),
        VIDEO_DOUBLER: asm("movs r0, #0; bx lr", VIDEO_DOUBLER),         # v7: no coins for adverts
        DEV_RECORDS_CAP: asm(f"cmp r0, #{cfg.dev_records_cap - 1}", DEV_RECORDS_CAP),   # v8: 255 records
        CUSTOM_IMAGE_MAX: asm(f"mov.w r4, #{cfg.custom_image_max}", CUSTOM_IMAGE_MAX),
        ECON_SPEND_HOOK: branch_to(lab["cave_econ_spend"], ECON_SPEND_HOOK),
        ECON_INCOME_HOOK: branch_to(lab["cave_econ_income"], ECON_INCOME_HOOK) + bytes.fromhex("00bf"),
        # coin shop -> plain "not enough coins" box (data/COIN_FLOWS.md section 4)
        SHOP_CTOR_BUTTONS: bytes.fromhex("cde90001"),
        SHOP_CTOR_TEXT: bytes.fromhex("2a46"),
        SHOP_CTOR_KIND: bytes.fromhex("00bf"),
        SHOP_CTOR_TITLE: bytes.fromhex("40f2a160"),
        SHOP_CTOR_SETUP: bytes.fromhex("40e0"),
        HEADER_COIN_TAP: bytes.fromhex("13e0"),
        REWARD_VIDEO_GRANT: bytes.fromhex("10e0"),
    }
    # A3: the stock deek style pick has steps at two control thresholds. anim_fit's style_fit replaces the
    # whole block (0x2e4152..0x2e4164) with a continuous pick, so these threshold patches only apply
    # when that hook is off.
    if not anim_fit.style_hook_active(cfg):
        patches[SM_DRIBBLE_HIGH] = asm(f"cmp r0, #{cfg.dribble_skill_high}", SM_DRIBBLE_HIGH)
        patches[SM_DRIBBLE_LOW] = asm(f"cmp r0, #{cfg.dribble_skill_low}", SM_DRIBBLE_LOW)
    if not cfg.no_retune:
        patches.update(retune_patches())
    limits = {P2: P2_RET - P2, P3: P3_RET - P3, P4: 4, P1: P1_RET - P1, S1: 4, S2: 4, S3: 4, A1: 4, A2: 4,
              SM_DRIBBLE_HIGH: 2, SM_DRIBBLE_LOW: 2, T1: 4, T2: 4, T3: 4, C1: 12,
              MARKET_SERIALIZE_HOOK: 4, MARKET_TURN_HOOK: 4, MARKET_SCREEN_HOOK: 4,
              STOCK_BUY_SEARCH_HOOK: 4, STOCK_BUY_SCOUT_HOOK: 4, STOCK_SELL_HOOK: 4,
              ECON_MATCH_HOOK: 4, ECON_SEASON_HOOK: 4,
              SAVE_VERSION_SETUP: 2, SAVE_VERSION_BOOT: 2, GET_CREDITS: 8, VIDEO_DOUBLER: 4,
              ECON_SPEND_HOOK: 4, ECON_INCOME_HOOK: 6, SHOP_CTOR_BUTTONS: 4, SHOP_CTOR_TEXT: 2,
              SHOP_CTOR_KIND: 2, SHOP_CTOR_TITLE: 4, SHOP_CTOR_SETUP: 2, HEADER_COIN_TAP: 2, REWARD_VIDEO_GRANT: 2,
              DEV_RECORDS_CAP: 2, CUSTOM_IMAGE_MAX: 4}
    for site, blob in patches.items():
        if site in limits and len(blob) > limits[site]:
            sys.exit(f"patch at {site:#x} too long")
        o = va2off(site)
        lib[o:o + len(blob)] = blob
        if cfg.list:
            n = len(code) if site == CAVE else len(cave2_code) if site == CAVE2 else len(blob)
            print(f"--- {site:#x}")
            for i in CS.disasm(bytes(lib[o:o + n]), site):
                print(f"  {i.address:08x}: {i.mnemonic:8s} {i.op_str}")

    # v8: animation clip cache capacity (anim_caps.py; every site checked against the stock bytes)
    import anim_caps

    def _stock_insn(site):
        ins = next(CS.disasm(bytes(ORIG[va2off(site):va2off(site) + 4]), site))
        return ins.mnemonic, ins.op_str
    for site, blob in anim_caps.patches(_stock_insn, asm, cfg.anim_capacity, cfg.anim_count).items():
        o = va2off(site)
        lib[o:o + len(blob)] = blob

    # limits_v32: POTW roster + minimum squad (limits_caps.py; every site checked against the stock bytes)
    import limits_caps
    for site, blob in limits_caps.code_patches(ORIG, va2off, asm, cfg.potw_max, cfg.min_squad).items():
        o = va2off(site)
        if bytes(lib[o:o + len(blob)]) != bytes(ORIG[o:o + len(blob)]):
            sys.exit(f"limits_caps site {site:#x} was already patched by another change")
        lib[o:o + len(blob)] = blob
        if cfg.list:
            print(f"--- {site:#x}")
            for i in CS.disasm(blob, site):
                print(f"  {i.address:08x}: {i.mnemonic:8s} {i.op_str}")

    # ai_fair (v34): blind CPU penalty keeper, no in-match difficulty rubber band, smaller post-match steps
    if not cfg.stock_ai:
        import ai_fair
        for site, blob in ai_fair.code_patches(ORIG, va2off, asm, cfg.dd_step).items():
            o = va2off(site)
            if bytes(lib[o:o + len(blob)]) != bytes(ORIG[o:o + len(blob)]):
                sys.exit(f"ai_fair site {site:#x} was already patched by another change")
            lib[o:o + len(blob)] = blob
            if cfg.list:
                print(f"--- {site:#x}")
                for i in CS.disasm(blob, site):
                    print(f"  {i.address:08x}: {i.mnemonic:8s} {i.op_str}")

    # created_v33: created-player id range 32 -> created_max (created_caps.py; stock-checked sites)
    import created_caps
    for site, blob in created_caps.code_patches(ORIG, va2off, asm, cfg.created_max).items():
        o = va2off(site)
        if bytes(lib[o:o + len(blob)]) != bytes(ORIG[o:o + len(blob)]):
            sys.exit(f"created_caps site {site:#x} was already patched by another change")
        lib[o:o + len(blob)] = blob
        if cfg.list:
            print(f"--- {site:#x}")
            for i in CS.disasm(blob, site):
                print(f"  {i.address:08x}: {i.mnemonic:8s} {i.op_str}")

    # v8: extend the library with CAVE3 (code) + MODDATA (data) and emit the runtime C hooks
    # (elf_extend.py, modcore_build.py, career_market/modcore.c). No code-size limit for new hooks.
    layout = None
    if not cfg.no_extend:
        import json
        import elf_extend
        import modcore_build
        lib, layout = elf_extend.extend(lib)
        mc_info = modcore_build.emit(lib, layout, RUNTIME_HOOKS)
        elf_extend.check(lib, layout)
        Path(cfg.out + ".layout.json").write_text(json.dumps(
            dict(layout, hooks={k: {kk: hex(vv) for kk, vv in v.items()} for k, v in mc_info.items()}), indent=1))
    potw_buf = limits_caps.potw_reloc_patch(lib, ORIG, layout, cfg.potw_max)
    if potw_buf:
        print(f"POTW roster {cfg.potw_max} entries at MODDATA {potw_buf:#x} (GOT {limits_caps.POTW_GOT:#x})")
    # anim_guard (v35a): no divide-by-zero crash in CPlayer::SetAnimControl from appended clip data
    if layout is not None:
        import anim_guard
        guard = anim_guard.apply(lib, ORIG, va2off, layout, asm)
        print(f"anim_guard: {len(anim_guard.SITES)} divisions in SetAnimControl -> safe_idiv {guard:#x}")
    # anim_fit (v36): pick appended DLS26 clips by control / tackling / strength (anim_fit.py); refuses an
    # --anim-count that adds a clip without a distribution rule
    if layout is not None and not cfg.no_anim_fit:
        import anim_fit
        fit = anim_fit.apply(lib, ORIG, va2off, layout, asm, cfg, DRIBBLE_STYLE_POOLS)
        print("anim_fit: " + ", ".join(f"{k} {v:#x}" for k, v in fit.items()))
    elif cfg.anim_count > 2535:
        print(f"WARNING: {cfg.anim_count - 2535} appended clips are picked by geometry only (anim_fit not applied)")
    # anim_motion (v36): minimum crossfade into appended clips, capped by their contact tick; optional
    # per-clip cadence trim (anim_motion.py). Must follow the patch loop that writes A1.
    if layout is not None and not cfg.no_anim_motion:
        import anim_motion
        motion = anim_motion.apply(lib, ORIG, va2off, layout, asm, cfg)
        if motion:
            print(f"anim_motion: A1 -> {motion['blend']:#x} (chains {motion['chained']:#x}), "
                  f"{len(motion['table'])} clip rules" + (f", cadence {motion['trims']}" if "trims" in motion else ""))
    # squad_v33: user squad 32 -> squad_max (squad_caps.py; rewrites the save version to 0xB6 itself)
    import squad_caps
    squad = squad_caps.apply(lib, ORIG, layout, asm, cfg.squad_max, cfg.list)
    if squad:
        import json
        lay = json.loads(Path(cfg.out + ".layout.json").read_text())
        lay["squad_v33"] = {k: (hex(v) if isinstance(v, int) else v) for k, v in squad.items()
                            if k not in ("syms", "tramps", "stubs")}
        lay["squad_v33"]["syms"] = {k: hex(v) for k, v in squad["syms"].items()}
        lay["squad_v33"]["tramps"] = {k: hex(v) for k, v in squad["tramps"].items()}
        lay["squad_v33"]["stubs"] = {hex(k): hex(v) for k, v in squad["stubs"].items()}
        Path(cfg.out + ".layout.json").write_text(json.dumps(lay, indent=1))
        print(f"squad_v33: user squad {cfg.squad_max}, {squad['sites']} sites, save version {squad['save_version']:#x}")

    out = Path(cfg.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(lib)
    print(f"cave used {len(code)} code + {len(MARKET_RODATA)} data / {CAVE_SIZE} bytes; labels "
          + ", ".join(f"{k}={v:#x}" for k, v in lab.items()))
    print(f"cave2 used {len(cave2_code)} code + {len(cave2_data)} data / {CAVE2_SIZE} bytes; labels "
          + ", ".join(f"{k}={v:#x}" for k, v in lab2.items()))
    if layout:
        print(f"CAVE3 {layout['cave3_va']:#x} ({layout['cave3_size']:#x} bytes), MODDATA {layout['moddata_va']:#x} "
              f"({layout['moddata_size']:#x} bytes), runtime hooks: {len(RUNTIME_HOOKS)}")
    print(f"wrote {out}  sha256={hashlib.sha256(lib).hexdigest()[:16]}")


if __name__ == "__main__":
    main()
