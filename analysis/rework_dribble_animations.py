#!/usr/bin/env python3
"""Gently smooth rotational and upper-body position kinks in DLS18 dribble clips.

The edit is deliberately narrow: it changes quaternion samples in selected arm,
torso, and upper/lower leg tracks, plus small position corrections in selected
upper-body explicit-position tracks. Loops use wraparound neighbors; one-shot
special moves use interior neighbors and preserve both endpoint poses. SAT
headers, translations, and hand/foot/toe position and rotation tracks remain
unchanged.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "analysis"))
sys.path.insert(0, str(ROOT / "analysis" / ".." / "analysis"))
from decode_assets import BONE_REMAP, decode_sat  # noqa: E402
from build_catalog import parse_kpx  # noqa: E402

DEFAULT_ANIM_PAK = ROOT / "unpacked" / "apk" / "assets" / "data" / "anims" / "anims.pak"
DEFAULT_SAT_DIR = ROOT / "mod" / "source" / "animations"
DEFAULT_PAK_OUT = ROOT / "mod" / "build" / "anims_reworked.pak"
DEFAULT_METRICS = ROOT / "mod" / "build" / "dribble_animation_metrics.json"
CLIPS = (
    2245, 2247, 2249, 2251, 2253, 2255,
    2257, 2259, 2261, 2263,
    2265, 2267, 2269, 2271, 2273, 2275, 2277, 2279, 2281, 2283,
    2285, 2287, 2289,
)
SPECIAL_MOVE_CLIPS = (67, 69, 71, 73, 624, 2329, 2331, 2335)
ALL_CLIPS = CLIPS + SPECIAL_MOVE_CLIPS

# Strength, angular deadband and maximum change (degrees) per model-bone index.
# Feet, toes, hands, pelvis and root are intentionally absent from these sets.
ARM_BONES = {5, 9, 10, 15, 18, 19, 24, 28, 29, 34, 37, 38}
TORSO_BONES = {2, 20, 39, 40, 41}
LEG_BONES = {4, 12, 16, 17, 23, 31, 35, 36}
FILTER_PROFILES = {
    # Low-control takes keep their direct rhythm with light joint cleanup.
    2245: {
        "arm": (0.22, 6.0, 4.5),
        "torso": (0.15, 8.0, 3.0),
        "leg": (0.12, 11.0, 2.5),
    },
    2257: {
        "arm": (0.20, 6.5, 4.0),
        "torso": (0.14, 8.5, 2.8),
        "leg": (0.11, 11.5, 2.2),
    },
    2283: {
        "arm": (0.22, 6.0, 4.5),
        "torso": (0.15, 8.0, 3.0),
        "leg": (0.12, 11.0, 2.5),
    },
    # Mid-control takes receive the smoothest upper-body finish.
    2249: {
        "arm": (0.32, 6.0, 6.0),
        "torso": (0.22, 7.0, 4.5),
        "leg": (0.16, 9.5, 3.5),
    },
    2261: {
        "arm": (0.30, 6.0, 5.5),
        "torso": (0.20, 7.5, 4.0),
        "leg": (0.15, 10.0, 3.2),
    },
    2285: {
        "arm": (0.38, 5.0, 7.0),
        "torso": (0.26, 6.0, 5.0),
        "leg": (0.18, 9.0, 4.0),
    },
    2287: {
        "arm": (0.34, 5.5, 6.5),
        "torso": (0.24, 6.5, 4.5),
        "leg": (0.16, 9.5, 3.5),
    },
    # High-control alternate takes retain the quickest changes and accents.
    2255: {
        "arm": (0.15, 8.0, 3.5),
        "torso": (0.12, 9.0, 3.0),
        "leg": (0.08, 12.0, 2.0),
    },
    2259: {
        "arm": (0.13, 8.5, 3.0),
        "torso": (0.10, 9.5, 2.5),
        "leg": (0.07, 12.0, 1.8),
    },
    2289: {
        "arm": (0.15, 8.0, 3.5),
        "torso": (0.12, 9.0, 3.0),
        "leg": (0.08, 12.0, 2.0),
    },
}

# The decoded locomotion family contains 23 non-female base takes. Apply the
# matching tier profile to the alternate authored takes too, so a candidate
# that wins on direction fit does not fall back to an unedited sample stream.
FILTER_PROFILES.update({
    # Low tier: ordinary forward takes and the full regular 45-degree family.
    2247: FILTER_PROFILES[2245],
    2265: FILTER_PROFILES[2283],
    2267: FILTER_PROFILES[2283],
    2269: FILTER_PROFILES[2283],
    2271: FILTER_PROFILES[2283],
    2273: FILTER_PROFILES[2283],
    2275: FILTER_PROFILES[2283],
    2277: FILTER_PROFILES[2283],
    2279: FILTER_PROFILES[2283],
    2281: FILTER_PROFILES[2283],
    # Mid tier: alternate forward and sprint takes share their family profile.
    2251: FILTER_PROFILES[2249],
    2263: FILTER_PROFILES[2261],
    # High tier: the forward-jog alternate keeps the light accent-preserving pass.
    2253: FILTER_PROFILES[2255],
})

# Special moves are one-shot clips, not cycles. Keep their authored accents
# with a light pass; edit only interior arm, torso, and leg rotations.
SPECIAL_MOVE_PROFILE = {
    "arm": (0.18, 7.0, 3.5),
    "torso": (0.13, 8.5, 2.8),
    "leg": (0.09, 11.0, 2.0),
}
FILTER_PROFILES.update({clip_id: SPECIAL_MOVE_PROFILE for clip_id in SPECIAL_MOVE_CLIPS})

LOW_CONTROL_CLIPS = {
    2245, 2247, 2257, 2265, 2267, 2269, 2271, 2273, 2275, 2277, 2279, 2281, 2283,
}
MID_CONTROL_CLIPS = {2249, 2251, 2261, 2263, 2285, 2287}
HIGH_CONTROL_CLIPS = {2253, 2255, 2259, 2289}

# Strength, kink deadband and maximum per-sample correction in the native raw
# SAT position units. High-control takes get the lightest position pass so
# their sharper movement accents are retained. Hands and feet are excluded.
POSITION_FILTER_PROFILES = {
    "low": {
        "arm": (0.12, 90.0, 45.0),
        "torso": (0.10, 100.0, 35.0),
    },
    "mid": {
        "arm": (0.16, 75.0, 55.0),
        "torso": (0.12, 85.0, 42.0),
    },
    "high": {
        "arm": (0.07, 110.0, 30.0),
        "torso": (0.06, 120.0, 22.0),
    },
}
SPECIAL_POSITION_PROFILE = {
    "arm": (0.08, 100.0, 32.0),
    "torso": (0.07, 110.0, 26.0),
}


def normalize(q: tuple[float, ...] | list[float]) -> tuple[float, float, float, float]:
    length = math.sqrt(sum(x * x for x in q))
    if length < 1e-12:
        return (0.0, 0.0, 0.0, 1.0)
    return tuple(x / length for x in q)  # type: ignore[return-value]


def align(q: tuple[float, ...], reference: tuple[float, ...]) -> tuple[float, ...]:
    return tuple(-x for x in q) if sum(a * b for a, b in zip(q, reference)) < 0 else q


def slerp(a: tuple[float, ...], b: tuple[float, ...], amount: float) -> tuple[float, ...]:
    a = normalize(a)
    b = align(normalize(b), a)
    dot = max(-1.0, min(1.0, sum(x * y for x, y in zip(a, b))))
    if dot > 0.9995:
        return normalize(tuple(x + amount * (y - x) for x, y in zip(a, b)))
    angle = math.acos(dot)
    scale_a = math.sin((1.0 - amount) * angle) / math.sin(angle)
    scale_b = math.sin(amount * angle) / math.sin(angle)
    return normalize(tuple(scale_a * x + scale_b * y for x, y in zip(a, b)))


def rotation_degrees(a: tuple[float, ...], b: tuple[float, ...]) -> float:
    a = normalize(a)
    b = align(normalize(b), a)
    return math.degrees(2.0 * math.acos(max(-1.0, min(1.0, abs(sum(x * y for x, y in zip(a, b)))))))


def local_kink_degrees(samples: list[list[int]], cyclic: bool = True) -> list[float]:
    """Distance from a sample to its neighbors' midpoint, with optional wrapping."""
    values = []
    count = len(samples)
    if count < 3:
        return values
    frames = range(count) if cyclic else range(1, count - 1)
    for frame in frames:
        previous_index = (frame - 1) % count if cyclic else frame - 1
        following_index = (frame + 1) % count if cyclic else frame + 1
        previous = normalize([x / 16384.0 for x in samples[previous_index][:4]])
        current = normalize([x / 16384.0 for x in samples[frame][:4]])
        following = normalize([x / 16384.0 for x in samples[following_index][:4]])
        midpoint = slerp(previous, following, 0.5)
        values.append(rotation_degrees(current, midpoint))
    return values


def local_position_kinks(samples: list[list[int]], cyclic: bool = True) -> list[float]:
    """Raw-unit distance from each XYZ sample to its neighbors' midpoint."""
    values = []
    count = len(samples)
    if count < 3:
        return values
    frames = range(count) if cyclic else range(1, count - 1)
    for frame in frames:
        previous_index = (frame - 1) % count if cyclic else frame - 1
        following_index = (frame + 1) % count if cyclic else frame + 1
        previous = samples[previous_index]
        current = samples[frame]
        following = samples[following_index]
        midpoint = [(a + b) * 0.5 for a, b in zip(previous, following)]
        values.append(math.sqrt(sum((value - middle) ** 2 for value, middle in zip(current, midpoint))))
    return values


def category(bone: int) -> str | None:
    if bone in ARM_BONES:
        return "arm"
    if bone in TORSO_BONES:
        return "torso"
    if bone in LEG_BONES:
        return "leg"
    return None


def edit_clip(source: bytes, clip_id: int) -> tuple[bytes, dict]:
    source_path = ROOT / "unpacked" / "kpx" / "anims" / f"{clip_id:04d}.sat"
    if source_path.read_bytes() != source:
        raise ValueError(f"{clip_id:04d}.sat archive member differs from the canonical extracted source")
    header, body = decode_sat(source_path)
    if body["payload_status"] != "decoded":
        raise ValueError(f"{clip_id:04d}.sat cannot be edited: {body['payload_status']}")
    if len(source) != header["expected_file_bytes"]:
        raise ValueError(f"{clip_id:04d}.sat source bytes do not match its decoded header")

    records = body["transform_records_raw"]
    active_slots = header["active_transform_slots"]
    edits = 0
    track_rows = []
    out = bytearray(source)
    cyclic = clip_id in CLIPS
    # One immutable source sample set is used throughout; filtering is never
    # recursive. Locomotion loops wrap at the seam. One-shot skill moves keep
    # their first and last poses exact and filter only interior samples.
    for track, slot in enumerate(active_slots):
        bone = BONE_REMAP[slot]
        group = category(bone)
        if group is None:
            continue
        strength, deadband, cap_degrees = FILTER_PROFILES[clip_id][group]
        samples = records[track]
        before_kinks = local_kink_degrees(samples, cyclic=cyclic)
        track_edits = 0
        max_edit = 0.0
        frames = range(len(samples)) if cyclic else range(1, len(samples) - 1)
        for frame in frames:
            current_raw = samples[frame]
            previous_index = (frame - 1) % len(samples) if cyclic else frame - 1
            following_index = (frame + 1) % len(samples) if cyclic else frame + 1
            previous = normalize([x / 16384.0 for x in samples[previous_index][:4]])
            current = normalize([x / 16384.0 for x in current_raw[:4]])
            following = normalize([x / 16384.0 for x in samples[following_index][:4]])
            midpoint = slerp(previous, following, 0.5)
            kink = rotation_degrees(current, midpoint)
            if kink <= deadband:
                continue
            correction = min(cap_degrees, strength * (kink - deadband))
            amount = min(1.0, correction / max(kink, 1e-9))
            changed = slerp(current, midpoint, amount)
            packed = [max(-32768, min(32767, round(component * 16384.0))) for component in changed]
            if packed == current_raw[:4]:
                continue
            record_offset = 0x68 + (track * header["frame_count"] + frame) * 14
            struct.pack_into("<4h", out, record_offset, *packed)
            track_edits += 1
            edits += 1
            max_edit = max(max_edit, rotation_degrees(current, [x / 16384.0 for x in packed]))

        new_samples = [list(struct.unpack_from("<7h", out, 0x68 + (track * header["frame_count"] + frame) * 14))
                       for frame in range(header["frame_count"])]
        after_kinks = local_kink_degrees(new_samples, cyclic=cyclic)
        if track_edits:
            track_rows.append({
                "bone": bone,
                "animation_slot": slot,
                "group": group,
                "frames_changed": track_edits,
                "mean_local_kink_before_deg": round(sum(before_kinks) / max(1, len(before_kinks)), 4),
                "mean_local_kink_after_deg": round(sum(after_kinks) / max(1, len(after_kinks)), 4),
                "max_sample_rotation_change_deg": round(max_edit, 4),
            })

    # Explicit-position records are frame-major, while the header map stores
    # bone-slot -> packed-track. Keep that relationship explicit: active map
    # slots are not necessarily in packed-track order.
    position_start = 0x68 + header["frame_count"] * header["transform_track_count"] * 14
    position_count = header["position_track_count"]
    position_map = header["position_map_raw_signed"]
    position_track_to_slot = {}
    for slot, mapped_track in enumerate(position_map):
        if slot >= len(BONE_REMAP) or mapped_track < 0:
            continue
        if mapped_track >= position_count:
            raise ValueError(f"{clip_id:04d} position map points outside its packed tracks: {mapped_track}")
        if mapped_track in position_track_to_slot:
            raise ValueError(f"{clip_id:04d} position map duplicates packed track {mapped_track}")
        position_track_to_slot[mapped_track] = slot
    if set(position_track_to_slot) != set(range(position_count)):
        raise ValueError(f"{clip_id:04d} position map does not account for every packed track")

    if clip_id in SPECIAL_MOVE_CLIPS:
        position_profile_name = "special_move"
        position_profile = SPECIAL_POSITION_PROFILE
    elif clip_id in LOW_CONTROL_CLIPS:
        position_profile_name = "low"
        position_profile = POSITION_FILTER_PROFILES["low"]
    elif clip_id in MID_CONTROL_CLIPS:
        position_profile_name = "mid"
        position_profile = POSITION_FILTER_PROFILES["mid"]
    elif clip_id in HIGH_CONTROL_CLIPS:
        position_profile_name = "high"
        position_profile = POSITION_FILTER_PROFILES["high"]
    else:
        raise ValueError(f"{clip_id:04d} has no control-tier position profile")

    position_edits = 0
    position_track_rows = []
    selected_position_tracks = set()
    position_records = body["position_records_raw_by_frame"]
    for track in range(position_count):
        slot = position_track_to_slot[track]
        bone = BONE_REMAP[slot]
        group = category(bone)
        if group not in ("arm", "torso"):
            continue
        strength, deadband, cap_raw_units = position_profile[group]
        samples = [position_records[frame][track] for frame in range(header["frame_count"])]
        before_kinks = local_position_kinks(samples, cyclic=cyclic)
        track_edits = 0
        max_edit = 0.0
        frames = range(len(samples)) if cyclic else range(1, len(samples) - 1)
        for frame in frames:
            current = samples[frame]
            previous_index = (frame - 1) % len(samples) if cyclic else frame - 1
            following_index = (frame + 1) % len(samples) if cyclic else frame + 1
            previous = samples[previous_index]
            following = samples[following_index]
            midpoint = [(a + b) * 0.5 for a, b in zip(previous, following)]
            kink = math.sqrt(sum((value - middle) ** 2 for value, middle in zip(current, midpoint)))
            if kink <= deadband:
                continue
            correction = min(cap_raw_units, strength * (kink - deadband))
            amount = min(1.0, correction / max(kink, 1e-9))
            packed = [max(-32768, min(32767, round(value + amount * (middle - value))))
                      for value, middle in zip(current, midpoint)]
            if packed == current:
                continue
            record_offset = position_start + (frame * position_count + track) * 6
            struct.pack_into("<3h", out, record_offset, *packed)
            track_edits += 1
            position_edits += 1
            max_edit = max(max_edit, math.sqrt(sum((after - before) ** 2
                                                   for before, after in zip(current, packed))))

        new_samples = [list(struct.unpack_from("<3h", out,
                                               position_start + (frame * position_count + track) * 6))
                        for frame in range(header["frame_count"])]
        after_kinks = local_position_kinks(new_samples, cyclic=cyclic)
        if track_edits:
            selected_position_tracks.add(track)
            position_track_rows.append({
                "bone": bone,
                "position_map_slot": slot,
                "packed_track": track,
                "group": group,
                "frames_changed": track_edits,
                "mean_local_kink_before_raw_units": round(sum(before_kinks) / max(1, len(before_kinks)), 4),
                "mean_local_kink_after_raw_units": round(sum(after_kinks) / max(1, len(after_kinks)), 4),
                "max_sample_position_change_raw_units": round(max_edit, 4),
            })

    # Exact binary invariants: count/header and all translations are preserved;
    # only the selected upper-body position tracks and rotation tracks change.
    original_records = body["transform_records_raw"]
    for track, samples in enumerate(original_records):
        for frame in range(header["frame_count"]):
            after = struct.unpack_from("<7h", out, 0x68 + (track * header["frame_count"] + frame) * 14)
            before = samples[frame]
            if after[4:] != tuple(before[4:]):
                raise AssertionError(f"{clip_id:04d} track {track} translation changed")
            if category(BONE_REMAP[active_slots[track]]) is None and after != tuple(before):
                raise AssertionError(f"{clip_id:04d} unselected track changed: {track}")
            if not cyclic and frame in (0, header["frame_count"] - 1) and after != tuple(before):
                raise AssertionError(f"{clip_id:04d} one-shot endpoint changed: track {track}, frame {frame}")
    for frame in range(header["frame_count"]):
        for track in range(position_count):
            before = tuple(position_records[frame][track])
            after = struct.unpack_from("<3h", out, position_start + (frame * position_count + track) * 6)
            if track not in selected_position_tracks and after != before:
                raise AssertionError(f"{clip_id:04d} unselected position track changed: {track}")
            if not cyclic and frame in (0, header["frame_count"] - 1) and after != before:
                raise AssertionError(f"{clip_id:04d} one-shot position endpoint changed: track {track}, frame {frame}")

    # Validate the generated bytes with the same native-format decoder without
    # ever replacing the source file on disk.
    output_header, output_body = decode_sat_bytes(bytes(out))
    if output_body["payload_status"] != "decoded" or len(out) != len(source):
        raise AssertionError(f"{clip_id:04d} edited SAT no longer decodes")
    if output_header["frame_count"] != header["frame_count"] or output_header["transform_track_count"] != header["transform_track_count"]:
        raise AssertionError(f"{clip_id:04d} generated SAT counts changed")

    report = {
        "clip_id": clip_id,
        "name": next((row["animation_name"] for row in csv.DictReader(
            (ROOT / "catalog" / "animation_names.csv").open(encoding="utf-8"))
            if int(row["animation_id"]) == clip_id), ""),
        "frame_count": header["frame_count"],
        "sample_interval_ticks": header["sample_interval_ticks"],
        "sample_interval_seconds": header["sample_interval_seconds"],
        "transform_tracks": header["transform_track_count"],
        "position_tracks": header["position_track_count"],
        "smoothing_profile": FILTER_PROFILES[clip_id],
        "position_smoothing_profile": position_profile,
        "control_tier": position_profile_name,
        "temporal_mode": "cyclic_loop" if cyclic else "one_shot_endpoint_preserving",
        "rotational_samples_changed": edits,
        "track_edits": track_rows,
        "position_samples_changed": position_edits,
        "position_track_edits": position_track_rows,
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "edited_sha256": hashlib.sha256(out).hexdigest(),
        "invariants": ["unchanged header and frame count", "unchanged translations",
                       "unselected rotations and position tracks unchanged",
                       "pelvis/leg/hand/foot/toe positions and hand/foot/toe rotations unchanged",
                       "loop-edge rotations filtered with wraparound neighbors" if cyclic
                       else "first and last rotation and position samples unchanged; only interior samples filtered"],
    }
    return bytes(out), report


def decode_sat_bytes(data: bytes) -> tuple[dict, dict]:
    """Use the production SAT decoder on a temporary in-memory path safely."""
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".sat", delete=False) as stream:
        stream.write(data)
        path = Path(stream.name)
    try:
        return decode_sat(path)
    finally:
        path.unlink(missing_ok=True)


def repack_kpx(original: bytes, replacements: dict[int, bytes]) -> tuple[bytes, dict]:
    marker, entries = parse_kpx(original)
    file_count = struct.unpack_from("<I", original, 8)[0]
    folder_count = struct.unpack_from("<I", original, 4)[0]
    names_size = struct.unpack_from("<I", original, 12)[0]
    table_end = 16 + folder_count * 20 + file_count * 24 + names_size
    rebuilt = bytearray(original)
    changed = {}
    for entry in entries:
        index = entry["file_index"]
        if index not in replacements:
            continue
        replacement = replacements[index]
        if entry["compression"] != "zlib":
            raise ValueError(f"Expected zlib-compressed KPX member at {entry['path']}")
        compressed = zlib.compress(replacement, level=9)
        offset = len(rebuilt)
        rebuilt.extend(compressed)
        record_offset = 16 + folder_count * 20 + index * 24
        name_offset, _old_size, _old_offset, flag, metadata, _old_packed_size = struct.unpack_from(
            "<6I", rebuilt, record_offset)
        struct.pack_into("<6I", rebuilt, record_offset, name_offset, len(replacement), offset,
                         flag, metadata, len(compressed))
        changed[entry["path"]] = {
            "file_index": index,
            "decoded_size_before": entry["size"],
            "decoded_size_after": len(replacement),
            "stored_size_before": entry["stored_size"],
            "stored_size_after": len(compressed),
            "offset_after": offset,
        }
    # Parse the new KPX and confirm every decoded member equals the source or
    # its explicit replacement. The untouched KPX prefix preserves folders,
    # names, member order and metadata.
    new_marker, after_entries = parse_kpx(bytes(rebuilt))
    if new_marker != marker or len(after_entries) != len(entries):
        raise AssertionError("KPX structure changed unexpectedly")
    by_path_before = {entry["path"]: entry["data"] for entry in entries}
    by_path_after = {entry["path"]: entry["data"] for entry in after_entries}
    for entry in entries:
        expected = replacements.get(entry["file_index"], by_path_before[entry["path"]])
        if by_path_after[entry["path"]] != expected:
            raise AssertionError(f"KPX read-back differs for {entry['path']}")
    return bytes(rebuilt), {"marker": marker.decode("ascii", "replace"), "members": len(entries),
                            "table_bytes": table_end, "changed_members": changed}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-pak", type=Path, default=DEFAULT_ANIM_PAK)
    parser.add_argument("--sat-dir", type=Path, default=DEFAULT_SAT_DIR)
    parser.add_argument("--pak-out", type=Path, default=DEFAULT_PAK_OUT)
    parser.add_argument("--metrics-out", type=Path, default=DEFAULT_METRICS)
    args = parser.parse_args()

    original_pak = args.source_pak.read_bytes()
    _, entries = parse_kpx(original_pak)
    replacements = {}
    clip_reports = []
    args.sat_dir.mkdir(parents=True, exist_ok=True)
    entry_by_path = {entry["path"]: entry for entry in entries}
    for clip_id in ALL_CLIPS:
        path = f"{clip_id:04d}.sat"
        if path not in entry_by_path:
            raise ValueError(f"{path} is absent from the source animation KPX")
        edited, report = edit_clip(entry_by_path[path]["data"], clip_id)
        output_sat = args.sat_dir / path
        output_sat.write_bytes(edited)
        replacements[entry_by_path[path]["file_index"]] = edited
        try:
            report["edited_file"] = str(output_sat.resolve().relative_to(ROOT)).replace("\\", "/")
        except ValueError:
            report["edited_file"] = str(output_sat.resolve())
        clip_reports.append(report)

    packed, pak_report = repack_kpx(original_pak, replacements)
    args.pak_out.parent.mkdir(parents=True, exist_ok=True)
    args.pak_out.write_bytes(packed)
    report = {
        "approach": "adaptive quaternion smoothing: cyclic for locomotion loops, endpoint-preserving for one-shot skill moves",
        "clip_reports": clip_reports,
        "pak_report": pak_report,
        "source_pak_sha256": hashlib.sha256(original_pak).hexdigest(),
        "edited_pak_sha256": hashlib.sha256(packed).hexdigest(),
        "edited_pak_bytes": len(packed),
    }
    args.metrics_out.parent.mkdir(parents=True, exist_ok=True)
    args.metrics_out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"clips": [{"id": r["clip_id"], "rotation_samples_changed": r["rotational_samples_changed"],
                                  "position_samples_changed": r["position_samples_changed"],
                                  "sha256": r["edited_sha256"]} for r in clip_reports],
                      "pak": str(args.pak_out), "pak_sha256": report["edited_pak_sha256"],
                      "members": pak_report["members"]}, indent=2))


if __name__ == "__main__":
    main()
