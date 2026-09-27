#!/usr/bin/env python3
"""Map DLS18 animation IDs to the native library's readable animation names."""

from __future__ import annotations

import argparse
import csv
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "catalog" / "animation_names.csv")
    args = parser.parse_args()

    import sys

    sys.path.insert(0, str(ROOT / "analysis"))
    import armdis  # noqa: PLC0415

    data = armdis.D
    table_va = armdis.find("ANIM_sName")
    table_off = armdis.va2off(table_va)
    table_size = next(size for address, size, *_rest, name in armdis.SYMS if name == "ANIM_sName")
    if table_off is None or table_size % 4:
        raise RuntimeError("ANIM_sName is not a file-backed array of 32-bit pointers")

    rows = []
    for animation_id in range(table_size // 4):
        name_va = struct.unpack_from("<I", data, table_off + animation_id * 4)[0]
        name_off = armdis.va2off(name_va)
        if name_off is None:
            raise RuntimeError(f"animation {animation_id} name pointer {name_va:#x} is not file-backed")
        end = data.find(b"\0", name_off, name_off + 512)
        if end < 0:
            raise RuntimeError(f"animation {animation_id} name is not null terminated")
        name = data[name_off:end].decode("ascii")
        rows.append({
            "animation_id": animation_id,
            "clip_file": f"{animation_id:04d}.sat",
            "animation_name": name,
            "family": name.split("_", 2)[1] if name.startswith("ANM_") else "",
            "variant": "suffix_f_variant" if name.endswith("_f") else "unsuffixed",
        })

    if len(rows) != 2535:
        raise RuntimeError(f"expected 2535 native animation names, found {len(rows)}")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    print(f"Mapped {len(rows)} animation IDs to native names: {args.out}")


if __name__ == "__main__":
    main()
