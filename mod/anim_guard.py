"""Guard CPlayer::SetAnimControl against a zero divisor from animation data (anim_guard, v35a).

Device crash 2026-09-27 (v35 + DLS26 anims v15_turns): SIGFPE from __aeabi_idiv0 (0x5c1282) called by
CPlayer::SetAnimControl+0x392 (bl 0x5c05e8 at 0x2e2c52), reached from CPlayer::ControlTakeBall ->
UpdateTake. The divisor there, [sp+0xb4] (fp), is the number of ticks until the clip's action point
(the ball contact); an appended clip (r10 = 0x9f9 = 2553 at the crash) gives 0 ticks. The same fp
divides twice more (0x2e2cbe, 0x2e2ccc), and SetAnimControl also divides by the clip's
TAnimData+0x18 (0x2e2a26) and by a derived angle term (0x2e29fc). Stock clips never produce a zero
divisor, so the guard changes nothing for them.

Every `bl __aeabi_idiv` (0x5c05e8, libgcc, local to the library) in SetAnimControl is redirected to a
four-instruction stub in CAVE3:
    safe_idiv: cmp r1, #0 ; it eq ; moveq r1, #1 ; b.w 0x5c05e8
The stub keeps every register and flag contract of __aeabi_idiv (callers treat flags as clobbered
across a call; r1 is an argument register). It lives in the 256 bytes just below the squad_caps region
(the last 64 KB of CAVE3), after checking they are unused (zero).
"""
from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs

import elf_extend

IDIV = 0x5C05E8
SITES = (0x2E29FC, 0x2E2A26, 0x2E2C52, 0x2E2CBE, 0x2E2CCC)   # bl __aeabi_idiv in CPlayer::SetAnimControl
STUB_SIZE = 0x100
SQUAD_REGION = 0x10000
CS = Cs(CS_ARCH_ARM, CS_MODE_THUMB)


def _one(code, addr):
    ins = list(CS.disasm(bytes(code), addr))
    if len(ins) != 1:
        raise SystemExit(f"anim_guard: {addr:#x} does not decode to one instruction")
    return ins[0]


def apply(lib, orig, va2off, layout, asm):
    """Patch `lib` (bytearray, extended) in place. Returns the stub address."""
    if layout is None:
        raise SystemExit("anim_guard: needs the CAVE3 segment (drop --no-extend)")
    stub = layout["cave3_va"] + layout["cave3_size"] - SQUAD_REGION - STUB_SIZE
    off = elf_extend.va2off(lib, stub)
    if any(lib[off:off + STUB_SIZE]):
        raise SystemExit(f"anim_guard: CAVE3 {stub:#x} is not free")
    code = asm("cmp r1, #0\nit eq\nmoveq r1, #1", stub)
    tail = asm(f"b.w {IDIV:#x}", stub + len(code))
    ins = _one(tail, stub + len(code))
    if ins.mnemonic != "b.w" or int(ins.op_str.lstrip("#"), 16) != IDIV:
        raise SystemExit("anim_guard: tail branch mis-encoded")
    blob = code + tail
    lib[off:off + len(blob)] = blob
    for site in SITES:
        so = elf_extend.va2off(lib, site)
        oo = va2off(site)
        stock = _one(orig[oo:oo + 4], site)
        if stock.mnemonic != "bl" or int(stock.op_str.lstrip("#"), 16) != IDIV:
            raise SystemExit(f"anim_guard: {site:#x} is not the stock bl __aeabi_idiv")
        if bytes(lib[so:so + 4]) != bytes(orig[oo:oo + 4]):
            raise SystemExit(f"anim_guard: {site:#x} was already patched by another change")
        new = asm(f"bl {stub:#x}", site)
        got = _one(new, site)
        if got.mnemonic != "bl" or int(got.op_str.lstrip("#"), 16) != stub or len(new) != 4:
            raise SystemExit(f"anim_guard: bl at {site:#x} mis-encoded")
        lib[so:so + 4] = new
    return stub
