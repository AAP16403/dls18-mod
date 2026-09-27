#!/usr/bin/env python3
"""Shared helpers for tuning DLS26-ported clips against DLS18 gameplay data.

Database record layout (100-byte file record; native CAnimManager::AnimDataFill
@0x2a98a4, SET_ROOT_POS @0x2aa058, GetActionTime @0x2aa194):

  +0x00 u8   category (state list)
  +0x02 u8   bit0 = mirror
  +0x04 s16  loop/cycle multiplier (rate = 0x10000 / (+4 * +6) * +0x5c / 100)
  +0x06 s16  clip duration in 30 Hz ticks  (TAnimData+0x5c)
  +0x08 s16  root-curve sample count        (TAnimData+0x5e)
  +0x0a s16  root-curve sample interval     (TAnimData+0x60)
  +0x28 u32  nonzero -> root XZ curve blob follows (2 * +8 s16: x,z per sample)
  +0x2c u32  nonzero -> root rotation blob follows (+8 s16)
  +0x30 + 8*k  action point k (k=0..3): u16 time (ticks), s16 x, s16 y (height), s16 z
             TAnimData time = (t << 17) / +6 stored as u16  -> t must stay below +6 / 2
             position scaled by 0x17d / 16 into world units
  +0x5c s16  playback speed percent

Root curve world value = raw * 0x17d / 16 (both curve and action-point coordinates).
"""

from __future__ import annotations

import math
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "analysis"))
from build_catalog import parse_kpx  # noqa: E402

ROW = 100
STOCK_PAK = ROOT / "unpacked" / "apk" / "assets" / "data" / "anims" / "anims.pak"
V30_PAK = ROOT / "mod" / "build" / "anims_career_market_v30.pak"
V15_PAK = ROOT / "mod" / "build" / "anims_career_market_v30_dls26_tackle_stumble_append_v15_turns.pak"
V16_PAK = ROOT / "mod" / "build" / "anims_career_market_v30_dls26_tackle_stumble_append_v16_tuned.pak"
WORLD = 0x17D / 16.0  # raw curve/action units -> world units


class AnimDB:
    """Parsed animdb.adb: records plus per-record curve blobs (kept in order)."""

    def __init__(self, data: bytes):
        self.count = struct.unpack_from("<I", data)[0]
        self.records = [bytearray(data[4 + i * ROW:4 + (i + 1) * ROW]) for i in range(self.count)]
        cursor = 4 + self.count * ROW
        self.curves: list[list[bytes | None]] = []
        for i, rec in enumerate(self.records):
            blobs: list[bytes | None] = []
            for marker in (0x28, 0x2C):
                if struct.unpack_from("<I", rec, marker)[0]:
                    size = struct.unpack_from("<I", data, cursor)[0]
                    blobs.append(bytes(data[cursor + 4:cursor + 4 + size]))
                    cursor += 4 + size
                else:
                    blobs.append(None)
            self.curves.append(blobs)
        if cursor != len(data):
            raise ValueError(f"animdb walk ended at {cursor}, file is {len(data)}")

    def pack(self) -> bytes:
        out = bytearray(struct.pack("<I", len(self.records)))
        for rec in self.records:
            out += rec
        for rec, blobs in zip(self.records, self.curves):
            for marker, blob in zip((0x28, 0x2C), blobs):
                has = struct.unpack_from("<I", rec, marker)[0] != 0
                if has != (blob is not None):
                    raise ValueError("curve flag and blob disagree")
                if blob is not None:
                    out += struct.pack("<I", len(blob)) + blob
        return bytes(out)

    # --- field helpers -------------------------------------------------------------
    def s16(self, i: int, off: int) -> int:
        return struct.unpack_from("<h", self.records[i], off)[0]

    def u16(self, i: int, off: int) -> int:
        return struct.unpack_from("<H", self.records[i], off)[0]

    def duration(self, i: int) -> int:
        return self.s16(i, 6)

    def action_points(self, i: int) -> list[tuple[int, int, int, int]]:
        pts = []
        for k in range(4):
            t, x, y, z = struct.unpack_from("<Hhhh", self.records[i], 0x30 + 8 * k)
            if t == 0:
                break
            pts.append((t, x, y, z))
        return pts

    def root_curve(self, i: int) -> list[tuple[int, int]]:
        blob = self.curves[i][0]
        if blob is None:
            return []
        n = len(blob) // 4
        return [struct.unpack_from("<hh", blob, 4 * k) for k in range(n)]

    def rot_curve(self, i: int) -> list[int]:
        blob = self.curves[i][1]
        if blob is None:
            return []
        return [struct.unpack_from("<h", blob, 2 * k)[0] for k in range(len(blob) // 2)]

    def root_at_tick(self, i: int, tick: float) -> tuple[float, float]:
        """Native SET_ROOT_POS_NO_SCALE sampling, in raw curve units."""
        curve = self.root_curve(i)
        if not curve:
            return (0.0, 0.0)
        step = self.s16(i, 0x0A)
        k = int(tick // step)
        k = max(0, min(len(curve) - 2, k))
        f = (tick - k * step) / step
        a, b = curve[k], curve[k + 1]
        return (a[0] * (1 - f) + b[0] * f, a[1] * (1 - f) + b[1] * f)

    def native_action_time_u16(self, i: int, k: int = 0) -> int:
        t = self.u16(i, 0x30 + 8 * k)
        d = self.duration(i)
        return ((t << 17) // d) & 0xFFFF if d else 0

    def native_action_ticks(self, i: int, k: int = 0, speed: int = 0x200) -> int:
        """CAnimManager::GetActionTime with the default speed (0x100000/0x800)."""
        d = self.duration(i)
        mult = self.s16(i, 4)
        pct = self.s16(i, 0x5C)
        if d <= 0 or mult <= 0:
            return 0
        rate = (0x10000 // (mult * d)) * pct // 100
        if rate == 0:
            return 0
        return (self.native_action_time_u16(i, k) * speed) // (rate << 10)


def load_pak(path: Path) -> tuple[bytes, dict[str, dict]]:
    data = path.read_bytes()
    _, entries = parse_kpx(data)
    return data, {e["path"]: e for e in entries}


def sat_frames(data: bytes) -> dict:
    """Decode a DLS18 SAT from bytes (base clips only)."""
    interval = struct.unpack_from("<h", data, 2)[0]
    frames = struct.unpack_from("<b", data, 4)[0]
    mirror = data[5]
    tcount = struct.unpack_from("<b", data, 7)[0]
    pcount = struct.unpack_from("<b", data, 0x32)[0]
    tmap = list(struct.unpack_from("<42b", data, 8))
    pmap = list(struct.unpack_from("<45b", data, 0x33))
    out = {"interval": interval, "frames": frames, "mirror": mirror, "tcount": tcount,
           "pcount": pcount, "tmap": tmap, "pmap": pmap}
    if mirror:
        return out
    cur = 0x68
    trans = []
    for _t in range(tcount):
        tr = []
        for _f in range(frames):
            tr.append(list(struct.unpack_from("<7h", data, cur)))
            cur += 14
        trans.append(tr)
    pos = []
    for _f in range(frames):
        fr = []
        for _t in range(pcount):
            fr.append(list(struct.unpack_from("<3h", data, cur)))
            cur += 6
        pos.append(fr)
    out["trans"] = trans
    out["pos"] = pos
    out["size_ok"] = cur == len(data)
    return out


def pos_track_for_slot(sat: dict, slot: int) -> int | None:
    v = sat["pmap"][slot]
    return v if v >= 0 else None


def mag(v) -> float:
    return math.sqrt(sum(c * c for c in v))


# --- forward kinematics on the DLS18 player rig -------------------------------------------
BONE_REMAP = [0, 1, 2, 3, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37,
              38, 20, 21, 22, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
              19, 39, 40, 41]
SLOT_R_FOOT, SLOT_R_TOE, SLOT_L_FOOT, SLOT_L_TOE = 8, 13, 27, 32
_RIG = None


def _rig():
    global _RIG
    if _RIG is None:
        import numpy as np
        from preview_sat_skeleton import DEFAULT_MODEL, read_rig, transform_matrix
        root, links, names, bind = read_rig(DEFAULT_MODEL)
        order = []  # (bone, parent) in traversal order

        def visit(node, parent):
            order.append((node, parent))
            child = links[node][0]
            while child != 0xFF:
                visit(child, node)
                child = links[child][1]
        visit(root, None)
        locals_ = [transform_matrix(b[3:7], b[7:10], b[:3]) for b in bind]
        _RIG = {"order": order, "bind": bind, "bind_locals": locals_, "tm": transform_matrix, "np": np,
                "names": names}
    return _RIG


def fk_positions(tmap: list[int], trans: list[list[list[int]]]) -> list[list[list[float]]]:
    """Model-space bone positions per frame, in SAT position units (cm * 32), indexed by
    animation slot (0..41). Reproduces the stock SAT position tracks exactly."""
    rig = _rig()
    np = rig["np"]
    active = [slot for slot, v in enumerate(tmap[:42]) if v >= 0]
    frames = len(trans[0]) if trans else 0
    out = []
    for f in range(frames):
        locals_ = list(rig["bind_locals"])
        for track, slot in enumerate(active):
            bone = BONE_REMAP[slot]
            rec = trans[track][f]
            q = [v / 16384.0 for v in rec[:4]]
            t = [v / 128.0 for v in rec[4:7]]
            locals_[bone] = rig["tm"](q, t, rig["bind"][bone][:3])
        worlds = [None] * 42
        for bone, parent in rig["order"]:
            worlds[bone] = locals_[bone] if parent is None else locals_[bone] @ worlds[parent]
        pts = []
        for slot in range(42):
            p = worlds[BONE_REMAP[slot]][3, :3] * 32.0
            pts.append([float(p[0]), float(p[1]), float(p[2])])
        out.append(pts)
    return out


# --- DLS26 13.430 source clips ------------------------------------------------------------
D26_ROOT = ROOT.parent / "dls26" / "decoded_13.430"
_D26 = {}


def d26_source(src_id: int) -> dict:
    """DLS26 clip: decoded tracks, sample times, root/rotation curves, action points."""
    if src_id in _D26:
        return _D26[src_id]
    import csv
    sys.path.insert(0, str(ROOT.parent / "dls26"))
    from decode_sat_13430 import decode  # noqa: E402
    db = (D26_ROOT / "unpacked" / "kpx" / "anims" / "animdb.adb").read_bytes()
    if "rows" not in _D26:
        with (D26_ROOT / "catalog" / "animdb_13430_records.csv").open(newline="", encoding="utf-8") as fh:
            _D26["rows"] = {int(r["animation_id"]): r for r in csv.DictReader(fh)}
    row = _D26["rows"][src_id]

    def arr(which):
        off, n = row[which + "_offset"], row[which + "_bytes"]
        if not off:
            return []
        return list(struct.unpack_from(f"<{int(n) // 2}h", db, int(off)))
    rec = db[4 + src_id * 104:4 + (src_id + 1) * 104]
    clip = decode(src_id)
    times = arr("primary")
    sec = arr("secondary")
    root = list(zip(sec[0::2], sec[1::2]))
    rot_raw = arr("tertiary")
    rot = []
    for v in rot_raw:  # DLS26 stores 0..2047 per circle; unwrap to a continuous signed curve
        v = v - 2048 if v >= 1024 else v
        if rot:
            while v - rot[-1] > 1024:
                v -= 2048
            while v - rot[-1] < -1024:
                v += 2048
        rot.append(v)
    dur = struct.unpack_from("<H", rec, 4)[0]
    aps = []
    for k in range(4):
        t, x, y, z = struct.unpack_from("<Hhhh", rec, 0x30 + 8 * k)
        if t:
            aps.append((t, x, y, z))
    out = {"id": src_id, "name": row["animation_name"], "dur": dur, "times": times, "root": root,
           "rot": rot, "aps": aps, "rot_tracks": clip["rotation_tracks"], "pos_tracks": clip["position_tracks"],
           "category": rec[0]}
    if len(times) != len(clip["rotation_tracks"][0]) or times[0] != 0 or times[-1] != dur:
        raise ValueError(f"DLS26 {src_id}: sample times disagree with SAT")
    if root and len(root) != len(times):
        raise ValueError(f"DLS26 {src_id}: root curve length differs from sample count")
    _D26[src_id] = out
    return out


def interp_curve(times: list[int], values: list, tau: float):
    """Piecewise-linear interpolation of scalar or tuple samples at source time tau."""
    if not values:
        return None
    if tau <= times[0]:
        return values[0]
    if tau >= times[-1]:
        return values[-1]
    from bisect import bisect_right
    r = bisect_right(times, tau)
    l = r - 1
    f = (tau - times[l]) / (times[r] - times[l])
    a, b = values[l], values[r]
    if isinstance(a, (tuple, list)):
        return tuple(a[i] * (1 - f) + b[i] * f for i in range(len(a)))
    return a * (1 - f) + b * f
