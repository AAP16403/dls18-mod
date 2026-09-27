"""Distribute the appended DLS26 clips (IDs 2535-2560) by player ability (anim_fit, v37: continuous rules).

Every rule is a continuous function of the stat over the whole 1..99 range: linear score biases (no tier
switch, no jump) or linear probability ramps with a small floor (no clip is ever excluded outright).

Which native code picks each category (libDLS18 5.064, see mod/README.md "Ability-fit selection"):

  category 1  (control / dribble touches)  CPlayer::SetAnimControl, called from ControlTakeBall and
              DribbleBall. Each candidate is scored on its action point 0 against the projected ball
              (timing, angle and distance terms, lower is better) and random(1024) is added to scores
              that beat the best. NOTE: the older control-tier cave at A2 (0x2e0f74,
              CPlayer::SetAnimFromStateLoco) only sees the state 0 / 4 lists (walk/jog clips): every
              caller passes state 0 or 4, or r1 = -1 which no category-1 record matches (+0x6e is never
              -1). The tier IDs 2245-2290 are category-1 records, so that cave never affects them.
  category 19 (deeks)  GC_SpecialMoveDribbling picks a style (0/1/2) from control (stock: control >= 85
              -> random(3), >= 75 -> 50 % style 0, else style 2; replaced by style_fit below),
              NewPlayerStateXDeek -> GA_SetAnimFromDeek keeps only clips whose TAnimData+0x6e (record
              +0xe) equals the style.
  category 9  (standing tackles)  ACT_TackleSetPlayerState: per clip, its action point (contact point)
              is fitted to the predicted ball at the contact tick; a clip is rejected when the miss per
              tick exceeds the tackling-scaled reach (AttributeInterpolate stat 9), and the lowest
              distance + (angle << 6) wins. No random term.
  category 6  (stumbles)  CPlayer::Trip -> NewPlayerStateX(6) -> CPlayer::SetAnimFromStateGen:
              score = |type - record +0xe| << 14 + (direction difference << 3), random(32) added to
              scores that beat the best. Every appended stumble copies its template's type and direction,
              so it ties with the template and the random(32) term splits them.

Hooks (each checked against the stock bytes in the stock and the input library, branches verified):

  SetAnimControl        0x2e2c70  ldr r5,[sp,#0x6c]; cmp r6,r5          -> b.w ctrl_fit
  GA_SetAnimFromDeek    0x2e43de  style filter (14 bytes)               -> b.w deek_fit + nops
  ACT_TackleSetPlayerState 0x2ea7f4  lsl.w r0,fp,#6                     -> b.w tackle_fit
  SetAnimFromStateGen   0x2dd0fa  ldr r1,[sp,#0x18]; cmp sl,r1          -> b.w stumble_fit
  GC_SpecialMoveDribbling 0x2e4152  stock style pick (18 bytes)         -> b.w style_fit + nops
                                    (replaces build_mod's SM_DRIBBLE_HIGH/LOW threshold patches)

Every stub reproduces the displaced instructions and returns with an unconditional b.w to one of the
stock continuations (PATCHING_RULES 1/2). The code lives in CAVE3 [+0x20000, +0x24000). The stubs that call
XSYS_Random (0x3851ac, clobbers r0-r3, ip, lr) read every [sp, #..] operand before they push.

Rules (c = control CPlayer+0x127, t = tackling +0x123, s = strength +0x122; L/H = --dribble-skill-low/high,
ramp(x; lo, hi, top, floor) = clamp(((x - lo) * ceil(1024 * top / (hi - lo))) >> 10, floor, top)):
  close control 2553-2558   score + CLOSE_STEP * (CLOSE_MID - c)      (negative above the mid, positive below)
  aerial control 2559/2560  score + AERIAL_STEP * (AERIAL_MID - c)    (steeper, higher mid; never excluded)
  stock tiers 2245-2290     low pool  + TIER_STEP * (c - L)
                            mid pool  + TIER_STEP * (|c - (L+H)/2| - (H-L)/2)
                            high pool + TIER_STEP * (H - c)
                            (piecewise linear; low = mid at L, mid = high at H; --no-ctrl-fit-stock drops it)
  deeks 2547-2550           kept when XSYS_Random(64) < ramp(c; DEEK_HIGH_LO, DEEK_HIGH_HI, 64, 1)
  deeks 2551/2552           kept when XSYS_Random(64) < ramp(c; DEEK_MID_LO, DEEK_MID_HI, 64, 1)
  deek style pick           r = XSYS_Random(100): style 1 if r < T1, style 0 if r < T1 + T0, else style 2
                            T1 = ramp(c; STYLE1_LO, 99, 34, 1), T0 = ramp(c; STYLE0_LO, 99, 33, 2)
  lunge tackle 2535/2536    allowed when XSYS_Random(64) < ramp(t; LUNGE_LO, LUNGE_HI, 64, 1), else
                            score + 0x40000 (about 90 degrees: only chosen when no stock clip fits)
  stumbles 2537/2538 (recover and keep running)  score - b
  stumbles 2539-2546 (backward, long sideways, general falls)  score + b
                            b = clamp(((s - MID) * ceil(8192 / SPAN)) >> 8, -32, 32)
  The stumble bias stays inside the random(32) tie-break, so it only reweights a port against its
  same-type, same-direction template, never the stumble type or direction.
"""
import re

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs

import elf_extend

REGION_OFFSET = 0x20000          # CAVE3 + 0x20000 .. + 0x24000 belongs to anim_fit
REGION_SIZE = 0x4000
SLOT = 0x400                     # one stub per slot
XSYS_RANDOM = 0x3851AC           # _Z11XSYS_Randomi (Thumb, local to the library)
STOCK_ANIM_COUNT = 2535
LUNGE_PENALTY = 0x40000
STUMBLE_BIAS_LIMIT = 32          # the random(32) tie-break width in SetAnimFromStateGen
KEEP_FLOOR = 1                   # deek / lunge: minimum XSYS_Random(64) threshold (1/64, never excluded)
STYLE_TOP = {1: 34, 0: 33}       # style pick at control 99: 34 % style 1, 33 % style 0, 33 % style 2
STYLE_FLOOR = {1: 1, 0: 2}       # ... and at least 1 % / 2 % at any control

OFF_CONTROL, OFF_TACKLING, OFF_STRENGTH = 0x127, 0x123, 0x122

# Every appended clip needs exactly one rule; apply() refuses a build whose --anim-count adds an ID
# that is not listed here, and test_anim_fit.py checks the categories against the animation package.
RULES = {
    "tackle_lunge": dict(ids=(2535, 2536), category=9, picker="ACT_TackleSetPlayerState"),
    "stumble_recover": dict(ids=(2537, 2538), category=6, picker="SetAnimFromStateGen"),
    "stumble_dramatic": dict(ids=tuple(range(2539, 2547)), category=6, picker="SetAnimFromStateGen"),
    "deek_high": dict(ids=(2547, 2548, 2549, 2550), category=19, picker="GA_SetAnimFromDeek"),
    "deek_mid": dict(ids=(2551, 2552), category=19, picker="GA_SetAnimFromDeek"),
    "control_close": dict(ids=tuple(range(2553, 2559)), category=1, picker="SetAnimControl"),
    "control_aerial": dict(ids=(2559, 2560), category=1, picker="SetAnimControl"),
}


def coverage():
    """clip id -> rule name for every appended clip that has a distribution rule."""
    out = {}
    for name, rule in RULES.items():
        for clip in rule["ids"]:
            if clip in out:
                raise SystemExit(f"anim_fit: clip {clip} has two rules ({out[clip]}, {name})")
            out[clip] = name
    return out


# (site, stock bytes, stock disassembly, continuations the stub returns to)
HOOKS = {
    "ctrl_fit": dict(site=0x2E2C70, stock="1b9dae42", size=4, func=(0x2E28C0, 0x864),
                     cont=(0x2E2C76, 0x2E2C80)),
    "deek_fit": dict(site=0x2E43DE, stock="19f101001cbfb8f96e00484562d1", size=14, func=(0x2E425C, 0x3DC),
                     cont=(0x2E43EC, 0x2E44B2)),
    "tackle_fit": dict(site=0x2EA7F4, stock="4fea8b10", size=4, func=(0x2EA558, 0x470),
                       cont=(0x2EA800, 0x2EA864)),
    "stumble_fit": dict(site=0x2DD0FA, stock="06998a45", size=4, func=(0x2DD028, 0x12C),
                        cont=(0x2DD100, 0x2DD12E)),
    # ldrb.w r0,[r4,#0x127]; cmp r0,#85; blo; movs r0,#3; blx XSYS_Random; mov r6,r0; b 0x2e417e
    # (0x2e416c..0x2e417e, the 2-way pool, is only reached from the replaced blo and becomes dead)
    "style_fit": dict(site=0x2E4152, stock="94f82701552808d30320ddf646eb06460ce0", size=18,
                      func=(0x2E40D0, 0x18C), cont=(0x2E417E,)),
    # deek success roll: stock r5 = control-30 (style 0), control-40 (style 1), 50 (style 2), then
    # XSYS_Random(50) < r5 at 0x2e41dc. The whole r5 computation (0x2e41c2..0x2e41dc) is replaced.
    "roll_fit": dict(site=0x2E41C2, stock="04d0012e08d16ff0270001e06ff01d0094f82711451800e03225", size=26,
                     func=(0x2E40D0, 0x18C), cont=(0x2E41DC,)),
    # kick-selection flags: stock control >= 75 -> flag 4 half the time (8-frame blocks), >= 80 always,
    # >= 90 also 0x200 a quarter of the time (32-frame blocks). r1 = control, r6 = flags so far; the
    # continuation needs r0 = flags and r3 = [sp, #0x18]. 0x2e5f98..0x2e5fde is replaced.
    "kick_fit": dict(site=0x2E5F98, stock="502911d3069b46f004005a291bd32549794409680968ca1701ebd2614909890708"
                                          "bf46f401700ee0069b4b2930460ad31b48784400680068c11700ebd160042121ea"
                                          "d0003043",
                     size=0x46, func=(0x2E5DB4, 0x290), cont=(0x2E5FDE,)),
}
KICK_COUNTER_GOT = 0x73063C      # GOT slot of the frame counter both stock branches read (0x2e5fa6, 0x2e5fc8)

CS = Cs(CS_ARCH_ARM, CS_MODE_THUMB)


def _mul(top, span):
    """Q10 slope so that ((x - lo) * mul) >> 10 reaches `top` exactly at x = lo + span."""
    return -(-top * 1024 // span)


def ramp(x, lo, hi, top, floor):
    """The stubs' fixed-point ramp: clamp(((x - lo) * mul) >> 10, floor, top) (asrs = floor division)."""
    return max(floor, min(top, ((x - lo) * _mul(top, hi - lo)) >> 10))


def params(cfg):
    """Tunables with their defaults, from the build_mod namespace."""
    g = lambda name, default: getattr(cfg, name, default)
    p = dict(low=g("dribble_skill_low", 60), high=g("dribble_skill_high", 85),
             tier_step=g("ctrl_fit_tier_step", 0x28), stock=not g("no_ctrl_fit_stock", False),
             close_mid=g("ctrl_fit_close_mid", 75), close_step=g("ctrl_fit_close_step", 0x28),
             aerial_mid=g("ctrl_fit_aerial_mid", 85), aerial_step=g("ctrl_fit_aerial_step", 0x40),
             deek_high_lo=g("deek_fit_high_lo", 25), deek_high_hi=g("deek_fit_high_hi", 99),
             deek_mid_lo=g("deek_fit_mid_lo", 5), deek_mid_hi=g("deek_fit_mid_hi", 80),
             style_fit=not g("no_deek_style_fit", False),
             style1_lo=g("deek_style1_lo", 25), style0_lo=g("deek_style0_lo", 10),
             lunge_lo=g("lunge_tackling_lo", 30), lunge_hi=g("lunge_tackling_hi", 95),
             stumble_mid=g("stumble_strength_mid", 60), stumble_span=g("stumble_strength_span", 40),
             roll_fit=not g("no_deek_roll_fit", False),
             roll0_lo=g("deek_roll0_lo", 5), roll0_hi=g("deek_roll0_hi", 85),
             roll1_lo=g("deek_roll1_lo", 15), roll1_hi=g("deek_roll1_hi", 95),
             kick_fit=not g("no_kick_fit", False),
             kick4_lo=g("kick_flag4_lo", 45), kick4_hi=g("kick_flag4_hi", 92),
             kick200_lo=g("kick_flag200_lo", 60), kick200_hi=g("kick_flag200_hi", 99))
    if not 40 <= p["low"] < p["high"] <= 99:
        raise SystemExit("anim_fit: dribble skill ratings must satisfy 40 <= low < high <= 99")
    for k in ("tier_step", "close_step", "aerial_step"):
        if not 0 <= p[k] <= 0xFFFF:
            raise SystemExit(f"anim_fit: {k} must be 0..0xFFFF")
    for k in ("close_mid", "aerial_mid"):
        if not 1 <= p[k] <= 99:
            raise SystemExit(f"anim_fit: {k} must be 1..99")
    for lo, hi in (("deek_high_lo", "deek_high_hi"), ("deek_mid_lo", "deek_mid_hi"), ("lunge_lo", "lunge_hi")):
        if not 0 <= p[lo] < p[hi] <= 99:
            raise SystemExit(f"anim_fit: {lo}/{hi} must satisfy 0 <= lo < hi <= 99")
    for k in ("style1_lo", "style0_lo"):
        if not 0 <= p[k] < 99:
            raise SystemExit(f"anim_fit: {k} must be 0..98")
    if not 0 <= p["stumble_mid"] <= 99 or not 8 <= p["stumble_span"] <= 99:
        raise SystemExit("anim_fit: --stumble-strength-mid must be 0..99 and --stumble-strength-span 8..99")
    p["mid_centre"] = (p["low"] + p["high"]) // 2
    p["mid_half"] = (p["high"] - p["low"]) // 2
    p["deek_high_mul"] = _mul(64, p["deek_high_hi"] - p["deek_high_lo"])
    p["deek_mid_mul"] = _mul(64, p["deek_mid_hi"] - p["deek_mid_lo"])
    p["lunge_mul"] = _mul(64, p["lunge_hi"] - p["lunge_lo"])
    p["style1_mul"] = _mul(STYLE_TOP[1], 99 - p["style1_lo"])
    p["style0_mul"] = _mul(STYLE_TOP[0], 99 - p["style0_lo"])
    p["stumble_mul"] = -(-STUMBLE_BIAS_LIMIT * 256 // p["stumble_span"])
    for lo, hi in (("roll0_lo", "roll0_hi"), ("roll1_lo", "roll1_hi")):
        if not 0 <= p[lo] < p[hi] <= 99:
            raise SystemExit(f"anim_fit: {lo}/{hi} must satisfy 0 <= lo < hi <= 99")
    p["roll0_mul"] = _mul(ROLL_TOP, p["roll0_hi"] - p["roll0_lo"])
    p["roll1_mul"] = _mul(ROLL_TOP, p["roll1_hi"] - p["roll1_lo"])
    for lo, hi in (("kick4_lo", "kick4_hi"), ("kick200_lo", "kick200_hi")):
        if not 0 <= p[lo] < p[hi] <= 99:
            raise SystemExit(f"anim_fit: {lo}/{hi} must satisfy 0 <= lo < hi <= 99")
    p["kick4_mul"] = _mul(100, p["kick4_hi"] - p["kick4_lo"])
    p["kick200_mul"] = _mul(KICK200_TOP, p["kick200_hi"] - p["kick200_lo"])
    return p


# --- reference model (used by the tests) --------------------------------------------------------
def ctrl_bias(clip, control, p, pools):
    """Score change in SetAnimControl (lower score wins; random(1024) is the stock tie-break)."""
    if 2553 <= clip <= 2558:
        return p["close_step"] * (p["close_mid"] - control)
    if 2559 <= clip <= 2560:
        return p["aerial_step"] * (p["aerial_mid"] - control)
    if not p["stock"]:
        return 0
    for name, ranges in pools.items():
        if any(a <= clip <= b for a, b in ranges):
            if name == "low":
                return p["tier_step"] * (control - p["low"])
            if name == "high":
                return p["tier_step"] * (p["high"] - control)
            return p["tier_step"] * (abs(control - p["mid_centre"]) - p["mid_half"])
    return 0


def deek_threshold(clip, control, p):
    """GA_SetAnimFromDeek keeps the clip when XSYS_Random(64) < this value (64 = always, no call)."""
    if 2547 <= clip <= 2550:
        return ramp(control, p["deek_high_lo"], p["deek_high_hi"], 64, KEEP_FLOOR)
    if 2551 <= clip <= 2552:
        return ramp(control, p["deek_mid_lo"], p["deek_mid_hi"], 64, KEEP_FLOOR)
    return 64


def deek_keep_probability(clip, control, p):
    return deek_threshold(clip, control, p) / 64


def style_thresholds(control, p):
    """(T1, T1 + T0): style 1 when XSYS_Random(100) < T1, style 0 when < T1 + T0, else style 2."""
    t1 = ramp(control, p["style1_lo"], 99, STYLE_TOP[1], STYLE_FLOOR[1])
    t0 = ramp(control, p["style0_lo"], 99, STYLE_TOP[0], STYLE_FLOOR[0])
    return t1, t1 + t0


def style_probabilities(control, p):
    t1, t10 = style_thresholds(control, p)
    return {0: (t10 - t1) / 100, 1: t1 / 100, 2: (100 - t10) / 100}


ROLL_TOP = 50                    # the stock roll is XSYS_Random(50) < threshold
ROLL_FLOOR = {0: 3, 1: 2}        # 6 % / 4 % at the lowest control: never an automatic failure


def roll_threshold(style, control, p):
    """Deek succeeds when XSYS_Random(50) < this value (stock: control-30 / control-40 / 50)."""
    if style == 0:
        return ramp(control, p["roll0_lo"], p["roll0_hi"], ROLL_TOP, ROLL_FLOOR[0])
    if style == 1:
        return ramp(control, p["roll1_lo"], p["roll1_hi"], ROLL_TOP, ROLL_FLOOR[1])
    return ROLL_TOP


KICK200_TOP = 35                 # flag 0x200 at most 35 % of the time (stock 25 % from control 90)


def kick_draws(counter):
    """(d4, d200): 0..99 draws for flag 4 (8-frame blocks) and 0x200 (32-frame blocks), as the stub."""
    def draw(block, mul):
        return ((((block * mul) & 0xFFFFFFFF) >> 4 & 0xFFFF) * 100) >> 16
    return draw(counter >> 3, 0x9E37), draw(counter >> 5, 0x85EB)


def kick_thresholds(control, p):
    """Flag 4 when d4 < t4; flags 0x204 when also d200 < t200."""
    return (ramp(control, p["kick4_lo"], p["kick4_hi"], 100, 1),
            ramp(control, p["kick200_lo"], p["kick200_hi"], KICK200_TOP, 1))


def kick_flags(base_flags, control, counter, p):
    t4, t200 = kick_thresholds(control, p)
    d4, d200 = kick_draws(counter)
    flags = base_flags
    if d4 < t4:
        flags |= 4
        if d200 < t200:
            flags |= 0x204
    return flags


def lunge_threshold(tackling, p):
    """The lunge is allowed when XSYS_Random(64) < this value (64 = always, no call)."""
    return ramp(tackling, p["lunge_lo"], p["lunge_hi"], 64, KEEP_FLOOR)


def stumble_bias(clip, strength, p):
    if not 2537 <= clip <= 2546:
        return 0
    b = ((strength - p["stumble_mid"]) * p["stumble_mul"]) >> 8      # arithmetic shift, as asrs
    b = max(-STUMBLE_BIAS_LIMIT, min(STUMBLE_BIAS_LIMIT, b))
    return -b if clip <= 2538 else b


# --- assembly ------------------------------------------------------------------------------------
def _range_checks(ranges, target, prefix, reg="sl"):
    lines = []
    for i, (first, last) in enumerate(ranges):
        nxt = f"{prefix}_n{i}"
        lines += [f"    movw    r0, #{first}",
                  f"    subs.w  r3, {reg}, r0",
                  f"    cmp     r3, #{last - first}",
                  f"    bhi     {nxt}",
                  f"    b       {target}",
                  f"{nxt}:"]
    return "\n".join(lines)


def _ramp_asm(reg, tmp, lo, mul, top, floor, op="muls"):
    """reg = stat -> reg = clamp(((stat - lo) * mul) >> 10, floor, top); tmp is clobbered."""
    return (f"    sub.w   {reg}, {reg}, #{lo}\n"
            f"    movw    {tmp}, #{mul}\n"
            f"    {op:<7} {reg}, {tmp}, {reg}\n"      # op="mul" (32-bit) when tmp is a high register
            f"    asrs    {reg}, {reg}, #10\n"
            f"    cmp     {reg}, #{floor}\n"
            f"    it      lt\n"
            f"    movlt   {reg}, #{floor}\n"
            f"    cmp     {reg}, #{top}\n"
            f"    it      gt\n"
            f"    movgt   {reg}, #{top}")


def ctrl_src(p, pools):
    c = HOOKS["ctrl_fit"]["cont"]
    if p["stock"]:
        stock = (f"{_range_checks(pools['low'], 'cf_low', 'cf_l')}\n"
                 f"{_range_checks(pools['mid'], 'cf_mid', 'cf_m')}\n"
                 f"{_range_checks(pools['high'], 'cf_high', 'cf_h')}\n"
                 f"    b       cf_score\n"
                 f"cf_low:\n"
                 f"    subs    r1, #{p['low']}\n"
                 f"    b       cf_tier\n"
                 f"cf_high:\n"
                 f"    rsb.w   r1, r1, #{p['high']}\n"
                 f"    b       cf_tier\n"
                 f"cf_mid:\n"
                 f"    subs    r1, #{p['mid_centre']}\n"
                 f"    it      mi\n"
                 f"    rsbmi   r1, r1, #0\n"
                 f"    subs    r1, #{p['mid_half']}\n"
                 f"cf_tier:\n"
                 f"    movw    r0, #{p['tier_step']}\n"
                 f"    b       cf_mul\n")
    else:
        stock = "    b       cf_score\n"
    return f"""ctrl_fit:
    ldr     r0, [sp, #0x108]
    ldrb.w  r1, [r0, #{OFF_CONTROL:#x}]
    movw    r0, #2553
    subs.w  r2, sl, r0
    cmp     r2, #7
    bhi     cf_stock
    cmp     r2, #6
    bhs     cf_aerial
    rsb.w   r1, r1, #{p['close_mid']}
    movw    r0, #{p['close_step']}
    b       cf_mul
cf_aerial:
    rsb.w   r1, r1, #{p['aerial_mid']}
    movw    r0, #{p['aerial_step']}
    b       cf_mul
cf_stock:
{stock}cf_mul:
    mla     r6, r1, r0, r6
cf_score:
    ldr     r5, [sp, #0x6c]
    cmp     r6, r5
    blt     cf_better
    b.w     {c[1]:#x}
cf_better:
    b.w     {c[0]:#x}"""


def deek_src(p):
    # Live at the hook: r4 (loop index), r5 (random score), r6 (clip), r7, r8 (TAnimData*), sb (style),
    # sl (action time). r0-r3, ip, lr are dead on both continuations (the function saved lr), so the
    # XSYS_Random call needs no save except the threshold (pushed; the control is read before the push).
    keep, skip = HOOKS["deek_fit"]["cont"]
    return f"""deek_fit:
    adds.w  r0, sb, #1
    beq     df_style_ok
    ldrsh.w r0, [r8, #0x6e]
    cmp     r0, sb
    beq     df_style_ok
    b.w     {skip:#x}
df_style_ok:
    movw    r0, #2547
    subs.w  r1, r6, r0
    cmp     r1, #5
    bhi     df_keep
    ldr     r0, [sp, #0x8c]
    ldrb.w  r0, [r0, #{OFF_CONTROL:#x}]
    cmp     r1, #4
    bhs     df_mid
    sub.w   r0, r0, #{p['deek_high_lo']}
    movw    r1, #{p['deek_high_mul']}
    b       df_ramp
df_mid:
    sub.w   r0, r0, #{p['deek_mid_lo']}
    movw    r1, #{p['deek_mid_mul']}
df_ramp:
    muls    r0, r1, r0
    asrs    r0, r0, #10
    cmp     r0, #64
    bge     df_keep
    cmp     r0, #{KEEP_FLOOR}
    it      lt
    movlt   r0, #{KEEP_FLOOR}
    push    {{r0, r1}}
    movs    r0, #64
    bl      {XSYS_RANDOM:#x}
    pop     {{r1, r2}}
    cmp     r0, r1
    blo     df_keep
    b.w     {skip:#x}
df_keep:
    b.w     {keep:#x}"""


def tackle_src(p):
    better, worse = HOOKS["tackle_fit"]["cont"]
    return f"""tackle_fit:
    lsl.w   r0, fp, #6
    add     r6, r0
    movw    r0, #2535
    subs.w  r0, sb, r0
    cmp     r0, #1
    bhi     tf_score
    ldr     r0, [sp, #0x64]
    ldrb.w  r0, [r0, #{OFF_TACKLING:#x}]
    sub.w   r0, r0, #{p['lunge_lo']}
    movw    r1, #{p['lunge_mul']}
    muls    r0, r1, r0
    asrs    r0, r0, #10
    cmp     r0, #64
    bge     tf_score
    cmp     r0, #{KEEP_FLOOR}
    it      lt
    movlt   r0, #{KEEP_FLOOR}
    push    {{r0, lr}}
    movs    r0, #64
    bl      {XSYS_RANDOM:#x}
    pop     {{r1, r2}}
    cmp     r0, r1
    blo     tf_score
    add.w   r6, r6, #{LUNGE_PENALTY:#x}
tf_score:
    ldr     r0, [sp, #0x2c]
    cmp     r6, r0
    blt     tf_better
    b.w     {worse:#x}
tf_better:
    b.w     {better:#x}"""


def stumble_src(p):
    better, worse = HOOKS["stumble_fit"]["cont"]
    return f"""stumble_fit:
    movw    r0, #2537
    subs.w  r0, r5, r0
    cmp     r0, #9
    bhi     sf_score
    ldrb.w  r2, [fp, #{OFF_STRENGTH:#x}]
    sub.w   r2, r2, #{p['stumble_mid']}
    movw    r3, #{p['stumble_mul']}
    muls    r2, r3, r2
    asrs    r2, r2, #8
    cmp     r2, #{STUMBLE_BIAS_LIMIT}
    it      gt
    movgt   r2, #{STUMBLE_BIAS_LIMIT}
    cmn     r2, #{STUMBLE_BIAS_LIMIT}
    it      lt
    mvnlt   r2, #{STUMBLE_BIAS_LIMIT - 1}
    cmp     r0, #1
    bhi     sf_add
    rsbs    r2, r2, #0
sf_add:
    add     sl, r2
sf_score:
    ldr     r1, [sp, #0x18]
    cmp     sl, r1
    blt     sf_better
    b.w     {worse:#x}
sf_better:
    b.w     {better:#x}"""


def style_src(p):
    # Live at the hook: r4 (CPlayer), r5 (controller), r7 (direction); r6 = 6 (state) becomes the style.
    # r0-r3, ip, lr are dead at the continuation (the stock block itself calls XSYS_Random there).
    (done,) = HOOKS["style_fit"]["cont"]
    return f"""style_fit:
    ldrb.w  r2, [r4, #{OFF_CONTROL:#x}]
    mov     r0, r2
{_ramp_asm('r0', 'r3', p['style1_lo'], p['style1_mul'], STYLE_TOP[1], STYLE_FLOOR[1])}
    mov     r1, r2
{_ramp_asm('r1', 'r3', p['style0_lo'], p['style0_mul'], STYLE_TOP[0], STYLE_FLOOR[0])}
    add     r1, r0
    push    {{r0, r1}}
    movs    r0, #100
    bl      {XSYS_RANDOM:#x}
    pop     {{r1, r2}}
    movs    r6, #1
    cmp     r0, r1
    blo     sm_done
    movs    r6, #0
    cmp     r0, r2
    blo     sm_done
    movs    r6, #2
sm_done:
    b.w     {done:#x}"""


def roll_src(p):
    # Live at the hook: r4 (CPlayer), r6 (style), r7; r5 becomes the threshold. r0-r3 are dead at the
    # continuation (it calls XSYS_Random(50) straight away).
    (done,) = HOOKS["roll_fit"]["cont"]
    return f"""roll_fit:
    ldrb.w  r1, [r4, #{OFF_CONTROL:#x}]
    cmp     r6, #1
    beq     rf_s1
    cmp     r6, #0
    beq     rf_s0
    movs    r5, #{ROLL_TOP}
    b.w     {done:#x}
rf_s0:
{_ramp_asm('r1', 'r2', p['roll0_lo'], p['roll0_mul'], ROLL_TOP, ROLL_FLOOR[0])}
    mov     r5, r1
    b.w     {done:#x}
rf_s1:
{_ramp_asm('r1', 'r2', p['roll1_lo'], p['roll1_mul'], ROLL_TOP, ROLL_FLOOR[1])}
    mov     r5, r1
    b.w     {done:#x}"""


def kick_src(p):
    """Needs the stub address: the frame counter is reached PC-relative through its GOT slot."""
    (done,) = HOOKS["kick_fit"]["cont"]

    def build(stub, delta=0):
        return f"""kick_fit:
    mov     lr, r1
    movw    r2, #{delta & 0xFFFF}
    movt    r2, #{delta >> 16 & 0xFFFF}
kf_pc:
    add     r2, pc
    ldr     r2, [r2]
    ldr     r2, [r2]
    lsrs    r3, r2, #3
    movw    r1, #0x9e37
    muls    r3, r1, r3
    ubfx    r3, r3, #4, #16
    movs    r1, #100
    muls    r3, r1, r3
    lsrs    r3, r3, #16
    lsrs    r2, r2, #5
    movw    r1, #0x85eb
    muls    r2, r1, r2
    ubfx    r2, r2, #4, #16
    movs    r1, #100
    muls    r2, r1, r2
    lsrs    r2, r2, #16
    mov     r0, r6
    mov     r1, lr
{_ramp_asm('r1', 'ip', p['kick4_lo'], p['kick4_mul'], 100, 1, 'mul')}
    cmp     r3, r1
    bhs     kf_done
    orr     r0, r0, #4
    mov     r1, lr
{_ramp_asm('r1', 'ip', p['kick200_lo'], p['kick200_mul'], KICK200_TOP, 1, 'mul')}
    cmp     r2, r1
    bhs     kf_done
    orr     r0, r0, #0x204
kf_done:
    ldr     r3, [sp, #0x18]
    b.w     {done:#x}"""

    def at(stub, asm=None):
        # 'add r2, pc' sits after a 16-bit mov and two 32-bit movw/movt: stub + 10, reads pc = stub + 14
        return build(stub, (KICK_COUNTER_GOT - (stub + 10 + 4)) & 0xFFFFFFFF)
    return at


def active_hooks(p):
    return [n for n in HOOKS if (n != "style_fit" or p["style_fit"]) and (n != "roll_fit" or p["roll_fit"])
            and (n != "kick_fit" or p["kick_fit"])]


def sources(cfg, pools):
    p = params(cfg)
    srcs = {"ctrl_fit": ctrl_src(p, pools), "deek_fit": deek_src(p),
            "tackle_fit": tackle_src(p), "stumble_fit": stumble_src(p)}
    if p["style_fit"]:
        srcs["style_fit"] = style_src(p)
    if p["roll_fit"]:
        srcs["roll_fit"] = roll_src(p)
    if p["kick_fit"]:
        srcs["kick_fit"] = kick_src(p)
    return p, srcs


def _one(code, addr):
    ins = list(CS.disasm(bytes(code), addr))
    if len(ins) != 1:
        raise SystemExit(f"anim_fit: {addr:#x} does not decode to one instruction")
    return ins[0]


def _encode_branch(asm, mnemonic, target, addr):
    """Encode b.w/bl and check Keystone's decoded destination (it can be off by a few bytes)."""
    for cand in [target] + [target + d for d in range(-0x20, 0x22, 2) if d]:
        try:
            blob = asm(f"{mnemonic} {cand:#x}", addr)
        except SystemExit:
            continue
        ins = next(CS.disasm(blob, addr), None)
        if ins is not None and ins.mnemonic == mnemonic and len(blob) == 4 \
                and int(ins.op_str.lstrip("#"), 16) == target:
            return blob
    raise SystemExit(f"anim_fit: cannot encode {mnemonic} {target:#x} at {addr:#x}")


def assemble(asm, src, addr, far_targets):
    """Assemble one stub, re-encode its far branches, and verify every branch in it."""
    code = bytearray(asm(src, addr))
    end = addr + len(code)
    # far branches in source order <-> far branches in the code, one to one (the two continuations of a
    # hook are close together, so matching by distance would be ambiguous)
    wanted = [(m, int(t, 16)) for m, t in re.findall(r"^\s*(b\.w|bl)\s+(0x[0-9a-fA-F]+)\s*$", src, re.M)]
    if any(t not in far_targets for _, t in wanted):
        raise SystemExit(f"anim_fit: stub at {addr:#x} branches to an address outside its declared targets")
    far = [ins for ins in CS.disasm(bytes(code), addr) if ins.mnemonic in ("b.w", "bl")
           and ins.op_str.startswith("#0x") and not addr <= int(ins.op_str[1:], 16) < end]
    if len(far) != len(wanted):
        raise SystemExit(f"anim_fit: stub at {addr:#x}: {len(far)} far branches decoded, {len(wanted)} in source")
    for ins, (mnem, tgt) in zip(far, wanted):
        if ins.mnemonic != mnem:
            raise SystemExit(f"anim_fit: {ins.address:#x} decodes as {ins.mnemonic}, source has {mnem}")
        if int(ins.op_str[1:], 16) != tgt:
            code[ins.address - addr:ins.address - addr + 4] = _encode_branch(asm, mnem, tgt, ins.address)
    # label addresses: every non-label source line is exactly one instruction (asm() checked the count)
    insns = [ins.address for ins in CS.disasm(bytes(code), addr)] + [end]
    labels, k = set(), 0
    for line in src.splitlines():
        if re.match(r"^\w+:\s*$", line):
            labels.add(insns[k])
        elif line.strip():
            k += 1
    if k != len(insns) - 1:
        raise SystemExit(f"anim_fit: stub at {addr:#x}: {len(insns) - 1} instructions for {k} source lines")
    decoded = 0
    for ins in CS.disasm(bytes(code), addr):
        decoded += ins.size
        if ins.mnemonic in ("bx", "blx") or ins.mnemonic.startswith("pop") and "pc" in ins.op_str:
            raise SystemExit(f"anim_fit: {ins.mnemonic} at {ins.address:#x}: stubs return with b.w only")
        if ins.mnemonic.startswith("b") and ins.op_str.startswith("#0x"):
            tgt = int(ins.op_str[1:], 16)
            if addr <= tgt < end:
                if tgt not in labels:
                    raise SystemExit(f"anim_fit: local branch at {ins.address:#x} to {tgt:#x} misses every label")
                continue
            if ins.mnemonic not in ("b.w", "bl") or tgt not in far_targets:
                raise SystemExit(f"anim_fit: branch {ins.mnemonic} {ins.op_str} at {ins.address:#x} "
                                 "leaves the stub to an unexpected target or is conditional")
    if decoded != len(code):
        raise SystemExit(f"anim_fit: stub at {addr:#x} does not decode completely")
    return bytes(code)


def _check_no_branch_into(orig, va2off, hook):
    """No branch in the hooked function lands inside the replaced bytes (after the first halfword)."""
    start, size = hook["func"]
    site, n = hook["site"], hook["size"]
    cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    cs.skipdata = True
    o = va2off(start)
    for ins in cs.disasm(bytes(orig[o:o + size]), start):
        if site <= ins.address < site + n:
            continue                       # replaced together with the hook bytes
        if ins.mnemonic.startswith(("b", "cb")) and "#0x" in ins.op_str:
            tgt = int(ins.op_str.split("#")[-1], 16)
            if site < tgt < site + n:
                raise SystemExit(f"anim_fit: {ins.address:#x} branches into the hook bytes at {tgt:#x}")


def style_hook_active(cfg):
    """True when apply() will replace GC_SpecialMoveDribbling's style pick (build_mod then skips the
    SM_DRIBBLE_HIGH/LOW threshold patches, which sit inside / behind the replaced bytes)."""
    return (not getattr(cfg, "no_extend", False) and not getattr(cfg, "no_anim_fit", False)
            and not getattr(cfg, "no_deek_style_fit", False))


def apply(lib, orig, va2off, layout, asm, cfg, pools):
    """Patch `lib` (extended bytearray) in place. Returns {stub name: address}."""
    if layout is None:
        raise SystemExit("anim_fit: needs the CAVE3 segment (drop --no-extend)")
    count = getattr(cfg, "anim_count", STOCK_ANIM_COUNT)
    cov = coverage()
    missing = [i for i in range(STOCK_ANIM_COUNT, count) if i not in cov]
    if missing:
        raise SystemExit(f"anim_fit: appended clip IDs without a distribution rule: {missing} "
                         "(add them to anim_fit.RULES and the stubs)")
    p, srcs = sources(cfg, pools)
    base = layout["cave3_va"] + REGION_OFFSET
    if layout["cave3_size"] < REGION_OFFSET + REGION_SIZE:
        raise SystemExit("anim_fit: CAVE3 is too small")
    off = elf_extend.va2off(lib, base)
    if any(lib[off:off + REGION_SIZE]):
        raise SystemExit(f"anim_fit: CAVE3 {base:#x}..{base + REGION_SIZE:#x} is not free")
    if len(HOOKS) * SLOT > REGION_SIZE:
        raise SystemExit("anim_fit: more stubs than slots")
    out = {}
    for i, (name, hook) in enumerate(HOOKS.items()):
        if name not in srcs:
            continue
        stub = base + i * SLOT
        far = set(hook["cont"]) | {XSYS_RANDOM}
        src = srcs[name](stub) if callable(srcs[name]) else srcs[name]
        code = assemble(asm, src, stub, far)
        if name == "kick_fit":
            add = next(i for i in CS.disasm(code, stub) if i.mnemonic == "add" and i.op_str == "r2, pc")
            if add.address != stub + 10:
                raise SystemExit(f"anim_fit: kick_fit 'add r2, pc' at {add.address:#x}, expected {stub + 10:#x}")
        if len(code) > SLOT:
            raise SystemExit(f"anim_fit: {name} is {len(code)} bytes (> {SLOT})")
        so = elf_extend.va2off(lib, stub)
        lib[so:so + len(code)] = code
        site, n = hook["site"], hook["size"]
        oo = va2off(site)
        if bytes(orig[oo:oo + n]).hex() != hook["stock"]:
            raise SystemExit(f"anim_fit: {site:#x} is not the stock code (wrong game version?)")
        lo = elf_extend.va2off(lib, site)
        if bytes(lib[lo:lo + n]) != bytes(orig[oo:oo + n]):
            raise SystemExit(f"anim_fit: {site:#x} was already patched by another change")
        _check_no_branch_into(orig, va2off, hook)
        jump = _encode_branch(asm, "b.w", stub, site)
        if _one(jump, site).mnemonic != "b.w":
            raise SystemExit(f"anim_fit: hook at {site:#x} mis-encoded")
        lib[lo:lo + n] = jump + b"\x00\xbf" * ((n - 4) // 2)
        out[name] = stub
    return out
