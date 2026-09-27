"""CAVE3 generator: runtime C hooks through MODDATA slots (see career_market/modcore.c).

For each hook {name, site, length}:
  site:   b.w mc_hook_i (+ nop padding when length > 4)
  CAVE3:  mc_hook_i: sub sp,#4; push {r0}; r0 = &slot_i (movw/movt delta + add r0, pc); ldr r0,[r0]
                     cbz r0, orig; str r0,[sp,#4]; pop {r0, pc}          -> C thunk, state untouched
          orig:      pop {r0}; add sp,#4
          resume_i:  <displaced instructions>; b.w site+length
The descriptor at the start of CAVE3 lists {site, resume|1, fnv1a(name), length} per hook.
Nothing in a stub changes the flags before the displaced instructions run (sub/add sp, push/pop,
movw/movt, add r0,pc, ldr, cbz and str never set flags).
"""
import struct

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs
from keystone import KS_ARCH_ARM, KS_MODE_THUMB, Ks

import elf_extend

MAGIC = 0x3144434D              # "MCD1"
CS = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
KS = Ks(KS_ARCH_ARM, KS_MODE_THUMB)
BRANCHES = ("b", "bl", "blx", "bx", "cbz", "cbnz", "tbb", "tbh")


def fnv1a(name):
    h = 2166136261
    for ch in name.encode():
        h = ((h ^ ch) * 16777619) & 0xFFFFFFFF
    return h


def asm_at(src, addr):
    enc, count = KS.asm(src, addr)
    lines = [l for l in src.strip().splitlines() if l.strip() and not l.strip().endswith(":")]
    if count != len(lines):
        raise SystemExit(f"modcore_build: keystone assembled {count} of {len(lines)} statements at {addr:#x}")
    return bytes(enc)


def bw(frm, to):
    """b.w from `frm` to `to`, verified by disassembly (keystone can mis-encode far branches)."""
    code = asm_at(f"b.w {to:#x}", frm)
    ins = list(CS.disasm(code, frm))
    assert len(ins) == 1 and ins[0].mnemonic == "b.w" and int(ins[0].op_str.lstrip("#"), 16) == to, \
        f"b.w {frm:#x} -> {to:#x} mis-encoded"
    return code


def check_displaced(lib, site, length):
    off = elf_extend.va2off(lib, site)
    code = bytes(lib[off:off + length])
    ins = list(CS.disasm(code, site))
    if sum(i.size for i in ins) != length:
        raise SystemExit(f"hook at {site:#x}: {length} bytes do not end on an instruction boundary")
    for i in ins:
        if i.mnemonic.split(".")[0] in BRANCHES or "pc" in i.op_str or i.mnemonic.startswith("it"):
            raise SystemExit(f"hook at {site:#x}: displaced '{i.mnemonic} {i.op_str}' is not position independent")
    return code


def emit(lib, layout, hooks):
    """Writes the descriptor, stubs and site patches into `lib` (a bytearray, already extended).
    Returns {name: {slot, stub, resume}}."""
    cave = layout["cave3_va"]
    slots = layout["moddata_va"]
    desc_size = 16 + 16 * len(hooks)
    pc = cave + ((desc_size + 3) & ~3)
    blobs, entries, info = [], [], {}
    for i, h in enumerate(hooks):
        site, length = h["site"], h["length"]
        displaced = check_displaced(lib, site, length)
        slot_va = slots + 4 * i
        start = pc
        head = asm_at("sub sp, #4\npush {r0}", pc)
        pc += len(head)
        # movw/movt a constant, then `add r0, pc`: the constant is the distance from that add's pc
        anchor = pc + 8                   # movw (4) + movt (4), then the add
        delta = (slot_va - (anchor + 4)) & 0xFFFFFFFF
        mid = asm_at(f"movw r0, #{delta & 0xFFFF:#x}\nmovt r0, #{delta >> 16:#x}\nadd r0, pc\nldr r0, [r0]", pc)
        pc += len(mid)
        # cbz orig (forward over: str (2) + pop (2)), then str/pop
        tail_src = "str r0, [sp, #4]\npop {r0, pc}"
        tail = asm_at(tail_src, pc + 2)
        cbz = asm_at(f"cbz r0, {pc + 2 + len(tail):#x}", pc)
        pc += len(cbz) + len(tail)
        orig = asm_at("pop {r0}\nadd sp, #4", pc)
        pc += len(orig)
        resume = pc
        pc += len(displaced)
        back = bw(pc, site + length)
        pc += len(back)
        if pc % 4:
            pad = b"\x00\xbf"
            pc += 2
        else:
            pad = b""
        blob = head + mid + cbz + tail + orig + displaced + back + pad
        blobs.append((start, blob))
        entries.append(struct.pack("<IIII", site, resume | 1, fnv1a(h["name"]), length))
        info[h["name"]] = dict(slot=i, stub=start, resume=resume, slot_va=slot_va)
        # site patch
        site_patch = bw(site, start) + b"\x00\xbf" * ((length - 4) // 2)
        if length < 4 or (length - 4) % 2:
            raise SystemExit(f"hook at {site:#x}: needs 4 + 2n displaced bytes")
        o = elf_extend.va2off(lib, site)
        lib[o:o + length] = site_patch
    if pc - cave > layout["cave3_size"]:
        raise SystemExit("CAVE3 full")
    header = struct.pack("<IIII", MAGIC, len(hooks), slots, 1)
    o = elf_extend.va2off(lib, cave)
    lib[o:o + len(header)] = header
    for k, e in enumerate(entries):
        lib[o + 16 + 16 * k:o + 32 + 16 * k] = e
    for start, blob in blobs:
        so = elf_extend.va2off(lib, start)
        lib[so:so + len(blob)] = blob
        verify_stub(blob, start, cave, layout["cave3_size"])
    return info


def verify_stub(blob, start, cave, size):
    """Every branch in a stub stays inside CAVE3 or is the verified b.w back to its site."""
    for i in CS.disasm(blob, start):
        m = i.mnemonic.split(".")[0]
        if m in ("bx", "blx"):
            raise SystemExit(f"CAVE3 stub {start:#x}: unexpected {i.mnemonic}")
        if m in ("b", "bl", "cbz", "cbnz") and i.op_str.startswith("#"):
            tgt = int(i.op_str.lstrip("#"), 16) if m in ("b", "bl") else int(i.op_str.split("#")[-1], 16)
            if not (cave <= tgt < cave + size) and m != "b":
                raise SystemExit(f"CAVE3 stub {start:#x}: {i.mnemonic} {i.op_str} leaves the cave")
