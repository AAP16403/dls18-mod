#!/usr/bin/env python3
"""Compare local DLS26 retargets by timing, endpoints, and motion steps.

These are screening measures for choosing clips to inspect, not a proof of
in-game animation quality.
"""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

from decode_assets import decode_sat
from port_dls26_tackle_candidate import DLS26, ROOT, port


CATALOG = DLS26 / "decoded_13.430" / "catalog" / "sat_13430_tackle_candidates.csv"


def pose_difference(a: dict, b: dict, frame: int) -> tuple[float, float]:
    angles = []
    for old, new in zip(a["transform_records_raw"], b["transform_records_raw"]):
        first = [value / 16384.0 for value in old[frame][:4]]
        second = [value / 16384.0 for value in new[frame][:4]]
        dot = abs(sum(first[i] * second[i] for i in range(4)))
        angles.append(math.degrees(2 * math.acos(min(1.0, dot))))
    distances = [math.dist(old, new)
                 for old, new in zip(a["position_records_raw_by_frame"][frame],
                                     b["position_records_raw_by_frame"][frame])]
    return sum(angles) / len(angles), sum(distances) / len(distances)


def motion_steps(body: dict) -> list[float]:
    frames = body["position_records_raw_by_frame"]
    return [sum(math.dist(a, b) for a, b in zip(frames[i], frames[i + 1])) / len(frames[i])
            for i in range(len(frames) - 1)]


def rank(target_id: int, family: str, out_csv: Path, scratch: Path,
         timing: str = "contact", edge_frames: int = 0) -> list[dict]:
    target = ROOT / "unpacked" / "kpx" / "anims" / f"{target_id:04d}.sat"
    target_header, target_body = decode_sat(target)
    if target_body["payload_status"] != "decoded":
        raise ValueError("Target clip does not have inline samples")
    with CATALOG.open(newline="", encoding="utf-8") as stream:
        source_rows = [row for row in csv.DictReader(stream) if row["family"] == family]
    rows = []
    for source_row in source_rows:
        source_id = int(source_row["dls26_id"])
        folder = scratch / f"{source_id:04d}_to_{target_id:04d}"
        try:
            manifest = port(source_id, target_id, folder, timing, edge_frames)
            _, body = decode_sat(folder / f"{target_id:04d}.sat")
            start_angle, start_position = pose_difference(target_body, body, 0)
            end_angle, end_position = pose_difference(target_body, body, -1)
            steps = motion_steps(body)
            source_duration = manifest["source_duration_ticks"]
            target_duration = manifest["target_duration_ticks"]
            source_contact = manifest["source_contact_tick_raw"]
            target_contact = manifest["target_contact_tick_raw"]
            row = {
                "source_id": source_id,
                "source_name": source_row["dls26_name"],
                "target_id": target_id,
                "source_duration_ticks": source_duration,
                "target_duration_ticks": target_duration,
                "duration_delta_dls18_ticks": round(source_duration / 2 - target_duration, 2),
                "pre_contact_speed_ratio": round(source_contact / (2 * target_contact), 3) if target_contact else "",
                "post_contact_speed_ratio": round((source_duration - source_contact) / (2 * (target_duration - target_contact)), 3) if target_duration != target_contact else "",
                "start_rotation_angle_mean_deg": round(start_angle, 2),
                "end_rotation_angle_mean_deg": round(end_angle, 2),
                "start_position_distance_mean_raw": round(start_position, 2),
                "end_position_distance_mean_raw": round(end_position, 2),
                "max_position_step_mean_raw": round(max(steps), 2),
                "mean_position_step_mean_raw": round(sum(steps) / len(steps), 2),
                "sat_sha256": manifest["candidate_sha256"],
                "error": "",
            }
        except (ValueError, OverflowError) as exc:
            row = {"source_id": source_id, "source_name": source_row["dls26_name"],
                   "target_id": target_id, "error": str(exc)}
        rows.append(row)
    columns = ["source_id", "source_name", "target_id", "source_duration_ticks",
               "target_duration_ticks", "duration_delta_dls18_ticks",
               "pre_contact_speed_ratio", "post_contact_speed_ratio",
               "start_rotation_angle_mean_deg", "end_rotation_angle_mean_deg",
               "start_position_distance_mean_raw", "end_position_distance_mean_raw",
               "max_position_step_mean_raw", "mean_position_step_mean_raw", "sat_sha256", "error"]
    out_csv.parent.mkdir(parents=True, exist_ok=True)
    with out_csv.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
    return rows


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target-id", type=int, required=True)
    parser.add_argument("--family", choices=("TACKLE", "TACKLESLIDE", "STUMBLE"), required=True)
    parser.add_argument("--timing", choices=("contact", "clock", "fit"), default="contact")
    parser.add_argument("--edge-frames", type=int, default=0)
    parser.add_argument("--out-csv", type=Path, required=True)
    parser.add_argument("--scratch", type=Path, default=ROOT.parent / "tmp" / "dls26_tackle_port_rank")
    args = parser.parse_args()
    results = rank(args.target_id, args.family, args.out_csv, args.scratch,
                   args.timing, args.edge_frames)
    print(f"wrote {args.out_csv}: {len(results)} candidates, {sum(not row.get('error') for row in results)} usable")
