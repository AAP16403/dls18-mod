#!/usr/bin/env python3
"""Append a DLS18-format base SAT and its mirror to an animation KPX."""

from __future__ import annotations

import argparse
import json
import struct
import zlib
from pathlib import Path

from build_catalog import parse_kpx


ROW = 100
CLIP_CAPACITY = 3840


def extend_database(data: bytes, template_id: int, source_db: bytes | None = None,
                    source_id: int | None = None, action_time: int | None = None) -> bytes:
    count = struct.unpack_from("<I", data)[0]
    if not 2535 <= count <= CLIP_CAPACITY - 2 or template_id + 1 >= count:
        raise ValueError("Animation count must leave room in the expanded cache")
    records_end = 4 + count * ROW
    records = [data[4 + i * ROW:4 + (i + 1) * ROW] for i in range(count)]
    if records[template_id][2] & 1 or not records[template_id + 1][2] & 1:
        raise ValueError("Template must be a base followed by its mirror")
    cursor = records_end
    selected = []
    for i, record in enumerate(records):
        start = cursor
        for marker in (0x28, 0x2C):
            if struct.unpack_from("<I", record, marker)[0]:
                if cursor + 4 > len(data):
                    raise ValueError(f"Truncated curve size for animation {i}")
                size = struct.unpack_from("<I", data, cursor)[0]
                cursor += 4 + size
                if cursor > len(data):
                    raise ValueError(f"Truncated curve for animation {i}")
        if i in (template_id, template_id + 1):
            selected.append(data[start:cursor])
    if cursor != len(data):
        raise ValueError(f"Database curve walk ended {len(data) - cursor} bytes early")
    new_base = bytearray(records[template_id])
    new_mirror = bytearray(records[template_id + 1])
    if source_db is not None:
        if source_id is None:
            raise ValueError("Source animation ID is required with a source database")
        source_count = struct.unpack_from("<I", source_db)[0]
        if not 0 <= source_id < source_count:
            raise ValueError("Source animation ID is outside the DLS26 database")
        source_record = source_db[4 + source_id * 104:4 + (source_id + 1) * 104]
        if len(source_record) != 104:
            raise ValueError("Source record is truncated")
        # The DLS26-to-DLS18 SAT port keeps spatial units, but maps the source
        # contact tick to the target tick. Keep the target's action time (+0x30)
        # and use the source contact position (+0x32..+0x37).
        x, y, z = struct.unpack_from("<3h", source_record, 0x32)
        if action_time is not None:
            if not 0 <= action_time <= 0xffff:
                raise ValueError("Action time must fit a DLS18 u16")
            struct.pack_into("<H", new_base, 0x30, action_time)
            struct.pack_into("<H", new_mirror, 0x30, action_time)
        struct.pack_into("<3h", new_base, 0x32, x, y, z)
        struct.pack_into("<3h", new_mirror, 0x32, x, y, -z)
    return (struct.pack("<I", count + 2) + data[4:records_end]
            + new_base + new_mirror
            + data[records_end:] + b"".join(selected))


def append(source: Path, candidate: Path, output: Path, template_id: int = 331,
           source_db: Path | None = None, source_id: int | None = None,
           action_time: int | None = None) -> dict:
    data = source.read_bytes()
    marker, entries = parse_kpx(data)
    if marker != b"\0KPX":
        raise ValueError("Expected a DLS18 animation KPX")
    by_name = {entry["path"]: entry for entry in entries}
    old_db = by_name["animdb.adb"]["data"]
    count = struct.unpack_from("<I", old_db)[0]
    if len(entries) != count + 1 or f"{count - 1:04d}.sat" not in by_name:
        raise ValueError("Animation database count and numbered SAT members disagree")
    new_db = extend_database(old_db, template_id,
                             source_db.read_bytes() if source_db is not None else None,
                             source_id, action_time)
    base = candidate.read_bytes()
    original = by_name[f"{template_id:04d}.sat"]["data"]
    mirror = by_name[f"{template_id + 1:04d}.sat"]["data"]
    if len(base) != len(original) or base[:8] != original[:8]:
        raise ValueError("Candidate does not match the template SAT layout")
    if mirror[5] == 0 or mirror[:3] != base[:3]:
        raise ValueError("Template mirror header does not match the base")

    folders, files, names_size = struct.unpack_from("<III", data, 4)
    if folders != 1 or files != len(entries):
        raise ValueError("Expected one root folder and one table entry per file")
    root = list(struct.unpack_from("<5I", data, 16))
    if root[1] != files or root[2] != 0 or root[3] != 0:
        raise ValueError("Unexpected root folder layout")
    old_names_offset = 16 + folders * 20 + files * 24
    old_names_end = old_names_offset + names_size
    base_filename = f"{count:04d}.sat"
    mirror_filename = f"{count + 1:04d}.sat"
    added_names = base_filename.encode() + b"\0" + mirror_filename.encode() + b"\0"
    shift = 2 * 24 + len(added_names)
    rows = [list(struct.unpack_from("<6I", data, 36 + i * 24)) for i in range(files)]
    for row in rows:
        row[2] += shift
    payload = bytearray(data[old_names_end:])

    def add_payload(raw: bytes, compressed: bool, metadata: int) -> tuple[int, int, int, int, int]:
        stored = zlib.compress(raw, 9) if compressed else raw
        offset = old_names_end + shift + len(payload)
        payload.extend(stored)
        return len(raw), offset, int(compressed), metadata, len(stored)

    db_index = by_name["animdb.adb"]["file_index"]
    db_meta = rows[db_index][4]
    rows[db_index][1:] = add_payload(new_db, bool(rows[db_index][3]), db_meta)
    sat_meta = rows[by_name[f"{template_id:04d}.sat"]["file_index"]][4]
    base_name_offset = names_size
    mirror_name_offset = names_size + len(base_filename) + 1
    for name_offset, raw in ((base_name_offset, base), (mirror_name_offset, mirror)):
        rows.append([name_offset, *add_payload(raw, True, sat_meta)])

    root[1] += 2
    result = bytearray(marker + struct.pack("<III", folders, files + 2, names_size + len(added_names)))
    result.extend(struct.pack("<5I", *root))
    for row in rows:
        result.extend(struct.pack("<6I", *row))
    result.extend(data[old_names_offset:old_names_end])
    result.extend(added_names)
    result.extend(payload)
    if len(result) != old_names_end + shift + len(payload):
        raise AssertionError("KPX offset mismatch")
    _, decoded = parse_kpx(result)
    decoded_by_name = {entry["path"]: entry["data"] for entry in decoded}
    if len(decoded) != files + 2 or decoded_by_name["animdb.adb"] != new_db:
        raise AssertionError("Repacked KPX did not decode")
    if decoded_by_name[base_filename] != base or decoded_by_name[mirror_filename] != mirror:
        raise AssertionError("Appended clips did not decode")
    for entry in entries:
        if entry["path"] != "animdb.adb" and decoded_by_name[entry["path"]] != entry["data"]:
            raise AssertionError(f"Existing member changed: {entry['path']}")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(result)
    return {"source": str(source), "candidate": str(candidate), "output": str(output),
            "template_ids": [template_id, template_id + 1], "appended_ids": [count, count + 1],
            "source_contact_record": source_id, "action_time": action_time,
            "file_count": len(decoded), "animdb_count": struct.unpack_from("<I", new_db)[0]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--template-id", type=int, default=331)
    parser.add_argument("--source-db", type=Path)
    parser.add_argument("--source-id", type=int)
    parser.add_argument("--action-time", type=int,
                        help="resampled DLS18 first action time; default keeps template time")
    args = parser.parse_args()
    print(json.dumps(append(args.source, args.candidate, args.output,
                            args.template_id, args.source_db, args.source_id,
                            args.action_time), indent=2))
