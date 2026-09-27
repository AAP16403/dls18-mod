#!/usr/bin/env python3
"""Raise v30's animation count to include appended DLS18 SAT clips."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs
from keystone import KS_ARCH_ARM, KS_MODE_THUMB, Ks

from armdis import va2off
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "mod"))
from anim_caps import COUNT_SITES, PRELOAD_BOUND, SENTINEL_SITES, SENTINEL_MOVT, STOCK_N, NEW_SENTINEL  # noqa: E402


def enable(source: Path, output: Path, count: int) -> dict:
    if not STOCK_N < count <= 3840:
        raise ValueError("Animation count must be 2536..3840")
    data = bytearray(source.read_bytes())
    disassembler = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    assembler = Ks(KS_ARCH_ARM, KS_MODE_THUMB)

    def disasm_at(site: int) -> tuple[str, str]:
        offset = va2off(site)
        if offset is None:
            raise ValueError(f"Unmapped site {site:#x}")
        insns = list(disassembler.disasm(bytes(data[offset:offset + 4]), site))
        if len(insns) != 1 or insns[0].size != 4:
            raise ValueError(f"Unexpected instruction at {site:#x}")
        return insns[0].mnemonic, insns[0].op_str

    def asm(code: str, site: int) -> bytes:
        encoded, count = assembler.asm(code, site)
        if count != 1:
            raise ValueError(f"Assembly failed at {site:#x}")
        return bytes(encoded)

    changes = {}

    def change(site: int, expected: str, value: int) -> None:
        mnemonic, operands = disasm_at(site)
        register, old_value = [part.strip() for part in operands.split(",", 1)]
        if mnemonic != expected or int(old_value.removeprefix("#"), 0) != (STOCK_N if site in COUNT_SITES + SENTINEL_SITES + [SENTINEL_MOVT] else (0x1B1C if site == PRELOAD_BOUND[0] else 5)):
            raise ValueError(f"Unexpected instruction at {site:#x}: {mnemonic} {operands}")
        code = asm(f"{mnemonic} {register}, #{value:#x}", site)
        if len(code) != 4:
            raise ValueError(f"Wrong encoded size at {site:#x}")
        changes[site] = code

    for site in COUNT_SITES:
        change(site, "movw", count)
    bound = count * 0x84
    change(PRELOAD_BOUND[0], "movw", bound & 0xffff)
    change(PRELOAD_BOUND[1], "movt", bound >> 16)
    for site in SENTINEL_SITES:
        change(site, "movw", NEW_SENTINEL)
    change(SENTINEL_MOVT, "movt", NEW_SENTINEL)
    for site, code in changes.items():
        offset = va2off(site)
        data[offset:offset + 4] = code
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)
    return {"source": str(source), "output": str(output), "animation_count": count,
            "patched_sites": [f"{site:#x}" for site in sorted(changes)]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--count", type=int, required=True)
    args = parser.parse_args()
    print(json.dumps(enable(args.source, args.output, args.count), indent=2))
