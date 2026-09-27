#!/usr/bin/env python3
"""Build a local DLS26-to-DLS18 tackle SAT candidate.

This is an experimental direct track-order retarget. It keeps the DLS18
target header and sample timing so the replacement fits its existing action.
It does not alter an APK or package.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import sys
from bisect import bisect_right
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DLS26 = ROOT.parent / "dls26"
sys.path.insert(0, str(DLS26))
sys.path.insert(0, str(ROOT / "analysis"))
from decode_sat_13430 import add_metadata, decode  # noqa: E402
from decode_assets import decode_sat  # noqa: E402


def interpolate(values: list[list[int]], times: list[int], tick: float) -> list[float]:
    if tick <= times[0]:
        return [float(value) for value in values[0]]
    if tick >= times[-1]:
        return [float(value) for value in values[-1]]
    right = bisect_right(times, tick)
    left = right - 1
    fraction = (tick - times[left]) / (times[right] - times[left])
    return [values[left][i] * (1.0 - fraction) + values[right][i] * fraction
            for i in range(len(values[left]))]


def interpolate_quaternion(values: list[list[int]], times: list[int], tick: float) -> list[int]:
    if tick <= times[0]:
        return values[0][3:7]
    if tick >= times[-1]:
        return values[-1][3:7]
    right = bisect_right(times, tick)
    left = right - 1
    fraction = (tick - times[left]) / (times[right] - times[left])
    a = [value / 16384.0 for value in values[left][3:7]]
    b = [value / 16384.0 for value in values[right][3:7]]
    if sum(a[i] * b[i] for i in range(4)) < 0:
        b = [-value for value in b]
    blend = [a[i] * (1.0 - fraction) + b[i] * fraction for i in range(4)]
    length = math.sqrt(sum(value * value for value in blend))
    return [max(-32768, min(32767, round(value / length * 16384.0))) for value in blend]


def contact_marker(path: Path, animation_id: int, record_bytes: int) -> int:
    data = path.read_bytes()
    offset = 4 + animation_id * record_bytes + 0x30
    if offset + 2 > len(data):
        raise ValueError(f"Animation DB has no record {animation_id}")
    return struct.unpack_from("<h", data, offset)[0]


def blend_quaternion(baseline: list[int], source: list[int], source_weight: float) -> list[int]:
    if source_weight <= 0:
        return baseline[:]
    if source_weight >= 1:
        return source[:]
    a = [value / 16384.0 for value in baseline]
    b = [value / 16384.0 for value in source]
    if sum(a[i] * b[i] for i in range(4)) < 0:
        b = [-value for value in b]
    blend = [a[i] * (1.0 - source_weight) + b[i] * source_weight for i in range(4)]
    length = math.sqrt(sum(value * value for value in blend))
    return [max(-32768, min(32767, round(value / length * 16384.0))) for value in blend]


def source_tick_at_target_tick(target_tick: float, target_duration: int,
                               source_duration: int, target_contact: int,
                               source_contact: int, timing: str) -> float:
    if timing == "fit":
        return source_duration * target_tick / target_duration
    if timing == "clock":
        return min(source_duration, target_tick * 2.0)
    if timing != "contact" or not (0 < target_contact < target_duration and 0 < source_contact < source_duration):
        raise ValueError("Contact timing needs valid contact markers in both animation databases")
    if target_tick <= target_contact:
        return source_contact * target_tick / target_contact
    return source_contact + (source_duration - source_contact) * (target_tick - target_contact) / (target_duration - target_contact)


def port(source_id: int, target_id: int, out_dir: Path, timing: str = "contact",
         edge_frames: int = 0, align_positions: bool = False) -> dict:
    source = decode(source_id)
    add_metadata(source)
    target_path = ROOT / "unpacked" / "kpx" / "anims" / f"{target_id:04d}.sat"
    original = target_path.read_bytes()
    target_header, target_body = decode_sat(target_path)
    if target_body["payload_status"] != "decoded":
        raise ValueError("Target DLS18 clip has no inline samples")
    if target_header["transform_track_count"] != source["rotation_track_count"] or target_header["position_track_count"] != source["position_track_count"]:
        raise ValueError("Track counts differ; direct track-order retarget is invalid")
    times = source["sample_times_raw"]
    if len(times) != source["sample_count"] or times[0] != 0 or times[-1] != source["duration_raw"]:
        raise ValueError("DLS26 timing record does not match the SAT header")
    frames = target_header["frame_count"]
    if edge_frames < 0 or edge_frames * 2 >= frames:
        raise ValueError("Edge blend needs fewer than half of the target frames")
    target_duration = (frames - 1) * target_header["sample_interval_ticks"]
    if target_duration <= 0:
        raise ValueError("Target duration is not positive")
    source_contact = contact_marker(DLS26 / "decoded_13.430" / "unpacked" / "kpx" / "anims" / "animdb.adb", source_id, 104)
    target_contact = contact_marker(ROOT / "unpacked" / "kpx" / "anims" / "animdb.adb", target_id, 100)
    source_ticks = [source_tick_at_target_tick(frame * target_header["sample_interval_ticks"],
                                               target_duration, source["duration_raw"],
                                               target_contact, source_contact, timing)
                    for frame in range(frames)]
    source_weights = [1.0 if edge_frames == 0 else min(1.0, frame / edge_frames,
                                                       (frames - 1 - frame) / edge_frames)
                      for frame in range(frames)]
    data = bytearray(original[:104])
    for track_id, track in enumerate(source["rotation_tracks"]):
        for frame, tick in enumerate(source_ticks):
            translation = interpolate(track, times, tick)[:3]
            quaternion = interpolate_quaternion(track, times, tick)
            baseline = target_body["transform_records_raw"][track_id][frame]
            weight = source_weights[frame]
            record = blend_quaternion(baseline[:4], quaternion, weight)
            record += [round(baseline[4 + channel] * (1.0 - weight) + translation[channel] * weight)
                       for channel in range(3)]
            data.extend(struct.pack("<7h", *record))
    for frame, tick in enumerate(source_ticks):
        for track_id, track in enumerate(source["position_tracks"]):
            position = interpolate(track, times, tick)
            baseline = target_body["position_records_raw_by_frame"][frame][track_id]
            if align_positions:
                source_first = interpolate(track, times, source_ticks[0])
                source_last = interpolate(track, times, source_ticks[-1])
                target_first = target_body["position_records_raw_by_frame"][0][track_id]
                target_last = target_body["position_records_raw_by_frame"][-1][track_id]
                progress = frame / max(1, frames - 1)
                position = [position[channel]
                            + (target_first[channel] - source_first[channel]) * (1.0 - progress)
                            + (target_last[channel] - source_last[channel]) * progress
                            for channel in range(3)]
            weight = source_weights[frame]
            data.extend(struct.pack("<3h", *(round(baseline[channel] * (1.0 - weight) + position[channel] * weight)
                                              for channel in range(3))))
    if len(data) != len(original):
        raise AssertionError("Candidate SAT size differs from target")
    out_dir.mkdir(parents=True, exist_ok=True)
    target = out_dir / f"{target_id:04d}.sat"
    target.write_bytes(data)
    decoded_header, decoded_body = decode_sat(target)
    if decoded_body["payload_status"] != "decoded" or decoded_header["frame_count"] != frames:
        raise AssertionError("Candidate cannot be read by the DLS18 SAT decoder")
    report = {
        "source_dls26_id": source_id,
        "source_dls26_name": source.get("animation_name", ""),
        "target_dls18_id": target_id,
        "target_dls18_sat": str(target_path),
        "candidate_sat": str(target),
        "source_duration_ticks": source["duration_raw"],
        "target_duration_ticks": target_duration,
        "source_contact_tick_raw": source_contact,
        "target_contact_tick_raw": target_contact,
        "timing": timing,
        "edge_blend_frames": edge_frames,
        "align_positions": align_positions,
        "source_weights": source_weights,
        "source_ticks_used": [round(tick, 4) for tick in source_ticks],
        "source_sample_count": source["sample_count"],
        "target_frame_count": frames,
        "track_order_assumption": "33 transform and 19 position tracks map by packed index; visual rig check required",
        "original_sha256": hashlib.sha256(original).hexdigest(),
        "candidate_sha256": hashlib.sha256(data).hexdigest(),
    }
    (out_dir / "port_manifest.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-id", type=int, default=2812)
    parser.add_argument("--target-id", type=int, default=331)
    parser.add_argument("--timing", choices=("contact", "clock", "fit"), default="contact")
    parser.add_argument("--edge-frames", type=int, default=0)
    parser.add_argument("--align-positions", action="store_true",
                        help="linearly align DLS26 position tracks to DLS18 endpoints")
    parser.add_argument("--out-dir", type=Path, default=ROOT / "mod" / "source" / "dls26_tackle_candidate_v1")
    args = parser.parse_args()
    print(json.dumps(port(args.source_id, args.target_id, args.out_dir, args.timing,
                          args.edge_frames, args.align_positions), indent=2))
