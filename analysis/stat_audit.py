"""Audit every CPlayer::AttributeInterpolate_Internal call: stat type + output range per call site.

Resolves the constant r1 (stat type), r2 (value at stat<=40), r3 (value at stat>=99) set up in the
instructions preceding each call. Writes analysis/stat_audit.csv.
"""
import csv
import re
import sys
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs

from armdis import D, find, name_at, va2off
from xref import callers

STAT = {1: "stamina", 2: "speed", 3: "acceleration", 4: "control", 5: "passing", 6: "crossing/long",
        7: "shooting", 8: "stat8(col9)", 9: "tackling", 10: "stat10", 11: "stat11", 12: "stat12"}
CS = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
CS.detail = False


def imm(op):
    m = re.search(r"#(-?0x[0-9a-f]+|-?\d+)$", op)
    return int(m.group(1), 0) if m else None


def resolve(call):
    """Walk back up to 24 instructions tracking the last constant written to r1/r2/r3."""
    start = call - 0x40
    ins = [i for i in CS.disasm(D[va2off(start):va2off(call)], start)]
    vals = {}
    sites = {}
    for i in ins:
        ops = i.op_str.split(", ")
        dst = ops[0]
        if dst not in ("r1", "r2", "r3"):
            continue
        v = imm(i.op_str)
        if i.mnemonic in ("movs", "mov", "mov.w", "movw"):
            vals[dst] = v
            sites[dst] = (i.address, i.mnemonic, i.size)
        elif i.mnemonic in ("mvn", "mvns") and v is not None:
            vals[dst] = ~v
            sites[dst] = (i.address, i.mnemonic, i.size)
        elif i.mnemonic == "movt":
            if vals.get(dst) is not None and v is not None:
                vals[dst] = (vals[dst] & 0xFFFF) | (v << 16)
                if vals[dst] >= 0x80000000:
                    vals[dst] -= 1 << 32
        else:
            vals[dst] = None
            sites[dst] = None
    return vals, sites


def main():
    rows = []
    for s, n in callers("_ZN7CPlayer29AttributeInterpolate_InternalE15EPlayerStatTypeiiiii"):
        vals, sites = resolve(s)
        st, lo, hi = vals.get("r1"), vals.get("r2"), vals.get("r3")
        spread = ""
        if isinstance(lo, int) and isinstance(hi, int) and lo and hi:
            spread = f"{max(lo, hi) / min(lo, hi):.2f}" if lo * hi > 0 else "sign-change"
        rows.append(dict(site=f"{s:#x}", func=n, stat=STAT.get(st, st), lo=lo, hi=hi, spread=spread,
                         lo_insn=sites.get("r2") and f"{sites['r2'][0]:#x}:{sites['r2'][1]}",
                         hi_insn=sites.get("r3") and f"{sites['r3'][0]:#x}:{sites['r3'][1]}"))
    out = Path(__file__).parent / "stat_audit.csv"
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)
    for r in rows:
        print(f"{r['site']}  {r['stat']!s:14} lo={r['lo']!s:>8} hi={r['hi']!s:>8} x{r['spread']:<6} {r['func'][:70]}")


if __name__ == "__main__":
    main()
