#!/usr/bin/env python3
"""Export the 20 animation candidate groups loaded from animdb.adb."""

from __future__ import annotations

import argparse
import csv
import struct
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ANIMDB = ROOT / "unpacked" / "kpx" / "anims" / "animdb.adb"
NAMES = ROOT / "catalog" / "animation_names.csv"
ROW_BYTES = 0x64


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, default=ROOT / "catalog" / "animdb_candidate_groups.csv")
    args = parser.parse_args()

    data = ANIMDB.read_bytes()
    if len(data) < 4:
        raise ValueError("animdb.adb is shorter than its count header")
    count = struct.unpack_from("<I", data)[0]
    end = 4 + count * ROW_BYTES
    if end > len(data):
        raise ValueError(f"animdb rows exceed file length: {end} > {len(data)}")
    with NAMES.open(newline="", encoding="utf-8") as stream:
        names = {int(row["animation_id"]): row["animation_name"] for row in csv.DictReader(stream)}
    if len(names) != count:
        raise ValueError(f"animation-name table has {len(names)} rows; animdb has {count}")

    groups: dict[int, list[int]] = defaultdict(list)
    for animation_id in range(count):
        category = data[4 + animation_id * ROW_BYTES]
        if category >= 20:
            raise ValueError(f"animation {animation_id} has unsupported category {category}")
        groups[category].append(animation_id)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["animdb_candidate_group", "animation_count", "animation_ids", "animation_names"])
        for category in range(20):
            ids = groups[category]
            writer.writerow([category, len(ids), ";".join(map(str, ids)),
                             ";".join(names[i] for i in ids)])
    print(f"wrote {args.out} ({count} animations in {len(groups)} loader groups)")


if __name__ == "__main__":
    main()
