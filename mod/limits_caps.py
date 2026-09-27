"""Raise game limits that do not touch the save format (data_limits/DATA_LIMITS.md, limits_v32).

1. POTW roster (DATA_LIMITS #7): CPlayerDevelopment::ms_tPOTWPlayers is a 16 x 12 B static array at
   0x75ccc4. Every reference in the library goes through its single GOT slot 0x730790 (R_ARM_GLOB_DAT,
   verified by a full scan of .rel.dyn, raw words and every `add rN, pc` in .text):
     LoadPOTWConfigInfo, ValidateConfigInfo, UpdatePOTW and CFEMsgPOTW::Init.
   The slot is turned into an R_ARM_RELATIVE relocation that points at a buffer in MODDATA, so every
   user sees the moved array. Fixed-size code that must change with it:
     0x20e462  LoadPOTWConfigInfo init loop (cmp r0,#0xc0 = 16 entries) -> rewritten as a count loop
     0x20e67a  cmp r0,#0xf   (entry check before the reader loop)      -> cmp r0,#N-1
     0x20e748  cmp r0,#0x10  (reader loop bound)                        -> cmp r0,#N
     0x24ebb2  CFEMsgPOTW::Init copies the non-user entries into an in-object 16 x 12 B array
               (this+0x4e4..0x5a3). The copy block is rewritten to stop at 16 entries, so the message
               box never overflows (it shows at most 16, as before).
   CSeasonPOTWInfo is unchanged: its weighted pool size is a saved u8 (sum of the entry weights).
   That was already true with 16 entries, so the POTW config must keep the total weight <= 255.
2. Minimum squad (DATA_LIMITS #1): CTransfers::CanRemovePlayer 0x21309c `cmp r0,#0x11` (a club may
   sell only while it has >= min+1 players). Valid range 11..31 (11 = a starting XI, 31 = one below the
   32-player link cap). Default 16 = stock bytes.
"""
import struct

POTW_GOT = 0x730790
POTW_SYM = "_ZN18CPlayerDevelopment15ms_tPOTWPlayersE"
POTW_STOCK = 16
POTW_ENTRY = 12
POTW_MODDATA_OFFSET = 0x8000      # MODDATA +0 .. +0x100 = modcore hook slots (64 x 4)
POTW_INIT_LOOP = 0x20E462
POTW_ENTRY_CHECK = 0x20E67A
POTW_LOOP_BOUND = 0x20E748
POTW_MSG_COPY = 0x24EBB2
POTW_MSG_EXIT = 0x24EBE4          # CFEMsgPOTW::Init: ldr.w r6,[r5,#0x4e0] after the copy loop
SQUAD_MIN_CMP = 0x21309C

STOCK = {
    # strh r2,[r1,r0]; adds r6,r1,r0; adds r0,#0xc; cmp r0,#0xc0; str r3,[r6,#4]; strb r7,[r6,#8]; bne
    POTW_INIT_LOOP: bytes.fromhex("0a520e180c30c02873603772f8d1"),
    POTW_ENTRY_CHECK: bytes.fromhex("0f28"),        # cmp r0,#0xf
    POTW_LOOP_BOUND: bytes.fromhex("1028"),         # cmp r0,#0x10
    # ldr.w r0,[r5,#0x4e0]; ldr r1,[r6,#8]; vldr d16,[r6]; add.w r0,r0,r0,lsl#1; add.w r0,r5,r0,lsl#2;
    # str.w r1,[r0,#0x4ec]; addw r0,r0,#0x4e4; vstr d16,[r0]; ldr.w r0,[r5,#0x4e0]; adds r0,#1;
    # str.w r0,[r5,#0x4e0]
    POTW_MSG_COPY: bytes.fromhex("d5f8e004b168d6ed000b00eb400005eb8000c0f8ec1400f2e440c0ed000bd5f8e0040130c5f8e004"),
    SQUAD_MIN_CMP: bytes.fromhex("1128"),           # cmp r0,#0x11
}


def _check_stock(orig, va2off):
    for site, exp in STOCK.items():
        got = bytes(orig[va2off(site):va2off(site) + len(exp)])
        if got != exp:
            raise SystemExit(f"limits_caps: unexpected bytes at {site:#x}: {got.hex()} (expected {exp.hex()})")


def code_patches(orig, va2off, asm, potw_max=64, squad_min=16):
    """Returns {site: bytes} for the in-image code changes. Every site is checked against the stock bytes."""
    if not POTW_STOCK <= potw_max <= 255:
        raise SystemExit("--potw-max must be 16..255 (cmp imm8)")
    if not 11 <= squad_min <= 31:
        raise SystemExit("--min-squad must be 11..31")
    _check_stock(orig, va2off)
    out = {}

    def put(site, src):
        code = asm(src, site)
        if len(code) != len(STOCK[site]):
            raise SystemExit(f"limits_caps: {site:#x} re-encoded to {len(code)} bytes, expected {len(STOCK[site])}")
        out[site] = code

    if potw_max != POTW_STOCK:
        # r1 = array base, r0 = 0, r2 = 0xffff, r3 = -1, r7 = 10 on entry (unchanged). r0/r1/r6 are dead after.
        loop = POTW_INIT_LOOP
        put(loop, "strh r2, [r1]\nstr r3, [r1, #4]\nstrb r7, [r1, #8]\nadds r1, #0xc\nadds r0, #1\n"
                  f"cmp r0, #{potw_max}\nbne {loop:#x}")
        put(POTW_ENTRY_CHECK, f"cmp r0, #{potw_max - 1}")
        put(POTW_LOOP_BOUND, f"cmp r0, #{potw_max}")
        # r5 = this, r6 = &entry. r0-r3 are scratch here (the loop tail reloads r0; calls clobber r1-r3).
        body = asm("ldr.w r0, [r5, #0x4e0]\ncmp r0, #16\n"
                   f"bge {POTW_MSG_EXIT:#x}\n"
                   "add.w r1, r0, r0, lsl #1\nadds r0, #1\nstr.w r0, [r5, #0x4e0]\n"
                   "add.w r1, r5, r1, lsl #2\naddw r1, r1, #0x4e4\n"
                   "ldm.w r6, {r0, r2, r3}\nstm r1!, {r0, r2, r3}", POTW_MSG_COPY)
        pad = len(STOCK[POTW_MSG_COPY]) - len(body)
        if pad < 0 or pad % 2:
            raise SystemExit(f"limits_caps: CFEMsgPOTW copy block is {len(body)} bytes")
        out[POTW_MSG_COPY] = body + b"\x00\xbf" * (pad // 2)
    if squad_min != 16:
        put(SQUAD_MIN_CMP, f"cmp r0, #{squad_min + 1}")
    return out


def _sections(b):
    shoff, = struct.unpack_from("<I", b, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 0x2E)
    sh = [struct.unpack_from("<10I", b, shoff + i * shentsize) for i in range(shnum)]
    stroff = sh[shstrndx][4]
    return {b[stroff + s[0]:b.index(b"\0", stroff + s[0])].decode(): s for s in sh}


def potw_reloc_patch(lib, orig, layout, potw_max):
    """Repoint the POTW GOT slot at a MODDATA buffer (in place on `lib`, an extended bytearray).
    Returns the buffer VA, or None when potw_max is stock."""
    if potw_max == POTW_STOCK:
        return None
    if layout is None:
        raise SystemExit("--potw-max > 16 needs the MODDATA segment (drop --no-extend)")
    buf = layout["moddata_va"] + POTW_MODDATA_OFFSET
    if POTW_MODDATA_OFFSET + potw_max * POTW_ENTRY > layout["moddata_size"]:
        raise SystemExit("limits_caps: POTW buffer does not fit MODDATA")
    sec = _sections(orig)
    rel, dynsym, dynstr = sec[".rel.dyn"], sec[".dynsym"], sec[".dynstr"]
    got_sec = next(s for s in sec.values() if s[3] <= POTW_GOT < s[3] + s[5] and s[4])
    slot_off = POTW_GOT - got_sec[3] + got_sec[4]
    hits = []
    for i in range(rel[5] // 8):
        off, info = struct.unpack_from("<II", orig, rel[4] + i * 8)
        if off == POTW_GOT:
            hits.append((rel[4] + i * 8, info))
    if len(hits) != 1:
        raise SystemExit(f"limits_caps: expected one relocation for GOT {POTW_GOT:#x}, found {len(hits)}")
    ent, info = hits[0]
    nm = struct.unpack_from("<I", orig, dynsym[4] + (info >> 8) * 16)[0]
    name = orig[dynstr[4] + nm:orig.index(b"\0", dynstr[4] + nm)].decode()
    slot = struct.unpack_from("<I", orig, slot_off)[0]
    if info & 0xFF != 21 or name != POTW_SYM or slot != 0:
        raise SystemExit(f"limits_caps: GOT {POTW_GOT:#x} reloc is type {info & 0xff} '{name}', slot {slot:#x}")
    if bytes(lib[ent:ent + 8]) != bytes(orig[ent:ent + 8]) or bytes(lib[slot_off:slot_off + 4]) != bytes(4):
        raise SystemExit("limits_caps: POTW relocation/GOT slot already modified")
    struct.pack_into("<II", lib, ent, POTW_GOT, 23)          # R_ARM_RELATIVE, no symbol
    struct.pack_into("<I", lib, slot_off, buf)               # addend: base + buf at load time
    return buf
