#!/usr/bin/env python3
"""Render a stick-skeleton comparison from DLS18 SAT samples and the player FTM rig."""

from __future__ import annotations

import argparse
import csv
import struct
from pathlib import Path

import matplotlib.pyplot as plt
from PIL import Image, ImageDraw

from decode_assets import BONE_REMAP, decode_sat

ROOT = Path(__file__).resolve().parents[1]
ANIM_ROOT = ROOT / "unpacked" / "kpx" / "anims"
DEFAULT_MODEL = ROOT / "unpacked" / "kpx" / "models" / "player" / "body" / "body_0_1.ftm"


def read_rig(path: Path) -> tuple[int, list[tuple[int, int]], dict[int, str], list[tuple[tuple[float, ...], tuple[float, ...], tuple[float, ...]]]]:
    data = path.read_bytes()
    if len(data) < 24 or data[8:12] != b"FTTM":
        raise ValueError(f"{path} is not a supported FTTM file")

    offset = 24
    root = None
    links = None
    names: dict[int, str] = {}
    bind_locals = None
    while offset + 8 <= len(data):
        block_type, _version, block_size = struct.unpack_from("<HHI", data, offset)
        if block_size < 8 or offset + block_size > len(data):
            raise ValueError(f"invalid FTM block at {offset:#x}: size {block_size}")
        payload = offset + 8
        if block_type == 0x25 and links is None:  # CFTT_FTMLoader::LoadHierarchy
            bone_count, root = struct.unpack_from("<HH", data, payload)
            pair_offset = payload + 8
            if pair_offset + bone_count * 2 != offset + block_size:
                raise ValueError("hierarchy block size does not match its node count")
            links = [tuple(data[pair_offset + i * 2:pair_offset + i * 2 + 2]) for i in range(bone_count)]
        elif block_type == 0x1C and bind_locals is None:
            # This record is the static local transform: scale XYZ, quaternion XYZW,
            # then translation XYZ as floats. An 8-byte record prefix precedes each
            # transform, which supplies channels omitted by SAT.
            bone_count = 42
            record_offset = payload + 8
            if payload + bone_count * 48 <= offset + block_size:
                bind_locals = [struct.unpack_from("<3f4f3f", data, record_offset + i * 48)
                               for i in range(bone_count)]
        elif block_type == 0x1E:
            bone = struct.unpack_from("<H", data, payload)[0]
            name = data[payload + 2:offset + block_size].split(b"\0", 1)[0].decode("ascii", "replace")
            names[bone] = name
        offset += block_size
    if links is None or root is None:
        raise ValueError("player FTM has no hierarchy block (type 0x25)")
    if bind_locals is None or len(bind_locals) != len(links):
        raise ValueError("player FTM has no 42-bone static transform block (type 0x1c)")
    if len(names) != len(links):
        raise ValueError(f"player FTM has names for {len(names)} of {len(links)} bones")
    return root, links, names, bind_locals


def transform_matrix(quaternion, translation, scale):
    """Recreate CFTTQuaternion32::GetMatrixEv layout; vectors are row vectors."""
    import numpy as np

    x, y, z, w = quaternion
    norm = (x * x + y * y + z * z + w * w) ** 0.5
    if norm <= 1e-9:
        x = y = z = 0.0
        w = 1.0
    else:
        x, y, z, w = x / norm, y / norm, z / norm, w / norm

    matrix = np.eye(4, dtype=float)
    rotation = np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ])
    matrix[:3, :3] = np.diag(scale) @ rotation
    matrix[3, :3] = translation
    return matrix


def sample_world(header: dict, body: dict, frame: int, root: int, links: list[tuple[int, int]],
                 bind_locals, order: str = "local-parent"):
    import numpy as np

    locals_ = [transform_matrix(bind[3:7], bind[7:10], bind[:3]) for bind in bind_locals]
    active = set()
    records = body["transform_records_raw"]
    for packed_track, slot in enumerate(header["active_transform_slots"]):
        bone = BONE_REMAP[slot]
        record = records[packed_track][frame]
        bind = bind_locals[bone]
        quaternion = [value / 16384.0 for value in record[:4]]
        translation = [value / 128.0 for value in record[4:7]]
        locals_[bone] = transform_matrix(quaternion, translation, bind[:3])
        active.add(bone)

    worlds = [None] * len(links)
    edges: list[tuple[int, int]] = []
    visiting: set[int] = set()

    def visit(node: int, parent: int | None) -> None:
        if node == 0xFF:
            return
        if node >= len(links) or node in visiting:
            raise ValueError(f"invalid or cyclic hierarchy reference {node}")
        visiting.add(node)
        if parent is None:
            worlds[node] = locals_[node]
        else:
            edges.append((parent, node))
            if order == "parent-local":
                worlds[node] = worlds[parent] @ locals_[node]
            else:
                worlds[node] = locals_[node] @ worlds[parent]

        child, sibling = links[node]
        while child != 0xFF:
            next_sibling = links[child][1]
            visit(child, node)
            child = next_sibling
        visiting.remove(node)

    visit(root, None)
    for i, world in enumerate(worlds):
        if world is None:
            worlds[i] = np.eye(4, dtype=float)
    points = [matrix[3, :3].copy() for matrix in worlds]
    return points, edges, active


def make_preview(clip_ids: list[int], model: Path, out: Path, sat_dir: Path = ANIM_ROOT) -> None:
    root, links, names, bind_locals = read_rig(model)
    frames_by_clip = []
    for clip_id in clip_ids:
        path = sat_dir / f"{clip_id:04d}.sat"
        header, body = decode_sat(path)
        if body["payload_status"] != "decoded":
            raise ValueError(f"{clip_id:04d}.sat: {body['payload_status']}")
        frame_count = header["frame_count"]
        indices = sorted(set((0, frame_count // 2, frame_count - 1)))
        frames_by_clip.append((clip_id, header, body, indices))

    fig = plt.figure(figsize=(12, max(11, 2.0 * len(frames_by_clip))), dpi=160)
    grid = fig.add_gridspec(len(frames_by_clip), 3, wspace=0.06, hspace=0.15)
    for row, (clip_id, header, body, indices) in enumerate(frames_by_clip):
        for frame_col, frame_index in enumerate(indices):
            ax = fig.add_subplot(grid[row, frame_col])
            points, edges, active = sample_world(header, body, frame_index, root, links, bind_locals)
            for parent, child in edges:
                a, b = points[parent], points[child]
                if sum((a[i] - b[i]) ** 2 for i in range(3)) < 0.04:
                    continue
                names_pair = names[parent] + names[child]
                color = "#2e7dce" if " L " in names_pair else "#e2763b" if " R " in names_pair else "#28455f"
                ax.plot([a[0], b[0]], [a[2], b[2]], color=color, linewidth=1.8)
            visible = [i for i in active if i < len(points)]
            if visible:
                coords = [points[i] for i in visible]
                ax.scatter([p[0] for p in coords], [p[2] for p in coords],
                           s=9, color="#f5a623", zorder=4)
            for bone in (2, 21, 39, 41, 8, 27, 11, 30):
                p = points[bone]
                label = names[bone].replace("Bip01 ", "")
                ax.text(p[0], p[2], label, fontsize=5.0, color="#40556b")
            ax.set_xlim(-90, 90)
            ax.set_ylim(-10, 170)
            ax.set_aspect("equal", adjustable="box")
            ax.axis("off")
            ax.set_title(f"frame {frame_index}/{header['frame_count'] - 1}", fontsize=9, pad=1)
            if frame_col == 0:
                ax.text(0.02, 0.96, f"{clip_id:04d}", transform=ax.transAxes,
                        fontsize=9, color="#20384b")

    fig.suptitle("DLS18 dribble clips on the named 42-bone player rig\nAmber points mark bones animated by the SAT clip",
                 fontsize=13)
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, bbox_inches="tight", pad_inches=0.06)
    plt.close(fig)
    print(f"wrote {out}")


def export_rig_csv(model: Path, out: Path) -> None:
    root, links, names, bind_locals = read_rig(model)
    parents: dict[int, int | None] = {root: None}

    def visit(node: int) -> None:
        child, _sibling = links[node]
        while child != 0xFF:
            parents[child] = node
            visit(child)
            child = links[child][1]

    visit(root)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["bone_index", "name", "parent_index", "first_child", "next_sibling",
                         "animation_slots", "scale_x", "scale_y", "scale_z", "quat_x", "quat_y",
                         "quat_z", "quat_w", "translation_x", "translation_y", "translation_z"])
        for bone, ((child, sibling), bind) in enumerate(zip(links, bind_locals)):
            slots = [str(slot) for slot, mapped_bone in enumerate(BONE_REMAP) if mapped_bone == bone]
            values = [round(value, 7) for value in bind[:10]]
            writer.writerow([bone, names[bone], "" if parents.get(bone) is None else parents[bone],
                             "" if child == 0xFF else child, "" if sibling == 0xFF else sibling,
                             ";".join(slots), *values])
    print(f"wrote {out}")


def make_clip_gif(clip_id: int, model: Path, out: Path, sat_dir: Path = ANIM_ROOT) -> None:
    """Render every SAT keyframe in front and side projection for motion review."""
    from decode_assets import decode_sat

    root, links, names, bind_locals = read_rig(model)
    header, body = decode_sat(sat_dir / f"{clip_id:04d}.sat")
    if body["payload_status"] != "decoded":
        raise ValueError(f"{clip_id:04d}.sat: {body['payload_status']}")

    animation_name = str(clip_id)
    names_csv = ROOT / "catalog" / "animation_names.csv"
    if names_csv.is_file():
        with names_csv.open(newline="", encoding="utf-8") as stream:
            animation_name = next((row["animation_name"] for row in csv.DictReader(stream)
                                   if int(row["animation_id"]) == clip_id), animation_name)

    width, height, scale = 800, 500, 1.9
    centers = (205, 595)
    floor_y = 425
    key_labels = {2: "Head", 21: "Pelvis", 11: "L hand", 30: "R hand", 8: "L foot", 27: "R foot"}
    frames: list[Image.Image] = []
    for frame_index in range(header["frame_count"]):
        points, edges, active = sample_world(header, body, frame_index, root, links, bind_locals)
        canvas = Image.new("RGB", (width, height), "white")
        draw = ImageDraw.Draw(canvas)
        draw.text((24, 12), f"{clip_id:04d}  {animation_name}", fill="#18334b")
        draw.text((24, 34), f"frame {frame_index + 1}/{header['frame_count']}   "
                             f"{frame_index * header['sample_interval_seconds']:.2f} s",
                  fill="#576b7b")
        for panel, axis, heading in ((0, 0, "Front (X / Z)"), (1, 1, "Side (Y / Z)")):
            center_x = centers[panel]
            draw.text((center_x - 44, 60), heading, fill="#576b7b")
            draw.line((center_x - 150, floor_y, center_x + 150, floor_y), fill="#d9e1e8", width=1)

            def project(bone: int) -> tuple[float, float]:
                p = points[bone]
                return center_x + float(p[axis]) * scale, floor_y - float(p[2]) * scale

            for parent, child in edges:
                a, b = points[parent], points[child]
                if sum((a[i] - b[i]) ** 2 for i in range(3)) < 0.04:
                    continue
                pair = names[parent] + names[child]
                color = "#2e7dce" if " L " in pair else "#e2763b" if " R " in pair else "#28455f"
                draw.line((*project(parent), *project(child)), fill=color, width=4)
            for bone in range(len(points)):
                x, y = project(bone)
                radius = 4 if bone in active else 2
                fill = "#f5a623" if bone in active else "#28455f"
                draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=fill)
            if panel == 0:
                for bone, label in key_labels.items():
                    x, y = project(bone)
                    draw.text((x + 5, y - 8), label, fill="#40556b")
        frames.append(canvas)

    out.parent.mkdir(parents=True, exist_ok=True)
    duration_ms = max(1, round(header["sample_interval_seconds"] * 1000))
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=duration_ms,
                   loop=0, optimize=False, disposal=2)
    print(f"wrote {out}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clips", type=int, nargs="+", default=[2265, 2285, 2287, 2289])
    parser.add_argument("--sat-dir", type=Path, default=ANIM_ROOT,
                        help="directory containing the SAT clips to preview")
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--out", type=Path, default=ROOT / "previews" / "animations" / "dribble_skill_pose_grid.png")
    parser.add_argument("--gifs", action="store_true", help="also render one front/side animated GIF per clip")
    parser.add_argument("--rig-csv", type=Path, default=ROOT / "decoded_assets" / "models" / "player_rig_hierarchy.csv")
    args = parser.parse_args()
    make_preview(args.clips, args.model, args.out, args.sat_dir)
    export_rig_csv(args.model, args.rig_csv)
    if args.gifs:
        for clip_id in args.clips:
            make_clip_gif(clip_id, args.model, args.out.parent / f"dribble_{clip_id:04d}.gif", args.sat_dir)


if __name__ == "__main__":
    main()
