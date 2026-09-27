"""Raise the animation clip cache cap (CAnimLib) - see anim_limits/ANIMATION_LIMITS.md 1.4.

CAnimLib is one object with fixed arrays sized for 2535 clips. For a capacity N its layout is:
  clip[N] u32 @0 | state[N] u8 @4N | count @align4(5N) | bytes[N] u32 @+4 | lastUsed[N] u32 @+4N |
  permanent, dynamic, pak file, pak fs (4 words) @+4N | size = that + 16
N = 2535 reproduces every stock constant (0x279c, 0x3184, 0x3188, 0x5924, 0x80c0..0x80cc, 0x80d0).
All of them are 16-bit movw immediates, so N can grow in place up to 3854; the default is 3840.

Separately, `count` is the number of clips that exist (anims.pak 0000..count-1.sat): the constructor
loads every one of them, so the load/free loop bounds and PreLoadAnims' bound (count * 0x84) follow
the real clip count, not the capacity. With count > 2535 the NIS "no anim" sentinel (2535) would be a
real id, so it moves to 0x7fff.
"""
import re

STOCK_N = 2535


def layout(n):
    s = 4 * n
    c = (5 * n + 3) & ~3
    b = c + 4
    lu = b + 4 * n
    t = lu + 4 * n
    return dict(clip=0, state=s, count=c, bytes=b, last_used=lu, tail=t, size=t + 16)


STOCK = layout(STOCK_N)
assert (STOCK["state"], STOCK["count"], STOCK["bytes"], STOCK["last_used"], STOCK["tail"], STOCK["size"]) == \
    (0x279C, 0x3184, 0x3188, 0x5924, 0x80C0, 0x80D0)

# movw sites in CAnimLib (all verified by a full scan of every CAnimLib function; 0x279c is both the
# state[] offset and the byte size of clip[], which are the same value for any N)
OFFSET_SITES = {
    0x2F850C: 0x80C0, 0x2F851A: 0x3184, 0x2F8524: 0x80C4, 0x2F8532: 0x80C8, 0x2F8548: 0x80CC,
    0x2F8558: 0x279C, 0x2F8568: 0x279C, 0x2F85F8: 0x80CC, 0x2F8620: 0x279C, 0x2F8652: 0x3188,
    0x2F8660: 0x80C4, 0x2F8666: 0x80C0, 0x2F8676: 0x279C, 0x2F8680: 0x3184, 0x2F8690: 0x5924,
    0x2F86D8: 0x279C, 0x2F8758: 0x80C4, 0x2F876E: 0x279C, 0x2F8774: 0x5924, 0x2F87FC: 0x279C,
    0x2F880E: 0x80C4, 0x2F8814: 0x80C0, 0x2F881E: 0x3188, 0x2F8836: 0x3188, 0x2F8848: 0x3184,
    0x2F885A: 0x279C, 0x2F8890: 0x80CC, 0x2F88A6: 0x279C, 0x2F88E0: 0x279C, 0x2F88EE: 0x5924,
    0x2F8900: 0x80C4, 0x2F892C: 0x279C, 0x2F8944: 0x5924, 0x2F895A: 0x279C, 0x2F897E: 0x80CC,
    0x2F8982: 0x3184, 0x2F8994: 0x80C8,
    0x2FAE96: 0x80D0,                        # CGfxCharacter::Init: operator new(sizeof(CAnimLib))
}
COUNT_SITES = [0x2F8576, 0x2F8778, 0x2F8866, 0x2F889A, 0x2F8962]      # loop bounds: 0x9e7
PRELOAD_BOUND = (0x2F86E4, 0x2F86F4)       # movw sb,#0x1b1c ; movt sb,#5  (count * 0x84)
SENTINEL_SITES = [0x2CBA00, 0x2CC8C2]      # movw rN,#0x9e7 ("no anim" in NIS actions)
SENTINEL_MOVT = 0x2CD198                   # movt r2,#0x9e7
NEW_SENTINEL = 0x7FFF


def new_offset(old, n):
    new = layout(n)
    for key in ("state", "count", "bytes", "last_used", "size"):
        if old == STOCK[key]:
            return new[key]
    if STOCK["tail"] <= old < STOCK["tail"] + 16:
        return new["tail"] + (old - STOCK["tail"])
    raise ValueError(f"unknown CAnimLib constant {old:#x}")


def patches(disasm_at, asm, capacity=3840, count=STOCK_N):
    """Returns {site: bytes}. disasm_at(site) -> (mnemonic, op_str) of the stock instruction."""
    if not STOCK_N <= capacity <= 3854:
        raise SystemExit("anim capacity must be 2535..3854 (movw immediates)")
    if not 1 <= count <= capacity:
        raise SystemExit("anim count must be 1..capacity")
    out = {}

    def movw(site, expect, value, mnem="movw"):
        m, ops = disasm_at(site)
        reg, imm = re.match(r"(\w+), #(0x[0-9a-f]+|\d+)", ops).groups()
        if m != mnem or int(imm, 0) != expect:
            raise SystemExit(f"anim_caps: {site:#x} is '{m} {ops}', expected {mnem} #{expect:#x}")
        if value > 0xFFFF:
            raise SystemExit(f"anim_caps: {value:#x} does not fit {mnem} at {site:#x}")
        code = asm(f"{mnem} {reg}, #{value:#x}", site)
        if len(code) != 4:
            raise SystemExit(f"anim_caps: {site:#x} re-encoded to {len(code)} bytes")
        out[site] = code

    if capacity != STOCK_N:
        for site, old in OFFSET_SITES.items():
            movw(site, old, new_offset(old, capacity))
    if count != STOCK_N:
        for site in COUNT_SITES:
            movw(site, STOCK_N, count)
        bound = count * 0x84
        movw(PRELOAD_BOUND[0], 0x1B1C, bound & 0xFFFF)
        movw(PRELOAD_BOUND[1], 5, bound >> 16, "movt")
    if count > STOCK_N:
        for site in SENTINEL_SITES:
            movw(site, STOCK_N, NEW_SENTINEL)
        movw(SENTINEL_MOVT, STOCK_N, NEW_SENTINEL, "movt")
    return out
