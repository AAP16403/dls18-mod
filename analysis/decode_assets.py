#!/usr/bin/env python3
"""Decode DLS18 SAT animation samples and export supported static FTM meshes.

The parser preserves each source asset and writes derived files under
decoded_assets/. It reads the format as used by the bundled DLS18 ARM library.
"""

from __future__ import annotations

import argparse
import csv
import gzip
import json
import math
import struct
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
ANIM_ROOT = ROOT / "unpacked" / "kpx" / "anims"
MODEL_ROOTS = [ROOT / "unpacked" / "kpx" / "models", ROOT / "unpacked" / "kpx" / "env"]
OUT_ROOT = ROOT / "decoded_assets"

# This 42-entry table is the native library's bone_remap constant. The SAT
# sampler checks one activity byte per slot, then uses this table to select
# the destination model-bone slot for that animation track.
BONE_REMAP = [
    0, 1, 2, 3, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
    38, 20, 21, 22, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
    19, 39, 40, 41,
]

SAT_HEADER_BYTES = 0x68
SAT_MAP_BYTES = 42
SAT_POSITION_MAP_BYTES = 45


def signed8(value: int) -> int:
    return value - 256 if value >= 128 else value


def safe_relative(path: Path, base: Path) -> str:
    return path.relative_to(base).as_posix()


def decode_sat(path: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    data = path.read_bytes()
    if len(data) < SAT_HEADER_BYTES:
        raise ValueError(f"SAT header is truncated ({len(data)} bytes)")

    interval_ticks = struct.unpack_from("<h", data, 2)[0]
    frame_count = signed8(data[4])
    format_flag = data[5]
    map_count = signed8(data[6])
    transform_track_count = signed8(data[7])
    position_track_count = signed8(data[0x32])
    header_maps = [signed8(v) for v in data[8 : 8 + SAT_MAP_BYTES]]
    position_map = [signed8(v) for v in data[0x33 : 0x33 + SAT_POSITION_MAP_BYTES]]
    active_slots = [i for i, value in enumerate(header_maps[: max(0, map_count)]) if value >= 0]
    active_position_slots = [i for i, value in enumerate(position_map) if value >= 0]
    track_map = [
        {
            "packed_track": track,
            "animation_slot": slot,
            "model_bone_slot": BONE_REMAP[slot],
            "activity_value": header_maps[slot],
        }
        for track, slot in enumerate(active_slots)
    ]

    header = {
        "header_bytes": SAT_HEADER_BYTES,
        "header_word_0": struct.unpack_from("<I", data, 0)[0],
        "sample_interval_ticks": interval_ticks,
        "sample_interval_seconds": interval_ticks / 30.0,
        "frame_count": frame_count,
        "frame_times_seconds": [frame * interval_ticks / 30.0 for frame in range(max(0, frame_count))],
        "format_flag": format_flag,
        "transform_map_count": map_count,
        "transform_track_count": transform_track_count,
        "transform_map_raw_signed": header_maps,
        "active_transform_slots": active_slots,
        "active_transform_bone_slots": [BONE_REMAP[i] for i in active_slots],
        "packed_transform_track_map": track_map,
        "position_track_count": position_track_count,
        "position_map_raw_signed": position_map,
        "active_position_map_slots": active_position_slots,
        "position_map_to_track": {
            str(i): value for i, value in enumerate(position_map) if value >= 0
        },
        "header_words_at_0x60_and_0x64": list(struct.unpack_from("<II", data, 0x60)),
        "rotation_encoding": "signed int16 quaternion components divided by 16384",
        "rotation_scale": 1.0 / 16384.0,
        "translation_encoding": "signed int16 position components divided by 128",
        "translation_scale": 1.0 / 128.0,
        "position_stream_encoding": "signed int16 XYZ triples; engine interpolation returns these raw units",
        "transform_record_order": "track-major, then frame",
        "position_record_order": "frame-major, then track",
    }

    body: dict[str, Any] = {
        "transform_records_raw": [],
        "position_records_raw_by_frame": [],
        "payload_status": "not present",
    }

    if format_flag != 0:
        body["payload_status"] = "header-only variant; the native loader does not read inline sample arrays"
        body["inline_transform_bytes"] = 0
        body["inline_position_bytes"] = 0
        return header, body

    if frame_count < 0 or transform_track_count < 0 or position_track_count < 0:
        body["payload_status"] = "negative signed count in header"
        return header, body

    transform_bytes = frame_count * transform_track_count * 14
    position_bytes = frame_count * position_track_count * 6
    expected_size = SAT_HEADER_BYTES + transform_bytes + position_bytes
    header["inline_transform_bytes"] = transform_bytes
    header["inline_position_bytes"] = position_bytes
    header["expected_file_bytes"] = expected_size
    header["actual_file_bytes"] = len(data)
    if expected_size != len(data):
        body["payload_status"] = "size does not match native loader's count formula"
        body["trailing_or_missing_bytes"] = len(data) - expected_size
        return header, body

    cursor = SAT_HEADER_BYTES
    transforms: list[list[list[int]]] = []
    for track in range(transform_track_count):
        frames: list[list[int]] = []
        for frame in range(frame_count):
            frames.append(list(struct.unpack_from("<7h", data, cursor)))
            cursor += 14
        transforms.append(frames)

    positions_by_frame: list[list[list[int]]] = []
    for frame in range(frame_count):
        tracks = []
        for track in range(position_track_count):
            tracks.append(list(struct.unpack_from("<3h", data, cursor)))
            cursor += 6
        positions_by_frame.append(tracks)

    body["payload_status"] = "decoded"
    body["transform_records_raw"] = transforms
    body["position_records_raw_by_frame"] = positions_by_frame
    return header, body


def write_animation_catalog() -> dict[str, int]:
    out_dir = OUT_ROOT / "animations"
    out_dir.mkdir(parents=True, exist_ok=True)
    files = sorted(ANIM_ROOT.rglob("*.sat"), key=lambda p: p.as_posix().lower())
    columns = [
        "file", "bytes", "interval_ticks", "frame_count", "format_flag",
        "map_count", "active_transform_slots", "transform_tracks",
        "position_tracks", "active_position_slots", "expected_bytes", "status",
    ]
    counts: dict[str, int] = {}
    with (out_dir / "index.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        for path in files:
            try:
                header, body = decode_sat(path)
                relative = Path(safe_relative(path, ANIM_ROOT))
                clip = {
                    "source": relative.as_posix(),
                    "header": header,
                    **body,
                }
                output = out_dir / relative.with_suffix(".json.gz")
                output.parent.mkdir(parents=True, exist_ok=True)
                with gzip.open(output, "wt", encoding="utf-8") as zf:
                    json.dump(clip, zf, separators=(",", ":"))
                status = body["payload_status"]
                counts[status] = counts.get(status, 0) + 1
                writer.writerow({
                    "file": relative.as_posix(),
                    "bytes": path.stat().st_size,
                    "interval_ticks": header.get("sample_interval_ticks"),
                    "frame_count": header.get("frame_count"),
                    "format_flag": header.get("format_flag"),
                    "map_count": header.get("transform_map_count"),
                    "active_transform_slots": len(header.get("active_transform_slots", [])),
                    "transform_tracks": header.get("transform_track_count"),
                    "position_tracks": header.get("position_track_count"),
                    "active_position_slots": len(header.get("active_position_map_slots", [])),
                    "expected_bytes": header.get("expected_file_bytes", ""),
                    "status": status,
                })
            except Exception as exc:
                counts["error"] = counts.get("error", 0) + 1
                writer.writerow({"file": path.name, "bytes": path.stat().st_size, "status": f"error: {exc}"})
    return {"clips": len(files), **counts}


def read_c_string(data: bytes, start: int, end: int) -> str | None:
    for offset in range(start, min(end, len(data))):
        if data[offset] < 0x20 or data[offset] > 0x7E:
            continue
        stop = offset
        while stop < min(end, len(data)) and 0x20 <= data[stop] <= 0x7E:
            stop += 1
        raw = data[offset:stop]
        if len(raw) >= 4 and sum(chr(c).isalpha() for c in raw) >= 3:
            return raw.decode("ascii", "replace")
    return None


def parse_static_ftm(path: Path) -> dict[str, Any] | None:
    data = path.read_bytes()
    if len(data) < 0xC4 or data[8:12] not in (b"FTTM", b"XGSM"):
        return None

    vertex_count, triangle_count = struct.unpack_from("<HH", data, 0x7C)
    if not (3 <= vertex_count <= 50000 and 1 <= triangle_count <= 100000):
        return None
    position_offset = 0xC4
    position_end = position_offset + vertex_count * 12
    index_offset = position_end + 8
    index_count = triangle_count * 3
    if index_offset + index_count * 2 > len(data):
        return None

    positions = list(struct.iter_unpack("<3f", data[position_offset:position_end]))
    if len(positions) != vertex_count or not all(
        math.isfinite(component) and abs(component) < 1_000_000
        for vertex in positions for component in vertex
    ):
        return None

    indices = list(struct.unpack_from(f"<{index_count}H", data, index_offset))
    if not indices or max(indices) >= vertex_count:
        return None

    # These offsets are relative to the shared platform-data base at 0x70.
    normal_offset = 0x70 + struct.unpack_from("<I", data, 0x90)[0]
    uv_offset = 0x70 + struct.unpack_from("<I", data, 0x9C)[0]
    if normal_offset + vertex_count * 12 > len(data) or uv_offset + vertex_count * 8 > len(data):
        return None
    normals = list(struct.iter_unpack("<3f", data[normal_offset : normal_offset + vertex_count * 12]))
    uvs = list(struct.iter_unpack("<2f", data[uv_offset : uv_offset + vertex_count * 8]))
    if not all(math.isfinite(c) and abs(c) < 1000 for row in normals for c in row):
        return None
    if not all(math.isfinite(c) and abs(c) < 1000 for row in uvs for c in row):
        return None

    bounds = [[min(v[i] for v in positions), max(v[i] for v in positions)] for i in range(3)]
    return {
        "source": safe_relative(path, ROOT),
        "name": read_c_string(data, 0x44, 0x70),
        "magic": data[8:12].decode("ascii"),
        "version_bytes": data[12:16].hex(),
        "file_bytes": len(data),
        "vertex_count": vertex_count,
        "triangle_count": triangle_count,
        "position_offset": position_offset,
        "index_offset": index_offset,
        "normal_offset": normal_offset,
        "uv_offset": uv_offset,
        "bounds": bounds,
        "positions": positions,
        "normals": normals,
        "uvs": uvs,
        "indices": indices,
    }


def write_obj(mesh: dict[str, Any], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write(f"# DLS18 static mesh export from {mesh['source']}\n")
        f.write(f"# vertices={mesh['vertex_count']} triangles={mesh['triangle_count']}\n")
        for x, y, z in mesh["positions"]:
            f.write(f"v {x:.7g} {y:.7g} {z:.7g}\n")
        for u, v in mesh["uvs"]:
            f.write(f"vt {u:.7g} {v:.7g}\n")
        for x, y, z in mesh["normals"]:
            f.write(f"vn {x:.7g} {y:.7g} {z:.7g}\n")
        idx = mesh["indices"]
        for i in range(0, len(idx), 3):
            a, b, c = (value + 1 for value in idx[i : i + 3])
            f.write(f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}\n")


def write_model_exports() -> dict[str, int]:
    out_dir = OUT_ROOT / "models"
    out_dir.mkdir(parents=True, exist_ok=True)
    files = sorted((p for root in MODEL_ROOTS for p in root.rglob("*.ftm")), key=lambda p: p.as_posix().lower())
    columns = ["source", "magic", "name", "bytes", "vertices", "triangles", "positions", "indices", "normals", "uvs", "status"]
    counts: dict[str, int] = {}
    decoded: list[dict[str, Any]] = []
    with (out_dir / "index.csv").open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        for path in files:
            rel = safe_relative(path, ROOT)
            try:
                mesh = parse_static_ftm(path)
                if mesh is None:
                    status = "not matched by static-mesh layout"
                    with path.open("rb") as source:
                        source.seek(8)
                        magic = source.read(4).decode("ascii", "replace")
                    writer.writerow({"source": rel, "magic": magic, "bytes": path.stat().st_size, "status": status})
                    counts[status] = counts.get(status, 0) + 1
                    continue
                rel_out = Path(path.relative_to(ROOT / "unpacked" / "kpx"))
                obj_path = out_dir / rel_out.with_suffix(".obj")
                json_path = out_dir / rel_out.with_suffix(".json")
                write_obj(mesh, obj_path)
                meta = {key: value for key, value in mesh.items() if key not in {"positions", "normals", "uvs", "indices"}}
                json_path.parent.mkdir(parents=True, exist_ok=True)
                json_path.write_text(json.dumps(meta, indent=2), encoding="utf-8")
                mesh["obj_path"] = obj_path
                decoded.append(mesh)
                status = "static mesh exported"
                counts[status] = counts.get(status, 0) + 1
                writer.writerow({
                    "source": rel,
                    "magic": mesh["magic"],
                    "name": mesh["name"] or "",
                    "bytes": mesh["file_bytes"],
                    "vertices": mesh["vertex_count"],
                    "triangles": mesh["triangle_count"],
                    "positions": hex(mesh["position_offset"]),
                    "indices": hex(mesh["index_offset"]),
                    "normals": hex(mesh["normal_offset"]),
                    "uvs": hex(mesh["uv_offset"]),
                    "status": status,
                })
            except Exception as exc:
                counts["error"] = counts.get("error", 0) + 1
                writer.writerow({"source": rel, "bytes": path.stat().st_size, "status": f"error: {exc}"})

    write_preview_images(decoded)
    rig_map_count = write_player_rig_catalog()
    return {"ftm_files": len(files), "decoded_static_meshes": len(decoded), "player_rig_maps": rig_map_count, **counts}


def write_player_rig_catalog() -> int:
    """Preserve the common 42-byte map present in most player FTM headers."""
    player_root = ROOT / "unpacked" / "kpx" / "models" / "player"
    out_path = OUT_ROOT / "models" / "player_rig_maps.csv"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    columns = ["source", "magic", "bytes", "raw_u32_at_0x44", "raw_u32_at_0x48", "map_count", "map_bytes_hex", "map_values_signed", "populated_slots"]
    rows = 0
    with out_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        for path in sorted(player_root.rglob("*.ftm"), key=lambda p: p.as_posix().lower()):
            data = path.read_bytes()
            if len(data) < 0x7E or data[8:12] not in (b"FTTM", b"XGSM"):
                continue
            declared_count = struct.unpack_from("<I", data, 0x4C)[0]
            if declared_count != 42 or 0x54 + declared_count > len(data):
                continue
            raw = data[0x54 : 0x54 + declared_count]
            values = [signed8(v) for v in raw]
            populated = [i for i, value in enumerate(values) if value >= 0]
            writer.writerow({
                "source": safe_relative(path, ROOT),
                "magic": data[8:12].decode("ascii"),
                "bytes": len(data),
                "raw_u32_at_0x44": struct.unpack_from("<I", data, 0x44)[0],
                "raw_u32_at_0x48": struct.unpack_from("<I", data, 0x48)[0],
                "map_count": declared_count,
                "map_bytes_hex": raw.hex(),
                "map_values_signed": json.dumps(values, separators=(",", ":")),
                "populated_slots": json.dumps(populated, separators=(",", ":")),
            })
            rows += 1
    return rows


def write_preview_images(meshes: list[dict[str, Any]]) -> None:
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from mpl_toolkits.mplot3d.art3d import Poly3DCollection
    except ImportError:
        return

    preview_dir = ROOT / "previews" / "models"
    preview_dir.mkdir(parents=True, exist_ok=True)
    preferred = ["ball/ball_0.ftm", "trophies/trophy_uc.ftm", "goal/goal_frame.ftm"]
    selected: list[dict[str, Any]] = []
    for suffix in preferred:
        found = next((m for m in meshes if m["source"].replace("\\", "/").endswith(suffix)), None)
        if found:
            selected.append(found)
    if len(selected) < 3:
        selected.extend(m for m in meshes if m not in selected and len(selected) < 3)

    for mesh in selected:
        points = mesh["positions"]
        faces = mesh["indices"]
        tri = [[points[faces[i]], points[faces[i + 1]], points[faces[i + 2]]] for i in range(0, len(faces), 3)]
        fig = plt.figure(figsize=(7, 6), dpi=150)
        ax = fig.add_subplot(111, projection="3d")
        collection = Poly3DCollection(tri, facecolor="#6e8fb2", edgecolor="#223344", linewidth=0.08, alpha=1.0)
        ax.add_collection3d(collection)
        limits = mesh["bounds"]
        centers = [(lo + hi) / 2 for lo, hi in limits]
        spans = [hi - lo for lo, hi in limits]
        radius = max(max(spans) / 2, 1e-3)
        ax.set_xlim(centers[0] - radius, centers[0] + radius)
        ax.set_ylim(centers[1] - radius, centers[1] + radius)
        ax.set_zlim(centers[2] - radius, centers[2] + radius)
        ax.set_box_aspect((1, 1, 1))
        ax.view_init(elev=22, azim=35)
        ax.set_axis_off()
        ax.set_title(f"{mesh['name'] or Path(mesh['source']).name}  |  {mesh['vertex_count']} vertices, {mesh['triangle_count']} triangles")
        out = preview_dir / (Path(mesh["source"]).stem + ".png")
        fig.savefig(out, bbox_inches="tight", pad_inches=0.08)
        plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=["animations", "models", "all"], nargs="?", default="all")
    args = parser.parse_args()
    results: dict[str, Any] = {}
    if args.target in ("animations", "all"):
        results["animations"] = write_animation_catalog()
    if args.target in ("models", "all"):
        results["models"] = write_model_exports()
    print(json.dumps(results, indent=2))


if __name__ == "__main__":
    main()
