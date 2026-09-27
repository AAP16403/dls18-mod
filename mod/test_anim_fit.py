"""anim_fit checks: emulate the five selection hooks of a built libDLS18.so at a non-zero base, and check
statically that every appended animation ID has a distribution rule, and that every rule is continuous
in its stat (no jump between adjacent stat values, no clip excluded at any stat value).

usage: python mod/test_anim_fit.py <libDLS18.so> [anims.pak]
Also run from test_patch.py. The expected values come from build_mod's parser defaults, so build with them.
"""
import json
import math
import struct
import sys
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs
from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_THUMB, Uc
from unicorn.arm_const import (UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R3, UC_ARM_REG_R4,
                               UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
                               UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_SP)

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "analysis"))
import anim_fit  # noqa: E402
import elf_extend  # noqa: E402

BASE = 0x40000000
IMG = 0xA00000
DATA = 0x50000000
STACK_TOP = DATA + 0x80000
PLAYER = DATA + 0x1000
ANIMDATA = DATA + 0x2000
PRESERVED = (UC_ARM_REG_R4, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11)
DEFAULT_PAK = HERE / "build" / "anims_career_market_v30_dls26_tackle_stumble_append_v16_tuned.pak"


class Emu:
    def __init__(self, lib):
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        uc.mem_map(BASE, IMG)
        uc.mem_write(BASE, bytes(elf_extend.load_image(lib, BASE, IMG)))
        uc.mem_map(DATA, 0x100000)
        self.stops = set()
        self.random_value = 0
        self.random_calls = []
        uc.hook_add(UC_HOOK_CODE, self._hook)

    def _hook(self, uc, addr, size, _):
        if addr in self.stops:
            uc.emu_stop()
        elif addr == BASE + anim_fit.XSYS_RANDOM:
            self.random_calls.append(uc.reg_read(UC_ARM_REG_R0))
            uc.reg_write(UC_ARM_REG_R0, self.random_value)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    def w(self, addr, fmt, v):
        self.uc.mem_write(addr, struct.pack(fmt, v))

    def run(self, hook, regs, frame, modified=()):
        """Enter at the patched game site; return (continuation index, regs after)."""
        h = anim_fit.HOOKS[hook]
        sp = STACK_TOP - 0x400
        self.uc.mem_write(sp, bytes(0x200))
        for off, v in frame.items():
            self.w(sp + off, "<i" if v < 0 else "<I", v)
        base_regs = {r: 0x1100 + i for i, r in enumerate(PRESERVED)}
        base_regs.update(regs)
        base_regs[UC_ARM_REG_SP] = sp
        base_regs[UC_ARM_REG_LR] = 0xDEAD0001
        for r, v in base_regs.items():
            self.uc.reg_write(r, v & 0xFFFFFFFF)
        self.stops = {BASE + c for c in h["cont"]}
        self.uc.emu_start((BASE + h["site"]) | 1, 0, count=400)
        pc = self.uc.reg_read(UC_ARM_REG_PC)
        if pc - BASE not in h["cont"]:
            raise AssertionError(f"{hook}: stopped at {pc:#x}")
        out = {r: self.uc.reg_read(r) for r in list(base_regs) + [UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R1]}
        ok = out[UC_ARM_REG_SP] == sp and all(out[r] == (base_regs[r] & 0xFFFFFFFF) for r in PRESERVED
                                              if r not in modified)
        return h["cont"].index(pc - BASE), out, ok


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def db_records(pak):
    from anim_tune_lib import AnimDB, load_pak
    _, entries = load_pak(pak)
    return AnimDB(entries["animdb.adb"]["data"])


def run(lib_path, pak=None):
    import build_mod as bm
    lib = Path(lib_path).read_bytes()
    cfg = bm.build_parser().parse_args([])
    p = anim_fit.params(cfg)
    pools = bm.DRIBBLE_STYLE_POOLS
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok &= bool(cond)
        print(f"  [{'OK' if cond else 'FAIL'}] {name}{(' -- ' + detail) if detail and not cond else ''}")

    print("anim_fit")
    # --- static: every appended clip has a rule, with the category its picker reads ------------
    cov = anim_fit.coverage()
    check("rules cover every ID 2535..2560", all(i in cov for i in range(2535, 2561)),
          str([i for i in range(2535, 2561) if i not in cov]))
    pak = Path(pak) if pak else DEFAULT_PAK
    if pak.exists():
        db = db_records(pak)
        appended = list(range(anim_fit.STOCK_ANIM_COUNT, db.count))
        check(f"every appended ID in the package ({len(appended)}) has a rule",
              all(i in cov for i in appended), str([i for i in appended if i not in cov]))
        bad = [(i, db.records[i][0], anim_fit.RULES[cov[i]]["category"]) for i in appended if i in cov
               and db.records[i][0] != anim_fit.RULES[cov[i]]["category"]]
        check("rule categories match the package categories", not bad, str(bad))
        # tackle reach: the lunge's contact point (action point 0) is where the selector and the contact
        # check test the ball, so it must lie inside the stock standing-tackle envelope
        stock9 = [i for i in range(anim_fit.STOCK_ANIM_COUNT) if db.records[i][0] == 9]
        reach = {i: math.hypot(db.action_points(i)[0][1], db.action_points(i)[0][3]) for i in stock9 + [2535, 2536]}
        ticks = {i: db.native_action_ticks(i) for i in reach}
        check("lunge 2535/2536 contact reach <= longest stock standing tackle",
              max(reach[2535], reach[2536]) <= max(reach[i] for i in stock9),
              f"{reach[2535]:.0f} vs {max(reach[i] for i in stock9):.0f}")
        check("lunge contact tick equals the stock tackles' tick",
              ticks[2535] == ticks[2536] and ticks[2535] in {ticks[i] for i in stock9})
        style = {i: db.s16(i, 0xE) for i in range(2547, 2553)}
        check("deeks 2547-2552 keep styles 1/0 (never the low-control style 2)", all(v in (0, 1) for v in style.values()),
              str(style))
    else:
        print(f"  (package {pak} missing: package checks skipped)")
    try:
        anim_fit.apply(bytearray(64), b"", None, dict(cave3_va=0, cave3_size=0x40000), None,
                       type("C", (), {"anim_count": 2562})(), pools)
        check("apply() refuses an appended ID without a rule", False)
    except SystemExit as e:
        check("apply() refuses an appended ID without a rule", "without a distribution rule" in str(e), str(e))

    # --- hooks are in place ---------------------------------------------------------------------
    lay = json.loads(Path(str(lib_path) + ".layout.json").read_text())
    lo_va = lay["cave3_va"] + anim_fit.REGION_OFFSET
    cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    for name, h in anim_fit.HOOKS.items():
        o = elf_extend.va2off(lib, h["site"])
        ins = next(cs.disasm(lib[o:o + 4], h["site"]))
        tgt = int(ins.op_str.lstrip("#"), 16) if ins.mnemonic == "b.w" else 0
        check(f"{name}: b.w at {h['site']:#x} into the anim_fit region",
              lo_va <= tgt < lo_va + anim_fit.REGION_SIZE, f"{ins.mnemonic} {ins.op_str}")

    emu = Emu(lib)
    stats = range(0, 100)
    samples = (20, 40, 60, 75, 85, 99)

    # --- category 1: SetAnimControl ---------------------------------------------------------
    good = regs_ok = True
    fails = []
    ids = list(range(2553, 2561)) + [a for rr in pools.values() for a, b in rr] + \
        [b for rr in pools.values() for a, b in rr] + [1331, 1360, 2244, 2291, 2552, 2561]
    for clip in ids:
        for c in (1, 20, 45, 59, 60, 61, 72, 73, 84, 85, 86, 99):
            emu.w(PLAYER + 0x127, "<B", c)
            for score, best in ((50000, 60000), (50000, 40000), (50000, 50600)):
                cont, out, rok = emu.run("ctrl_fit", {UC_ARM_REG_R10: clip, UC_ARM_REG_R6: score},
                                         {0x108: PLAYER, 0x6C: best})
                new = score + anim_fit.ctrl_bias(clip, c, p, pools)
                want_cont = 0 if new < best else 1
                if s32(out[UC_ARM_REG_R6]) != new or cont != want_cont or out[UC_ARM_REG_R5] != best:
                    good = False
                    fails.append((clip, c, s32(out[UC_ARM_REG_R6]) - score, new - score))
                regs_ok &= rok
    check("SetAnimControl: every pool ID / new ID gets the rule's bias, stock IDs untouched", good, str(fails[:6]))
    check("SetAnimControl: sp and r4/r7-r11 preserved, r5 = best, correct continuation", regs_ok)
    full, fails = True, []
    for clip in (2553, 2559, 2245, 2249, 2253):
        for c in stats:
            emu.w(PLAYER + 0x127, "<B", c)
            cont, out, rok = emu.run("ctrl_fit", {UC_ARM_REG_R10: clip, UC_ARM_REG_R6: 50000},
                                     {0x108: PLAYER, 0x6C: 60000})
            if s32(out[UC_ARM_REG_R6]) - 50000 != anim_fit.ctrl_bias(clip, c, p, pools) or not rok:
                full = False
                fails.append((clip, c))
    check("SetAnimControl: emulated bias = model at every control 0..99 (close, aerial, low/mid/high pool)",
          full, str(fails[:6]))
    table = {c: [anim_fit.ctrl_bias(i, c, p, pools) for i in (2553, 2559, 2245, 2249, 2253)] for c in samples}
    print(f"      bias (close 2553, aerial 2559, low 2245, mid 2249, high 2253) by control: {table}")
    close = [anim_fit.ctrl_bias(2553, c, p, pools) for c in range(1, 100)]
    aerial = [anim_fit.ctrl_bias(2559, c, p, pools) for c in range(1, 100)]
    check("close control: linear, preferred above its mid, penalised below, zero at the mid",
          all(b > a for a, b in zip(close[1:], close)) and close[p["close_mid"] - 1] == 0
          and close[98] < 0 < close[19])
    check("aerial control: linear and never excluded; costlier than close control below its mid",
          all(b > a for a, b in zip(aerial[1:], aerial)) and all(-0x10000 < v < 0x10000 for v in aerial)
          and all(aerial[c - 1] > close[c - 1] for c in range(1, p["aerial_mid"])))
    low, mid, high = ([anim_fit.ctrl_bias(i, c, p, pools) for c in range(1, 100)] for i in (2245, 2249, 2253))
    best_pool = [min(("low", "mid", "high"), key=lambda n, k=k: {"low": low, "mid": mid, "high": high}[n][k])
                 for k in range(99)]
    check("stock pools: low preferred at low control, mid in the middle, high at the top (crossing at low/high)",
          best_pool[19] == "low" and best_pool[71] == "mid" and best_pool[98] == "high"
          and low[p["low"] - 1] == mid[p["low"] - 1] and high[p["high"] - 1] <= mid[p["high"] - 1])

    # --- category 19: GA_SetAnimFromDeek style filter + keep probability ------------------------
    good = True
    fails = []
    for clip in list(range(2547, 2553)) + [67, 73, 2331, 2335, 69]:
        for c in (1, 20, 25, 26, 45, 60, 72, 80, 85, 99):
            emu.w(PLAYER + 0x127, "<B", c)
            thr = anim_fit.deek_threshold(clip, c, p)
            for clip_style, want_style in ((0, 0), (1, 1), (2, 2), (1, 0), (0, -1), (2, 1)):
                emu.w(ANIMDATA + 0x6E, "<h", clip_style)
                style_ok = want_style == -1 or clip_style == want_style
                for rnd in sorted({0, max(thr - 1, 0), min(thr, 63), 63}):
                    emu.random_value, emu.random_calls = rnd, []
                    cont, out, rok = emu.run("deek_fit", {UC_ARM_REG_R6: clip, UC_ARM_REG_R9: want_style,
                                                          UC_ARM_REG_R8: ANIMDATA}, {0x8C: PLAYER})
                    keep = style_ok and (thr >= 64 or rnd < thr)
                    calls = [64] if style_ok and thr < 64 else []
                    if cont != (0 if keep else 1) or not rok or emu.random_calls != calls:
                        good = False
                        fails.append((clip, c, clip_style, want_style, rnd, cont, emu.random_calls))
    check("deeks: style filter unchanged, new deeks kept when random(64) < ramp(control), stock deeks untouched",
          good, str(fails[:6]))
    full, fails = True, []
    emu.w(ANIMDATA + 0x6E, "<h", 1)
    for clip in (2547, 2551):
        for c in stats:
            emu.w(PLAYER + 0x127, "<B", c)
            thr = anim_fit.deek_threshold(clip, c, p)
            for rnd, keep in (((thr - 1, True), (thr, False)) if thr < 64 else ((63, True),)):
                emu.random_value, emu.random_calls = rnd, []
                cont, _, rok = emu.run("deek_fit", {UC_ARM_REG_R6: clip, UC_ARM_REG_R9: -1,
                                                    UC_ARM_REG_R8: ANIMDATA}, {0x8C: PLAYER})
                if cont != (0 if keep else 1) or not rok:
                    full = False
                    fails.append((clip, c, rnd))
    check("deeks: emulated keep threshold = model at every control 0..99 (2547, 2551)", full, str(fails[:6]))
    print("      deek keep % (2547-2550, 2551/2552) by control: "
          + str({c: (round(100 * anim_fit.deek_keep_probability(2547, c, p)),
                     round(100 * anim_fit.deek_keep_probability(2551, c, p))) for c in samples}))

    # --- deek style pick: GC_SpecialMoveDribbling (style_fit) ----------------------------------
    if p["style_fit"]:
        good, fails = True, []
        for c in stats:
            emu.w(PLAYER + 0x127, "<B", c)
            t1, t10 = anim_fit.style_thresholds(c, p)
            for rnd in sorted({0, t1 - 1, t1, t10 - 1, t10, 99}):
                emu.random_value, emu.random_calls = rnd, []
                cont, out, rok = emu.run("style_fit", {UC_ARM_REG_R4: PLAYER, UC_ARM_REG_R5: 0x5555,
                                                       UC_ARM_REG_R6: 6, UC_ARM_REG_R7: 0x1234}, {})
                want = 1 if rnd < t1 else 0 if rnd < t10 else 2
                if out[UC_ARM_REG_R6] != want or cont != 0 or not rok or out[UC_ARM_REG_R5] != 0x5555 \
                        or emu.random_calls != [100]:
                    good = False
                    fails.append((c, rnd, out[UC_ARM_REG_R6], want, emu.random_calls))
        check("deek style: random(100) against the control ramps at every control 0..99, r4/r5/r7-r11 kept",
              good, str(fails[:6]))
        from armdis import D as STOCK, va2off as stock_off
        tail = (0x2E416C, 0x2E417E)
        o, so = elf_extend.va2off(lib, tail[0]), stock_off(tail[0])
        check("deek style: SM_DRIBBLE threshold patches skipped (dead 2-way block left stock)",
              lib[o:o + tail[1] - tail[0]] == STOCK[so:so + tail[1] - tail[0]])
        print("      deek style % (style 1, style 0, style 2) by control: "
              + str({c: tuple(round(100 * anim_fit.style_probabilities(c, p)[k]) for k in (1, 0, 2))
                     for c in samples}))

    # --- deek success roll: GC_SpecialMoveDribbling (roll_fit) ---------------------------------
    if p["roll_fit"]:
        good, fails = True, []
        for c in stats:
            emu.w(PLAYER + 0x127, "<B", c)
            for style in (0, 1, 2, 5):
                emu.random_value, emu.random_calls = 0, []
                cont, out, rok = emu.run("roll_fit", {UC_ARM_REG_R4: PLAYER, UC_ARM_REG_R5: 0x5555,
                                                      UC_ARM_REG_R6: style, UC_ARM_REG_R7: 0x1234}, {})
                want = anim_fit.roll_threshold(style, c, p)
                if out[UC_ARM_REG_R5] != want or cont != 0 or not rok or emu.random_calls or out[UC_ARM_REG_R6] != style:
                    good = False
                    fails.append((c, style, out[UC_ARM_REG_R5], want))
        check("deek success roll: threshold = continuous control ramp for styles 0/1, 50 for style 2, at every control 0..99",
              good, str(fails[:6]))
        print("      deek success % (style 1, style 0, style 2) by control: "
              + str({c: tuple(2 * anim_fit.roll_threshold(s, c, p) for s in (1, 0, 2)) for c in samples}))

    # --- kick-selection flags: GPA_KickSetupSelectionFlags (kick_fit) --------------------------
    if p["kick_fit"]:
        counter_at = DATA + 0x3000
        emu.w(BASE + anim_fit.KICK_COUNTER_GOT, "<I", counter_at)
        good, fails, frames = True, [], (0, 8, 40, 77, 256, 1000, 4097, 12345, 99999)
        for c in stats:
            for counter in frames:
                emu.w(counter_at, "<I", counter)
                cont, out, rok = emu.run("kick_fit", {UC_ARM_REG_R1: c, UC_ARM_REG_R6: 0x82,
                                                      UC_ARM_REG_R4: PLAYER, UC_ARM_REG_R0: 0, UC_ARM_REG_R3: 0}, {0x18: 0x77})
                want = anim_fit.kick_flags(0x82, c, counter, p)
                if out[UC_ARM_REG_R0] != want or out[UC_ARM_REG_R3] != 0x77 or cont != 0 or not rok:
                    good = False
                    fails.append((c, counter, hex(out[UC_ARM_REG_R0]), hex(want)))
        check("kick flags: continuous control ramps against frame-block draws, r3 = [sp,#0x18], at every control 0..99",
              good, str(fails[:6]))
        freq = {}
        for c in samples:
            t4, t200 = anim_fit.kick_thresholds(c, p)
            n4 = n204 = 0
            for counter in range(0, 32 * 400, 8):
                f = anim_fit.kick_flags(0, c, counter, p)
                n4 += bool(f & 4)
                n204 += bool(f & 0x200)
            freq[c] = (round(100 * n4 / 1600), round(100 * n204 / 1600))
        print(f"      kick flag % of frames (4, 0x200) by control: {freq}")

    # --- category 9: ACT_TackleSetPlayerState ---------------------------------------------------
    good = True
    fails = []
    for clip in (2535, 2536, 331, 334, 739):
        for t in (1, 20, 30, 31, 32, 50, 62, 75, 94, 95, 99):
            emu.w(PLAYER + 0x123, "<B", t)
            thr = anim_fit.lunge_threshold(t, p)
            for rnd in sorted({0, max(thr - 1, 0), min(thr, 63), 63}):
                emu.random_value, emu.random_calls = rnd, []
                dist, ang, best = 9000, 3, 1 << 20
                cont, out, rok = emu.run("tackle_fit", {UC_ARM_REG_R9: clip, UC_ARM_REG_R6: dist,
                                                        UC_ARM_REG_R11: ang}, {0x64: PLAYER, 0x2C: best})
                score = dist + (ang << 6)
                if clip in (2535, 2536):
                    allowed = thr >= 64 or rnd < thr
                    score += 0 if allowed else anim_fit.LUNGE_PENALTY
                    calls_ok = emu.random_calls == ([64] if thr < 64 else [])
                else:
                    calls_ok = emu.random_calls == []
                if s32(out[UC_ARM_REG_R6]) != score or cont != (0 if score < best else 1) or not rok or not calls_ok:
                    good = False
                    fails.append((clip, t, rnd, s32(out[UC_ARM_REG_R6]) - dist - (ang << 6), emu.random_calls))
    check("tackle: lunge penalised unless random(64) < ramp(tackling); stock clips untouched", good, str(fails[:6]))
    full, fails = True, []
    for t in stats:
        emu.w(PLAYER + 0x123, "<B", t)
        thr = anim_fit.lunge_threshold(t, p)
        for rnd, allowed in (((thr - 1, True), (thr, False)) if thr < 64 else ((63, True),)):
            emu.random_value = rnd
            _, out, rok = emu.run("tackle_fit", {UC_ARM_REG_R9: 2535, UC_ARM_REG_R6: 9000, UC_ARM_REG_R11: 3},
                                  {0x64: PLAYER, 0x2C: 1 << 20})
            if s32(out[UC_ARM_REG_R6]) != 9000 + (3 << 6) + (0 if allowed else anim_fit.LUNGE_PENALTY) or not rok:
                full = False
                fails.append((t, rnd))
    check("tackle: emulated lunge threshold = model at every tackling 0..99", full, str(fails[:6]))
    print("      lunge allowed % by tackling: "
          + str({t: round(100 * anim_fit.lunge_threshold(t, p) / 64) for t in samples}))

    # --- category 6: SetAnimFromStateGen --------------------------------------------------------
    good = True
    fails = []
    for clip in list(range(2537, 2547)) + [366, 368, 1257, 2517, 2536, 2547]:
        for s in (0, 20, 21, 22, 40, 59, 60, 61, 80, 98, 99):
            emu.w(PLAYER + 0x122, "<B", s)
            for best in (100000, 99990, 100020):
                cont, out, rok = emu.run("stumble_fit", {UC_ARM_REG_R5: clip, UC_ARM_REG_R10: 100000,
                                                         UC_ARM_REG_R11: PLAYER}, {0x18: best},
                                         modified=(UC_ARM_REG_R10,))
                b = anim_fit.stumble_bias(clip, s, p)
                if s32(out[UC_ARM_REG_R10]) != 100000 + b or out[UC_ARM_REG_R1] != best or \
                        cont != (0 if 100000 + b < best else 1) or not rok:
                    good = False
                    fails.append((clip, s, s32(out[UC_ARM_REG_R10]) - 100000, b))
    check("stumbles: strength bias within the random(32) tie-break, other clips untouched", good, str(fails[:6]))
    full, fails = True, []
    for clip in (2537, 2541):
        for s in stats:
            emu.w(PLAYER + 0x122, "<B", s)
            _, out, rok = emu.run("stumble_fit", {UC_ARM_REG_R5: clip, UC_ARM_REG_R10: 100000,
                                                  UC_ARM_REG_R11: PLAYER}, {0x18: 1 << 20},
                                  modified=(UC_ARM_REG_R10,))
            if s32(out[UC_ARM_REG_R10]) - 100000 != anim_fit.stumble_bias(clip, s, p) or not rok:
                full = False
                fails.append((clip, s))
    check("stumbles: emulated bias = model at every strength 0..99 (2537, 2541)", full, str(fails[:6]))

    def p_port(b, n=32):
        # template first: best = s + rT; port wins when b < rT and b + rP < rT
        return sum(1 for rt in range(n) for rp in range(n) if b < rt and b + rp < rt) / n / n
    sp = {s: (round(100 * p_port(anim_fit.stumble_bias(2541, s, p))), round(100 * p_port(anim_fit.stumble_bias(2537, s, p))))
          for s in samples}
    print(f"      P(port beats its tied template) % (dramatic 2541, recover 2537) by strength: {sp}")
    check("weak players get the dramatic ports more, strong players the recover-and-run port",
          sp[40][0] > sp[60][0] > sp[85][0] and sp[40][1] < sp[60][1] < sp[85][1])

    # --- continuity: every rule is a continuous function of its stat over 1..99 -----------------
    # (the emulation above proves stub == model at every stat value; this checks the model)
    t_of = lambda c: anim_fit.style_thresholds(c, p)
    curves = {
        "close 2553 bias": (lambda c: anim_fit.ctrl_bias(2553, c, p, pools), p["close_step"], "bias"),
        "aerial 2559 bias": (lambda c: anim_fit.ctrl_bias(2559, c, p, pools), p["aerial_step"], "bias"),
        "low pool 2245 bias": (lambda c: anim_fit.ctrl_bias(2245, c, p, pools), p["tier_step"], "bias"),
        "mid pool 2249 bias": (lambda c: anim_fit.ctrl_bias(2249, c, p, pools), p["tier_step"], "bias"),
        "high pool 2253 bias": (lambda c: anim_fit.ctrl_bias(2253, c, p, pools), p["tier_step"], "bias"),
        "deek 2547 keep /64": (lambda c: anim_fit.deek_threshold(2547, c, p), 1, "p64"),
        "deek 2551 keep /64": (lambda c: anim_fit.deek_threshold(2551, c, p), 1, "p64"),
        "style 1 %": (lambda c: t_of(c)[0], 1, "p100"),
        "style 0 %": (lambda c: t_of(c)[1] - t_of(c)[0], 1, "p100"),
        "style 2 %": (lambda c: 100 - t_of(c)[1], 2, "p100"),
        "lunge allow /64": (lambda t: anim_fit.lunge_threshold(t, p), 1, "p64"),
        "deek roll style 0 /50": (lambda c: anim_fit.roll_threshold(0, c, p), 1, "p100"),
        "deek roll style 1 /50": (lambda c: anim_fit.roll_threshold(1, c, p), 1, "p100"),
        "kick flag 4 %": (lambda c: anim_fit.kick_thresholds(c, p)[0], 4, "p100"),
        "kick flag 0x200 %": (lambda c: anim_fit.kick_thresholds(c, p)[1], 2, "p100"),
        "stumble 2537 bias": (lambda s: anim_fit.stumble_bias(2537, s, p), 1, "tie"),
        "stumble 2541 bias": (lambda s: anim_fit.stumble_bias(2541, s, p), 1, "tie"),
    }
    jumps, excluded, report = [], [], {}
    for name, (f, limit, kind) in curves.items():
        vals = [f(x) for x in range(1, 100)]
        step = max(abs(b - a) for a, b in zip(vals, vals[1:]))
        report[name] = step
        # bias steps <= 0x40 (1/16 of the random(1024) tie-break); probability steps <= 1/64 or 2 %
        if step > limit or (kind == "bias" and step > 0x40):
            jumps.append((name, step, limit))
        if kind == "bias" and any(abs(v) >= 0x10000 for v in vals):
            excluded.append((name, "exclusion-size bias"))
        if kind in ("p64", "p100") and min(vals) < 1:
            excluded.append((name, f"probability 0 at stat {1 + vals.index(min(vals))}"))
        if kind == "tie" and any(abs(v) > STUMBLE_LIMIT for v in vals):
            excluded.append((name, "bias beyond the random(32) tie-break"))
    print(f"      largest change between adjacent stat values: {report}")
    check("continuity: no rule jumps between adjacent stat values (1..99)", not jumps, str(jumps))
    check("continuity: no clip or style is excluded at any stat value (probability > 0, bias finite)",
          not excluded, str(excluded))
    clamped = {}
    for name, f, lo_, hi_, top, floor in (
            ("deek 2547", lambda c: c, p["deek_high_lo"], p["deek_high_hi"], 64, 1),
            ("deek 2551", lambda c: c, p["deek_mid_lo"], p["deek_mid_hi"], 64, 1),
            ("lunge", lambda c: c, p["lunge_lo"], p["lunge_hi"], 64, 1),
            ("roll 0", lambda c: c, p["roll0_lo"], p["roll0_hi"], anim_fit.ROLL_TOP, anim_fit.ROLL_FLOOR[0]),
            ("roll 1", lambda c: c, p["roll1_lo"], p["roll1_hi"], anim_fit.ROLL_TOP, anim_fit.ROLL_FLOOR[1]),
            ("kick 4", lambda c: c, p["kick4_lo"], p["kick4_hi"], 100, 1),
            ("kick 0x200", lambda c: c, p["kick200_lo"], p["kick200_hi"], anim_fit.KICK200_TOP, 1),
            ("style 1", lambda c: c, p["style1_lo"], 99, anim_fit.STYLE_TOP[1], anim_fit.STYLE_FLOOR[1]),
            ("style 0", lambda c: c, p["style0_lo"], 99, anim_fit.STYLE_TOP[0], anim_fit.STYLE_FLOOR[0])):
        mul = -(-top * 1024 // (hi_ - lo_))
        clamped[name] = sum(1 for c in range(1, 100) if not floor <= ((c - lo_) * mul) >> 10 <= top)
    clamped["stumble"] = sum(1 for s in range(1, 100)
                             if abs(((s - p["stumble_mid"]) * p["stumble_mul"]) >> 8) > STUMBLE_LIMIT)
    print(f"      stat values (of 99) held at a clamp: {clamped}")
    # kick flags pick special kick types: they stay rare (1 %) below control 45 / 60 instead of ramping over 1..99
    wide = {k: v for k, v in clamped.items() if not k.startswith("kick")}
    check("continuity: ramps are wide (each clamps at most about a third of 1..99; kick flags about 60)",
          max(wide.values()) <= 36 and all(v <= 62 for k, v in clamped.items() if k.startswith("kick")), str(clamped))
    return ok


STUMBLE_LIMIT = anim_fit.STUMBLE_BIAS_LIMIT


if __name__ == "__main__":
    good = run(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
    print("ALL PASS" if good else "FAILURES")
    sys.exit(0 if good else 1)
