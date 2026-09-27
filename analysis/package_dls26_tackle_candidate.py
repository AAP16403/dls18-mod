#!/usr/bin/env python3
"""Add experimental DLS26 SAT candidates to a DLS18 animation PAK."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from build_catalog import parse_kpx
from rework_dribble_animations import repack_kpx


ROOT = Path(__file__).resolve().parents[1]


def package(source_pak: Path, candidate_sats: list[Path], output_pak: Path) -> dict:
    source = source_pak.read_bytes()
    _, entries = parse_kpx(source)
    by_path = {entry["path"]: entry for entry in entries}
    replacements = {}
    candidates = []
    for candidate_sat in candidate_sats:
        candidate = candidate_sat.read_bytes()
        match = by_path.get(candidate_sat.name)
        if match is None:
            raise ValueError(f"{candidate_sat.name} is absent from {source_pak}")
        if len(candidate) != match["size"]:
            raise ValueError(f"{candidate_sat.name} differs in size from the existing package member")
        if match["file_index"] in replacements:
            raise ValueError(f"Duplicate replacement for {candidate_sat.name}")
        replacements[match["file_index"]] = candidate
        candidates.append({"path": str(candidate_sat), "member": match["path"],
                           "sha256": hashlib.sha256(candidate).hexdigest()})
    packed, details = repack_kpx(source, replacements)
    output_pak.parent.mkdir(parents=True, exist_ok=True)
    output_pak.write_bytes(packed)
    report = {
        "source_pak": str(source_pak),
        "candidate_sats": candidates,
        "output_pak": str(output_pak),
        "source_sha256": hashlib.sha256(source).hexdigest(),
        "output_sha256": hashlib.sha256(packed).hexdigest(),
        "changed_members": details["changed_members"],
    }
    output_pak.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-pak", type=Path, default=ROOT / "mod" / "build" / "anims_career_market_v30.pak")
    parser.add_argument("--candidate-sat", type=Path, nargs="+", default=[ROOT / "mod" / "source" / "dls26_tackle_candidate_2306" / "0331.sat"])
    parser.add_argument("--output-pak", type=Path, default=ROOT / "mod" / "build" / "anims_career_market_v30_dls26_tackle_2306_v2.pak")
    args = parser.parse_args()
    print(json.dumps(package(args.source_pak, args.candidate_sat, args.output_pak), indent=2))
