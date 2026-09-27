#!/usr/bin/env python3
"""Tune the DLS26-derived clips of the v15 animation package to DLS18 gameplay data (v16).

What the v15 package got wrong (see analysis/DLS26_13430_animation_port.md, "v16 tuning"):
  * DLS18 root motion lives in the animation database (record +0x28 XZ curve, +0x2c heading
    curve), not in the SAT. v15 kept the template's curves under the DLS26 pose, so the body
    travelled along the old clip's path (e.g. appended tackle 2535 lunged forward while the
    331 curve carried it 63 degrees to the left).
  * The 14 appended deek/control records (2547-2560) had no action points at all. SetAnimControl
    divides by the ticks to action point 0, which is how clip 2553 crashed the game.
  * Action point times are 30 Hz ticks (TAnimData = (t << 17) / dur), the record duration is in
    60 Hz ticks, and position tracks were linearly offset without touching the transforms.

v16 rebuilds each port from the DLS26 source:
  * SAT transforms are resampled through a time map that puts every source contact marker on a
    DLS18 action-point tick; position tracks are recomputed from forward kinematics so they agree
    with the transforms (stock clips satisfy this to 0.1 cm).
  * The database root and heading curves come from the source clip through the same time map.
    Replaced tackle/slide slots keep their stock contact tick, contact point and total travel:
    the root path is bent with one similarity transform before contact and one after.
  * Appended clips take the source duration (even-rounded), their own curves and the source's
    action points (time-mapped); close-control clips also get the DLS18 end-of-clip ball point.

Usage: python tune_dls26_ports.py [--out PAK] [--sat-dir DIR]
"""

from __future__ import annotations

import argparse
import cmath
import hashlib
import json
import math
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from anim_tune_lib import (ROOT, V15_PAK, V16_PAK, V30_PAK, AnimDB, d26_source, fk_positions,  # noqa: E402
                           interp_curve, load_pak, sat_frames)
from build_catalog import parse_kpx  # noqa: E402

SAT_DIR = ROOT / "mod" / "source" / "dls26_tuned_v16"
REPORT = ROOT / "analysis" / "dls26_tuned_v16_build.json"

# id: target DLS18 id (base of a base/mirror pair where one exists)
# src: DLS26 13.430 source, tmpl: DLS18 layout/record template (replacements are their own template)
# mode: replace (existing slot, stock timing kept) or append (new id, source timing)
# kind: tackle / slide / stumble / deek / control / loop / sidestep / turn
# edge: frames blended with the template pose at each end (v15 values, reduced around contacts)
PORTS = [
    dict(id=331, src=2306, mode="replace", kind="tackle", edge=0),
    dict(id=737, src=3648, mode="replace", kind="slide", edge=0),
    dict(id=1286, src=1316, mode="replace", kind="slide", edge=0),
    dict(id=2535, src=2308, tmpl=331, mode="append", kind="tackle", edge=0),
    dict(id=2537, src=2408, tmpl=2517, mode="append", kind="stumble", edge=4),
    dict(id=2539, src=870, tmpl=368, mode="append", kind="stumble", edge=2),
    dict(id=2541, src=864, tmpl=366, mode="append", kind="stumble", edge=2),
    dict(id=2543, src=1292, tmpl=1257, mode="append", kind="stumble", edge=2),
    dict(id=2545, src=1294, tmpl=1259, mode="append", kind="stumble", edge=2),
    dict(id=2547, src=3304, tmpl=2331, mode="append", kind="deek", edge=4),
    dict(id=2549, src=3306, tmpl=2335, mode="append", kind="deek", edge=4),
    dict(id=2551, src=3308, tmpl=2335, mode="append", kind="deek", edge=4),
    dict(id=2553, src=3260, tmpl=1331, mode="append", kind="control", edge=4),
    dict(id=2555, src=3262, tmpl=1333, mode="append", kind="control", edge=4),
    dict(id=2557, src=3264, tmpl=1335, mode="append", kind="control", edge=4),
    dict(id=2559, src=3282, tmpl=1360, mode="append", kind="control", edge=4),
    dict(id=512, src=72, mode="replace", kind="loop", edge=0),
    dict(id=514, src=74, mode="replace", kind="loop", edge=0),
    dict(id=516, src=76, mode="replace", kind="loop", edge=0),
    dict(id=686, src=186, mode="replace", kind="sidestep", edge=2),
    dict(id=693, src=188, mode="replace", kind="sidestep", edge=2),
    dict(id=695, src=190, mode="replace", kind="sidestep", edge=2),
    dict(id=698, src=192, mode="replace", kind="sidestep", edge=2),
    dict(id=702, src=194, mode="replace", kind="sidestep", edge=2),
    dict(id=13, src=510, mode="replace", kind="turn", edge=0),
    dict(id=15, src=514, mode="replace", kind="turn", edge=0),
    dict(id=17, src=518, mode="replace", kind="turn", edge=0),
    dict(id=19, src=522, mode="replace", kind="turn", edge=0),
    dict(id=1253, src=1288, mode="replace", kind="turn", edge=0),
    dict(id=1255, src=1290, mode="replace", kind="turn", edge=0),
]
# Appended stumbles keep the v15 decision on action points: only 2537 carries one.
STUMBLE_AP = {2537}
UNUSED_AP = (0, 0, 240, 0)


# ---------------------------------------------------------------- helpers
def wrap_rot(v: float) -> int:
    """Heading curve sample (2048 per turn). Kept continuous (not wrapped): CPlayer::GetTrueRot
    interpolates two samples linearly, adds (value << 3) to the player angle and masks with 0x3fff,
    so an unwrapped full spin (3304: -1969) stays smooth, whereas a +-1024 wrap would swing the
    body through zero between the two samples at the wrap."""
    return max(-4095, min(4095, int(round(v))))


def quat_nlerp(a, b, f):
    a = [x / 16384.0 for x in a]
    b = [x / 16384.0 for x in b]
    if sum(a[i] * b[i] for i in range(4)) < 0:
        b = [-x for x in b]
    q = [a[i] * (1 - f) + b[i] * f for i in range(4)]
    n = math.sqrt(sum(x * x for x in q)) or 1.0
    return [max(-32768, min(32767, round(x / n * 16384.0))) for x in q]


def sample_source_track(track, times, tau):
    """DLS26 7-channel record (t0..t2, q0..q3) at tau -> DLS18 order (q0..q3, t0..t2)."""
    from bisect import bisect_right
    if tau <= times[0]:
        a = b = track[0]
        f = 0.0
    elif tau >= times[-1]:
        a = b = track[-1]
        f = 0.0
    else:
        r = bisect_right(times, tau)
        a, b = track[r - 1], track[r]
        f = (tau - times[r - 1]) / (times[r] - times[r - 1])
    q = quat_nlerp(a[3:7], b[3:7], f)
    t = [a[i] * (1 - f) + b[i] * f for i in range(3)]
    return q, t


def sample_dls18_frame(trans, track, fpos):
    n = len(trans[track])
    fpos = max(0.0, min(n - 1.0, fpos))
    k = min(n - 2, int(fpos))
    f = fpos - k
    a, b = trans[track][k], trans[track][k + 1]
    return quat_nlerp(a[:4], b[:4], f), [a[4 + i] * (1 - f) + b[4 + i] * f for i in range(3)]


class TimeMap:
    """Piecewise-linear map from target clip fraction to DLS26 source ticks."""

    def __init__(self, anchors):
        self.a = sorted(anchors)

    def __call__(self, frac):
        a = self.a
        if frac <= a[0][0]:
            return a[0][1]
        if frac >= a[-1][0]:
            return a[-1][1]
        for (x0, y0), (x1, y1) in zip(a, a[1:]):
            if x0 <= frac <= x1:
                return y0 + (y1 - y0) * (frac - x0) / (x1 - x0) if x1 > x0 else y1
        return a[-1][1]

    def inverse(self, tau):
        a = self.a
        for (x0, y0), (x1, y1) in zip(a, a[1:]):
            if y0 <= tau <= y1:
                return x0 + (x1 - x0) * (tau - y0) / (y1 - y0) if y1 > y0 else x1
        return 1.0 if tau > a[-1][1] else 0.0


def ap_to_root(x, z):
    """Action-point (x, z) -> root-curve (x forward, z) coordinates (same raw units)."""
    return complex(-x, z)


def root_to_ap(c):
    return (-c.real, c.imag)


def smoothstep(x: float) -> float:
    x = max(0.0, min(1.0, x))
    return x * x * (3 - 2 * x)


def pose_yaw(fk, fpos) -> float:
    """Body yaw of a FK pose (DB heading units, 2048 per turn) from the thigh axis:
    0 when the right-to-left thigh axis points along +Y (facing -X, the DLS18 forward)."""
    k = max(0, min(len(fk) - 2, int(fpos)))
    f = fpos - k
    r = [fk[k][12][i] * (1 - f) + fk[k + 1][12][i] * f for i in range(2)]
    l = [fk[k][31][i] * (1 - f) + fk[k + 1][31][i] * f for i in range(2)]
    return (math.atan2(l[1] - r[1], l[0] - r[0]) - math.pi / 2) * 2048 / (2 * math.pi)


def similarity(src: complex, dst: complex) -> complex:
    if abs(src) < 1e-6:
        return complex(1, 0)
    return dst / src


# ---------------------------------------------------------------- one port
def build(p, v30, v15, db30, db15, db_out):
    tid, S = p["id"], d26_source(p["src"])
    tmpl = p.get("tmpl", tid)
    append = p["mode"] == "append"
    tsat = sat_frames(v30[f"{tmpl:04d}.sat"]["data"])
    t_hdr0 = struct.unpack_from("<h", v30[f"{tmpl:04d}.sat"]["data"], 0)[0]
    base_rec = db15.records[tid]
    has_mirror = tid + 1 < db15.count and db15.records[tid + 1][2] & 1 and \
        v15[f"{tid + 1:04d}.sat"]["data"][5] != 0
    stock_aps = db30.action_points(tmpl)
    interval = tsat["interval"]
    if append:
        # New ids are free to take a finer layout: a 2-tick SAT interval (the most common stock
        # value) gives a root-curve sample every 4 record ticks instead of 6, which keeps more of
        # the source's root path (DLS26 samples every ~3 ticks).
        interval = 2
        dur = 2 * max(1, math.ceil(S["dur"] / 2))
        hdr0 = dur // 2
        frames = math.ceil(hdr0 / interval) + 1
        if frames > 127:
            raise ValueError(f"{tid}: {frames} frames")
    else:
        dur = db30.duration(tid)
        hdr0 = t_hdr0
        frames = tsat["frames"]
        if 2 * hdr0 != dur:
            raise ValueError(f"{tid}: SAT header duration {hdr0} vs record {dur}")
    step = 2 * interval
    n_curve = frames
    if not append and (db30.s16(tid, 8) != n_curve or db30.s16(tid, 0xA) != step):
        raise ValueError(f"{tid}: stock curve layout differs from SAT layout")

    # ---- time map and action points ----------------------------------------------------
    src_aps = S["aps"]
    anchors = [(0.0, 0.0), (1.0, float(S["dur"]))]
    new_aps = []  # (t30, x, y, z)
    notes = []
    if p["kind"] in ("tackle", "slide") and not append:
        t18 = stock_aps[0][0]
        anchors.append((2 * t18 / dur, float(src_aps[0][0])))
        new_aps = [stock_aps[0]]
    elif p["kind"] in ("tackle", "deek", "control") or (p["kind"] == "stumble" and tid in STUMBLE_AP):
        last = 0
        limit = math.ceil(dur / 2) - 1
        for (t26, x, y, z) in src_aps:
            t18 = round(t26 / S["dur"] * dur / 2)
            t18 = max(last + 1, min(limit, t18))
            anchors.append((2 * t18 / dur, float(t26)))
            new_aps.append((t18, x, y, z))
            last = t18
    tm = TimeMap(anchors)
    frac_of_frame = [min(1.0, f * interval / hdr0) for f in range(frames)]
    taus = [tm(fr) for fr in frac_of_frame]

    # ---- edge frames ----------------------------------------------------------------------
    # Blending the first/last frames toward the template pose only suits clips without XZ root
    # motion: under a source root curve the template's leg pose makes the planted foot skate
    # (2553: 58 units/tick with a 3-frame blend, 20 without; DLS26 native 23). The engine's
    # own two-clip crossfade (CPlayer+0x6e) covers the entry/exit instead.
    edge = p["edge"]
    rec_has_xz = struct.unpack_from("<I", base_rec, 0x28)[0] != 0
    if edge and rec_has_xz and S["root"]:
        notes.append(f"edge blend {edge}->0 (root-motion clip)")
        edge = 0

    # ---- transforms ----------------------------------------------------------------------
    trans = []
    for k in range(33):
        tr = []
        for f in range(frames):
            q, t = sample_source_track(S["rot_tracks"][k], S["times"], taus[f])
            w = 1.0 if edge == 0 else min(1.0, f / edge, (frames - 1 - f) / edge)
            if w < 1.0:
                tpos = frac_of_frame[f] * t_hdr0 / tsat["interval"]
                bq, bt = sample_dls18_frame(tsat["trans"], k, tpos)
                q = quat_nlerp(bq, q, w)
                t = [bt[i] * (1 - w) + t[i] * w for i in range(3)]
            tr.append(q + [max(-32768, min(32767, round(v))) for v in t])
        trans.append(tr)
    fk = fk_positions(tsat["tmap"], trans)
    pcount = tsat["pcount"]
    slot_of_track = {tsat["pmap"][s]: s for s in range(42) if tsat["pmap"][s] >= 0}
    if sorted(slot_of_track) != list(range(pcount)):
        raise ValueError(f"{tid}: position map incomplete")
    header = bytearray(v30[f"{tmpl:04d}.sat"]["data"][:0x68])
    struct.pack_into("<h", header, 0, hdr0)
    struct.pack_into("<h", header, 2, interval)
    header[4] = frames
    body = bytearray(header)
    for k in range(33):
        for f in range(frames):
            body += struct.pack("<7h", *trans[k][f])
    for f in range(frames):
        for tr in range(pcount):
            body += struct.pack("<3h", *(max(-32768, min(32767, round(v))) for v in fk[f][slot_of_track[tr]]))
    sat_out = {tid: bytes(body)}
    if has_mirror:
        mh = bytearray(header)
        mh[5] = 1
        mh[0x60:0x68] = bytes(8)
        old_m = v15[f"{tid + 1:04d}.sat"]["data"]
        if not append and bytes(mh) != old_m:
            raise ValueError(f"{tid + 1}: stock mirror header differs from regenerated one")
        sat_out[tid + 1] = bytes(mh)
    chk = sat_frames(body)
    if not chk["size_ok"] or chk["frames"] != frames:
        raise AssertionError(f"{tid}: SAT does not decode")

    # ---- database root and heading curves ------------------------------------------------
    rec = bytearray(base_rec)
    old_curves = db15.curves[tid]
    has_xz = struct.unpack_from("<I", rec, 0x28)[0] != 0
    has_rot = struct.unpack_from("<I", rec, 0x2C)[0] != 0
    fr_curve = [min(1.0, j * step / dur) for j in range(n_curve)]
    tau_curve = [tm(fr) for fr in fr_curve]
    xz = None
    rot = None
    travel = {}
    if has_xz and S["root"]:
        pts = [complex(*interp_curve(S["times"], S["root"], t)) for t in tau_curve]
        if p["kind"] in ("tackle", "slide") and not append:
            # Bend the source root path so that the source ball point meets the stock action
            # point at the stock contact tick, and the clip ends on the stock displacement.
            fc = 2 * stock_aps[0][0] / dur
            tau_c = tm(fc)
            r_c = complex(*interp_curve(S["times"], S["root"], tau_c))
            r_e = complex(*interp_curve(S["times"], S["root"], float(S["dur"])))
            ball_src = ap_to_root(src_aps[0][1], src_aps[0][3])
            off = ball_src - r_c
            target_c = ap_to_root(stock_aps[0][1], stock_aps[0][3]) - off
            e_stock = complex(*db30.root_at_tick(tid, dur))
            m_a = similarity(r_c, target_c)
            m_b = similarity(r_e - r_c, e_stock - target_c)
            bent = []
            for fr, c in zip(fr_curve, pts):
                bent.append(c * m_a if fr <= fc else target_c + (c - r_c) * m_b)
            travel = {"pre_contact_scale": round(abs(m_a), 3),
                      "pre_contact_rotation_deg": round(math.degrees(cmath.phase(m_a)), 1),
                      "post_contact_scale": round(abs(m_b), 3),
                      "post_contact_rotation_deg": round(math.degrees(cmath.phase(m_b)), 1),
                      "source_travel": [round(r_e.real), round(r_e.imag)],
                      "stock_travel": [round(e_stock.real), round(e_stock.imag)]}
            pts = bent
        xz = [(max(-32768, min(32767, round(c.real))), max(-32768, min(32767, round(c.imag)))) for c in pts]
    elif has_xz:
        xz = db15.root_curve(tid) if len(db15.root_curve(tid)) == n_curve else None
        if xz is None:
            raise ValueError(f"{tid}: template xz curve cannot be reused")
        notes.append("source has no root curve; template XZ curve kept")
    if has_rot:
        if S["rot"]:
            rot = [wrap_rot(interp_curve(S["times"], S["rot"], t)) for t in tau_curve]
        else:
            rot = db15.rot_curve(tid)
            if len(rot) != n_curve:
                raise ValueError(f"{tid}: template heading curve cannot be reused")
    if rot and p["kind"] in ("tackle", "slide") and not append:
        # Exit alignment: the next clip in the DLS18 chain expects the body to face where the
        # stock clip leaves it (heading curve + pose yaw). Ramp the difference in after contact.
        def end_value(curve):
            k = min(len(curve) - 2, dur // step)
            f = (dur - k * step) / step
            return curve[k] * (1 - f) + curve[k + 1] * f
        stock_sat = sat_frames(v30[f"{tid:04d}.sat"]["data"])
        stock_fk = fk_positions(stock_sat["tmap"], stock_sat["trans"])
        f_end = min(frames - 1.0, hdr0 / interval)
        new_yaw = pose_yaw(fk, f_end)
        stock_yaw = pose_yaw(stock_fk, f_end)
        want = end_value(db30.rot_curve(tid)) + stock_yaw
        have = end_value(rot) + new_yaw
        delta = want - have
        delta = (delta + 1024) % 2048 - 1024
        fc = 2 * stock_aps[0][0] / dur
        rot = [wrap_rot(r + delta * smoothstep((fr - fc) / (1 - fc))) for r, fr in zip(rot, fr_curve)]
        travel["exit_heading_correction_deg"] = round(delta * 360 / 2048, 1)
        travel["exit_yaw_stock_deg"] = round((end_value(db30.rot_curve(tid)) + stock_yaw) * 360 / 2048, 1)
        travel["exit_yaw_source_deg"] = round(have * 360 / 2048, 1)
    if not has_xz and S["root"] and max(abs(complex(*c)) for c in S["root"]) > 150:
        notes.append("template record has no XZ root motion; source travel of "
                     f"{abs(complex(*S['root'][-1])):.0f} units not representable")

    # ---- control clips: DLS18 end-of-clip ball point --------------------------------------
    if p["kind"] == "control" and new_aps:
        t_end = dur // 2 - 1
        tpl_aps = db30.action_points(tmpl)
        tpl_end = tpl_aps[-1]
        tpl_dur = db30.duration(tmpl)
        tpl_root_end = complex(*db30.root_at_tick(tmpl, tpl_dur))
        tpl_rot_end = db30.rot_curve(tmpl)[-1] if db30.rot_curve(tmpl) else 0
        lead = ap_to_root(tpl_end[1], tpl_end[3]) - tpl_root_end
        new_root_end = complex(*xz[-1]) if xz else complex(0, 0)
        # Evaluate the new curve at the clip end the way the engine does.
        k = min(n_curve - 2, dur // step)
        f = (dur - k * step) / step
        if xz:
            new_root_end = complex(xz[k][0] * (1 - f) + xz[k + 1][0] * f, xz[k][1] * (1 - f) + xz[k + 1][1] * f)
        new_rot_end = (rot[k] * (1 - f) + rot[k + 1] * f) if rot else 0
        dtheta = (new_rot_end - tpl_rot_end) * 2 * math.pi / 2048
        ball = new_root_end + lead * cmath.exp(1j * dtheta)
        bx, bz = root_to_ap(ball)
        if len(new_aps) < 4 and t_end > new_aps[-1][0]:
            new_aps.append((t_end, round(bx), tpl_end[2], round(bz)))
            notes.append("end-of-clip ball point added from template lead "
                         f"({round(lead.real)}, {round(lead.imag)}) rotated {math.degrees(dtheta):.1f} deg")

    # ---- write record(s) -----------------------------------------------------------------
    struct.pack_into("<h", rec, 6, dur)
    struct.pack_into("<h", rec, 8, n_curve)
    struct.pack_into("<h", rec, 0xA, step)
    aps_full = list(new_aps) + [UNUSED_AP] * (4 - len(new_aps))
    if p["kind"] in ("loop", "sidestep", "turn") or (p["kind"] == "stumble" and tid not in STUMBLE_AP):
        aps_full = None  # keep template action-point block
    if aps_full is not None:
        for k, (t, x, y, z) in enumerate(aps_full):
            struct.pack_into("<Hhhh", rec, 0x30 + 8 * k, t, x, y, z)
    curves = [None, None]
    if has_xz:
        curves[0] = b"".join(struct.pack("<hh", x, z) for x, z in xz)
    if has_rot:
        curves[1] = b"".join(struct.pack("<h", r) for r in rot)
        # +0x58 / +0x5a: start and end heading in curve units / 32 (TAnimData +0x1c/+0x1e, read by
        # CPlayer::GetTrueRot before the first and after the last curve interval). Stock records
        # all satisfy this to within one unit.
        struct.pack_into("<hh", rec, 0x58, round(rot[0] / 32), round(rot[-1] / 32))
    db_out.records[tid] = rec
    db_out.curves[tid] = curves
    if has_mirror:
        mrec = bytearray(db15.records[tid + 1])
        struct.pack_into("<h", mrec, 6, dur)
        struct.pack_into("<h", mrec, 8, n_curve)
        struct.pack_into("<h", mrec, 0xA, step)
        if aps_full is not None:
            for k, (t, x, y, z) in enumerate(aps_full):
                struct.pack_into("<Hhhh", mrec, 0x30 + 8 * k, t, x, y, -z if t else z)
        mcur = [None, None]
        if has_xz:
            mcur[0] = b"".join(struct.pack("<hh", x, -z) for x, z in xz)
        if has_rot:
            mcur[1] = b"".join(struct.pack("<h", wrap_rot(-r)) for r in rot)
            struct.pack_into("<hh", mrec, 0x58, round(-rot[0] / 32), round(-rot[-1] / 32))
        if (struct.unpack_from("<I", mrec, 0x28)[0] != 0) != has_xz or \
                (struct.unpack_from("<I", mrec, 0x2C)[0] != 0) != has_rot:
            raise ValueError(f"{tid + 1}: mirror curve markers differ from base")
        db_out.records[tid + 1] = mrec
        db_out.curves[tid + 1] = mcur

    # ---- native checks -------------------------------------------------------------------
    for i in ([tid, tid + 1] if has_mirror else [tid]):
        for k, (t, *_r) in enumerate(db_out.action_points(i)):
            if (t << 17) // dur >= 0x10000:
                raise ValueError(f"{i}: action point {k} time {t} overflows TAnimData u16 (dur {dur})")
            if db_out.native_action_ticks(i, k) <= 0:
                raise ValueError(f"{i}: action point {k} gives zero ticks")
    return sat_out, {
        "id": tid, "mirror": tid + 1 if has_mirror else None, "source": S["id"], "source_name": S["name"],
        "template": tmpl, "mode": p["mode"], "kind": p["kind"], "dur": dur, "sat_frames": frames,
        "sat_interval": interval, "edge_frames": edge, "time_anchors": [(round(a, 4), round(b, 2)) for a, b in tm.a],
        "action_points": db_out.action_points(tid), "travel_fit": travel, "notes": notes,
    }


# ---------------------------------------------------------------- package
def repack(src_pak: bytes, replacements: dict[str, bytes]) -> bytes:
    marker, entries = parse_kpx(src_pak)
    folders, files, names_size = struct.unpack_from("<III", src_pak, 4)
    table = 16 + folders * 20
    names_end = table + files * 24 + names_size
    out = bytearray(src_pak[:names_end])
    payload = bytearray()
    by_index = sorted(entries, key=lambda e: e["file_index"])
    for e in by_index:
        raw = replacements.get(e["path"], e["data"])
        row = list(struct.unpack_from("<6I", src_pak, table + e["file_index"] * 24))
        stored = zlib.compress(raw, 9) if row[3] else raw
        row[1], row[2], row[5] = len(raw), names_end + len(payload), len(stored)
        struct.pack_into("<6I", out, table + e["file_index"] * 24, *row)
        payload += stored
    out += payload
    _, check = parse_kpx(bytes(out))
    got = {e["path"]: e["data"] for e in check}
    for e in entries:
        if got[e["path"]] != replacements.get(e["path"], e["data"]):
            raise AssertionError(f"repack read-back differs: {e['path']}")
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", type=Path, default=V15_PAK)
    ap.add_argument("--out", type=Path, default=V16_PAK)
    ap.add_argument("--sat-dir", type=Path, default=SAT_DIR)
    args = ap.parse_args()
    v15_bytes = args.base.read_bytes()
    _, v15 = load_pak(args.base)
    _, v30 = load_pak(V30_PAK)
    db30 = AnimDB(v30["animdb.adb"]["data"])
    db15 = AnimDB(v15["animdb.adb"]["data"])
    db_out = AnimDB(v15["animdb.adb"]["data"])
    sats = {}
    reports = []
    for p in PORTS:
        s, r = build(p, v30, v15, db30, db15, db_out)
        sats.update(s)
        reports.append(r)
        print(f"{r['id']:5d} <- {r['source']:5d} {r['kind']:8s} dur {r['dur']:3d} frames {r['sat_frames']:2d} "
              f"APs {r['action_points']} {r['travel_fit'] or ''} {'; '.join(r['notes'])}")
    # every record with action points must give positive ticks (crash guard for SetAnimControl)
    bad = [i for i in range(db_out.count) for k in range(len(db_out.action_points(i)))
           if db_out.native_action_ticks(i, k) <= 0 and db_out.records[i][0] in (1, 8, 9, 19)]
    if bad:
        raise SystemExit(f"records with zero action ticks: {bad}")
    db_bytes = db_out.pack()
    AnimDB(db_bytes)  # re-parse
    repl = {f"{i:04d}.sat": b for i, b in sats.items()}
    repl["animdb.adb"] = db_bytes
    pak = repack(v15_bytes, repl)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(pak)
    args.sat_dir.mkdir(parents=True, exist_ok=True)
    for i, b in sats.items():
        (args.sat_dir / f"{i:04d}.sat").write_bytes(b)
    (args.sat_dir / "animdb.adb").write_bytes(db_bytes)
    summary = {"output": str(args.out), "bytes": len(pak), "sha256": hashlib.sha256(pak).hexdigest(),
               "base": str(args.base), "animdb_records": db_out.count,
               "kpx_members": len(parse_kpx(pak)[1]), "changed_members": sorted(repl), "ports": reports}
    REPORT.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {args.out} ({len(pak)} bytes, sha256 {summary['sha256'][:16]}...), {len(repl)} members changed")


if __name__ == "__main__":
    main()
