#!/usr/bin/env python3
"""Generate the known-good procedural animation reference rig as a self-contained GLB.

The file uses only Python's standard library so it can be regenerated on any
engine checkout without Blender. Import the output into Blender when an editable
reference is useful, or load it directly through Zahlen's ModelPrefab importer.
"""

from __future__ import annotations

import json
import math
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "resources" / "assets" / "ProceduralAnimationBaseRig.glb"

nodes: list[dict] = []
parents: list[int] = []
bone_names: list[str] = []
bone_by_name: dict[str, int] = {}


def add_bone(name: str, parent: int, translation: tuple[float, float, float]) -> int:
    index = len(nodes)
    node = {"name": name, "translation": list(translation)}
    nodes.append(node)
    parents.append(parent)
    bone_names.append(name)
    bone_by_name[name] = index
    if parent >= 0:
        nodes[parent].setdefault("children", []).append(index)
    return index


root = add_bone("Root", -1, (0.0, 0.0, 0.0))
hips = add_bone("DEF-Hips", root, (0.0, 1.0, 0.0))
spine = add_bone("DEF-Spine", hips, (0.0, 0.15, 0.0))
sup_spine = add_bone("DEF-Sup_Spine", spine, (0.0, 0.16, 0.0))
chest = add_bone("DEF-Chest", sup_spine, (0.0, 0.17, 0.0))
neck = add_bone("DEF-Neck", chest, (0.0, 0.16, 0.0))
head = add_bone("DEF-Head", neck, (0.0, 0.12, 0.0))

upper_arm_l = add_bone("DEF-Upper_arm.L", chest, (0.21, 0.10, 0.0))
forearm_l = add_bone("DEF-Forearm.L", upper_arm_l, (0.28, 0.0, 0.0))
hand_l = add_bone("DEF-Hand.L", forearm_l, (0.24, 0.0, 0.0))
upper_arm_r = add_bone("DEF-Upper_arm.R", chest, (-0.21, 0.10, 0.0))
forearm_r = add_bone("DEF-Forearm.R", upper_arm_r, (-0.28, 0.0, 0.0))
hand_r = add_bone("DEF-Hand.R", forearm_r, (-0.24, 0.0, 0.0))

thigh_l = add_bone("DEF-Thigh.L", hips, (0.12, -0.06, 0.0))
shin_l = add_bone("DEF-Shin.L", thigh_l, (0.0, -0.43, 0.015))
foot_l = add_bone("DEF-Foot.L", shin_l, (0.0, -0.42, 0.025))
toe_l = add_bone("DEF-Toe.L", foot_l, (0.0, -0.045, 0.18))
thigh_r = add_bone("DEF-Thigh.R", hips, (-0.12, -0.06, 0.0))
shin_r = add_bone("DEF-Shin.R", thigh_r, (0.0, -0.43, 0.015))
foot_r = add_bone("DEF-Foot.R", shin_r, (0.0, -0.42, 0.025))
toe_r = add_bone("DEF-Toe.R", foot_r, (0.0, -0.045, 0.18))

hair_bones: list[int] = []
for strand in range(18):
    angle = math.tau * strand / 18.0
    parent = head
    for link in range(6):
        translation = (
            (math.cos(angle) * 0.15, 0.075, math.sin(angle) * 0.15)
            if link == 0
            else (0.0, -0.105, 0.0)
        )
        parent = add_bone(
            f"DEF-Hair_S{strand + 1:02d}_{link + 1:02d}", parent, translation
        )
        hair_bones.append(parent)


def global_position(index: int) -> tuple[float, float, float]:
    x = y = z = 0.0
    cursor = index
    depth = 0
    while cursor >= 0 and depth <= len(nodes):
        tx, ty, tz = nodes[cursor].get("translation", (0.0, 0.0, 0.0))
        x += tx
        y += ty
        z += tz
        cursor = parents[cursor]
        depth += 1
    return (x, y, z)


globals_ = [global_position(i) for i in range(len(bone_names))]
positions: list[float] = []
normals: list[float] = []
texcoords: list[float] = []
colors: list[float] = []
joints: list[int] = []
weights: list[float] = []
indices: list[int] = []

BOX_INDICES = (
    0, 2, 1, 1, 2, 3,
    4, 5, 6, 5, 7, 6,
    0, 1, 4, 1, 5, 4,
    2, 6, 3, 3, 6, 7,
    0, 4, 2, 2, 4, 6,
    1, 3, 5, 3, 7, 5,
)


def add_box(
    center: tuple[float, float, float],
    half: tuple[float, float, float],
    joint: int,
    color: tuple[float, float, float, float],
) -> None:
    base = len(positions) // 3
    cx, cy, cz = center
    hx, hy, hz = half
    for x_sign, y_sign, z_sign in (
        (-1, -1, -1), (1, -1, -1), (-1, 1, -1), (1, 1, -1),
        (-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1),
    ):
        positions.extend((cx + x_sign * hx, cy + y_sign * hy, cz + z_sign * hz))
        inv_len = 1.0 / math.sqrt(3.0)
        normals.extend((x_sign * inv_len, y_sign * inv_len, z_sign * inv_len))
        texcoords.extend(((x_sign + 1) * 0.5, (y_sign + 1) * 0.5))
        colors.extend(color)
        joints.extend((joint, 0, 0, 0))
        weights.extend((1.0, 0.0, 0.0, 0.0))
    indices.extend(base + index for index in BOX_INDICES)


def midpoint(a: int, b: int) -> tuple[float, float, float]:
    pa, pb = globals_[a], globals_[b]
    return tuple((pa[i] + pb[i]) * 0.5 for i in range(3))


def segment_box(a: int, b: int, radius: float, color: tuple[float, float, float, float]) -> None:
    pa, pb = globals_[a], globals_[b]
    half = tuple(max(abs(pb[i] - pa[i]) * 0.5, radius) for i in range(3))
    add_box(midpoint(a, b), half, a, color)


body_color = (0.32, 0.72, 0.92, 1.0)
limb_color = (0.92, 0.60, 0.24, 1.0)
hair_color = (0.72, 0.18, 0.86, 1.0)
add_box(globals_[hips], (0.19, 0.11, 0.13), hips, body_color)
segment_box(spine, sup_spine, 0.13, body_color)
segment_box(sup_spine, chest, 0.16, body_color)
segment_box(chest, neck, 0.12, body_color)
add_box((globals_[head][0], globals_[head][1] + 0.11, globals_[head][2]), (0.14, 0.16, 0.13), head, body_color)
for a, b in ((upper_arm_l, forearm_l), (forearm_l, hand_l), (upper_arm_r, forearm_r), (forearm_r, hand_r)):
    segment_box(a, b, 0.055, limb_color)
add_box(globals_[hand_l], (0.09, 0.045, 0.075), hand_l, limb_color)
add_box(globals_[hand_r], (0.09, 0.045, 0.075), hand_r, limb_color)
for a, b in ((thigh_l, shin_l), (shin_l, foot_l), (thigh_r, shin_r), (shin_r, foot_r)):
    segment_box(a, b, 0.075, limb_color)
add_box((globals_[foot_l][0], globals_[foot_l][1], globals_[foot_l][2] + 0.08), (0.10, 0.055, 0.16), foot_l, limb_color)
add_box((globals_[foot_r][0], globals_[foot_r][1], globals_[foot_r][2] + 0.08), (0.10, 0.055, 0.16), foot_r, limb_color)
for bone in hair_bones:
    position = globals_[bone]
    add_box((position[0], position[1] - 0.045, position[2]), (0.018, 0.055, 0.018), bone, hair_color)

blob = bytearray()
buffer_views: list[dict] = []
accessors: list[dict] = []


def append_view(data: bytes, target: int | None = None) -> int:
    while len(blob) % 4:
        blob.append(0)
    offset = len(blob)
    blob.extend(data)
    view = {"buffer": 0, "byteOffset": offset, "byteLength": len(data)}
    if target is not None:
        view["target"] = target
    buffer_views.append(view)
    return len(buffer_views) - 1


def accessor(data: bytes, component_type: int, count: int, kind: str, target: int | None = None, **extra: object) -> int:
    view = append_view(data, target)
    value = {"bufferView": view, "componentType": component_type, "count": count, "type": kind}
    value.update(extra)
    accessors.append(value)
    return len(accessors) - 1


def pack_floats(values: list[float]) -> bytes:
    return struct.pack(f"<{len(values)}f", *values)


def pack_ushorts(values: list[int]) -> bytes:
    return struct.pack(f"<{len(values)}H", *values)


vertex_count = len(positions) // 3
position_accessor = accessor(
    pack_floats(positions), 5126, vertex_count, "VEC3", 34962,
    min=[min(positions[i::3]) for i in range(3)],
    max=[max(positions[i::3]) for i in range(3)],
)
normal_accessor = accessor(pack_floats(normals), 5126, vertex_count, "VEC3", 34962)
uv_accessor = accessor(pack_floats(texcoords), 5126, vertex_count, "VEC2", 34962)
color_accessor = accessor(pack_floats(colors), 5126, vertex_count, "VEC4", 34962)
joint_accessor = accessor(pack_ushorts(joints), 5123, vertex_count, "VEC4", 34962)
weight_accessor = accessor(pack_floats(weights), 5126, vertex_count, "VEC4", 34962)
index_accessor = accessor(pack_ushorts(indices), 5123, len(indices), "SCALAR", 34963)

inverse_bind_matrices: list[float] = []
for x, y, z in globals_:
    inverse_bind_matrices.extend((
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        -x, -y, -z, 1.0,
    ))
ibm_accessor = accessor(pack_floats(inverse_bind_matrices), 5126, len(bone_names), "MAT4")

mesh_node = len(nodes)
nodes.append({"name": "ProceduralBaseRigMesh", "mesh": 0, "skin": 0})

document = {
    "asset": {"version": "2.0", "generator": "Zahlen procedural base-rig generator"},
    "scene": 0,
    "scenes": [{"name": "ProceduralAnimationBaseRig", "nodes": [root, mesh_node]}],
    "nodes": nodes,
    "skins": [{
        "name": "ProceduralAnimationBaseSkin",
        "inverseBindMatrices": ibm_accessor,
        "skeleton": root,
        "joints": list(range(len(bone_names))),
    }],
    "meshes": [{
        "name": "ProceduralAnimationBaseMesh",
        "primitives": [{
            "attributes": {
                "POSITION": position_accessor,
                "NORMAL": normal_accessor,
                "TEXCOORD_0": uv_accessor,
                "COLOR_0": color_accessor,
                "JOINTS_0": joint_accessor,
                "WEIGHTS_0": weight_accessor,
            },
            "indices": index_accessor,
            "material": 0,
            "mode": 4,
        }],
    }],
    "materials": [{
        "name": "ProceduralBaseMaterial",
        "doubleSided": True,
        "pbrMetallicRoughness": {
            "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
            "metallicFactor": 0.0,
            "roughnessFactor": 0.78,
        },
    }],
    "buffers": [{"byteLength": len(blob)}],
    "bufferViews": buffer_views,
    "accessors": accessors,
}

json_chunk = json.dumps(document, separators=(",", ":")).encode("utf-8")
while len(json_chunk) % 4:
    json_chunk += b" "
while len(blob) % 4:
    blob.append(0)

total_length = 12 + 8 + len(json_chunk) + 8 + len(blob)
OUTPUT.parent.mkdir(parents=True, exist_ok=True)
with OUTPUT.open("wb") as stream:
    stream.write(struct.pack("<4sII", b"glTF", 2, total_length))
    stream.write(struct.pack("<I4s", len(json_chunk), b"JSON"))
    stream.write(json_chunk)
    stream.write(struct.pack("<I4s", len(blob), b"BIN\0"))
    stream.write(blob)

print(
    f"Wrote {OUTPUT} ({OUTPUT.stat().st_size} bytes, "
    f"{len(bone_names)} bones, {vertex_count} vertices, {len(indices) // 3} triangles)"
)
