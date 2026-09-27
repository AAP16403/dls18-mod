#!/usr/bin/env python3
"""Numeric validation of the DLS26-ported clips against their DLS18 stock references.

For every port listed in tune_dls26_ports.PORTS it evaluates three versions:
  stock  - the v30 slot (replacements) or the template slot the record was copied from (appends)
  v15    - anims_career_market_v30_dls26_tackle_stumble_append_v15_turns.pak
  v16    - anims_career_market_v30_dls26_tackle_stumble_append_v16_tuned.pak

Measures (engine conventions derived in DLS26_13430_animation_port.md, "v16 tuning"):
  ap_tick      first action point, 30 Hz ticks (TAnimData time = (t << 17) / record duration)
  ap_game      CAnimManager::GetActionTime result at the default speed (must be > 0)
  contact_tick tick where a foot or toe bone passes closest to the action-point ball position,
               using the SAT transforms (FK) plus the record's root and heading curves
  ball_gap     foot/toe-to-ball distance at the action-point tick (root units, 16 = 1 cm);
               stock clips sit at 150-450 because the ball centre is ahead of the ankle bone
  travel       root displacement at the end of the clip (record curve), root units
  slide        mean horizontal speed of a planted foot (within 2 cm of its lowest height),
               root units per 60 Hz tick; for clips without an XZ curve (loops, sidesteps)
               'sweep' is the stance-foot speed in the pose, which the game must match with
               its own movement speed
Usage: python validate_dls26_ports.py [--csv OUT] [--md OUT]
"""

from __future__ import annotations

import argparse
import csv
import math
import struct
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from anim_tune_lib import (ROOT, SLOT_L_FOOT, SLOT_L_TOE, SLOT_R_FOOT, SLOT_R_TOE, V15_PAK, V16_PAK,  # noqa: E402
                           V30_PAK, AnimDB, d26_source, fk_positions, interp_curve, load_pak, sat_frames)
from tune_dls26_ports import PORTS  # noqa: E402

EFFECTORS = (SLOT_R_FOOT, SLOT_R_TOE, SLOT_L_FOOT, SLOT_L_TOE)
FEET = ((SLOT_R_FOOT, SLOT_R_TOE), (SLOT_L_FOOT, SLOT_L_TOE))


class Version:
    def __init__(self, name, path):
        self.name = name
        _, self.pak = load_pak(path)
        self.db = AnimDB(self.pak["animdb.adb"]["data"])
        self._fk = {}

    def clip(self, i):
        if i not in self._fk:
            data = self.pak[f"{i:04d}.sat"]["data"]
            s = sat_frames(data)
            s["hdr0"] = struct.unpack_from("<h", data, 0)[0]
            s["fk"] = fk_positions(s["tmap"], s["trans"])
            s["fine"] = fine_fk(s)
            self._fk[i] = s
        return self._fk[i]


SUB = 4  # FK sub-samples per SAT frame (transforms interpolated like the engine sampler)


def fine_fk(s):
    from tune_dls26_ports import sample_dls18_frame
    n = (s["frames"] - 1) * SUB + 1
    trans = [[None] * n for _ in range(len(s["trans"]))]
    for k in range(len(s["trans"])):
        for j in range(n):
            q, t = sample_dls18_frame(s["trans"], k, j / SUB)
            trans[k][j] = q + t
    return np.array(fk_positions(s["tmap"], trans))


def pose_fine(s, frac, slot=None):
    fpos = max(0.0, min(s["frames"] - 1.0, frac * s["hdr0"] / s["interval"])) * SUB
    k = min(len(s["fine"]) - 2, int(fpos))
    f = fpos - k
    a = s["fine"][k] * (1 - f) + s["fine"][k + 1] * f
    return a if slot is None else a[slot]


def heading_at(db, i, tick):
    rot = db.rot_curve(i)
    if not rot:
        return 0.0
    step = db.s16(i, 0xA)
    k = max(0, min(len(rot) - 2, int(tick // step)))
    f = (tick - k * step) / step
    return (rot[k] * (1 - f) + rot[k + 1] * f) * 2 * math.pi / 2048


def world(v, i, slot, frac):
    """Bone position in action-point coordinates (x: -forward, z: left, y: height; root units)."""
    s = v.clip(i)
    p = pose_fine(s, frac, slot)
    dur = v.db.duration(i)
    tick = frac * dur
    th = heading_at(v.db, i, tick)
    c, sn = math.cos(th), math.sin(th)
    px, py = c * p[0] - sn * p[1], sn * p[0] + c * p[1]
    if v.db.u16(i, 0xC) & 0x1000 and v.db.root_curve(i):
        rx, rz = v.db.root_at_tick(i, tick)
    else:
        rx = rz = 0.0
    return np.array([px / 2 - rx, p[2] / 2, py / 2 + rz])


def metrics(v, i):
    db = v.db
    s = v.clip(i)
    dur = db.duration(i)
    aps = db.action_points(i)
    out = {"dur": dur, "frames": s["frames"], "interval": s["interval"], "n_ap": len(aps)}
    out["ap_ticks"] = " ".join(str(a[0]) for a in aps)
    out["ap_game"] = " ".join(str(db.native_action_ticks(i, k)) for k in range(len(aps)))
    out["ap_ok"] = all(db.native_action_ticks(i, k) > 0 and (a[0] << 17) // dur < 0x10000
                       for k, a in enumerate(aps))
    if aps:
        t0, x, y, z = aps[0]
        ball = np.array([x, y, z], float)
        grid = np.linspace(0, 1, 8 * s["frames"] + 1)
        best = None
        for slot in EFFECTORS:
            d = [np.linalg.norm(world(v, i, slot, g) - ball) for g in grid]
            j = int(np.argmin(d))
            if best is None or d[j] < best[0]:
                best = (d[j], grid[j], slot)
        out["contact_tick"] = round(best[1] * dur / 2, 1)
        out["contact_gap"] = round(best[0])
        fa = 2 * t0 / dur
        out["ball_gap"] = round(min(np.linalg.norm(world(v, i, slot, fa) - ball) for slot in EFFECTORS))
        out["contact_err"] = round(out["contact_tick"] - t0, 1)
    if db.u16(i, 0xC) & 0x1000 and db.root_curve(i):
        e = db.root_at_tick(i, dur)
        out["travel"] = (round(e[0]), round(e[1]))
        out["travel_len"] = round(math.hypot(*e))
        out["travel_deg"] = round(math.degrees(math.atan2(e[1], e[0])))
    else:
        out["travel"] = (0, 0)
        out["travel_len"] = 0
        out["travel_deg"] = 0
    rot = db.rot_curve(i)
    out["heading_end_deg"] = round(heading_at(db, i, dur) * 180 / math.pi) if rot else 0
    # foot slide (planted foot) in world, and stance sweep in the pose, on the sub-frame grid
    slides, sweeps = [], []
    fracs = np.linspace(0.0, 1.0, int(dur) + 1)  # one sample per 60 Hz tick
    poses = [pose_fine(s, fr) for fr in fracs]
    for foot, toe in FEET:
        zs = [min(p[foot][2], p[toe][2]) for p in poses]
        zmin = min(zs)
        prev = None
        for j, fr in enumerate(fracs):
            if zs[j] < zmin + 64:
                w1 = world(v, i, foot, fr)
                if prev is not None and prev[0] == j - 1:
                    slides.append(math.hypot(w1[0] - prev[1][0], w1[2] - prev[1][2]))
                    sweeps.append(math.hypot(*(poses[j][foot][:2] - poses[j - 1][foot][:2])) / 2)
                prev = (j, w1)
    out["slide"] = round(float(np.mean(slides)), 1) if slides else None
    out["sweep"] = round(float(np.median(sweeps)), 1) if sweeps else None
    return out


def source_metrics(src, pmap):
    """The same measures on the DLS26 clip itself (its own pose, root and heading curves)."""
    S = d26_source(src)
    times = S["times"]

    def w(slot, tau):
        p = np.array(interp_curve(times, [tuple(x) for x in S["pos_tracks"][pmap[slot]]], tau))
        th = interp_curve(times, S["rot"], tau) * 2 * math.pi / 2048 if S["rot"] else 0.0
        r = interp_curve(times, S["root"], tau) if S["root"] else (0.0, 0.0)
        c, sn = math.cos(th), math.sin(th)
        return np.array([(c * p[0] - sn * p[1]) / 2 - r[0], p[2] / 2, (sn * p[0] + c * p[1]) / 2 + r[1]])
    out = {"dur": S["dur"], "frames": len(times), "interval": "", "n_ap": len(S["aps"]),
           "ap_ticks": " ".join(f"{a[0] / 2:g}" for a in S["aps"]), "ap_game": "", "ap_ok": True}
    if S["aps"]:
        t0, x, y, z = S["aps"][0]
        ball = np.array([x, y, z], float)
        best = min((np.linalg.norm(w(s, tau) - ball), tau) for s in EFFECTORS
                   for tau in np.linspace(0, S["dur"], 4 * S["dur"] + 1))
        out["contact_tick"] = round(best[1] / 2, 1)
        out["contact_gap"] = round(best[0])
        out["contact_err"] = round((best[1] - t0) / 2, 1)
        out["ball_gap"] = round(min(np.linalg.norm(w(s, t0) - ball) for s in EFFECTORS))
    e = S["root"][-1] if S["root"] else (0, 0)
    out["travel"] = tuple(e)
    out["travel_len"] = round(math.hypot(*e))
    out["travel_deg"] = round(math.degrees(math.atan2(e[1], e[0]))) if S["root"] else 0
    out["heading_end_deg"] = round(S["rot"][-1] * 360 / 2048) if S["rot"] else 0
    slides, sweeps = [], []
    ticks = list(range(0, S["dur"] + 1))
    for foot, toe in FEET:
        P = [interp_curve(times, [tuple(x) for x in S["pos_tracks"][pmap[foot]]], t) for t in ticks]
        T = [interp_curve(times, [tuple(x) for x in S["pos_tracks"][pmap[toe]]], t) for t in ticks]
        zs = [min(a[2], b[2]) for a, b in zip(P, T)]
        zmin = min(zs)
        for j in range(1, len(ticks)):
            if zs[j] < zmin + 64 and zs[j - 1] < zmin + 64:
                a, b = w(foot, ticks[j - 1]), w(foot, ticks[j])
                slides.append(math.hypot(b[0] - a[0], b[2] - a[2]))
                sweeps.append(math.hypot(P[j][0] - P[j - 1][0], P[j][1] - P[j - 1][1]) / 2)
    out["slide"] = round(float(np.mean(slides)), 1) if slides else None
    out["sweep"] = round(float(np.median(sweeps)), 1) if sweeps else None
    return out


def edge_jump(v, clip, ref_v, ref_clip):
    """Mean position difference (cm) over all bones between a clip's first/last playable pose
    and the reference clip's, i.e. what the engine's crossfade has to hide at entry and exit."""
    a, b = v.clip(clip), ref_v.clip(ref_clip)
    res = []
    for frac in (0.0, 1.0):
        pa = pose_at(a, frac)
        pb = pose_at(b, frac)
        res.append(round(float(np.mean(np.linalg.norm(pa - pb, axis=1))) / 32, 1))
    return res


def pose_at(s, frac):
    fpos = max(0.0, min(s["frames"] - 1.0, frac * s["hdr0"] / s["interval"]))
    k = min(s["frames"] - 2, int(fpos))
    f = fpos - k
    return np.array(s["fk"][k]) * (1 - f) + np.array(s["fk"][k + 1]) * f


def run():
    vs = {"stock": Version("stock", V30_PAK), "v15": Version("v15", V15_PAK), "v16": Version("v16", V16_PAK)}
    rows = []
    for p in PORTS:
        tid = p["id"]
        ref = p.get("tmpl", tid)
        for name, v in vs.items():
            clip = ref if name == "stock" else tid
            m = metrics(v, clip)
            if name != "stock":
                m["entry_jump_cm"], m["exit_jump_cm"] = edge_jump(v, clip, vs["stock"], ref)
            m.update({"id": tid, "version": name, "clip": clip, "kind": p["kind"], "mode": p["mode"],
                      "source": p["src"]})
            rows.append(m)
            if name == "stock":
                sm = source_metrics(p["src"], vs["stock"].clip(ref)["pmap"])
                sm.update({"id": tid, "version": "dls26", "clip": p["src"], "kind": p["kind"],
                           "mode": p["mode"], "source": p["src"]})
                rows.append(sm)
    return rows


COLS = ["id", "kind", "mode", "source", "version", "clip", "dur", "frames", "interval", "n_ap", "ap_ticks",
        "ap_game", "ap_ok", "contact_tick", "contact_err", "contact_gap", "ball_gap", "travel", "travel_len",
        "travel_deg", "heading_end_deg", "slide", "sweep", "entry_jump_cm", "exit_jump_cm"]


def markdown(rows):
    head = ("| id | kind | ver | dur | AP ticks | game ticks | contact tick (err) | ball gap | travel (len, deg) "
            "| heading end | slide | sweep | entry/exit jump cm |")
    lines = [head, "|" + "---|" * 13]
    for r in rows:
        ct = f"{r.get('contact_tick', '')} ({r.get('contact_err', '')})" if "contact_tick" in r else "-"
        lines.append(f"| {r['id']}{'' if r['version'] != 'stock' else ' ref ' + str(r['clip'])} | {r['kind']} "
                     f"| {r['version']} | {r['dur']} | {r['ap_ticks'] or '-'} | {r['ap_game'] or '-'} | {ct} "
                     f"| {r.get('ball_gap', '-')} | {r['travel_len']}, {r['travel_deg']} | {r['heading_end_deg']} "
                     f"| {r['slide'] if r['slide'] is not None else '-'} | {r['sweep'] if r['sweep'] is not None else '-'} "
                     f"| {str(r['entry_jump_cm']) + ' / ' + str(r['exit_jump_cm']) if 'entry_jump_cm' in r else '-'} |")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--csv", type=Path, default=ROOT / "analysis" / "dls26_tuned_v16_validation.csv")
    ap.add_argument("--md", type=Path, default=ROOT / "analysis" / "dls26_tuned_v16_validation.md")
    args = ap.parse_args()
    rows = run()
    with args.csv.open("w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=COLS, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    args.md.write_text(markdown(rows), encoding="utf-8")
    print(markdown(rows))
    bad = [r for r in rows if r["version"] == "v16" and not r["ap_ok"]]
    if bad:
        raise SystemExit(f"v16 clips with invalid action points: {[r['id'] for r in bad]}")


if __name__ == "__main__":
    main()
