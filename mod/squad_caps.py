"""Raise the user squad cap from 32 to 64 (squad_v33). Same pattern as limits_caps.py.

Design (see squad/squad64.c for the runtime side):
  Every stock structure keeps its layout and a valid view of at most 32 players (TTeamPlayerLink 0x108,
  CTeamLineup id[32], CTeamManagement player-state[32], CTeam players[32]). The user team's players
  32..63 live in overflow tables in MODDATA, owned by a C blob placed in the last 64 KB of CAVE3:
    link overflow (user team 0x102 only), season-lineup overflow and season player states (only the
    CTeamManagement at MP_cMyProfile+0x14+0x6e0), and a TPlayerInfo pool for the squad screen.
  resync keeps the stock 32-part = lineup slots 0..31 (XI, bench, first reserves) consistent across
  link, lineup and states, so every unpatched function (match engine, AI, stats, tournaments, market
  bridge, network) sees a normal 32-player squad; the patched functions see all 64.

Patch kinds (every site is checked against the stock bytes, in the stock library and in the input):
  ENTRY  b.w <C function> at a function entry; wrappers reach the original body through a trampoline
         (the displaced entry instructions + b.w back), generated here.
  CALL   blx <PLT GetPlayerCount / RemovePlayerByID> -> bl <C helper> (same 4 bytes, same clobbers).
  DETOUR CalculateLinks user loop (id fetch + bound) -> asm stub -> C helper -> back.
  INPLACE squad-screen count/clamps (CFETeamManagement) re-encoded at the same size.
  SAVE   profile save version = max(current, 0xB6); the overflow blocks are gated at 0xB6, so a save
         without them (<= 0xB5) loads as a normal 32-player squad and an older build refuses 0xB6 saves.

Needs WSL with arm-linux-gnueabi-gcc/as/ld/readelf (Ubuntu gcc-arm-linux-gnueabi) at build time.
Integration in build_mod.py: after elf_extend/modcore (needs the layout) and after the SAVE_VERSION patches:
    squad_info = squad_caps.apply(lib, ORIG, layout, asm, cfg.squad_max, cfg.list)
"""
import os
import re
import struct
import subprocess
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs
from keystone import KS_ARCH_ARM, KS_MODE_THUMB, Ks

import elf_extend

HERE = Path(__file__).resolve().parent
C_SOURCE = HERE / "squad" / "squad64.c"
BUILD_DIR = Path(os.environ.get("SQUAD_BUILD_DIR", r"F:\android modding\tmp\squad_v33_cc"))
REGION_SIZE = 0x10000             # last 64 KB of CAVE3
TRAMP_SLOT = 16
TEXT_OFFSET = 0x400               # C text starts here inside the region (after the trampolines)
BSS_OFFSET = 0x1000               # MODDATA +0 .. +0x100 modcore slots; +0x8000.. POTW (limits_caps)
BSS_LIMIT = 0x8000
SQUAD_STOCK = 32
SAVE_VERSION_SITES = (0x376AC6, 0x376B72)   # build_mod.py SAVE_VERSION_SETUP / SAVE_VERSION_BOOT
SQUAD_SAVE_GATE = 0xB6            # min version of the overflow blocks; never reuse 0xB6 for another block

CS = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
KS = Ks(KS_ARCH_ARM, KS_MODE_THUMB)

# game symbols used by the C blob: name -> (address, is_function)
GAME_SYMS = {
    "MP_cMyProfile": (0x84A260, False),
    "DB_ms_pInstance": (0x75CCA4, False),
    "MatchSetup_ms_tInfo": (0x7C2AAC, False),
    "MC_bResettingProfile": (0x83F251, False),
    "GetTeamLink": (0x20934C, True),
    "UpdateStatsAddPlayer": (0x36DCBE, True),
    "UpdateStatsRemovePlayer": (0x36DC90, True),
    "SerInt": (0x207ADA, True),
    "SerU16": (0x207C20, True),
    "SerU8": (0x205CB8, True),
    "SpecSerialize": (0x207ED8, True),
    "StateSerialize": (0x2F2B5C, True),
    "TPlayerInfoCtor": (0x2B2D4A, True),
    "GetPlayerInfo": (0x20805C, True),
    "FixLink": (0x208FEC, True),
    "UpdateTeamRoles": (0x2B2CE8, True),
    "StateReduceInjury": (0x2F2F1C, True),
    "StateReduceSuspension": (0x2F2F80, True),
    "CConfigGetVar": (0x1FF0EC, True),
    "GetCreditRechargeSeconds": (0x20184C, True),
    "PU_GetGeneralPosFromPos": (0x2B3930, True),
    "CCore_InGame": (0x203834, True),
    "IsCreatedPlayerID": (0x20C5C2, True),
}

# ENTRY hooks: site -> (C function, trampoline name or None, stock bytes of the first instructions)
ENTRY = {
    0x20A620: ("sq_GetTeamSpecificData", None, "10b50c46"),
    0x2081E2: ("sq_ContainsPlayer", None, "4268012a"),
    0x207F54: ("sq_GetPlayerShirtNum", None, "80b5d0f804c0"),
    0x207F80: ("sq_SetPlayerShirtNum", None, "10b5d0f804c0"),
    0x207FAA: ("sq_SwapPlayerShirtNums", None, "70b5d0f804c0"),
    0x2102D4: ("sq_CanAddPlayer", None, "b0b50546"),
    0x210318: ("sq_CanAddCreatedPlayer", None, "80b50748"),
    0x20A40C: ("sq_AddPlayerToLink", "orig_AddPlayerToLink", "2de9f04f"),
    0x209DD8: ("sq_RemovePlayerFromLink", "orig_RemovePlayerFromLink", "2de9f047"),
    0x207F06: ("sq_LinkSerialize", "orig_LinkSerialize", "f0b581b0"),
    0x20D208: ("sq_NewDreamTeam", "orig_NewDreamTeam", "2de9f04f"),
    0x20CB1C: ("sq_GetFirstAvailableShirtNumber", None, "2de9f043"),
    0x2B3A74: ("sq_PU_GetPlayerPositionCounts", None, "2de9f04f"),
    0x2F0554: ("sq_LuAddPlayer", None, "02780132"),
    0x2F06D8: ("sq_LuGetID", None, "1f298ebf"),
    0x2F06D0: ("sq_LuSetID", None, "00eb4100"),
    0x2F06E8: ("sq_LuGetIndex", None, "821c0020"),
    0x2F0704: ("sq_LuHasPlayer", None, "0122"),
    0x2F0722: ("sq_LuSwap", None, "0230"),
    0x2F0736: ("sq_LuSwapByID", None, "10b5831c"),
    0x2F057C: ("sq_LuRemovePlayerByID", "orig_LuRemovePlayerByID", "2de9f04f"),
    0x2F0298: ("sq_LuSerialize", "orig_LuSerialize", "2de9f041"),
    0x2F0278: ("sq_LuReset", "orig_LuReset", "c2ef7108"),
    0x2F025C: ("sq_LuCtor", "orig_LuCtor", "c2ef7108"),
    0x2F1826: ("sq_LuHaveCreatedPlayerInSquad", "orig_LuHaveCreatedPlayerInSquad", "80b50178"),
    0x2F0780: ("sq_ForceFirst11", "orig_ForceFirst11", "2de9f04f"),
    0x2F29DE: ("sq_TmAddPlayer", "orig_TmAddPlayer", "b0b50c46"),
    0x2F2A82: ("sq_TmAddPlayerState", None, "0930"),
    0x2F2A6A: ("sq_GetSeasonPlayerStateByID", None, "4ff0ff32"),
    0x2F2A3E: ("sq_RemovePlayerState", None, "0330"),
    0x2F297C: ("sq_TmRemovePlayerByID", "orig_TmRemovePlayerByID", "b0b50d46"),
    0x2F1F30: ("sq_TmSerialize", "orig_TmSerialize", "70b584b0"),
    0x2F1E46: ("sq_TmReset", "orig_TmReset", "10b50446"),
    0x2F2002: ("sq_TmNextSeason", "orig_TmNextSeason", "90f89011"),
    0x2F2038: ("sq_EnergyReplenish", "orig_EnergyReplenish", "f0b581b0"),
    0x2F2EE6: ("sq_ReduceInjSusp", "orig_ReduceInjSusp", "f0b581b0"),
    0x2F245C: ("sq_IsEnergyFullForAllPlayers", None, "0430"),
    0x2F3034: ("sq_GetNumInjuries", None, "0022"),
    0x2F25C8: ("sq_CanSwap", "orig_CanSwap", "2de9f047"),
    0x2F2118: ("sq_TmVerify", "orig_TmVerify", "2de9f04f"),
    0x2F24A2: ("sq_TmSetDefaults", "orig_TmSetDefaults", "2de9f04f"),
    0x20BAA0: ("sq_PlayersLoad", "orig_PlayersLoad", "2de9f04f"),
    0x2B2298: ("sq_CTeamGetPlayerInfoByID", "orig_CTeamGetPlayerInfoByID", "90f848c1"),
    0x2B2C74: ("sq_CTeamSwapPlayers", "orig_CTeamSwapPlayers", "2de9f047"),
}

# CALL redirects: site -> (C function, stock blx bytes, stock PLT target)
_GPC = 0x1C1840                       # plt: CTeamLineup::GetPlayerCount
CALLS = {
    0x2F21D2: ("sq_verify_remove", "cff6fcee", 0x1C1FCC),   # TM::Verify: RemovePlayerByID (not in link 32-part)
    0x2F221A: ("sq_lineup_count", "cff612eb", _GPC),        # TM::Verify: link players missing from the lineup
    0x20DF30: ("sq_lineup_count", "b3f786ec", _GPC),        # CPlayerDevelopment::Verify (x4)
    0x20DF74: ("sq_lineup_count", "b3f764ec", _GPC),
    0x20DFD6: ("sq_lineup_count", "b3f734ec", _GPC),
    0x20E052: ("sq_lineup_count", "b3f7f6eb", _GPC),
    0x20E2E8: ("sq_lineup_count", "b3f7aaea", _GPC),        # CPlayerDevelopment::AddPlayer replacement scan (x2)
    0x20E306: ("sq_lineup_count", "b3f79cea", _GPC),
    0x20FEB0: ("sq_lineup_count", "b1f7c6ec", _GPC),        # CPreTrainedPlayers::RemovePlayers (x2)
    0x20FECA: ("sq_lineup_count", "b1f7baec", _GPC),
}

# DETOURS in CDataBase::CalculateLinks (user players loop): site -> (stock bytes, resume, stub source)
DETOURS = {
    0x209516: ("08eb8600d0f888a0", 0x20951E, "sq_vlink_id",   # add.w r0,r8,r6,lsl#2 ; ldr.w sl,[r0,#0x88]
               "push {{r0, r1, r2, r3, ip, lr}}\nmov r0, r8\nmov r1, r6\nbl {target:#x}\nmov sl, r0\n"
               "pop {{r0, r1, r2, r3, ip, lr}}\nb.w {resume:#x}"),
    0x209728: ("d8f80400", 0x20972C, "sq_vlink_count",        # ldr.w r0,[r8,#4]
               "push {{r1, r2, r3, r4, ip, lr}}\nmov r0, r8\nbl {target:#x}\n"
               "pop {{r1, r2, r3, r4, ip, lr}}\nb.w {resume:#x}"),
}

# INPLACE squad-screen patches: site -> (stock bytes, assembly template; {rows} = squad_max-11, {last} = squad_max-1)
INPLACE = {
    0x23E1B4: ("0021152290f848010b38", "bl {sq_team_count:#x}\nsubs r0, #0xb\nmovs r1, #0\nmovs r2, #{rows}"),  # SetupSubsTable
    0x23EE00: ("0021152290f848010b38", "bl {sq_team_count:#x}\nsubs r0, #0xb\nmovs r1, #0\nmovs r2, #{rows}"),  # SellSelection
    0x23FD20: ("90f84801", "bl {sq_team_count:#x}"),     # GetPlayerCardById: count
    0x23FD28: ("152c", "cmp r4, #{rows}"),               #   clamp
    0x23FD2A: ("a8bf1524", "it ge\nmovge r4, #{rows}"),
    0x23F4B6: ("1f2d", "cmp r5, #{last}"),               # GetPlayerCard
    0x23F924: ("1f2d", "cmp r5, #{last}"),               # DeletePlayerCard
    0x23E93E: ("202e", "cmp r6, #{count}"),              # ProcessSelect card loops
    0x23EC2A: ("1f2d", "cmp r5, #{last}"),
    0x23EC76: ("202d", "cmp r5, #{count}"),
}


def _asm(src, addr):
    if re.search(r"#\s*\{", src):
        raise SystemExit(f"squad_caps: unformatted placeholder at {addr:#x}")
    enc, count = KS.asm(src, addr)
    stmts = sum(1 + ln.count(";") for ln in src.splitlines())
    if count != stmts:
        raise SystemExit(f"squad_caps: keystone assembled {count} of {stmts} statements at {addr:#x}")
    return bytes(enc)


def _branch(mnemonic, frm, to):
    """b.w/bl from `frm` to `to`, verified by disassembly (keystone can mis-encode far branches)."""
    for cand in range(to - 0x20, to + 0x22, 2):
        try:
            code = _asm(f"{mnemonic} {cand:#x}", frm)
        except Exception:
            continue
        ins = list(CS.disasm(code, frm))
        if len(ins) == 1 and ins[0].mnemonic == mnemonic and int(ins[0].op_str.lstrip("#"), 16) == to:
            return code
    raise SystemExit(f"squad_caps: could not encode {mnemonic} {frm:#x} -> {to:#x}")


def _wsl(path):
    p = str(Path(path).resolve())
    return "/mnt/" + p[0].lower() + p[2:].replace("\\", "/")


def _run(args):
    r = subprocess.run(["wsl", "-e"] + args, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"squad_caps: {' '.join(args[:2])} failed:\n{r.stdout}\n{r.stderr}")
    return r.stdout


def _elf_sections(b):
    shoff, = struct.unpack_from("<I", b, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", b, 0x2E)
    sh = [struct.unpack_from("<10I", b, shoff + i * shentsize) for i in range(shnum)]
    so = sh[shstrndx][4]
    return {b[so + s[0]:b.index(b"\0", so + s[0])].decode(): s for s in sh}


def _elf_symbols(b):
    sec = _elf_sections(b)
    st, strt = sec[".symtab"], sec[".strtab"]
    out = {}
    for i in range(st[5] // 16):
        nm, val, sz, info, oth, shn = struct.unpack_from("<IIIBBH", b, st[4] + i * 16)
        if nm:
            if info & 0xF == 2:                     # STT_FUNC: drop the Thumb bit
                val &= ~1
            out[b[strt[4] + nm:b.index(b"\0", strt[4] + nm)].decode()] = (val, sz, shn)
    return out


def check_displaced(orig, va2off, site, length):
    code = bytes(orig[va2off(site):va2off(site) + length])
    ins = list(CS.disasm(code, site))
    if sum(i.size for i in ins) != length:
        raise SystemExit(f"squad_caps: {site:#x}: {length} bytes do not end on an instruction boundary")
    for i in ins:
        m = i.mnemonic.split(".")[0]
        if m in ("b", "bl", "blx", "bx", "cbz", "cbnz", "tbb", "tbh") or "pc" in i.op_str or m.startswith("it"):
            raise SystemExit(f"squad_caps: {site:#x}: displaced '{i.mnemonic} {i.op_str}' is not position independent")
    return code


def _displace_len(orig, va2off, site):
    n = 0
    for ins in CS.disasm(bytes(orig[va2off(site):va2off(site) + 16]), site):
        n += ins.size
        if n >= 4:
            return n
    raise SystemExit(f"squad_caps: cannot decode {site:#x}")


def _func_branch_targets(orig, va2off, site, size=0x800):
    """Branch targets inside [site, site+size) (to make sure nothing jumps into displaced bytes)."""
    tg = set()
    for ins in CS.disasm(bytes(orig[va2off(site):va2off(site) + size]), site):
        m = ins.mnemonic.split(".")[0]
        if m in ("b", "cbz", "cbnz") or re.fullmatch(r"b(eq|ne|cs|hs|cc|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)", m):
            try:
                tg.add(int(ins.op_str.split("#")[-1], 16))
            except ValueError:
                pass
    return tg


def check_stock(orig, lib, va2off_orig, va2off_lib):
    """Every site must hold the stock bytes in the stock library and in the library being patched."""
    sites = [(s, bytes.fromhex(v[2])) for s, v in ENTRY.items()]
    sites += [(s, bytes.fromhex(v[1])) for s, v in CALLS.items()]
    sites += [(s, bytes.fromhex(v[0])) for s, v in DETOURS.items()]
    sites += [(s, bytes.fromhex(v[0])) for s, v in INPLACE.items()]
    for site, exp in sites:
        got = bytes(orig[va2off_orig(site):va2off_orig(site) + len(exp)])
        if got != exp:
            raise SystemExit(f"squad_caps: stock bytes at {site:#x} are {got.hex()}, expected {exp.hex()}")
        got = bytes(lib[va2off_lib(site):va2off_lib(site) + len(exp)])
        if got != exp:
            raise SystemExit(f"squad_caps: {site:#x} was already patched by another change ({got.hex()})")
    for site, v in CALLS.items():
        ins = next(CS.disasm(bytes.fromhex(v[1]), site))
        if ins.mnemonic != "blx" or int(ins.op_str.lstrip("#"), 16) != v[2]:
            raise SystemExit(f"squad_caps: {site:#x} is not blx {v[2]:#x}")


def build_blob(text_va, bss_va, tramps, squad_max, save_gate, build_dir=BUILD_DIR):
    """Compile + link squad64.c at text_va (bss at bss_va). Returns (text bytes, symbols, bss size)."""
    build_dir.mkdir(parents=True, exist_ok=True)
    gs = [".syntax unified", ".thumb"]
    for name, (addr, is_func) in GAME_SYMS.items():
        gs += [f".global {name}", (f".thumb_set {name}, {addr | 1:#x}" if is_func else f".set {name}, {addr:#x}")]
    for name, addr in tramps.items():
        gs += [f".global {name}", f".thumb_set {name}, {addr | 1:#x}"]
    (build_dir / "syms.s").write_text("\n".join(gs) + "\n")
    (build_dir / "squad64.ld").write_text(
        "SECTIONS {\n"
        f"  . = {text_va:#x};\n"
        "  .text : { *(.text .text.*) *(.rodata .rodata.*) }\n"
        "  .data : { *(.data .data.*) }\n"
        f"  . = {bss_va:#x};\n"
        "  .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }\n"
        "  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) *(.ARM.attributes) }\n"
        "}\n")
    obj, sobj, elf = build_dir / "squad64.o", build_dir / "syms.o", build_dir / "squad64.elf"
    _run(["arm-linux-gnueabi-gcc", "-mthumb", "-march=armv7-a", "-mfpu=vfpv3-d16", "-mfloat-abi=softfp", "-O2",
          "-fPIC", "-fvisibility=hidden", "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fno-common",
          "-ffunction-sections", "-fno-asynchronous-unwind-tables", "-fno-unwind-tables", "-Wall", "-Wextra",
          "-Werror", f"-DSQUAD_EXT_CAP={squad_max - SQUAD_STOCK}", f"-DSAVE_GATE={save_gate:#x}",
          "-c", _wsl(C_SOURCE), "-o", _wsl(obj)])
    rel = _run(["arm-linux-gnueabi-readelf", "-rW", _wsl(obj)])
    for line in rel.splitlines():
        m = re.match(r"^[0-9a-f]{8}\s+[0-9a-f]{8}\s+(R_ARM_\w+)", line)
        if m and m.group(1) not in ("R_ARM_REL32", "R_ARM_THM_CALL", "R_ARM_THM_JUMP24"):
            raise SystemExit(f"squad_caps: {m.group(1)} relocation in squad64.o (absolute address?)")
    _run(["arm-linux-gnueabi-as", _wsl(build_dir / "syms.s"), "-o", _wsl(sobj)])
    _run(["arm-linux-gnueabi-ld", "-T", _wsl(build_dir / "squad64.ld"), "-o", _wsl(elf), _wsl(obj), _wsl(sobj)])
    b = elf.read_bytes()
    sec = _elf_sections(b)
    for name, s in sec.items():
        if name in (".data", ".got", ".got.plt", ".rel.dyn", ".rel.plt", ".dynamic") and s[5]:
            raise SystemExit(f"squad_caps: squad64.elf has a non-empty {name} section")
    t = sec[".text"]
    if t[3] != text_va:
        raise SystemExit("squad_caps: .text not at the requested address")
    text = b[t[4]:t[4] + t[5]]
    bss = sec.get(".bss")
    bss_size = bss[5] if bss else 0
    if bss and bss[3] != bss_va:
        raise SystemExit("squad_caps: .bss not at the requested address")
    syms = {n: v for n, (v, sz, shn) in _elf_symbols(b).items()}
    return text, syms, bss_size


def verify_blob(text, text_va, allowed):
    """Every branch that leaves the blob lands on a game function / trampoline; no mode switches."""
    end = text_va + len(text)
    for ins in CS.disasm(text, text_va):
        m = ins.mnemonic.split(".")[0]
        if m == "blx" and ins.op_str.startswith("#"):
            raise SystemExit(f"squad_caps: blob {ins.address:#x}: blx to an immediate (ARM mode switch)")
        if m == "blx":
            raise SystemExit(f"squad_caps: blob {ins.address:#x}: indirect call {ins.op_str}")
        if m in ("b", "bl") and ins.op_str.startswith("#"):
            tgt = int(ins.op_str.lstrip("#"), 16)
            if not (text_va <= tgt < end) and tgt not in allowed:
                raise SystemExit(f"squad_caps: blob {ins.address:#x}: {ins.mnemonic} {tgt:#x} leaves the blob")


def apply(lib, orig, layout, asm=None, squad_max=64, list_patches=False):
    """Patch `lib` (bytearray, already extended by elf_extend) in place. Returns an info dict."""
    if squad_max == SQUAD_STOCK:
        return None
    if not SQUAD_STOCK < squad_max <= 64:
        raise SystemExit("squad_caps: --squad-max must be 32..64")
    if layout is None:
        raise SystemExit("squad_caps: needs the CAVE3/MODDATA segments (drop --no-extend)")
    va2off_lib = lambda va: elf_extend.va2off(lib, va)
    va2off_orig = lambda va: elf_extend.va2off(orig, va)
    check_stock(orig, lib, va2off_orig, va2off_lib)
    region = layout["cave3_va"] + layout["cave3_size"] - REGION_SIZE
    ro = va2off_lib(region)
    if any(lib[ro:ro + REGION_SIZE]):
        raise SystemExit(f"squad_caps: CAVE3 tail {region:#x} is not free")
    bss_va = layout["moddata_va"] + BSS_OFFSET
    # save version: the overflow blocks are gated at SQUAD_SAVE_GATE (0xB6, reserved for squad_v33);
    # the profile is written with max(current, gate) so an older build refuses the new saves
    cur = {lib[va2off_lib(s)] for s in SAVE_VERSION_SITES}
    if len(cur) != 1 or any(lib[va2off_lib(s) + 1] != 0x21 for s in SAVE_VERSION_SITES):
        raise SystemExit("squad_caps: save version sites are not 'movs r1, #imm'")
    save_gate = SQUAD_SAVE_GATE
    save_version = max(cur.pop(), save_gate)
    # trampolines
    tramps, tramp_code = {}, {}
    pc = region
    for site, (cfn, tname, _) in sorted(ENTRY.items()):
        if not tname:
            continue
        n = _displace_len(orig, va2off_orig, site)
        disp = check_displaced(orig, va2off_orig, site, n)
        inside = {t for t in _func_branch_targets(orig, va2off_orig, site) if site < t < site + n}
        if inside:
            raise SystemExit(f"squad_caps: {site:#x}: branch target inside the displaced bytes")
        code = disp + _branch("b.w", pc + n, site + n)
        code += b"\x00\xbf" * ((TRAMP_SLOT - len(code)) // 2)
        if len(code) != TRAMP_SLOT:
            raise SystemExit(f"squad_caps: trampoline for {site:#x} is {len(code)} bytes")
        tramps[tname] = pc
        tramp_code[pc] = code
        pc += TRAMP_SLOT
    if pc > region + TEXT_OFFSET:
        raise SystemExit("squad_caps: trampolines overflow")
    text_va = region + TEXT_OFFSET
    text, syms, bss_size = build_blob(text_va, bss_va, tramps, squad_max, save_gate)
    if BSS_OFFSET + bss_size > BSS_LIMIT or BSS_OFFSET + bss_size > layout["moddata_size"]:
        raise SystemExit(f"squad_caps: overflow tables ({bss_size:#x} bytes) do not fit MODDATA+{BSS_OFFSET:#x}")
    allowed = {a for a, f in GAME_SYMS.values() if f} | set(tramps.values())
    verify_blob(text, text_va, allowed)
    # detour stubs after the C text
    stub_pc = (text_va + len(text) + 3) & ~3
    stubs = {}
    for site, (_, resume, cfn, tmpl) in sorted(DETOURS.items()):
        code = bytearray(_asm(tmpl.format(resume=resume, target=syms[cfn]), stub_pc))
        for ins in CS.disasm(bytes(code), stub_pc):         # re-encode + verify every far branch
            if ins.mnemonic in ("bl", "b.w"):
                want = resume if ins.mnemonic == "b.w" else syms[cfn]
                code[ins.address - stub_pc:ins.address - stub_pc + 4] = _branch(ins.mnemonic, ins.address, want)
        if len(code) % 4:
            code += b"\x00\xbf"
        stubs[site] = (stub_pc, bytes(code))
        stub_pc += len(code)
    if stub_pc > region + REGION_SIZE:
        raise SystemExit("squad_caps: CAVE3 region overflow")
    # write region contents
    for addr, code in tramp_code.items():
        o = va2off_lib(addr); lib[o:o + len(code)] = code
    o = va2off_lib(text_va); lib[o:o + len(text)] = text
    for site, (addr, code) in stubs.items():
        o = va2off_lib(addr); lib[o:o + len(code)] = code
    patches = {}
    for site, (cfn, _, stock) in ENTRY.items():
        patches[site] = _branch("b.w", site, syms[cfn])
    for site, (cfn, _, _) in CALLS.items():
        patches[site] = _branch("bl", site, syms[cfn])
    for site, (stock, _, _, _) in DETOURS.items():
        code = _branch("b.w", site, stubs[site][0])
        n = len(bytes.fromhex(stock))
        patches[site] = code + (b"\xaf\xf3\x00\x80" * ((n - 4) // 4))   # nop.w (never executed)
    fmt = dict(rows=squad_max - 11, last=squad_max - 1, count=squad_max,
               **{k: v for k, v in syms.items() if k.startswith("sq_")})
    for site, (stock, tmpl) in INPLACE.items():
        code = bytearray(_asm(tmpl.format(**fmt), site))
        for ins in CS.disasm(bytes(code), site):
            if ins.mnemonic == "bl":
                code[ins.address - site:ins.address - site + 4] = _branch("bl", ins.address, syms["sq_team_count"])
        if len(code) != len(bytes.fromhex(stock)):
            raise SystemExit(f"squad_caps: {site:#x} re-encoded to {len(code)} bytes")
        patches[site] = bytes(code)
    for site in SAVE_VERSION_SITES:
        patches[site] = bytes([save_version, 0x21])
    for site, code in patches.items():
        o = va2off_lib(site)
        lib[o:o + len(code)] = code
        if list_patches:
            print(f"--- {site:#x}")
            for i in CS.disasm(code, site):
                print(f"  {i.address:08x}: {i.mnemonic:8s} {i.op_str}")
    return dict(region=region, text_va=text_va, text_size=len(text), bss_va=bss_va, bss_size=bss_size,
                save_version=save_version, save_gate=save_gate, tramps=tramps, stubs={k: v[0] for k, v in stubs.items()},
                syms={k: v for k, v in syms.items() if k.startswith(("sq_", "g_"))}, sites=len(patches))
