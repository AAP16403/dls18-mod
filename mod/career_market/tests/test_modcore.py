"""End-to-end test of the modcore runtime-hook framework under Unicorn.

1. Extends a libDLS18.so (elf_extend) and adds one runtime hook "test_double" on the real game
   function `Double(int*)` (0x1fc6e4: ldr r1,[r0]; lsls r1,r1,#1; str r1,[r0]; bx lr).
2. Loads that library at a non-zero base, segment by segment, as the Android loader would, and loads
   a test library built from ../modcore.c + modcore_test_handlers.c (relocations applied).
3. Runs Double() with: an empty slot (stock behaviour), a pass-through handler, and an overriding
   handler that sets the result, changes r1 and resumes at the site's `bx lr`.
usage: python test_modcore.py <libDLS18.so> <modcore_test.so>
"""
import struct
import sys
from pathlib import Path

from unicorn import UC_ARCH_ARM, UC_MODE_THUMB, UC_PROT_ALL, Uc
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
import elf_extend       # noqa: E402
import modcore_build    # noqa: E402

GAME = 0x40000000
TESTLIB = 0x20000000
STACK = 0x7F000000
STOP = 0x00002000
DATA = 0x10000000
SITE = 0x1FC6E4


def build_game(path):
    lib, layout = elf_extend.extend(open(path, "rb").read())
    info = modcore_build.emit(lib, layout, [dict(name="test_double", site=SITE, length=4)])
    elf_extend.check(lib, layout)
    return lib, layout, info


def load_so(uc, data, base):
    phoff, = struct.unpack_from("<I", data, 0x1C)
    phnum, = struct.unpack_from("<H", data, 0x2C)
    top, dyn = 0, None
    segs = []
    for i in range(phnum):
        t, off, va, _, fsz, msz, _, _ = struct.unpack_from("<8I", data, phoff + i * 32)
        if t == 1:
            segs.append((off, va, fsz))
            top = max(top, va + msz)
        elif t == 2:
            dyn = (off, fsz)
    uc.mem_map(base, (top + 0xFFFF) & ~0xFFFF, UC_PROT_ALL)
    for off, va, fsz in segs:
        uc.mem_write(base + va, data[off:off + fsz])
    if not dyn:
        return {}
    tags = {}
    for i in range(dyn[1] // 8):
        tag, val = struct.unpack_from("<iI", data, dyn[0] + i * 8)
        if tag == 0:
            break
        tags[tag] = val
    symtab, strtab = tags[6], tags[5]
    syms = {}
    nsyms = struct.unpack_from("<I", data, tags[4] + 4)[0] if 4 in tags else 0
    for k in range(nsyms):
        n, v, sz, info, oth, shn = struct.unpack_from("<IIIBBH", data, symtab + k * 16)
        name = data[strtab + n:data.index(b"\0", strtab + n)].decode()
        if v:
            syms[name] = v
    for st, szt in ((17, 18), (23, 2)):
        if st not in tags:
            continue
        for k in range(tags[szt] // 8):
            r_off, r_info = struct.unpack_from("<II", data, tags[st] + k * 8)
            rtype = r_info & 0xFF
            where = base + r_off
            cur = struct.unpack("<I", uc.mem_read(where, 4))[0]
            if rtype == 23:
                uc.mem_write(where, struct.pack("<I", (cur + base) & 0xFFFFFFFF))
            else:
                raise SystemExit(f"unexpected relocation type {rtype} in test library")
    return syms


def main():
    game, layout, info = build_game(sys.argv[1])
    test = open(sys.argv[2], "rb").read()
    ok = True
    for mode in (0, 1, 2):
        uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        img = elf_extend.load_image(game, 0, 0xA00000)
        uc.mem_map(GAME, len(img), UC_PROT_ALL)
        uc.mem_write(GAME, bytes(img))
        syms = load_so(uc, test, TESTLIB)
        uc.mem_map(STACK - 0x10000, 0x20000, UC_PROT_ALL)
        uc.mem_map(DATA, 0x1000, UC_PROT_ALL)
        uc.mem_map(0, 0x10000, UC_PROT_ALL)
        uc.mem_write(STOP, b"\x00\xbf\xfe\xe7")

        def call(addr, *args, sp=STACK):
            for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1), args):
                uc.reg_write(r, v)
            uc.reg_write(UC_ARM_REG_SP, sp)
            uc.reg_write(UC_ARM_REG_LR, STOP | 1)
            uc.emu_start(addr | 1, STOP, count=200000)
            return uc.reg_read(UC_ARM_REG_R0)

        installed = call(TESTLIB + syms["test_init"], GAME, mode) if mode else 0
        uc.mem_write(DATA, struct.pack("<i", 21))
        call(GAME + SITE, DATA, 0, sp=STACK - 0x14)          # an odd stack alignment on purpose
        value = struct.unpack("<i", uc.mem_read(DATA, 4))[0]
        r1 = uc.reg_read(UC_ARM_REG_R1)
        sp = uc.reg_read(UC_ARM_REG_SP)
        calls = call(TESTLIB + syms["test_calls"]) if mode else 0
        if mode == 0:
            good = value == 42 and sp == STACK - 0x14
        elif mode == 1:
            seen_r0 = call(TESTLIB + syms["test_seen_r0"])
            seen_sp = call(TESTLIB + syms["test_seen_sp"])
            good = installed == 1 and value == 42 and calls == 1 and seen_r0 == DATA and \
                seen_sp == STACK - 0x14 and sp == STACK - 0x14
        else:
            good = installed == 1 and value == 1000 and r1 == 0x1234 and calls == 1 and sp == STACK - 0x14
        ok &= good
        print(f"mode {mode}: {'OK ' if good else 'BAD'} value={value} r1={r1:#x} calls={calls} sp_delta={sp - (STACK - 0x14)}")
    print(f"layout: CAVE3 {layout['cave3_va']:#x} ({layout['cave3_size']:#x} bytes), MODDATA {layout['moddata_va']:#x}; "
          f"stub {info['test_double']['stub']:#x}")
    print("ALL OK" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
