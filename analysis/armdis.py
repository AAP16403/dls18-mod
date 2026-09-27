"""Symbol-aware Thumb disassembler for libDLS18.so.

usage: python armdis.py <symbol-substring-or-hex-addr> [count]
"""
import bisect
import struct
import subprocess
import sys
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs

LIB = Path(__file__).resolve().parent.parent / "unpacked/apk/lib/armeabi-v7a/libDLS18.so"
D = LIB.read_bytes()


def _sections():
    shoff = struct.unpack_from("<I", D, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", D, 0x2E)
    sh = [struct.unpack_from("<10I", D, shoff + i * shentsize) for i in range(shnum)]
    stroff = sh[shstrndx][4]
    out = {}
    for s in sh:
        name = D[stroff + s[0]:D.index(b"\0", stroff + s[0])].decode()
        out[name] = dict(addr=s[3], off=s[4], size=s[5])
    return out


SECT = _sections()


def _symbols():
    ds, dstr = SECT[".dynsym"], SECT[".dynstr"]
    syms = []
    for i in range(ds["size"] // 16):
        nm, val, sz, info, oth, shn = struct.unpack_from("<IIIBBH", D, ds["off"] + i * 16)
        name = D[dstr["off"] + nm:D.index(b"\0", dstr["off"] + nm)].decode()
        syms.append((val, sz, info & 0xF, shn, name))
    return syms


SYMS = _symbols()
DEFINED = sorted((v & ~1, sz, n) for v, sz, t, shn, n in SYMS if v and shn)
_ADDRS = [a for a, _, _ in DEFINED]


def _plt_map():
    """Map PLT stub address -> imported symbol (stub i <-> .rel.plt entry i)."""
    rel, plt = SECT[".rel.plt"], SECT[".plt"]
    res = {}
    for i in range(rel["size"] // 8):
        off, info = struct.unpack_from("<II", D, rel["off"] + i * 8)
        res[plt["addr"] + 20 + i * 12] = SYMS[info >> 8][4]
    return res


PLT = _plt_map()


def va2off(va):
    for s in SECT.values():
        if s["addr"] and s["addr"] <= va < s["addr"] + s["size"] and s["off"]:
            return va - s["addr"] + s["off"]
    return None


def demangle(names):
    try:
        p = subprocess.run(["wsl", "-e", "c++filt"], input="\n".join(names), capture_output=True, text=True)
        return p.stdout.splitlines()
    except Exception:
        return names


def name_at(va):
    va &= ~1
    if va in PLT:
        return "plt:" + PLT[va]
    i = bisect.bisect_right(_ADDRS, va) - 1
    if i >= 0:
        a, sz, n = DEFINED[i]
        if va == a:
            return n
        if va < a + max(sz, 1):
            return f"{n}+{va - a:#x}"
    return None


def find(q):
    if q.startswith("0x"):
        return int(q, 16)
    hits = [(a, sz, n) for a, sz, n in DEFINED if q in n]
    exact = [h for h in hits if h[2] == q]
    return (exact or hits)[0][0] if hits else None


def disasm(va, count=None, size=None):
    va &= ~1
    if size is None:
        i = bisect.bisect_right(_ADDRS, va) - 1
        size = DEFINED[i][1] if DEFINED[i][0] == va else 0x200
    cs = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    cs.skipdata = True
    off = va2off(va)
    lines = []
    for ins in cs.disasm(D[off:off + size], va):
        cmt = ""
        ops = ins.op_str
        if ins.mnemonic in ("bl", "blx", "b", "b.w", "cbz", "cbnz") or ins.mnemonic.startswith("b"):
            try:
                t = int(ops.split("#")[-1], 0)
                n = name_at(t)
                if n:
                    cmt = n
            except ValueError:
                pass
        if "[pc" in ops and ins.mnemonic.startswith(("ldr", "vldr")):
            try:
                imm = int(ops.split("#")[-1].rstrip("]"), 0) if "#" in ops else 0
                lit = ((ins.address + 4) & ~3) + imm
                lo = va2off(lit)
                w = struct.unpack_from("<I", D, lo)[0]
                f = struct.unpack_from("<f", D, lo)[0]
                cmt = f"lit@{lit:#x} = {w:#x} / {f:g}"
            except Exception:
                pass
        lines.append(f"{ins.address:08x}: {ins.mnemonic:8s} {ops:40s} {cmt}")
        if count and len(lines) >= count:
            break
    return lines


if __name__ == "__main__":
    for q in sys.argv[1:]:
        q, _, n = q.partition(":")
        va = find(q)
        print(f"== {name_at(va)} @ {va:#x}")
        print(chr(10).join(disasm(va, size=int(n, 0) if n else None)))
