"""Unicorn harness: runs the stock libDLS18.so rating/value functions and compares with dls_value_model.

Read-only use of unpacked/apk/lib/armeabi-v7a/libDLS18.so. Nothing is written back.
"""
from __future__ import annotations

import math
import os
import struct

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UcError
from unicorn.arm_const import (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_C1_C0_2,
                               UC_ARM_REG_FPEXC, UC_ARM_REG_CPSR)

import dls_value_model as M

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
LIB = os.path.join(ROOT, "unpacked", "apk", "lib", "armeabi-v7a", "libDLS18.so")
PLAYERS = os.path.join(ROOT, "unpacked", "db_decoded", "players.dat.decoded.bin")

STUB_BASE = 0x0F000000
STACK_BASE = 0x0E000000
HEAP = 0x0D000000
RET_MAGIC = 0x0F0FFFF0


class Native:
    def __init__(self, config=M.CONFIG_BUNDLED):
        D = open(LIB, "rb").read()
        self.D = D
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        phoff, = struct.unpack_from("<I", D, 0x1C)
        phentsize, phnum = struct.unpack_from("<HH", D, 0x2A)
        top = 0
        segs = []
        for i in range(phnum):
            p_type, p_off, p_va, p_pa, p_fsz, p_msz, p_fl, p_al = struct.unpack_from("<8I", D, phoff + i * phentsize)
            if p_type == 1:
                segs.append((p_off, p_va, p_fsz, p_msz))
                top = max(top, p_va + p_msz)
        size = (top + 0xFFFFF) & ~0xFFFFF
        uc.mem_map(0, size)
        for off, va, fsz, msz in segs:
            uc.mem_write(va, D[off:off + fsz])
        uc.mem_map(STUB_BASE, 0x100000)
        uc.mem_map(STACK_BASE, 0x100000)
        uc.mem_map(HEAP, 0x100000)
        # sections / symbols
        shoff, = struct.unpack_from("<I", D, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", D, 0x2E)
        sh = [struct.unpack_from("<10I", D, shoff + i * shentsize) for i in range(shnum)]
        stroff = sh[shstrndx][4]
        sect = {}
        for s in sh:
            nm = D[stroff + s[0]:D.index(b"\0", stroff + s[0])].decode()
            sect[nm] = s
        ds, dstr = sect[".dynsym"], sect[".dynstr"]
        self.syms = []
        for i in range(ds[5] // 16):
            nm, val, sz, info, oth, shn = struct.unpack_from("<IIIBBH", D, ds[4] + i * 16)
            name = D[dstr[4] + nm:D.index(b"\0", dstr[4] + nm)].decode()
            self.syms.append((name, val, shn))
        self.by_name = {n: v for n, v, shn in self.syms if shn and v}
        self.stubs = {}          # addr -> (name, fn)
        self.stub_for_name = {}
        self.next_stub = STUB_BASE
        self.overrides = {}      # symbol name -> python fn
        self._reloc(sect)
        uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
        uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        uc.hook_add(UC_HOOK_CODE, self._hook, begin=STUB_BASE, end=STUB_BASE + 0xFFFFF)
        # CConfig::ms_iVars
        self.ms_ivars = self.by_name["_ZN7CConfig8ms_iVarsE"]
        self.set_config(config)

    def _stub(self, name):
        if name not in self.stub_for_name:
            a = self.next_stub
            self.next_stub += 4
            # Thumb "bx lr" so that returning from stub works even if hook doesn't redirect
            self.uc.mem_write(a, b"\x70\x47\x00\xbf")
            self.stub_for_name[name] = a
            self.stubs[a] = name
        return self.stub_for_name[name]

    def _reloc(self, sect):
        D = self.D
        self.got_of = {}
        for secname in (".rel.dyn", ".rel.plt"):
            s = sect[secname]
            for i in range(s[5] // 8):
                off, info = struct.unpack_from("<II", D, s[4] + i * 8)
                t, si = info & 0xFF, info >> 8
                if t == 23:  # RELATIVE, base 0 -> unchanged
                    continue
                name, val, shn = self.syms[si]
                self.got_of.setdefault(name, []).append((off, t))
                if t in (21, 22, 2):   # GLOB_DAT, JUMP_SLOT, ABS32
                    if shn and val:
                        tgt = val
                    else:
                        tgt = self._stub(name) | 1
                    if t == 2:
                        tgt = (tgt + struct.unpack_from("<I", D, off)[0]) & 0xFFFFFFFF if False else tgt
                    self.uc.mem_write(off, struct.pack("<I", tgt))

    def override(self, name, fn):
        """Redirect every GOT slot of symbol `name` to a python stub."""
        a = self._stub("ovr:" + name) | 1
        self.overrides["ovr:" + name] = fn
        for off, t in self.got_of.get(name, []):
            self.uc.mem_write(off, struct.pack("<I", a))

    def set_config(self, cfg):
        vals = {}
        for g in range(4):
            for k in range(4):
                vals[0x15B + 4 * g + k] = cfg[g][k]
        vals[0x16B] = cfg["exp_whole"]
        vals[0x16C] = cfg["exp_tenths"]
        vals[0x179] = cfg["scout_price_percent"]
        for k, v in vals.items():
            self.uc.mem_write(self.ms_ivars + 4 * k, struct.pack("<i", v))

    def _hook(self, uc, addr, size, user):
        name = self.stubs.get(addr & ~1)
        if name is None:
            return
        r = [uc.reg_read(x) for x in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        if name in self.overrides:
            ret = self.overrides[name](self, r)
        elif name == "powf":
            x = struct.unpack("<f", struct.pack("<I", r[0]))[0]
            y = struct.unpack("<f", struct.pack("<I", r[1]))[0]
            ret = struct.unpack("<I", struct.pack("<f", math.pow(x, y)))[0]
        else:
            raise RuntimeError("unhandled import called: " + name)
        uc.reg_write(UC_ARM_REG_R0, (ret or 0) & 0xFFFFFFFF)

    def call(self, name_or_addr, *args, stack_args=()):
        uc = self.uc
        addr = self.by_name[name_or_addr] if isinstance(name_or_addr, str) else name_or_addr
        sp = STACK_BASE + 0xF0000
        sp -= 4 * len(stack_args)
        for i, v in enumerate(stack_args):
            uc.mem_write(sp + 4 * i, struct.pack("<I", v & 0xFFFFFFFF))
        regs = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)
        for reg, v in zip(regs, args):
            uc.reg_write(reg, v & 0xFFFFFFFF)
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, RET_MAGIC | 1)
        uc.emu_start(addr | 1, RET_MAGIC, count=2_000_000)
        v = uc.reg_read(UC_ARM_REG_R0)
        return v - (1 << 32) if v & 0x80000000 else v


def build_native():
    n = Native()
    # CSeason queries used by GetPlayerValue -> fresh-career answers (0 / not scouted)
    n.override("_ZN7CSeason14GetSeasonCountEv", lambda s, r: 0)
    n.override("_ZN7CSeason16GetMatchesPlayedEv", lambda s, r: 0)
    n.override("_ZN7CSeason15IsPlayerScoutedEt", lambda s, r: 0)
    # ms_bSecretPlayerTurn lives in .bss (zero) -> no discount
    return n


def run_crosscheck(verbose=False, limit=None):
    n = build_native()
    d = open(PLAYERS, "rb").read()
    cnt = struct.unpack_from("<I", d, 8)[0]
    info_addr = HEAP
    mism = 0
    checked = 0
    for i in range(cnt if limit is None else min(limit, cnt)):
        rom = d[12 + i * 0xB4:12 + (i + 1) * 0xB4]
        info = M.player_info_from_rom(rom)
        det = struct.unpack_from("<b", info, 0x80)[0]
        if not 0 <= det <= 22:
            continue
        n.uc.mem_write(info_addr, bytes(info))
        nr = n.call("_Z18PU_GetPlayerRatingP11TPlayerInfo", info_addr)
        nv = n.call("_ZN10CTransfers14GetPlayerValueEP11TPlayerInfoiibb", info_addr, -1, -1, 0, stack_args=(1,))
        pr = M.pu_get_player_rating(info)
        pv = M.get_player_value(info, -1, -1, False, True)
        checked += 1
        if (nr, nv) != (pr, pv):
            mism += 1
            if verbose and mism < 20:
                print("MISMATCH id", M.u16(info, 0), "native", nr, nv, "python", pr, pv)
    # sweep all (genPos, rating) pairs incl. out-of-range ratings through the native value code
    sweep_bad = 0
    for g in range(4):
        for rating in range(0, 111):
            info = bytearray(0xB0)
            n.uc.mem_write(info_addr, bytes(info))
            nv = n.call("_ZN10CTransfers14GetPlayerValueEP11TPlayerInfoiibb", info_addr, g, rating, 0, stack_args=(1,))
            pv = M.get_player_value(info, g, rating, False, True)
            if nv != pv:
                sweep_bad += 1
    # also with compiled-in defaults
    n.set_config(M.CONFIG_DEFAULTS)
    for g in range(4):
        for rating in range(0, 111):
            info = bytearray(0xB0)
            n.uc.mem_write(info_addr, bytes(info))
            nv = n.call("_ZN10CTransfers14GetPlayerValueEP11TPlayerInfoiibb", info_addr, g, rating, 0, stack_args=(1,))
            pv = M.get_player_value(info, g, rating, False, True, M.CONFIG_DEFAULTS)
            if nv != pv:
                sweep_bad += 1
    n.set_config(M.CONFIG_BUNDLED)
    # GetTeamValueTotal with stubbed link/info providers
    link = HEAP + 0x1000
    ids = [struct.unpack_from("<H", d, 12 + i * 0xB4)[0] for i in range(0, 40 * 7, 7)][:25]
    roms = {struct.unpack_from("<H", d, 12 + i * 0xB4)[0]: d[12 + i * 0xB4:12 + (i + 1) * 0xB4] for i in range(cnt)}
    buf = bytearray(0x108)
    struct.pack_into("<ii", buf, 0, 0x7777, len(ids))
    for j, pid in enumerate(ids):
        struct.pack_into("<I", buf, 0x88 + 4 * j, pid)
    n.uc.mem_write(link, bytes(buf))
    n.override("_ZN9CDataBase11GetTeamLinkEi", lambda s, r: link)

    def gpi(s, r):
        s.uc.mem_write(r[0], bytes(M.player_info_from_rom(roms[r[1]])))
        return 1
    n.override("_ZN9CDataBase13GetPlayerInfoER11TPlayerInfoiibP10TPlayerROMiP15TTeamPlayerLinki", gpi)
    n.override("_ZN11TPlayerInfoC1Ev", lambda s, r: r[0])
    tv_native = n.call("_ZN9CDataBase17GetTeamValueTotalEi", 0x7777)
    tv_py = M.team_value_total([M.player_info_from_rom(roms[p]) for p in ids])
    if verbose:
        print(f"native cross-check: {checked} players, {mism} rating/value mismatches; "
              f"value sweep mismatches {sweep_bad}/888; team total native={tv_native} python={tv_py}")
    assert mism == 0 and sweep_bad == 0 and tv_native == tv_py
    return checked


if __name__ == "__main__":
    run_crosscheck(verbose=True)
