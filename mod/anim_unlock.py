"""Unlock animations that ship with DLS18 but are never used (data only; see anim_limits/ANIMATION_LIMITS.md).

1. Skill move: clips 69/70 (stand-to-45 deg stepover and its mirror) are DEEK style 3, a style the
   picker never requests (it asks for 0, 1 or 2). They become style 2 (animdb.adb record +0x0e).
2. Goal celebrations: 25 solo celebration clips that no NIS list references are added to the stock
   lists in nis.pak goal/common/animlist.xml (lists hold up to 255 names; unknown names are skipped
   by the game with a log line, never a crash).

The paks are read from --base-apk (so earlier animation edits are kept) and written to --out-dir.
usage: python anim_unlock.py --base-apk build/DLS18_career_market_v7.apk --out-dir build/unlock
"""
import argparse
import json
import struct
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "analysis"))
from build_catalog import parse_kpx                  # noqa: E402
from rework_dribble_animations import repack_kpx     # noqa: E402

ANIMS = "assets/data/anims/anims.pak"
NIS = "assets/data/nis.pak"
ADB_RECORD = 100
STYLE_OFFSET = 0x0E
UNLOCK_DEEK = {69: 2, 70: 2}          # clip id -> new style (was 3)

STAND = [
    "ANM_NIS_EXE721__OWEN__RUB_HANDS_TOGETHER_GOAL_CELEB",
    "ANM_NIS_EXE723_TAKE_A_BOW_GOAL_CELEB",
    "ANM_NIS_ET234_XF0030_C_CELEBSTAND",
    "ANM_NIS_ET234_XF0030_D_CELEBSTAND",
    "ANM_NIS_ET234_XF0030_E_CELEBSTAND",
    "ANM_NIS_ET235_XF0030_B_CELEBSTAND",
    "ANM_NIS_ET235_XF0030_C_CELEBSTAND",
    "ANM_NIS_ET812_EX2098_CELEB_VIOLIN",
    "ANM_NIS_ET813_EX2099_CELEB_ARMS_WIDE",
    "ANM_NIS_ET818_EX2103_CELEB_SALUTE",
    "ANM_NIS_FT1032_CELEBRATION___STERLING_CELEBRATION___COVERING_FACE_2_MAX",
    "ANM_NIS_FT1032_CELEBRATION___STERLING_CELEBRATION___COVERING_FACE_JOSH",
    "ANM_NIS_FT1343_CELEBRATION_GENERIC_STANDING_ANIMATIONS_2_COLIN",
    "ANM_NIS_FT1343_CELEBRATION_GENERIC_STANDING_ANIMATIONS_3_NOEL",
    "ANM_NIS_FT1343_CELEBRATION_GENERIC_STANDING_ANIMATIONS_4_NOEL",
    "ANM_NIS_FT1343_CELEBRATION_GENERIC_STANDING_ANIMATIONS_COLIN",
]
DANCE = [
    "ANM_NIS_FT1031_CELEBRATION___GENERIC_DANCE_CELEBRATIONS_2_JOSH",
    "ANM_NIS_FT1031_CELEBRATION___GENERIC_DANCE_CELEBRATIONS_3_JOSH",
    "ANM_NIS_FT1031_CELEBRATION___GENERIC_DANCE_CELEBRATIONS_JOSH",
    "ANM_NIS_FT1043_CELEBRATION___GENERIC_DANCE_CELEBRATIONS_1_JOSH",
    "ANM_NIS_FT1043_CELEBRATION___GENERIC_DANCE_CELEBRATIONS_1_MAX",
]
RUNNING = [
    "ANM_NIS_FT1340_CELEBRATION_GENERIC_EXCITED_MOVING_ANIMATIONS_2_COLIN",
    "ANM_NIS_FT1340_CELEBRATION_GENERIC_EXCITED_MOVING_ANIMATIONS_3_NOEL",
    "ANM_NIS_FT1340_CELEBRATION_GENERIC_EXCITED_MOVING_ANIMATIONS_4_COLIN",
    "ANM_NIS_FT1340_CELEBRATION_GENERIC_EXCITED_MOVING_ANIMATIONS_5_COLIN",
    "ANM_NIS_FT1340_CELEBRATION_GENERIC_EXCITED_MOVING_ANIMATIONS_6_COLIN",
]
LIST_ADDS = {
    "GOALCELEB_STAND_ANIM_LIST": STAND + DANCE,
    "GOALCELEB_STANDLONG_ANIM_LIST": DANCE,
    "GOALCELEB_RUNNING_UNIQUE": RUNNING,
}
LIST_MAX = 255


def member(entries, suffix):
    found = [e for e in entries if e["path"].endswith(suffix)]
    if len(found) != 1:
        raise SystemExit(f"expected exactly one {suffix} member, found {len(found)}")
    return found[0]


def unlock_adb(pak):
    _, entries = parse_kpx(pak)
    e = member(entries, "animdb.adb")
    adb = bytearray(e["data"])
    count = struct.unpack_from("<I", adb, 0)[0]
    changes = {}
    for clip, style in UNLOCK_DEEK.items():
        rec = 4 + ADB_RECORD * clip
        if clip >= count or adb[rec] != 19:
            raise SystemExit(f"clip {clip} is not a DEEK (category 19) record")
        before = adb[rec + STYLE_OFFSET]
        if before not in (3, style):
            raise SystemExit(f"clip {clip}: unexpected style {before}")
        adb[rec + STYLE_OFFSET] = style
        changes[clip] = (before, style)
    new, report = repack_kpx(pak, {e["file_index"]: bytes(adb)})
    return new, changes


def unlock_nis(pak):
    _, entries = parse_kpx(pak)
    e = member(entries, "goal/common/animlist.xml")
    text = e["data"].decode("utf-8-sig")
    added = {}
    for name, anims in LIST_ADDS.items():
        tag = f"<Name>{name}</Name>"
        start = text.index(tag)
        end = text.index("</AnimList>", start)
        block = text[start:end]
        present = set(a.split("</Anim>")[0] for a in block.split("<Anim>")[1:])
        new = [a for a in anims if a not in present]
        if len(present) + len(new) > LIST_MAX:
            raise SystemExit(f"{name}: more than {LIST_MAX} anims")
        insert = "".join(f"    <Anim>{a}</Anim>\n" for a in new)
        line_start = text.rindex("\n", 0, end) + 1       # the "  </AnimList>" line
        text = text[:line_start] + insert + text[line_start:]
        added[name] = new
    data = ("﻿" + text).encode("utf-8") if e["data"].startswith(b"\xef\xbb\xbf") else text.encode("utf-8")
    new, report = repack_kpx(pak, {e["file_index"]: data})
    return new, added


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-apk", default=str(HERE / "build/DLS18_career_market_v7.apk"))
    ap.add_argument("--out-dir", default=str(HERE / "build/unlock"))
    a = ap.parse_args()
    z = zipfile.ZipFile(a.base_apk)
    out = Path(a.out_dir)
    out.mkdir(parents=True, exist_ok=True)
    anims, adb_changes = unlock_adb(z.read(ANIMS))
    nis, list_adds = unlock_nis(z.read(NIS))
    (out / "anims.pak").write_bytes(anims)
    (out / "nis.pak").write_bytes(nis)
    report = dict(deek_styles={str(k): v for k, v in adb_changes.items()},
                  celebrations_added={k: len(v) for k, v in list_adds.items()}, names=list_adds)
    (out / "unlock_report.json").write_text(json.dumps(report, indent=1))
    print(f"anims.pak: DEEK styles {adb_changes}")
    print(f"nis.pak: added {sum(len(v) for v in list_adds.values())} list entries "
          + ", ".join(f"{k} +{len(v)}" for k, v in list_adds.items()))
    print(f"wrote {out / 'anims.pak'} and {out / 'nis.pak'}")


if __name__ == "__main__":
    main()
