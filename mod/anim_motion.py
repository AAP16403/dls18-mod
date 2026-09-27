"""Motion coherence for the DLS26-ported clips (anim_motion, v36 candidate).

1. Crossfade into appended clips (IDs >= 2535), CPlayer::Animate 0x2DC5BE (site A1)
   SetAnim puts the old clip in the blend slot with weight 0x2000 (CPlayer+0x6e); Animate lowers it by a
   per-tick decay until it reaches 0. Stock decay is 0x400 (8 ticks); the stat rework's cave_blend makes it
   0x266..0x3A0 (13.3..8.8 ticks) from (acceleration + control) / 2. build_mod patches A1 with
   `bl cave_blend`. This module re-points A1 to `am_blend` in CAVE3, which calls the same cave_blend and then,
   only when the clip being faded in (CPlayer+0x54) is in [FIRST, FIRST + count), clamps the decay to that
   clip's [dmin, dmax]:
     dmax = 0x2000 // min_ticks        the fade lasts at least min_ticks (hides the new first pose)
     dmin = ceil(0x2000 / max_ticks)   tackles/controls: the new pose has >= 50% weight at the first
                                       action point (max_ticks = 2 * GetActionTime of action point 0)
   Every other clip keeps cave_blend's result exactly (am_blend returns cave_blend's r0 untouched).
   min_ticks comes from tmp/anim_motion/blend_gap.py: the mean-bone pose gap between the clip's first
   pose and its plausible predecessor loops, relative to the template's gap under the stock 8-tick fade
   (N = ceil(8 * gap_new / gap_template), never below 8).

2. Optional locomotion cadence trim (off by default), CPlayer::Animate 0x2DC536
   In locomotion (state 4) Animate advances the clip by ((2 * speed) / stride) * rate5e >> 7, where
   speed = GetRunSpeed = GetCurrentRunSpeed * stride / 52 is also the ground speed. Cadence is therefore
   already proportional to the player's actual speed, so the widened speed range adds no speed-dependent
   foot slide (see the report). The trim only exists to correct a clip whose pose sweep disagrees with its
   animdb stride/rate after device judgement: `ldrsh.w r1,[r4,#0x5e]` is replaced by `bl am_cadence`,
   which loads the same value and, for listed clip IDs, multiplies it by a Q12 scale (0.5..2.0).

CAVE3 region: [cave3_va + 0x24000, cave3_va + 0x28000) only (checked to be zero before writing).
Follows mod/PATCHING_RULES.md: PC-relative bl only, no far conditional branches, keystone statement
count check (the caller's asm()), every branch leaving the blob verified with capstone.
"""
from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs

import elf_extend

REGION_OFF, REGION_SIZE = 0x24000, 0x4000
A1 = 0x2DC5BE                  # CPlayer::Animate: add.w r0,r0,#0xfc00 (stock) / bl cave_blend (stat rework)
A1_STOCK = bytes.fromhex("00f57c40")
CAVE_BLEND_HEAD = bytes.fromhex("02b594f82521")     # push {r1,lr}; ldrb.w r2,[r4,#0x125]
CAD_SITE = 0x2DC536            # CPlayer::Animate loco branch: ldrsh.w r1,[r4,#0x5e]
CAD_STOCK = bytes.fromhex("b4f95e10")
FIRST = 2535
FADE = 0x2000
CS = Cs(CS_ARCH_ARM, CS_MODE_THUMB)

# base clip -> (min_ticks, max_ticks or 0); the mirror record (id + 1) gets the same values.
# min: blend_gap.py N_keep; max: 2 * ticks to action point 0 for ball-contact clips (tackle, control).
MEASURED = {
    2535: (8, 8),     # tackle, AP0 at game tick 4; gap 26.6 cm vs template 331 34.7 cm
    2537: (11, 0),    # stumble, gap 29.1 vs 21.7
    2539: (9, 0),     # stumble, 24.9 vs 24.8
    2541: (10, 0),    # stumble, 26.2 vs 21.7
    2543: (9, 0),     # stumble, 20.6 vs 20.6
    2545: (9, 0),     # stumble, 22.8 vs 22.8
    2547: (9, 0),     # deek, AP0 tick 10
    2549: (9, 0),     # deek, AP0 tick 11
    2551: (13, 0),    # deek, AP0 tick 13; gap 25.6 vs 16.3
    2553: (8, 12),    # control, AP0 tick 6
    2555: (8, 12),
    2557: (8, 12),
    2559: (8, 12),
}


def add_args(ap):
    """argparse options for build_mod.build_parser()."""
    ap.add_argument("--no-anim-motion", action="store_true",
                    help="skip anim_motion (crossfade rules for appended clips, cadence trim)")
    ap.add_argument("--anim-motion-blend-min", type=int, default=0, choices=range(0, 33), metavar="0..32",
                    help="extra crossfade floor in ticks for every clip >= 2535 (0 = measured table only)")
    ap.add_argument("--no-anim-motion-contact-cap", action="store_true",
                    help="do not cap the crossfade of appended tackles/controls at 2x ticks-to-contact")
    ap.add_argument("--anim-motion-cadence", default="",
                    help="optional loco cadence trim, e.g. '514=0.93' (scale 0.5..2.0; off by default)")


def _get(opts, name, default):
    if opts is None:
        return default
    if isinstance(opts, dict):
        return opts.get(name, default)
    return getattr(opts, name, default)


def blend_table(count, extra_min=0, contact_cap=True):
    """[(dmin, dmax)] (u16 decays per tick) for clip IDs FIRST .. FIRST+count-1."""
    rows = []
    for k in range(count):
        cid = FIRST + k
        base = cid if cid in MEASURED else cid - 1
        nmin, nmax = MEASURED.get(base, (8, 0))
        nmin = max(nmin, extra_min)
        if not contact_cap:
            nmax = 0
        if nmax and nmax < nmin:
            nmax = nmin
        dmax = FADE // nmin if nmin else 0xFFFF
        dmin = -(-FADE // nmax) if nmax else 0
        if dmin > dmax:
            raise SystemExit(f"anim_motion: clip {cid}: decay range {dmin}..{dmax} is empty")
        rows.append((dmin, dmax))
    return rows


def parse_cadence(text):
    out = []
    for item in filter(None, (s.strip() for s in text.split(","))):
        cid, _, val = item.partition("=")
        cid, val = int(cid, 0), float(val)
        if not 0 <= cid < 0xFFFF or not 0.5 <= val <= 2.0:
            raise SystemExit(f"anim_motion: bad cadence trim {item!r} (id=scale, scale 0.5..2.0)")
        out.append((cid, round(val * 4096)))
    return out


def _one(code, addr):
    ins = list(CS.disasm(bytes(code), addr))
    if len(ins) != 1:
        raise SystemExit(f"anim_motion: {addr:#x} does not decode to one instruction")
    return ins[0]


def _target(ins):
    return int(ins.op_str.lstrip("#"), 16)


def _check_exits(blob, base, allowed):
    """Every branch in the blob must stay inside it or land on an allowed address; no register branches
    except bx lr; every instruction must decode (nothing silently skipped)."""
    n = 0
    for ins in CS.disasm(bytes(blob), base):
        n += ins.size
        last = ins.op_str.split(",")[-1].strip()
        if ins.mnemonic.startswith(("b", "cb")) and last.startswith("#0x"):
            tgt = int(last[1:], 16)
            if not (base <= tgt < base + len(blob)) and tgt not in allowed:
                raise SystemExit(f"anim_motion: branch at {ins.address:#x} ({ins.mnemonic} {ins.op_str}) "
                                 f"leaves the blob to {tgt:#x}")
        if ins.mnemonic in ("bx", "blx") and ins.op_str.strip() != "lr":
            raise SystemExit(f"anim_motion: register branch at {ins.address:#x}")
    if n != len(blob):
        raise SystemExit(f"anim_motion: code at {base:#x} does not fully decode")


def _blend_asm(inner, count):
    """inner = 'bl 0x..' (chain to cave_blend) or the stock decay."""
    return "\n".join([
        "am_blend:",
        "push {r0, lr}",
        inner,
        "ldr r2, [r4, #0x54]",
        f"movw r3, #{FIRST}",
        "subs r2, r2, r3",
        f"cmp r2, #{count}",
        "bhs am_blend_out",
        "adr r3, am_blend_tab",
        "add.w r3, r3, r2, lsl #2",
        "ldr r2, [sp]",
        "subs r0, r2, r0",
        "ldrh.w r12, [r3]",
        "cmp r0, r12",
        "it lt",
        "movlt r0, r12",
        "ldrh.w r12, [r3, #2]",
        "cmp r0, r12",
        "it gt",
        "movgt r0, r12",
        "subs r0, r2, r0",
        "it lt",
        "movlt r0, #0",
        "am_blend_out:",
        "add sp, #4",
        "pop {pc}",
    ])


CAD_ASM = "\n".join([
    "am_cadence:",
    "ldrsh.w r1, [r4, #0x5e]",
    "ldr r2, [r4, #0x54]",
    "adr r3, am_cad_tab",
    "am_cad_loop:",
    "ldrh r12, [r3], #4",
    "cmp r12, r2",
    "beq am_cad_hit",
    "add.w r12, r12, #1",
    "lsrs.w r12, r12, #16",
    "beq am_cad_loop",
    "bx lr",
    "am_cad_hit:",
    "ldrh r12, [r3, #-2]",
    "mul r1, r1, r12",
    "asrs r1, r1, #12",
    "bx lr",
])


def _with_table(src, label, addr, asm):
    """Assemble src, pad it with NOPs to a 4-byte boundary and resolve `label` (used by one adr in src)
    at the first byte after the padding. Returns (code, table_address)."""
    pre = asm(src + "\n" + label + ":\nnop", addr)[:-2]       # label right after the code
    pad = (-(addr + len(pre))) % 4
    full = asm(src + "\n" + "nop\n" * (pad // 2) + label + ":\nnop", addr)
    code = full[:-2]
    if len(code) != len(pre) + pad:
        raise SystemExit(f"anim_motion: {label} placement changed the code size")
    tab = addr + len(code)
    adr = [i for i in CS.disasm(code, addr)          # capstone prints a wide adr as addw rX, pc, #imm
           if i.mnemonic.startswith("adr") or (i.mnemonic == "addw" and ", pc," in i.op_str)]
    if len(adr) != 1 or ((adr[0].address + 4) & ~3) + int(adr[0].op_str.split("#")[-1], 0) != tab:
        raise SystemExit(f"anim_motion: adr does not reach {label}")
    return code, tab


def apply(lib, orig, va2off, layout, asm, opts=None):
    """Patch `lib` (extended bytearray) in place. `opts`: argparse namespace or dict with anim_count and the
    add_args() options. Returns a dict describing what was written ({} if disabled)."""
    if layout is None:
        raise SystemExit("anim_motion: needs the CAVE3 segment (drop --no-extend)")
    if _get(opts, "no_anim_motion", False):
        return {}
    count = min(int(_get(opts, "anim_count", 2561)) - FIRST, 64)
    region = layout["cave3_va"] + REGION_OFF
    roff = elf_extend.va2off(lib, region)
    if any(lib[roff:roff + REGION_SIZE]):
        raise SystemExit(f"anim_motion: CAVE3 {region:#x}..{region + REGION_SIZE:#x} is not free")
    info = {"region": region}
    used = 0

    # --- 1. crossfade rules, chained to whatever A1 currently does --------------------------------------
    if count > 0:
        if bytes(orig[va2off(A1):va2off(A1) + 4]) != A1_STOCK:
            raise SystemExit("anim_motion: A1 stock bytes differ (wrong game version?)")
        a1o = elf_extend.va2off(lib, A1)
        cur = bytes(lib[a1o:a1o + 4])
        if cur == A1_STOCK:
            inner, allowed, chained = "sub.w r0, r0, #0x400\ncmp r0, #0\nit lt\nmovlt r0, #0", set(), None
        else:
            ins = _one(cur, A1)
            if ins.mnemonic != "bl":
                raise SystemExit(f"anim_motion: A1 holds {ins.mnemonic} {ins.op_str}, expected bl cave_blend")
            chained = _target(ins)
            co = elf_extend.va2off(lib, chained)
            if bytes(lib[co:co + len(CAVE_BLEND_HEAD)]) != CAVE_BLEND_HEAD:
                raise SystemExit(f"anim_motion: A1 calls {chained:#x}, which is not cave_blend")
            inner, allowed = f"bl {chained:#x}", {chained}
        table = blend_table(count, int(_get(opts, "anim_motion_blend_min", 0)),
                            not _get(opts, "no_anim_motion_contact_cap", False))
        code, tab_at = _with_table(_blend_asm(inner, count), "am_blend_tab", region, asm)
        if chained is not None:
            bls = [i for i in CS.disasm(code, region) if i.mnemonic == "bl"]
            if len(bls) != 1 or _target(bls[0]) != chained:
                raise SystemExit("anim_motion: chained bl cave_blend mis-encoded")
        _check_exits(code, region, allowed)
        tab = b"".join(dmin.to_bytes(2, "little") + dmax.to_bytes(2, "little") for dmin, dmax in table)
        lib[roff:roff + len(code) + len(tab)] = code + tab
        new = asm(f"bl {region:#x}", A1)
        got = _one(new, A1)
        if got.mnemonic != "bl" or _target(got) != region or len(new) != 4:
            raise SystemExit("anim_motion: bl at A1 mis-encoded")
        lib[a1o:a1o + 4] = new
        info.update(blend=region, blend_table=tab_at, chained=chained, table=table)
        used = len(code) + len(tab)

    # --- 2. optional cadence trim ----------------------------------------------------------------------
    trims = parse_cadence(_get(opts, "anim_motion_cadence", "") or "")
    if trims:
        so = elf_extend.va2off(lib, CAD_SITE)
        if bytes(orig[va2off(CAD_SITE):va2off(CAD_SITE) + 4]) != CAD_STOCK or bytes(lib[so:so + 4]) != CAD_STOCK:
            raise SystemExit(f"anim_motion: {CAD_SITE:#x} is not the stock ldrsh.w r1,[r4,#0x5e] (patched elsewhere?)")
        cad = (region + used + 3) & ~3
        body, ctab_at = _with_table(CAD_ASM, "am_cad_tab", cad, asm)
        _check_exits(body, cad, set())
        ctab = b"".join(c.to_bytes(2, "little") + s.to_bytes(2, "little") for c, s in trims) + b"\xff\xff\x00\x10"
        if cad + len(body) + len(ctab) > region + REGION_SIZE:
            raise SystemExit("anim_motion: region overflow")
        co = elf_extend.va2off(lib, cad)
        lib[co:co + len(body) + len(ctab)] = body + ctab
        new = asm(f"bl {cad:#x}", CAD_SITE)
        got = _one(new, CAD_SITE)
        if got.mnemonic != "bl" or _target(got) != cad or len(new) != 4:
            raise SystemExit("anim_motion: bl at cadence site mis-encoded")
        lib[so:so + 4] = new
        info.update(cadence=cad, cadence_table=ctab_at, trims=trims)
    return info
