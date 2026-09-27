"""Execute the patched code paths of libDLS18.so under Unicorn with stubbed game functions.

Checks the patched gameplay paths, then runs the stat, error, duel and foul caves from a non-zero base.
"""
import struct
import sys
from pathlib import Path

from unicorn import UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_THUMB, Uc
from unicorn.arm_const import *

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "analysis"))
from armdis import SECT, find  # noqa: E402
import build_mod as bm  # noqa: E402

LIB_PATH = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "build/lib/armeabi-v7a/libDLS18.so"
LIB = LIB_PATH.read_bytes()
MEM = 0x01000000
DATA = 0x00F00000          # scratch area for fake structs / stack (above .bss end 0x94c5a4)
CTRL, PLAYER, STACK = DATA, DATA + 0x1000, DATA + 0x80000
SENTINEL = DATA + 0x90000
SENT_WIN, SENT_LOSE = SENTINEL + 0x100, SENTINEL + 0x200
NONZERO_BASE = 0x40000000
BM_CFG = bm.build_parser().parse_args([])

STUB = {find("_Z26XCTRL_GetGameTouchTouchingi"): "touching",
        find("_Z19XCTRL_GetButtonDowni7EButton"): "button",
        find("_Z17AIGAME_InOpenPlayv"): "openplay",
        find("_ZN7CPlayer10SetUrgencyEi"): "urgency",
        find("_Z11XSYS_Randomi"): "random",
        find("_ZN7CPlayer4TripEii"): "trip",
        find("_ZN7CPlayer11GetRotPointE6TPoint"): "rotpoint",
        find("_ZN7CPlayer8IsFacingEii"): "facing",
        find("_Z22XMATH_InterpolateClampiiiii"): "interp"}


def _plt_got_fixups():
    """(GOT slot, real address) for imports that are defined inside libDLS18 itself."""
    from armdis import D, SYMS
    rel = SECT[".rel.plt"]
    out = []
    for i in range(rel["size"] // 8):
        off, info = struct.unpack_from("<II", D, rel["off"] + i * 8)
        val, shn = SYMS[info >> 8][0], SYMS[info >> 8][3]
        if val and shn:
            out.append((off, val))
    return out


GOT_FIX = _plt_got_fixups()


class Env:
    def __init__(self, touching=False, buttons=(), openplay=True, random=0, base=0,
                 facing=False, capture_interp=False):
        self.touching, self.buttons, self.openplay = touching, set(buttons), openplay
        self.random = random
        self.base = base
        self.data = DATA if not base else base + MEM
        self.ctrl = self.data
        self.player = self.data + 0x1000
        self.stack = self.data + 0x80000
        self.sentinel = self.data + 0x90000
        self.sent_win, self.sent_lose = self.sentinel + 0x100, self.sentinel + 0x200
        self.facing = facing
        self.rotpoint = self.player + 0x80
        self.capture_interp = capture_interp
        self.stop_addrs = set()
        self.interp_calls = []
        self.random_calls = []
        self.trip_calls = []
        self.trip_result = 1
        self.urgency_calls = []
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        uc.mem_map(base, MEM)
        if base:
            uc.mem_map(self.data, 0x00100000)
        for s in SECT.values():
            if s["addr"] and s["off"] and s["size"] and s["addr"] < MEM:
                uc.mem_write(base + s["addr"], LIB[s["off"]:s["off"] + s["size"]])
        for slot, val in GOT_FIX:
            uc.mem_write(base + slot, struct.pack("<I", base + val if base else val))
        uc.hook_add(UC_HOOK_CODE, self._hook)

    def _hook(self, uc, addr, size, _):
        stop = {self.sentinel, self.sent_win, self.sent_lose}
        if self.base:
            stop.update((self.base + bm.T1_EPI, self.base + bm.T2_SKIP))
        else:
            stop.update((bm.T1_EPI, bm.T2_SKIP))
        if addr in stop or addr in self.stop_addrs:
            uc.emu_stop()
            return
        offset = addr - self.base if self.base <= addr < self.base + MEM else addr
        kind = STUB.get(offset)
        if not kind:
            return
        r0, r1 = uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)
        if kind == "interp":
            if not self.capture_interp:
                return
            sp = uc.reg_read(UC_ARM_REG_SP)
            fifth = struct.unpack("<I", uc.mem_read(sp, 4))[0]
            self.interp_calls.append((r0, r1, uc.reg_read(UC_ARM_REG_R2),
                                      uc.reg_read(UC_ARM_REG_R3), fifth))
            ret = 0
        elif kind == "rotpoint":
            ret = self.rotpoint
        elif kind == "facing":
            ret = int(self.facing)
        else:
            ret = {"touching": int(self.touching), "button": int(r1 in self.buttons),
                   "openplay": int(self.openplay), "urgency": 0, "random": self.random,
                   "trip": self.trip_result}[kind]
        if kind == "random":
            self.random_calls.append(r0)
        if kind == "trip":
            self.trip_calls.append((r0, r1, uc.reg_read(UC_ARM_REG_R2)))
        if kind == "urgency":
            self.urgency_calls.append((r0, r1))
        uc.reg_write(UC_ARM_REG_R0, ret)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))   # return (LR keeps thumb bit)

    def w(self, addr, fmt, v):
        self.uc.mem_write(addr, struct.pack(fmt, v))

    def r(self, addr, fmt):
        return struct.unpack(fmt, self.uc.mem_read(addr, struct.calcsize(fmt)))[0]

    def run(self, start, stop, regs):
        for k, v in regs.items():
            self.uc.reg_write(k, v)
        start = start + self.base if self.base and start < self.data else start
        stop = stop + self.base if self.base and stop < self.data else stop
        self.uc.emu_start(start | 1, stop, count=5000)


def setup_ctrl(env, power, direction=0x1000, prev=0, stamina=0x50000, urgency=0x800):
    env.w(CTRL + 5, "<B", 0)
    env.w(CTRL + 8, "<I", PLAYER)
    env.w(CTRL + 0x7C, "<I", direction)
    env.w(CTRL + 0x80, "<i", power)
    env.w(CTRL + 0x54, "<B", prev)
    env.w(CTRL + 0x70, "<H", 0x55)
    env.w(PLAYER + 0x114, "<I", stamina)
    env.w(PLAYER + 0x7C, "<h", urgency)


def sprint_case(power, **kw):
    env = Env(**{k: kw.pop(k) for k in ("touching", "buttons", "openplay") if k in kw})
    setup_ctrl(env, power, **kw)
    # fake frame for the epilogue: add sp,#0x1c ; pop {r4-r11, pc}
    frame = b"\0" * 0x1C + struct.pack("<8I", *range(8)) + struct.pack("<I", SENTINEL | 1)
    env.uc.mem_write(STACK, frame)
    env.run(bm.P1, SENTINEL, {UC_ARM_REG_R4: CTRL, UC_ARM_REG_SP: STACK})
    assert env.uc.reg_read(UC_ARM_REG_PC) == SENTINEL, "did not return through epilogue"
    assert env.r(CTRL + 0x70, "<H") == 0
    return env.r(CTRL + 0x54, "<B")


def drib_case(sprint_flag, **kw):
    env = Env(**kw)
    setup_ctrl(env, 0, prev=sprint_flag)
    env.run(bm.P2, bm.P2_RET, {UC_ARM_REG_R10: CTRL, UC_ARM_REG_R4: PLAYER, UC_ARM_REG_SP: STACK})
    assert env.uc.reg_read(UC_ARM_REG_R0) == PLAYER
    return env.uc.reg_read(UC_ARM_REG_R1)


def offball_case(sprint_flag, **kw):
    env = Env(**kw)
    setup_ctrl(env, 0, prev=sprint_flag)
    env.run(bm.P3, bm.P3_RET, {UC_ARM_REG_R11: CTRL, UC_ARM_REG_R4: PLAYER, UC_ARM_REG_SP: STACK})
    return [u for p, u in env.urgency_calls if p == PLAYER]


def touch_case(touch, urgency):
    env = Env()
    env.w(PLAYER + 0x7C, "<h", urgency)
    env.run(bm.P4, bm.P4 + 4, {UC_ARM_REG_R0: touch, UC_ARM_REG_R5: PLAYER, UC_ARM_REG_R7: 0x1234,
                               UC_ARM_REG_SP: STACK})
    lit = struct.unpack_from("<I", LIB, SECT[".text"]["off"] - SECT[".text"]["addr"] + bm.P4_LIT)[0]
    assert env.uc.reg_read(UC_ARM_REG_R4) == lit, "r4 literal not reproduced"
    assert env.uc.reg_read(UC_ARM_REG_R7) == 0x1234
    return env.uc.reg_read(UC_ARM_REG_R5)


STAT_OFF = {1: 0x121, 2: 0x126, 3: 0x125, 4: 0x127, 5: 0x128, 6: 0x129, 7: 0x12A,
            8: 0x124, 9: 0x123, 10: 0x12B, 11: 0x12C, 12: 0x12D}


def call(env, func, args, stack_args=()):
    regs = {UC_ARM_REG_LR: SENTINEL | 1, UC_ARM_REG_SP: STACK}
    for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
        regs[r] = v & 0xFFFFFFFF
    env.uc.mem_write(STACK, b"".join(struct.pack("<i", v) for v in stack_args) + b"\0" * 64)
    env.run(func, SENTINEL, regs)
    assert env.uc.reg_read(UC_ARM_REG_PC) == SENTINEL
    return struct.unpack("<i", struct.pack("<I", env.uc.reg_read(UC_ARM_REG_R0)))[0]


def stat_value(stat, stype=7, lo=0, hi=1000):
    env = Env()
    env.w(PLAYER + STAT_OFF[stype], "<B", stat)
    return call(env, find("_ZN7CPlayer29AttributeInterpolate_InternalE15EPlayerStatTypeiiiii"),
                (PLAYER, stype, lo, hi), (-1, -1, -1))


def err_range(stat, kick_type):
    env = Env()
    for t in (5, 6, 7, 9):
        env.w(PLAYER + STAT_OFF[t], "<B", stat)
    return call(env, find("_Z29ACT_KickErrorAccuracyGetRangeP7CPlayeri"), (PLAYER, kick_type))


def main():
    ok = True

    def check(name, got, want):
        nonlocal ok
        good = got == want
        ok &= good
        print(f"{'PASS' if good else 'FAIL'}  {name:55s} got={got!r} want={want!r}")

    FULL, HALF, LOW = 0x4000, 0x2000, 0x0900
    check("full stick -> sprint", sprint_case(FULL), 1)
    check("half stick -> jog (no sprint)", sprint_case(HALF), 0)
    check("slight stick -> jog", sprint_case(LOW), 0)
    check("87.5% exactly -> sprint on", sprint_case(0x3800), 1)
    check("82% from jog -> stays jog (hysteresis)", sprint_case(0x3400), 0)
    check("82% while sprinting -> keeps sprint (hysteresis)", sprint_case(0x3400, prev=1), 1)
    check("75% while sprinting -> drops to jog", sprint_case(0x3000, prev=1), 0)
    check("no stick direction -> no sprint", sprint_case(FULL, direction=0xFFFF), 0)
    check("no stamina -> no sprint", sprint_case(FULL, stamina=0), 0)
    check("tired (<25% stamina) can't start sprint", sprint_case(FULL, stamina=0x20000), 0)
    check("tired but already sprinting keeps going", sprint_case(FULL, stamina=0x20000, prev=1), 1)
    check("not open play -> no sprint",sprint_case(FULL, openplay=False), 0)
    check("close control held -> no sprint", sprint_case(FULL, touching=True), 0)
    check("right finger on a button (pass) -> sprint allowed", sprint_case(FULL, touching=True, buttons={0}), 1)

    check("dribble: sprint flag -> urgency 0x1000", drib_case(1), 0x1000)
    check("dribble: no flag -> jog 0x800", drib_case(0), 0x800)
    close_control_pace = []
    for ctl in range(100):
        env = Env(touching=True)
        setup_ctrl(env, 0)
        env.w(PLAYER + 0x127, "<B", ctl)
        env.run(bm.P2, bm.P2_RET, {UC_ARM_REG_R10: CTRL, UC_ARM_REG_R4: PLAYER, UC_ARM_REG_SP: STACK})
        close_control_pace.append(env.uc.reg_read(UC_ARM_REG_R1))
    close_control_expected = [
        0x280 + ctl if ctl <= 40 else
        0x280 + 40 + ((ctl - 40) * (0x500 - 0x280 - 40) * 1111 >> 16)
        for ctl in range(100)
    ]
    check("close-control pace matches every raw control rating 0..99",
          close_control_pace, close_control_expected)
    check("close-control pace strictly rises at every raw control rating",
          all(a < b for a, b in zip(close_control_pace, close_control_pace[1:])), True)
    for ctl in (0, 30, 40, 70, 99):
        check(f"close-control pace example ({ctl})", close_control_pace[ctl], close_control_expected[ctl])
    check("dribble: finger on button -> jog", drib_case(0, touching=True, buttons={2}), 0x800)

    check("off-ball: sprint flag -> SetUrgency(0x1000)", offball_case(1), [0x1000])
    check("off-ball: none -> leave game's jog", offball_case(0), [])
    check("off-ball: close control -> SetUrgency(0x500)", offball_case(0, touching=True), [0x500])

    check("touch @ jog 0x800 unchanged", touch_case(0x700, 0x800), 0x700)
    check("touch @ tired jog 0x680 unchanged", touch_case(0x700, 0x680), 0x700)
    check("touch @ sprint 0x1000 unchanged", touch_case(0x900, 0x1000), 0x900)
    check("touch @ close control 0x380 -> 0.6875x", touch_case(0x700, 0x380), 0x700 * 0xB00 >> 12)
    check("touch @ standing 0 -> 0.25x", touch_case(0x700, 0), 0x700 * 0x400 >> 12)
    def near(a, b):
        return abs(a - b) <= 2

    def mapped_stat(stat):
        if stat >= 56:
            return stat
        table = bm.stat_table_q8(BM_CFG.stat_knee)
        index, fraction = stat >> 2, stat & 3
        q8 = table[index] + ((table[index + 1] - table[index]) * fraction >> 2)
        return (q8 + 128) >> 8

    def expect_stat(stat):
        v = min(max(mapped_stat(stat), 40), 99)
        return (v - 40) * 1000 // 59

    for st in range(100):
        got = stat_value(st)
        check(f"stat {st} -> value matches softplus map", near(got, expect_stat(st)), True)

    accel_steps = {st: stat_value(st, stype=3, lo=7, hi=27) for st in (62, 85)}
    accel_seconds = {st: 4096.0 / step / 60.0 for st, step in accel_steps.items()}
    print("      acceleration ramp: " + ", ".join(
        f"stat{st}={accel_steps[st]}/frame ({accel_seconds[st]:.2f}s)" for st in (62, 85)))
    check("acceleration ramp near 5.0s vs 3.1s", 
          4.5 <= accel_seconds[62] <= 5.5 and 2.8 <= accel_seconds[85] <= 3.4, True)

    shot_err = {st: err_range(st, 1) for st in (35, 54, 85, 92)}
    print("      shot error degrees: " + ", ".join(
        f"{st}={shot_err[st] * 2.8 / 128:.2f}" for st in shot_err))
    for st, got in shot_err.items():
        check(f"shot error at shooting {st}", abs(got - bm.kick_error_units(st, 12.0, 0.8)) <= 1, True)
    check("kick type 4 is a shot too", err_range(54, 4), err_range(54, 1))
    check("kick type 2 keeps its fixed perfect accuracy", err_range(40, 2), 0x20)
    for st in range(100):
        got = err_range(st, 6)
        check(f"pass error at passing {st}", abs(got - bm.kick_error_units(st, 5.0, 0.6)) <= 1, True)


    # --- retuned stat ranges, executed through the real game functions
    def speed(fn, stat):
        env = Env()
        env.w(PLAYER + 0x126, "<B", stat)
        return call(env, find(fn), (PLAYER,))

    def curve(stat):
        return min(max(mapped_stat(stat), 40), 99)

    def lerp(stat, lo, hi):
        return lo + (curve(stat) - 40) * (hi - lo) // 59

    for st in (45, 65, 90):
        check(f"sprint speed stat {st}", near(speed("_ZN7CPlayer14GetSprintSpeedEv", st), lerp(st, 2950, 5000)), True)
        check(f"jog speed stat {st}", near(speed("_ZN7CPlayer18GetAverageRunSpeedEv", st), lerp(st, 2950, 4600)), True)
    slow, fast = speed("_ZN7CPlayer14GetSprintSpeedEv", 62), speed("_ZN7CPlayer14GetSprintSpeedEv", 85)
    ratio_62_85 = fast / slow
    print(f"      sprint speed ratio stat85/stat62={ratio_62_85:.3f}")
    check("sprint speed ratio 62 vs 85 is 1.20..1.25", 1.20 <= ratio_62_85 <= 1.25, True)
    ratio_68_80 = (speed("_ZN7CPlayer14GetSprintSpeedEv", 80)
                   / speed("_ZN7CPlayer14GetSprintSpeedEv", 68))
    check("sprint speed ratio 68 vs 80 is at least 1.09", ratio_68_80 >= 1.09, True)
    jog_ratio = (speed("_ZN7CPlayer18GetAverageRunSpeedEv", 85)
                 / speed("_ZN7CPlayer18GetAverageRunSpeedEv", 62))
    check("jog speed ratio 62 vs 85 is 1.15..1.20", 1.15 <= jog_ratio <= 1.20, True)
    check("a slow player's sprint is still faster than his own jog",
          speed("_ZN7CPlayer14GetSprintSpeedEv", 55) > speed("_ZN7CPlayer18GetAverageRunSpeedEv", 55), True)

    def gk_react(stat):
        env = Env()
        env.w(PLAYER + 0x12B, "<B", stat)
        return call(env, find("_Z19GAI_GetReactionTimeP7CPlayer"), (PLAYER,))

    base = gk_react(99)
    check("GK reaction: elite keeper has no stat delay (only the difficulty term)", gk_react(99) == gk_react(120), True)
    check("GK reaction: poor keeper (45) +14 frames", gk_react(45) - base, 14)
    check("GK reaction: average keeper (72) ~7 frames", abs(gk_react(72) - base - 7) <= 1, True)

    # --- tackles: logistic duel at contact, before the existing ball-physics exits
    c2src = bm.cave2_asm(BM_CFG)
    lab = {n: bm.label_addr(c2src, n, bm.CAVE2)
           for n in ("cave2_tduel", "cave2_sduel", "cave2_foul")}
    TACK, DRIB = PLAYER, PLAYER + 0x400

    def tackle_env(tackling, control, rnd, has_ball=1, t_str=70, d_str=70, facing=False):
        env = Env(random=rnd, facing=facing)
        env.w(TACK + 0x123, "<B", tackling)
        env.w(TACK + 0x122, "<B", t_str)
        env.w(TACK + 0x2, "<H", 0x1234)
        env.w(TACK + 0x48, "<B", 1)
        env.w(DRIB + 0x127, "<B", control)
        env.w(DRIB + 0x122, "<B", d_str)
        env.w(DRIB + 0x84, "<B", has_ball)
        return env

    def standing(tackling, control, rnd, **kw):
        env = tackle_env(tackling, control, rnd, **kw)
        env.run(lab["cave2_tduel"], SENT_WIN, {UC_ARM_REG_R4: TACK, UC_ARM_REG_R9: DRIB, UC_ARM_REG_LR: SENT_WIN | 1,
                                             UC_ARM_REG_SP: STACK})
        pc = env.uc.reg_read(UC_ARM_REG_PC)
        if pc == SENT_WIN:
            assert env.uc.reg_read(UC_ARM_REG_R1) == bm.T1_LIT and env.uc.reg_read(UC_ARM_REG_R0) == 6
            assert env.uc.reg_read(UC_ARM_REG_SP) == STACK and env.uc.reg_read(UC_ARM_REG_R4) == TACK
            return "ball won", env
        assert pc == bm.T1_EPI and env.uc.reg_read(UC_ARM_REG_SP) == STACK
        return "tackler stumbles", env

    def slide(tackling, control, rnd, **kw):
        env = tackle_env(tackling, control, rnd, **kw)
        env.run(lab["cave2_sduel"], SENT_WIN, {UC_ARM_REG_R5: TACK, UC_ARM_REG_R8: DRIB, UC_ARM_REG_R0: 1,
                                             UC_ARM_REG_LR: SENT_WIN | 1, UC_ARM_REG_SP: STACK})
        pc = env.uc.reg_read(UC_ARM_REG_PC)
        if pc == SENT_WIN:
            assert env.uc.reg_read(UC_ARM_REG_R2) == bm.T2_LIT and env.uc.reg_read(UC_ARM_REG_R3) == 0xC
            assert env.uc.reg_read(UC_ARM_REG_R0) == 1
            return "ball won", env
        assert pc == bm.T2_SKIP and env.uc.reg_read(UC_ARM_REG_SP) == STACK
        assert env.r(TACK + 0x158, "<H") == 1 and env.uc.reg_read(UC_ARM_REG_R0) == 1
        return "slide misses ball", env

    check("standing: equal stats, roll 4999 (<50%)", standing(70, 70, 4999)[0], "ball won")
    res, env = standing(70, 70, 5000)
    check("standing: equal stats, roll 5000 -> tackler stumbles", res, "tackler stumbles")
    check("  stumble = CPlayer::Trip(tackler, 0, facing)", env.trip_calls, [(TACK, 0, 0x1234)])
    check("standing: loose ball (no carrier) always won", standing(40, 99, 9999, has_ball=0)[0], "ball won")
    check("standing: great defender 85 vs poor dribbler 60", standing(85, 60, 9000)[0], "ball won")
    check("standing: poor defender 50 vs star 90", standing(50, 90, 600)[0], "tackler stumbles")
    check("standing: strength +20 raises odds", standing(70, 70, 7500, t_str=85, d_str=65)[0], "ball won")
    check("standing: strength -20 lowers odds", standing(70, 70, 3000, t_str=60, d_str=80)[0], "tackler stumbles")
    check("standing: facing within 45 degrees adds z bonus",
          (standing(70, 70, 7000, facing=True)[0], standing(70, 70, 7400, facing=True)[0]),
          ("ball won", "tackler stumbles"))
    env = tackle_env(70, 70, 9999)
    env.trip_result = 0
    env.run(lab["cave2_tduel"], SENT_WIN, {UC_ARM_REG_R4: TACK, UC_ARM_REG_R9: DRIB, UC_ARM_REG_LR: SENT_WIN | 1,
                                         UC_ARM_REG_SP: STACK})
    check("standing: if Trip refused, tackle contact is cancelled", env.r(TACK + 0x144, "<i"), -1)
    check("slide: equal stats has both outcomes", (slide(70, 70, 3600)[0], slide(70, 70, 3800)[0]),
          ("ball won", "slide misses ball"))
    check("slide: strength term changes the odds", slide(70, 70, 5000, t_str=80, d_str=60)[0], "ball won")

    def foul(base, tackling):
        env = Env()
        env.w(TACK + 0x123, "<B", tackling)
        env.run(lab["cave2_foul"], SENT_WIN, {UC_ARM_REG_R0: base, UC_ARM_REG_R4: TACK, UC_ARM_REG_LR: SENT_WIN | 1,
                                            UC_ARM_REG_SP: STACK})
        assert env.uc.reg_read(UC_ARM_REG_R0) == 100
        return env.uc.reg_read(UC_ARM_REG_R6)

    print(f"      foul chance from base 40%: tackling50={foul(40, 50)}% 70={foul(40, 70)}% 90={foul(40, 90)}%")
    check("foul: clean tackler (90) fouls less", foul(40, 90) < foul(40, 70) < foul(40, 50), True)
    check("foul: smooth factor has no output floor/ceiling", (foul(1, 99), foul(100, 10)), (0, 139))

    from armdis import D as ORIGD, va2off as v2o
    patched = LIB[v2o(bm.C1):v2o(bm.C1) + 12]
    from capstone import CS_ARCH_ARM as A_, CS_MODE_THUMB as T_, Cs as C_
    dis = [f"{i.mnemonic} {i.op_str}" for i in C_(A_, T_).disasm(patched, bm.C1)]
    check("collision push weights = strength squared", dis, ["mul r4, r1, r1", "mul r8, r0, r0", "nop.w "])
    check("GL_SetTouch left stock (tackle reactions intact)", LIB[v2o(0x2E6686):v2o(0x2E6686) + 4], ORIGD[v2o(0x2E6686):v2o(0x2E6686) + 4])

    def sprint_drain(stamina, sprinting):
        # run the two UpdateSprint InterpolateClamp blocks (0x2da228 / 0x2da264) up to the call return
        env = Env()
        env.w(PLAYER + 0x121, "<B", stamina)
        start, stop = (0x2DA228, 0x2DA23E) if sprinting else (0x2DA264, 0x2DA27A)
        env.run(start, stop, {UC_ARM_REG_R4: PLAYER, UC_ARM_REG_SP: STACK})
        return env.uc.reg_read(UC_ARM_REG_R0)

    check("stamina: drain unfit(40)=0x600 fit(99)=0x280", (sprint_drain(40, True), sprint_drain(99, True)), (0x600, 0x280))
    check("stamina: recovery unfit=0x280 fit=0x600", (sprint_drain(40, False), sprint_drain(99, False)), (0x280, 0x600))
    drains = [sprint_drain(s, True) for s in range(1, 100)]
    recs = [sprint_drain(s, False) for s in range(1, 100)]
    print(f"      stamina 1/20/40: drain {drains[0]:#x}/{drains[19]:#x}/{drains[39]:#x}, "
          f"recovery {recs[0]:#x}/{recs[19]:#x}/{recs[39]:#x}")
    check("stamina: drain/recovery keep changing below 40 (no flat region, steps <= 16 per point)",
          all(b < a for a, b in zip(drains, drains[1:])) and all(b > a for a, b in zip(recs, recs[1:]))
          and max(max(a - b for a, b in zip(drains, drains[1:])), max(b - a for a, b in zip(recs, recs[1:]))) <= 16,
          True)

    def gk_delay(stat):
        env = Env()
        env.w(PLAYER + 0x12B, "<B", stat)
        env.run(0x29F730, 0x29F740, {UC_ARM_REG_R4: PLAYER, UC_ARM_REG_R6: 0, UC_ARM_REG_SP: STACK})
        return env.uc.reg_read(UC_ARM_REG_R0)

    delays = [gk_delay(s) for s in range(1, 100)]
    print(f"      GK reaction delay (frames) at 1/30/45/60/80/99: "
          f"{[delays[s - 1] for s in (1, 30, 45, 60, 80, 99)]}")
    check("GK reaction: 25 frames at stat 1 falling to 0 at 99, no flat region, <= 1 frame per point",
          delays[0] == 25 and delays[98] == 0 and all(0 <= a - b <= 1 for a, b in zip(delays, delays[1:]))
          and delays[44] in (13, 14), True)

    # --- animation crossfade (CPlayer::Animate)
    def blend(weight, accel, control):
        env = Env()
        env.w(PLAYER + 0x125, "<B", accel)
        env.w(PLAYER + 0x127, "<B", control)
        lab = bm.label_addr(bm.cave_asm(BM_CFG), "cave_blend", bm.CAVE)
        env.run(lab, SENT_WIN, {UC_ARM_REG_R0: weight, UC_ARM_REG_R1: 0xABCD, UC_ARM_REG_R4: PLAYER,
                                UC_ARM_REG_LR: SENT_WIN | 1, UC_ARM_REG_SP: STACK})
        assert env.uc.reg_read(UC_ARM_REG_R1) == 0xABCD, "r1 clobbered"
        return env.uc.reg_read(UC_ARM_REG_R0)

    def ticks(accel, control):
        w, n = 0x2000, 0
        while w:
            w = blend(w, accel, control)
            n += 1
            assert n < 40
        return n

    tw, tm, tb = ticks(40, 40), ticks(65, 70), ticks(99, 99)
    print(f"      crossfade length: clumsy={tw} ticks, average={tm}, elite={tb} (stock 8 for everyone)")
    check("clumsy player: ~13-14 tick crossfade", tw in (13, 14), True)
    check("elite player: ~9 tick crossfade", tb == 9, True)
    check("crossfade never wraps below zero", blend(0x100, 99, 99), 0)
    check("agility averages acceleration+control", ticks(99, 40) == ticks(40, 99) == ticks(70, 69), True)
    decays = [0x2000 - blend(0x2000, a, a) for a in range(0, 100)]
    print(f"      crossfade decay per tick at (acc+ctrl)/2 = 0/20/40/99: "
          f"{decays[0]:#x}/{decays[20]:#x}/{decays[40]:#x}/{decays[99]:#x}")
    check("crossfade: decay keeps falling below 40 (no clamp), positive, <= 6 per point",
          all(b > a for a, b in zip(decays, decays[1:])) and decays[0] >= 0x100
          and max(b - a for a, b in zip(decays, decays[1:])) <= 6, True)

    # The new caves must also work after Android relocates the shared object.
    nz = Env(base=NONZERO_BASE, capture_interp=True)
    c2src = bm.cave2_asm(BM_CFG)
    nlab = {n: bm.label_addr(c2src, n, bm.CAVE2) for n in (
        "cave2_stat", "cave2_err_entry", "cave2_err_exit", "cave2_duel_probability",
        "cave2_duel_roll", "cave2_tduel", "cave2_sduel", "cave2_foul")}

    stat_ok = True
    stat_args_ok = True
    for raw in range(100):
        nz.w(nz.stack, "<I", 777)
        nz.run(nlab["cave2_stat"], nz.sentinel, {
            UC_ARM_REG_R0: raw, UC_ARM_REG_R1: 40, UC_ARM_REG_R2: 99, UC_ARM_REG_R3: 1234,
            UC_ARM_REG_LR: nz.sentinel | 1, UC_ARM_REG_SP: nz.stack})
        r0, r1, r2, r3, fifth = nz.interp_calls[-1]
        expected = mapped_stat(raw)
        stat_ok &= abs(r0 - expected) <= 1
        stat_args_ok &= (r1, r2, r3, fifth) == (40, 99, 1234, 777)
    check("CAVE2 stat map matches the curve for raw 0..99 at base 0x40000000", stat_ok, True)
    check("CAVE2 stat map preserves r1-r3 and the stack argument", stat_args_ok, True)

    nz.stop_addrs.add(NONZERO_BASE + bm.S2_CONT)
    entry_sp = nz.stack
    nz.run(nlab["cave2_err_entry"], nz.sentinel, {
        UC_ARM_REG_R0: nz.player, UC_ARM_REG_R1: 6, UC_ARM_REG_R4: 0x44,
        UC_ARM_REG_R5: 0x55, UC_ARM_REG_R6: 0x66, UC_ARM_REG_R7: 0x77,
        UC_ARM_REG_LR: nz.sentinel | 1, UC_ARM_REG_SP: entry_sp})
    stored_type = nz.r(entry_sp - 0x28 + 0x0C, "<I")
    entry_ok = (nz.uc.reg_read(UC_ARM_REG_PC) == NONZERO_BASE + bm.S2_CONT
                and nz.uc.reg_read(UC_ARM_REG_SP) == entry_sp - 0x28 and stored_type == 6)
    nz.stop_addrs.clear()
    check("CAVE2 kick-error entry saves the type and reaches the original body", entry_ok, True)

    err_ok = True
    err_stack_ok = True
    for raw in range(100):
        nz.w(nz.player + 0x12A, "<B", raw)
        nz.w(nz.player + 0x128, "<B", raw)
        for kick_type, worst, best in ((1, 12.0, 0.8), (6, 5.0, 0.6)):
            frame = bytearray(0x28)
            struct.pack_into("<I", frame, 0x0C, kick_type)
            struct.pack_into("<4I", frame, 0x14, 0x44, 0x55, 0x66, 0x77)
            struct.pack_into("<I", frame, 0x24, nz.sentinel | 1)
            nz.uc.mem_write(nz.stack, bytes(frame))
            nz.run(nlab["cave2_err_exit"], nz.sentinel, {
                UC_ARM_REG_R4: nz.player, UC_ARM_REG_SP: nz.stack})
            got = nz.uc.reg_read(UC_ARM_REG_R0)
            err_ok &= abs(got - bm.kick_error_units(raw, worst, best)) <= 1
            err_stack_ok &= (nz.uc.reg_read(UC_ARM_REG_PC) == nz.sentinel
                             and nz.uc.reg_read(UC_ARM_REG_SP) == nz.stack + 0x28)
    check("CAVE2 raw shot/pass errors match their formulas for stats 0..99", err_ok, True)
    check("CAVE2 error exit restores its saved registers and stack", err_stack_ok, True)

    duel_probability_ok = True
    duel_prob_stack_ok = True
    # Raw player stats can produce z2 from -303 through +313, including the slide shift.
    for z2 in range(-303, 314):
        for slide_mode in (0, 1):
            nz.run(nlab["cave2_duel_probability"], nz.sentinel, {
                UC_ARM_REG_R0: z2, UC_ARM_REG_R1: slide_mode,
                UC_ARM_REG_R4: 0x44, UC_ARM_REG_R5: 0x55,
                UC_ARM_REG_R6: 0x66, UC_ARM_REG_R7: 0x77,
                UC_ARM_REG_LR: nz.sentinel | 1, UC_ARM_REG_SP: nz.stack})
            got = nz.uc.reg_read(UC_ARM_REG_R0) / 256.0
            want = bm.duel_probability(z2 / 2.0, bool(slide_mode),
                                       BM_CFG.duel_scale, BM_CFG.slide_shift)
            duel_probability_ok &= abs(got - want) <= 1.0
            duel_prob_stack_ok &= (nz.uc.reg_read(UC_ARM_REG_PC) == nz.sentinel
                                   and nz.uc.reg_read(UC_ARM_REG_SP) == nz.stack
                                   and (nz.uc.reg_read(UC_ARM_REG_R4), nz.uc.reg_read(UC_ARM_REG_R5),
                                        nz.uc.reg_read(UC_ARM_REG_R6), nz.uc.reg_read(UC_ARM_REG_R7))
                                   == (0x44, 0x55, 0x66, 0x77))
    check("CAVE2 standing/slide tables match across all valid half-point z values", duel_probability_ok, True)
    check("CAVE2 probability helper preserves stack and return state", duel_prob_stack_ok, True)

    foul_ok = True
    for tackling in range(100):
        nz.w(nz.player + 0x123, "<B", tackling)
        nz.run(nlab["cave2_foul"], nz.sentinel, {
            UC_ARM_REG_R0: 40, UC_ARM_REG_R4: nz.player,
            UC_ARM_REG_LR: nz.sentinel | 1, UC_ARM_REG_SP: nz.stack})
        q12 = bm.foul_table_q12(BM_CFG.foul_softness)[tackling]
        foul_ok &= nz.uc.reg_read(UC_ARM_REG_R6) == (40 * q12 >> 12)
    check("CAVE2 smooth foul factor matches the table for tackling 0..99", foul_ok, True)

    roll_ok = True
    tack_ptr, drib_ptr = nz.player, nz.player + 0x400
    nz.w(drib_ptr + 0x127, "<B", 50)
    nz.w(tack_ptr + 0x122, "<B", 70)
    nz.w(drib_ptr + 0x122, "<B", 70)
    for tackling in range(100):
        nz.w(tack_ptr + 0x123, "<B", tackling)
        for flags in (0, 4):
            nz.random = 0
            nz.run(nlab["cave2_duel_roll"], nz.sentinel, {
                UC_ARM_REG_R0: tack_ptr, UC_ARM_REG_R1: drib_ptr, UC_ARM_REG_R2: 0,
                UC_ARM_REG_R3: flags, UC_ARM_REG_LR: nz.sentinel | 1, UC_ARM_REG_SP: nz.stack})
            roll_ok &= nz.uc.reg_read(UC_ARM_REG_R0) == 1
            nz.random = 9999
            nz.run(nlab["cave2_duel_roll"], nz.sentinel, {
                UC_ARM_REG_R0: tack_ptr, UC_ARM_REG_R1: drib_ptr, UC_ARM_REG_R2: 0,
                UC_ARM_REG_R3: flags, UC_ARM_REG_LR: nz.sentinel | 1, UC_ARM_REG_SP: nz.stack})
            roll_ok &= nz.uc.reg_read(UC_ARM_REG_R0) == 0
    check("CAVE2 duel roll runs both outcomes for raw tackling 0..99", roll_ok, True)
    check("CAVE2 random stub receives 10000-point rolls", bool(nz.random_calls) and set(nz.random_calls) == {10000}, True)

    wrapper_ok = True
    nz.w(tack_ptr + 0x123, "<B", 70)
    nz.w(drib_ptr + 0x127, "<B", 70)
    nz.w(drib_ptr + 0x84, "<B", 1)
    nz.w(tack_ptr + 0x2, "<H", 0x1234)
    nz.w(tack_ptr + 0x48, "<B", 1)
    for name, regs, lose_pc in (
        ("cave2_tduel", {UC_ARM_REG_R4: tack_ptr, UC_ARM_REG_R9: drib_ptr}, NONZERO_BASE + bm.T1_EPI),
        ("cave2_sduel", {UC_ARM_REG_R5: tack_ptr, UC_ARM_REG_R8: drib_ptr, UC_ARM_REG_R0: 1},
         NONZERO_BASE + bm.T2_SKIP),
    ):
        nz.random = 0
        regs_win = dict(regs, **{})
        regs_win.update({UC_ARM_REG_LR: nz.sentinel | 1, UC_ARM_REG_SP: nz.stack})
        nz.run(nlab[name], nz.sentinel, regs_win)
        wrapper_ok &= (nz.uc.reg_read(UC_ARM_REG_PC) == nz.sentinel
                       and nz.uc.reg_read(UC_ARM_REG_SP) == nz.stack)
        nz.random = 9999
        regs_lose = dict(regs_win)
        nz.run(nlab[name], nz.sentinel, regs_lose)
        wrapper_ok &= (nz.uc.reg_read(UC_ARM_REG_PC) == lose_pc
                       and nz.uc.reg_read(UC_ARM_REG_SP) == nz.stack)
    check("CAVE2 standing/slide wrappers keep both original exits at a non-zero base", wrapper_ok, True)

    # --- A2 control-tier cave (SetAnimFromStateLoco): stock pools unchanged, appended IDs not touched
    a2 = Env(base=NONZERO_BASE)
    a2.stop_addrs = {NONZERO_BASE + 0x2E0F78, NONZERO_BASE + 0x2E0F88}
    bias = BM_CFG.dribble_style_bias
    a2_ok = True
    a2_fail = []
    for clip in [a for rr in bm.DRIBBLE_STYLE_POOLS.values() for a, b in rr] + \
            [b for rr in bm.DRIBBLE_STYLE_POOLS.values() for a, b in rr] + list(range(2535, 2561)) + [2244, 2291]:
        for control in (20, 59, 60, 84, 85, 99):
            a2.w(a2.player + 0x127, "<B", control)
            a2.w(a2.stack + 0x18, "<I", a2.player)
            tier = "low" if control < BM_CFG.dribble_skill_low else "mid" if control < BM_CFG.dribble_skill_high else "high"
            want = 0
            for name, ranges in bm.DRIBBLE_STYLE_POOLS.items():
                if any(x <= clip <= y for x, y in ranges):
                    want = -2 * bias if name == tier else bias // 2
            for best in (0x40000, 0x10000):
                a2.run(bm.A2, a2.sentinel, {UC_ARM_REG_R10: clip, UC_ARM_REG_R5: 0x20000, UC_ARM_REG_R11: best,
                                            UC_ARM_REG_SP: a2.stack})
                got = a2.uc.reg_read(UC_ARM_REG_R5)
                got = got - (1 << 32) if got & 0x80000000 else got
                pc = a2.uc.reg_read(UC_ARM_REG_PC) - NONZERO_BASE
                want_pc = 0x2E0F78 if 0x20000 + want < best else 0x2E0F88
                if got - 0x20000 != want or pc != want_pc or a2.uc.reg_read(UC_ARM_REG_R10) != clip:
                    a2_ok = False
                    a2_fail.append((clip, control, got - 0x20000, want))
    check("A2 dribble tiers: stock pool biases unchanged, appended IDs 2535-2560 untouched", a2_ok, True)
    if a2_fail:
        print("      ", a2_fail[:6])

    # --- anim_fit: ability-based selection of the appended DLS26 clips + static rule coverage
    import test_anim_fit
    check("anim_fit (test_anim_fit.py): hooks, rules and appended-ID coverage", test_anim_fit.run(LIB_PATH), True)

    print("ALL PASS" if ok else "FAILURES")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
