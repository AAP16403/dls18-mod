"""Run the real ARM libCareerMarket.so under Unicorn with the harness's mock game and compare results.

The x86 harness (harness.c) compiles native_bridge.c for the host. This script executes the actual
armeabi-v7a build with the same mocks (same roster/link semantics, same serializer) and computes the
same API digest (club views, transfer history, live rosters). Matching digests mean the shipped ARM
binary behaves exactly like the tested C code.

Usage: python arm_crosscheck.py <libCareerMarket.so> <dataset.txt> [seasons]
Compare with: harness <dataset.txt> <seasons> 1 --restart-every -1 --no-user   (api_digest=...)
"""
import struct
import sys

from unicorn import (UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_THUMB, UC_PROT_ALL, Uc, UcError)
from unicorn.arm_const import (UC_ARM_REG_CPSR, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP)

LIB = 0x40000000
FAKE = 0x10000000
FAKE_SIZE = 0x900000
HEAP = 0x30000000
HEAP_SIZE = 0x01000000
STACK = 0x7F000000
STACK_SIZE = 0x100000
STOP = 0x00002000
STRIDE = 0x108
USER = 0x102
DB_INSTANCE = 0x75CCA4
SEASON = FAKE + 0x84A260 + 0x14
COINS = FAKE + 0x84A260 + 0x2A7CC

uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
heap_next = HEAP


def alloc(size):
    global heap_next
    addr = heap_next
    heap_next += (size + 15) & ~15
    assert heap_next < HEAP + HEAP_SIZE
    return addr


def rd32(a):
    return struct.unpack("<I", uc.mem_read(a, 4))[0]


def rds32(a):
    return struct.unpack("<i", uc.mem_read(a, 4))[0]


def wr32(a, v):
    uc.mem_write(a, struct.pack("<I", v & 0xFFFFFFFF))


# ---------------------------------------------------------------- load the ELF shared object
def load_so(path):
    data = open(path, "rb").read()
    phoff, = struct.unpack_from("<I", data, 0x1C)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x2A)
    top = 0
    dyn = None
    segs = []
    for i in range(phnum):
        p_type, off, vaddr, _, filesz, memsz, _, _ = struct.unpack_from("<8I", data, phoff + i * phentsize)
        if p_type == 1:
            segs.append((off, vaddr, filesz, memsz))
            top = max(top, vaddr + memsz)
        elif p_type == 2:
            dyn = (off, filesz)
    uc.mem_map(LIB, (top + 0xFFFF) & ~0xFFFF, UC_PROT_ALL)
    for off, vaddr, filesz, memsz in segs:
        uc.mem_write(LIB + vaddr, data[off:off + filesz])
    tags = {}
    for i in range(dyn[1] // 8):
        tag, val = struct.unpack_from("<iI", data, dyn[0] + i * 8)
        if tag == 0:
            break
        tags[tag] = val
    symtab, strtab = tags[6], tags[5]

    def sym(index):
        name_off, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, symtab + index * 16)
        end = data.index(b"\0", strtab + name_off)
        return data[strtab + name_off:end].decode(), value

    for start_tag, size_tag in ((17, 18), (23, 2)):
        if start_tag not in tags:
            continue
        for k in range(tags[size_tag] // 8):
            r_off, r_info = struct.unpack_from("<II", data, tags[start_tag] + k * 8)
            rtype, rsym = r_info & 0xFF, r_info >> 8
            where = LIB + r_off
            if rtype == 23:                         # R_ARM_RELATIVE
                wr32(where, rd32(where) + LIB)
            elif rtype in (21, 22):                 # GLOB_DAT / JUMP_SLOT
                name, value = sym(rsym)
                assert value, f"undefined symbol {name}"
                wr32(where, LIB + value)
            elif rtype == 2:                        # ABS32
                name, value = sym(rsym)
                wr32(where, rd32(where) + LIB + value)
            else:
                raise SystemExit(f"unsupported relocation {rtype}")
    exports = {}
    nsyms = (tags[5] - tags[6]) // 16
    for i in range(1, nsyms):
        name, value = sym(i)
        if name.startswith("career_market_") and value:
            exports[name] = LIB + value
    return exports


# ---------------------------------------------------------------- mock game state (mirrors harness.c)
players = {}           # pid -> (pos, rating, value)
valid, kind = {}, {}
links_order = []
db = 0
live = default = 0
override = 0
pristine = b""
link_count = 0


def cur_table():
    return override if override else live


def link_in(table, team):
    for i in range(link_count):
        a = table + i * STRIDE
        if rds32(a) == team:
            return a
    return 0


def lcount(l):
    return rds32(l + 4)


def lids(l):
    return [rds32(l + 0x88 + 4 * i) for i in range(lcount(l))]


def lfind(l, pid):
    if not l:
        return -1
    for i in range(lcount(l)):
        if rds32(l + 0x88 + 4 * i) == pid:
            return i
    return -1


def lremove(l, pid):
    i = lfind(l, pid)
    if i < 0:
        return
    n = lcount(l)
    for j in range(i, n - 1):
        wr32(l + 0x88 + 4 * j, rd32(l + 0x88 + 4 * (j + 1)))
        wr32(l + 8 + 4 * j, rd32(l + 8 + 4 * (j + 1)))
    wr32(l + 4, n - 1)


def ladd(l, pid, spec):
    if not l or lcount(l) > 31 or lfind(l, pid) >= 0:
        return
    n = lcount(l)
    wr32(l + 0x88 + 4 * n, pid)
    wr32(l + 8 + 4 * n, spec)
    wr32(l + 4, n + 1)


def pid_of(info):
    return struct.unpack("<H", uc.mem_read(info, 2))[0]


def calc_links():
    user = bytes(uc.mem_read(link_in(live, USER), STRIDE))
    uc.mem_write(live, bytes(uc.mem_read(default, link_count * STRIDE)))
    uc.mem_write(link_in(live, USER), user)
    ucount = struct.unpack_from("<i", user, 4)[0]
    for k in range(ucount):
        pid = struct.unpack_from("<i", user, 0x88 + 4 * k)[0]
        for i in range(link_count):
            l = live + i * STRIDE
            if rds32(l) != USER:
                lremove(l, pid)


class Ser:
    buf = bytearray()
    pos = 0


def args(n):
    regs = [uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
    sp = uc.reg_read(UC_ARM_REG_SP)
    while len(regs) < n:
        regs.append(rd32(sp + 4 * (len(regs) - 4)))
    return regs[:n]


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def m_serialize(ser, ptr, minv):
    writing = uc.mem_read(ser + 0x1C, 1)[0]
    version = rds32(ser + 0x18)
    if writing:
        Ser.buf += uc.mem_read(ptr, 8)
        return 0
    if version < s32(minv):
        return 0
    uc.mem_write(ptr, bytes(Ser.buf[Ser.pos:Ser.pos + 8]))
    Ser.pos += 8
    return 0


def m_player_info(info, pid, a, b):
    if pid not in players:
        return 0
    pos = players[pid][0]
    blob = bytearray(0xB0)
    struct.pack_into("<H", blob, 0, pid)
    blob[0x7F] = pos
    blob[0x80] = 0 if pos == 0 else 5
    uc.mem_write(info, bytes(blob))
    return 1


def m_team_value(team):
    l = link_in(live, s32(team))
    return sum(players[p][2] for p in lids(l)) if l else 0


def m_specific(team, pid):
    l = link_in(cur_table(), s32(team))
    i = lfind(l, s32(pid))
    return l + 8 + 4 * i if i >= 0 else 0


def m_can_remove(team, info):
    team = s32(team)
    if team == -1:
        return 2
    l = link_in(cur_table(), team)
    if not l or lcount(l) < 17:
        return 0
    gk = sum(1 for p in lids(l) if players[p][0] == 0)
    if gk > 1:
        return 2
    return 1 if players[pid_of(info)][0] == 0 else 2


def m_can_add(team, info, x):
    team = s32(team)
    if team == -1:
        return 2
    l = link_in(cur_table(), team)
    if not l or lcount(l) > 31:
        return 0
    return 1 if lfind(l, pid_of(info)) >= 0 else 2


def m_set_override(links, count):
    global override
    override = links
    return 0


def m_generate(links, count, out):
    total = sum(lcount(links + i * STRIDE) for i in range(count))
    wr32(out, total)
    return alloc(total * 8 + 8)


def m_sign(info, frm, spec, calc, force, xi):
    pid = pid_of(info)
    ladd(link_in(live, USER), pid, rd32(spec) if spec else 0)
    lremove(link_in(live, s32(frm)), pid)
    if calc:
        calc_links()
    return 0


def m_sell(info, buyer, spec, calc):
    lremove(link_in(live, USER), pid_of(info))
    if calc:
        calc_links()
    return 0


MOCKS = {   # offset: (argc, fn)
    0x20C029: (0, lambda: link_count),
    0x20BF6D: (1, lambda i: live + s32(i) * STRIDE if 0 <= s32(i) < link_count else 0),
    0x20C3A1: (1, m_team_value),
    0x210885: (1, lambda t: 1 if valid.get(s32(t)) else 0),
    0x20C0A9: (1, lambda t: 1 if (kind.get(s32(t), 0) & 7) == 2 else 0),
    0x20C08F: (1, lambda t: 1 if (kind.get(s32(t), 0) & 7) == 3 else 0),
    0x20C049: (1, lambda t: 1 if kind.get(s32(t), 0) & 8 else 0),
    0x20DA0D: (4, m_player_info),
    0x2B35D1: (1, lambda info: players[pid_of(info)][1]),
    0x212B3D: (5, lambda info, a, b, c, d: players[pid_of(info)][2]),
    0x36A67D: (1, lambda s: rd32(s + 4)),
    0x36B031: (1, lambda s: rd32(s + 8)),
    0x36AA89: (1, lambda s: rd32(s)),
    0x20A621: (2, m_specific),
    0x20A40D: (6, lambda d, team, info, spec, f, xi: ladd(link_in(cur_table(), s32(team)), pid_of(info),
                                                        rd32(spec) if spec else 0) or 0),
    0x209DD9: (4, lambda d, team, pid, u: lremove(link_in(cur_table(), s32(team)), s32(pid)) or 0),
    0x20CBD5: (6, m_sign),
    0x20CC69: (4, m_sell),
    0x20E131: (2, lambda p, a: 0),
    0x2093B1: (4, lambda a, b, c, d: calc_links() or 0),
    0x375B89: (2, lambda p, m: 0),
    0x3FFAFD: (3, m_serialize),
    0x1D90FC: (2, lambda n, f: 1),
    0x20BF85: (2, m_set_override),
    0x213071: (2, m_can_remove),
    0x2102D5: (3, m_can_add),
    0x209E6D: (9, lambda *a: 0),
    0x20A751: (3, m_generate),
    0x20A857: (3, lambda p, n, f: 0),
    0x1C05E0: (1, lambda p: 0),
    0x20B71D: (3, lambda d, t, m: 0),
    0x36B02B: (1, lambda s: 17),                    # CSeason::GetStartTurn
    0x36B037: (1, lambda s: 18),                    # CSeason::GetStartLeagueTurn
    0x26348D: (1, lambda v: wr32(COINS, v) or 0),   # CCredits::SetCredits
}


def on_code(u, address, size, _):
    off = address - FAKE
    entry = MOCKS.get(off) or MOCKS.get(off | 1)
    if entry is None:
        raise SystemExit(f"unmocked game call at {off:#x}")
    argc, fn = entry
    result = fn(*args(argc))
    uc.reg_write(UC_ARM_REG_R0, (result or 0) & 0xFFFFFFFF)


def call(addr, *a):
    regs = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
    sp = STACK + STACK_SIZE - 0x100
    extra = list(a[4:])
    sp -= 4 * len(extra)
    for i, v in enumerate(extra):
        wr32(sp + 4 * i, v)
    for r, v in zip(regs, a[:4]):
        uc.reg_write(r, v & 0xFFFFFFFF)
    uc.reg_write(UC_ARM_REG_SP, sp)
    uc.reg_write(UC_ARM_REG_LR, STOP | 1)
    uc.emu_start(addr | 1, STOP)
    return uc.reg_read(UC_ARM_REG_R0)


def main():
    global db, live, default, pristine, link_count
    so, dataset = sys.argv[1], sys.argv[2]
    seasons = int(sys.argv[3]) if len(sys.argv) > 3 else 2
    exports = load_so(so)
    uc.mem_map(FAKE, FAKE_SIZE, UC_PROT_ALL)
    uc.mem_map(HEAP, HEAP_SIZE, UC_PROT_ALL)
    uc.mem_map(STACK, STACK_SIZE, UC_PROT_ALL)
    uc.mem_map(0, 0x10000, UC_PROT_ALL)
    uc.mem_write(STOP, b"\x70\x47")                       # bx lr (never reached: emu stops)
    for off in MOCKS:
        if off & 1:
            uc.mem_write(FAKE + (off & ~1), b"\x70\x47")  # Thumb bx lr
        else:
            uc.mem_write(FAKE + off, struct.pack("<I", 0xE12FFF1E))   # ARM bx lr (PLT stubs)
    uc.hook_add(UC_HOOK_CODE, on_code, begin=FAKE, end=FAKE + 0x500000)

    teams = []
    for line in open(dataset):
        f = line.split()
        if not f:
            continue
        if f[0] == "T":
            team, v, k, n = map(int, f[1:5])
            valid[team], kind[team] = v, k
            teams.append((team, [int(x) for x in f[5:5 + n]]))
        elif f[0] == "P":
            pid, pos, rating, age, value = map(int, f[1:6])
            players[pid] = (pos, rating, value)
    teams.sort()
    link_count = len(teams)
    blob = bytearray(link_count * STRIDE)
    for i, (team, roster) in enumerate(teams):
        base = i * STRIDE
        struct.pack_into("<i", blob, base, team)
        n = 0
        for j, pid in enumerate(roster[:32]):
            if pid in roster[:j]:
                continue
            struct.pack_into("<i", blob, base + 0x88 + 4 * n, pid)
            struct.pack_into("<I", blob, base + 8 + 4 * n, (j + 1) | (0x10000 if j < 11 else 0))
            n += 1
        struct.pack_into("<i", blob, base + 4, n)
    pristine = bytes(blob)
    db = alloc(0x100)
    default, live = alloc(len(blob)), alloc(len(blob))
    uc.mem_write(default, pristine)
    uc.mem_write(live, pristine)
    wr32(db + 0x24, default)
    wr32(db + 0x28, live)
    wr32(db + 0x38, link_count)
    wr32(FAKE + DB_INSTANCE, db)
    uc.mem_write(SEASON, struct.pack("<3i", 0, 17, 56))

    # first load of a pre-market save (version 0xAE): SerializeDreamTeam's CalculateLinks, then the hook
    calc_links()
    ser = alloc(0x40)
    uc.mem_write(ser, bytes(0x40))
    wr32(ser + 0x18, 0xAE)
    Ser.buf, Ser.pos = bytearray(), 0
    call(exports["career_market_on_serialize"], SEASON, ser, FAKE)
    wr32(COINS, 1500)                                   # a fresh career's coins (as in harness.c)

    for season in range(seasons):
        for turn in range(17, 57):
            uc.mem_write(SEASON, struct.pack("<3i", season, turn, 56))
            call(exports["career_market_on_turn"], SEASON, 0, FAKE)
            # v6: match income is paid by the mod's own economy hook, which harness.c --no-user does not
            # simulate, so no coins are added here either
        print(f"season {season} done, history={s32(call(exports['career_market_get_history_count']))}")

    h = 1469598103934665603

    def fnv(data):
        nonlocal h
        for b in data:
            h ^= b
            h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF

    out = alloc(64)
    for i in range(link_count):
        team = rds32(live + i * STRIDE)
        if call(exports["career_market_get_club"], team & 0xFFFFFFFF, out):
            fnv(bytes(uc.mem_read(out, 32)))
    n = s32(call(exports["career_market_get_history_count"]))
    fnv(struct.pack("<i", n))
    for k in range(n):
        if call(exports["career_market_get_history"], k, out):
            fnv(bytes(uc.mem_read(out, 32)))
    for i in range(link_count):
        l = live + i * STRIDE
        fnv(bytes(uc.mem_read(l, 8)))
        fnv(bytes(uc.mem_read(l + 0x88, 4 * lcount(l))))
    print(f"api_digest={h:016x}")


if __name__ == "__main__":
    main()
