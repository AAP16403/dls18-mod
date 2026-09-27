#!/usr/bin/env python3
"""Top-view previews of ported clips: stock reference vs v15 vs v16.

Each panel shows, seen from above with forward pointing up (cm from the clip start):
  grey line      root path from the animation database curve
  orange / blue  right / left foot paths (FK of the SAT transforms + root + heading curve);
                 thick where the foot is planted (within 2 cm of its lowest height)
  stars          action points (ball positions) with their 30 Hz tick
  crosses        foot positions at the first action point tick
Usage: python preview_dls26_tuning.py [--clips 331 2535 2553 2547] [--out PNG]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from anim_tune_lib import ROOT, V15_PAK, V16_PAK, V30_PAK  # noqa: E402
from tune_dls26_ports import PORTS  # noqa: E402
from validate_dls26_ports import FEET, Version, pose_fine, world  # noqa: E402


def to_plot(p):
    # action-point coords (x = -forward, z = left) -> (right, forward) in cm
    return -p[2] / 16.0, -p[0] / 16.0


def panel(ax, v, clip, title):
    db = v.db
    dur = db.duration(clip)
    fr = np.linspace(0, 1, int(dur) + 1)
    root = []
    for f in fr:
        if db.u16(clip, 0xC) & 0x1000 and db.root_curve(clip):
            rx, rz = db.root_at_tick(clip, f * dur)
        else:
            rx = rz = 0.0
        root.append(to_plot((-rx, 0, rz)))
    root = np.array(root)
    ax.plot(root[:, 0], root[:, 1], color="#9aa5b1", lw=1.2, label="root")
    s = v.clip(clip)
    for (foot, toe), col in zip(FEET, ("#e2763b", "#2e7dce")):
        pts = np.array([to_plot(world(v, clip, foot, f)) for f in fr])
        zs = np.array([min(pose_fine(s, f)[foot][2], pose_fine(s, f)[toe][2]) for f in fr])
        planted = zs < zs.min() + 64
        ax.plot(pts[:, 0], pts[:, 1], color=col, lw=0.8, alpha=0.7)
        for j in range(1, len(fr)):
            if planted[j] and planted[j - 1]:
                ax.plot(pts[j - 1:j + 1, 0], pts[j - 1:j + 1, 1], color=col, lw=3.0)
    aps = db.action_points(clip)
    for k, (t, x, y, z) in enumerate(aps):
        px, py = to_plot((x, y, z))
        ax.plot(px, py, marker="*", ms=11, color="#2a9d4b", zorder=5)
        ax.annotate(f"t{t}", (px, py), textcoords="offset points", xytext=(4, 4), fontsize=7, color="#1d6b35")
    if aps:
        fa = 2 * aps[0][0] / dur
        for foot, toe in FEET:
            for slot in (foot, toe):
                px, py = to_plot(world(v, clip, slot, fa))
                ax.plot(px, py, marker="x", ms=6, color="#333333", zorder=6)
    ax.set_title(title, fontsize=8)
    ax.set_aspect("equal", adjustable="datalim")
    ax.grid(True, lw=0.3, alpha=0.5)
    ax.tick_params(labelsize=6)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clips", type=int, nargs="+", default=[331, 2535, 2553, 2557, 2547, 737])
    ap.add_argument("--out", type=Path, default=ROOT / "previews" / "animations" / "dls26_tuned_v16_topview.png")
    args = ap.parse_args()
    vs = [("stock", Version("stock", V30_PAK)), ("v15", Version("v15", V15_PAK)), ("v16", Version("v16", V16_PAK))]
    ports = {p["id"]: p for p in PORTS}
    fig, axes = plt.subplots(len(args.clips), 3, figsize=(11, 3.6 * len(args.clips)), dpi=130)
    for row, cid in enumerate(args.clips):
        p = ports[cid]
        ref = p.get("tmpl", cid)
        for col, (name, v) in enumerate(vs):
            clip = ref if name == "stock" else cid
            label = f"{name} {clip}" + (f" (template of {cid})" if name == "stock" and ref != cid else "")
            if name != "stock":
                label += f" <- DLS26 {p['src']}"
            panel(axes[row][col], v, clip, label)
    fig.suptitle("Top view, forward up, cm: root path (grey), R/L foot (orange/blue, thick = planted), "
                 "action points (stars), feet at first action point (x)", fontsize=9)
    fig.tight_layout(rect=(0, 0, 1, 0.98))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.out)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
