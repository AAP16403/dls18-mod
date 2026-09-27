#!/usr/bin/env python3
"""Decode the player FTM skin records and render skinned SAT poses."""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from matplotlib import pyplot as plt
from mpl_toolkits.mplot3d.art3d import Poly3DCollection

from decode_assets import BONE_REMAP, decode_sat
from preview_sat_skeleton import DEFAULT_MODEL, read_rig, transform_matrix


ROOT = Path(__file__).resolve().parents[1]
ANIM_ROOT = ROOT / "unpacked" / "kpx" / "anims"
DEFAULT_OUT = ROOT / "previews" / "models"
WEIGHT_BLOCK = 0x24
GEOMETRY_BLOCK = 0x11
WEIGHT_RECORD_BYTES = 56


@dataclass
class SkinnedMesh:
    name: str
    positions: np.ndarray
    normals: np.ndarray
    faces: np.ndarray
    bone_indices: np.ndarray
    bone_weights: np.ndarray
    influence_positions: np.ndarray
    bounds: np.ndarray


def read_blocks(path: Path) -> list[tuple[int, int, int, bytes]]:
    data = path.read_bytes()
    if len(data) < 24 or data[8:12] != b"FTTM":
        raise ValueError(f"{path} is not a supported FTTM file")
    blocks = []
    offset = 24
    while offset + 8 <= len(data):
        kind, version, size = struct.unpack_from("<HHI", data, offset)
        if size < 8 or offset + size > len(data):
            raise ValueError(f"invalid FTM block at {offset:#x}: size {size}")
        blocks.append((kind, version, offset, data[offset + 8:offset + size]))
        offset += size
    if offset != len(data):
        raise ValueError(f"{len(data) - offset} trailing bytes after FTM blocks")
    return blocks


def read_skinned_meshes(path: Path) -> list[SkinnedMesh]:
    blocks = read_blocks(path)
    meshes: list[SkinnedMesh] = []
    weights_pending: tuple[int, bytes] | None = None
    part_number = 0
    for kind, _version, offset, payload in blocks:
        if kind == WEIGHT_BLOCK:
            weights_pending = (offset, payload)
        elif kind == GEOMETRY_BLOCK and weights_pending is not None:
            weight_offset, weight_payload = weights_pending
            weights_pending = None
            if len(weight_payload) < 4 or (len(weight_payload) - 4) % WEIGHT_RECORD_BYTES:
                raise ValueError(f"skin block at {weight_offset:#x} has an invalid record count")
            vertex_count = (len(weight_payload) - 4) // WEIGHT_RECORD_BYTES
            if len(payload) < 0x54:
                raise ValueError(f"geometry block at {offset:#x} is truncated")
            header_vertex_count, triangle_count = struct.unpack_from("<HH", payload, 4)
            if header_vertex_count != vertex_count:
                raise ValueError(
                    f"skin/mesh vertex mismatch at {offset:#x}: {vertex_count} != {header_vertex_count}"
                )
            position_offset = struct.unpack_from("<I", payload, 0x14)[0]
            normal_offset = struct.unpack_from("<I", payload, 0x18)[0]
            index_offset = struct.unpack_from("<I", payload, 0x2C)[0]
            positions_end = position_offset + vertex_count * 12
            normals_end = normal_offset + vertex_count * 12
            if max(positions_end, normals_end) > len(payload):
                raise ValueError(f"geometry streams exceed block at {offset:#x}")
            positions = np.frombuffer(payload, dtype="<f4", count=vertex_count * 3, offset=position_offset).reshape(-1, 3).copy()
            normals = np.frombuffer(payload, dtype="<f4", count=vertex_count * 3, offset=normal_offset).reshape(-1, 3).copy()
            index_count = triangle_count * 3
            if index_offset + index_count * 2 > len(payload):
                raise ValueError(f"index stream exceeds geometry block at {offset:#x}")
            indices = np.frombuffer(payload, dtype="<u2", count=index_count, offset=index_offset).astype(np.int64)
            valid = indices < vertex_count
            # All four original sections carry a two-value trailer after the final
            # complete triangle. Keep only the complete valid triangles for preview.
            complete_count = (int(valid.sum()) // 3) * 3
            if complete_count == 0 or not valid[:complete_count].all():
                raise ValueError(f"geometry block at {offset:#x} has invalid triangle indices")
            faces = indices[:complete_count].reshape(-1, 3)

            skin = np.frombuffer(weight_payload, dtype=np.uint8, offset=4).reshape(vertex_count, WEIGHT_RECORD_BYTES)
            bone_indices = skin[:, :4].copy()
            bone_weights = np.ndarray(
                (vertex_count, 4), dtype="<f4", buffer=weight_payload,
                offset=4 + 4, strides=(WEIGHT_RECORD_BYTES, 4),
            ).copy()
            # The first influence uses the geometry position; the three other
            # influence-local positions are stored after the four bone weights.
            influence_positions = np.zeros((vertex_count, 4, 3), dtype=np.float64)
            influence_positions[:, 0] = positions
            for influence in range(1, 4):
                start = 4 + 20 + (influence - 1) * 12
                influence_positions[:, influence] = np.ndarray(
                    (vertex_count, 3), dtype="<f4", buffer=weight_payload,
                    offset=start, strides=(WEIGHT_RECORD_BYTES, 4),
                )

            if np.any(bone_weights < -1e-6) or not np.isfinite(bone_weights).all():
                raise ValueError(f"invalid skin weights at {weight_offset:#x}")
            active = bone_weights > 1e-6
            if np.any(bone_indices[active] >= 42):
                raise ValueError(f"skin block at {weight_offset:#x} names a bone outside the 42-bone rig")
            bounds = np.asarray(struct.unpack_from("<6f", payload, 0x34), dtype=float).reshape(2, 3)
            meshes.append(SkinnedMesh(
                name=f"body_part_{part_number}", positions=positions, normals=normals,
                faces=faces, bone_indices=bone_indices, bone_weights=bone_weights,
                influence_positions=influence_positions, bounds=bounds,
            ))
            part_number += 1
    if not meshes:
        raise ValueError("no paired player skin and geometry blocks were found")
    return meshes


def build_world_matrices(
    model: Path,
    animation_path: Path | None = None,
    frame: int = 0,
    composition_order: str = "local-parent",
) -> tuple[list[np.ndarray], dict]:
    root, links, _names, bind_locals = read_rig(model)
    local_matrices = [transform_matrix(bind[3:7], bind[7:10], bind[:3]) for bind in bind_locals]
    clip = None
    if animation_path is not None:
        header, body = decode_sat(animation_path)
        if body["payload_status"] != "decoded":
            raise ValueError(f"{animation_path.name} has no inline transform samples")
        if not 0 <= frame < header["frame_count"]:
            raise ValueError(f"frame {frame} is outside {animation_path.name}")
        for packed_track, slot in enumerate(header["active_transform_slots"]):
            bone = BONE_REMAP[slot]
            record = body["transform_records_raw"][packed_track][frame]
            bind = bind_locals[bone]
            quaternion = [component / 16384.0 for component in record[:4]]
            translation = [component / 128.0 for component in record[4:7]]
            local_matrices[bone] = transform_matrix(quaternion, translation, bind[:3])
        clip = {"header": header, "body": body}

    world: list[np.ndarray | None] = [None] * len(links)
    visited: set[int] = set()

    def visit(node: int, parent: int | None) -> None:
        if node == 0xFF:
            return
        if node >= len(links) or node in visited:
            raise ValueError(f"invalid or cyclic hierarchy reference {node}")
        visited.add(node)
        if parent is None:
            world[node] = local_matrices[node]
        elif composition_order == "local-parent":
            world[node] = local_matrices[node] @ world[parent]
        else:
            world[node] = world[parent] @ local_matrices[node]
        child, _sibling = links[node]
        while child != 0xFF:
            next_sibling = links[child][1]
            visit(child, node)
            child = next_sibling

    visit(root, None)
    result = [matrix if matrix is not None else np.eye(4) for matrix in world]
    return result, clip


def skin_vertices(mesh: SkinnedMesh, matrices: list[np.ndarray]) -> np.ndarray:
    vertices = np.zeros((len(mesh.positions), 3), dtype=np.float64)
    for influence in range(4):
        weights = mesh.bone_weights[:, influence]
        active = weights > 1e-7
        if not active.any():
            continue
        bone_ids = mesh.bone_indices[active, influence]
        source = mesh.influence_positions[active, influence]
        homogeneous = np.column_stack((source, np.ones(len(source))))
        transformed = np.empty_like(source)
        for bone in np.unique(bone_ids):
            mask = bone_ids == bone
            transformed[mask] = (homogeneous[mask] @ matrices[int(bone)])[:, :3]
        vertices[active] += transformed * weights[active, None]
    return vertices


def render_pose(meshes: list[SkinnedMesh], vertices: list[np.ndarray], out: Path,
                title: str, side: str = "both") -> None:
    colors = ["#263f5c", "#607d8b", "#b17a56", "#8b6e8d", "#888888"]
    views = ["front", "side"] if side == "both" else [side]
    fig = plt.figure(figsize=(6.3 * len(views), 8.4))
    all_vertices = np.concatenate(vertices, axis=0)
    bounds = np.stack((all_vertices.min(axis=0), all_vertices.max(axis=0)))
    extent = bounds[1] - bounds[0]
    margin = np.maximum(extent * 0.04, 2.0)
    bounds[0] -= margin
    bounds[1] += margin
    axes = [fig.add_subplot(1, len(views), index + 1, projection="3d")
            for index in range(len(views))]
    for ax, view in zip(axes, views):
        for index, (mesh, posed) in enumerate(zip(meshes, vertices)):
            collection = Poly3DCollection(
                posed[mesh.faces], facecolors=colors[index % len(colors)],
                edgecolors="#17283a", linewidths=0.035, alpha=1.0,
                zsort="average", shade=True,
            )
            ax.add_collection3d(collection)
        ax.set_xlim(bounds[0, 0], bounds[1, 0])
        ax.set_ylim(bounds[0, 1], bounds[1, 1])
        ax.set_zlim(bounds[0, 2], bounds[1, 2])
        ax.set_box_aspect(extent + 2 * margin)
        ax.set_proj_type("ortho")
        ax.view_init(elev=0, azim=-90 if view == "front" else 0)
        ax.axis("off")
        ax.set_title(view.capitalize(), fontsize=12)
    fig.suptitle(title, fontsize=14, y=0.98)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    out.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out, dpi=180, bbox_inches="tight", facecolor="white")
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--clip", type=int)
    parser.add_argument("--frame", type=int, default=0)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--composition", choices=("local-parent", "parent-local"), default="local-parent")
    parser.add_argument("--view", choices=("both", "front", "side"), default="both")
    args = parser.parse_args()

    meshes = read_skinned_meshes(args.model)
    anim_path = None if args.clip is None else ANIM_ROOT / f"{args.clip}.sat"
    matrices, clip = build_world_matrices(args.model, anim_path, args.frame, args.composition)
    vertices = [skin_vertices(mesh, matrices) for mesh in meshes]
    for mesh, posed in zip(meshes, vertices):
        active = mesh.bone_weights > 1e-6
        sum_error = np.abs(mesh.bone_weights.sum(axis=1) - 1).max()
        print(f"{mesh.name}: {len(mesh.positions)} vertices, {len(mesh.faces)} triangles, "
              f"{active.sum(axis=1).mean():.2f} influences/vertex, max weight-sum error {sum_error:.6g}")
        print("  posed bounds:", np.round(posed.min(axis=0), 2).tolist(), np.round(posed.max(axis=0), 2).tolist())
    if args.out:
        out = args.out
    elif clip is None:
        out = DEFAULT_OUT / "player_bind_pose.png"
    else:
        out = DEFAULT_OUT / f"player_{args.clip}_frame_{args.frame:02d}.png"
    title = "DLS18 player mesh · bind pose" if clip is None else f"DLS18 player mesh · animation {args.clip} · frame {args.frame}"
    render_pose(meshes, vertices, out, title, args.view)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
