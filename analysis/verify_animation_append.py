#!/usr/bin/env python3
"""Verify appended animation members, database records, and native load count."""

from __future__ import annotations

import argparse
import struct
import zipfile
from pathlib import Path

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs

from armdis import va2off
from build_catalog import parse_kpx


COUNT_SITES = (0x2F8576, 0x2F8778, 0x2F8866, 0x2F889A, 0x2F8962)


def verify(apk: Path, first_new_id: int, last_new_id: int) -> dict:
    expected_count = last_new_id + 1
    with zipfile.ZipFile(apk) as archive:
        pak = archive.read("assets/data/anims/anims.pak")
        lib = archive.read("lib/armeabi-v7a/libDLS18.so")
    marker, entries = parse_kpx(pak)
    by_name = {entry["path"]: entry for entry in entries}
    db = by_name["animdb.adb"]["data"]
    count = struct.unpack_from("<I", db)[0]
    if count != expected_count:
        raise ValueError(f"animdb count {count} != expected {expected_count}")
    state_counts = [0] * 20
    for animation_id in range(count):
        category = db[4 + animation_id * 100]
        if category >= 20:
            raise ValueError(f"Animation {animation_id} has invalid state category {category}")
        state_counts[category] += 1
    missing = [f"{i:04d}.sat" for i in range(first_new_id, expected_count)
               if f"{i:04d}.sat" not in by_name]
    if missing:
        raise ValueError(f"Missing appended SAT members: {missing}")
    records = []
    for animation_id in range(first_new_id, expected_count):
        record = db[4 + animation_id * 100:4 + (animation_id + 1) * 100]
        sat = by_name[f"{animation_id:04d}.sat"]["data"]
        if len(record) != 100 or len(sat) < 104:
            raise ValueError(f"Truncated appended member {animation_id}")
        frame_count = sat[4]
        transform_tracks = sat[7]
        position_tracks = sat[0x32]
        expected_size = 104 + frame_count * transform_tracks * 14 + frame_count * position_tracks * 6
        if (sat[5] != int(bool(record[2] & 1)) or
                (sat[5] == 0 and len(sat) != expected_size)):
            raise ValueError(f"SAT/header mismatch for appended animation {animation_id}")
        records.append({"id": animation_id, "category": record[0],
                        "mirror": bool(record[2] & 1), "sat_bytes": len(sat)})

    disassembler = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    values = []
    for site in COUNT_SITES:
        offset = va2off(site)
        instruction = next(disassembler.disasm(lib[offset:offset + 4], site))
        if instruction.mnemonic != "movw":
            raise ValueError(f"Unexpected native count instruction at {site:#x}")
        values.append(int(instruction.op_str.split("#", 1)[1], 0))
    if values != [expected_count] * len(COUNT_SITES):
        raise ValueError(f"Native count sites disagree: {values}")
    return {"apk": str(apk), "marker": marker.decode("latin1"),
            "pak_members": len(entries), "animdb_count": count,
            "state_category_counts": state_counts,
            "native_count_sites": values, "appended": records}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--first-new-id", type=int, required=True)
    parser.add_argument("--last-new-id", type=int, required=True)
    args = parser.parse_args()
    import json
    print(json.dumps(verify(args.apk, args.first_new_id, args.last_new_id), indent=2))
