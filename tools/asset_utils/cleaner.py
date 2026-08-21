# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# tools/asset_utils/cleaner.py
import struct
import json
import os


def sanitize_glb(glb_path: str) -> bool:
    """Sanitizes GLB files to strictly conform to the glTF 2.0 specification."""
    if not os.path.exists(glb_path):
        print(f"[-] Cleaner Error: GLB file does not exist at '{glb_path}'.")
        return False

    try:
        with open(glb_path, "rb") as f:
            data = f.read()

        if len(data) < 20:
            print("[-] Cleaner Error: File is too small to contain a valid GLB header.")
            return False

        magic, version, length = struct.unpack_from("<III", data, 0)
        if magic != 0x46546C67:
            print(f"[-] Cleaner Error: Invalid GLB magic signature: 0x{magic:08X}.")
            return False

        chunk0_len, chunk0_type = struct.unpack_from("<II", data, 12)
        if chunk0_type != 0x4E4F534A:
            print(f"[-] Cleaner Error: Chunk 0 is not JSON: 0x{chunk0_type:08X}.")
            return False

        json_bytes = data[20 : 20 + chunk0_len]
        gltf = json.loads(json_bytes.decode("utf-8"))

    except Exception as e:
        print(f"[-] Cleaner Error: Exception occurred while reading binary data: {e}")
        return False

    modified = False

    # 1. Spec Fix: Clamp baseColorFactor values to [0.0, 1.0]
    if "materials" in gltf:
        for mat in gltf["materials"]:
            pbr = mat.get("pbrMetallicRoughness", {})
            if "baseColorFactor" in pbr:
                factor = pbr["baseColorFactor"]
                clamped = [max(0.0, min(1.0, val)) for val in factor]
                if factor != clamped:
                    pbr["baseColorFactor"] = clamped
                    modified = True

    # 2. Spec Fix: Validate animation channels against Armature Joints and Morph Targets
    if "animations" in gltf:
        # Collect all skeleton joint node indices
        joint_nodes = set()
        if "skins" in gltf:
            for skin in gltf["skins"]:
                joint_nodes.update(skin.get("joints", []))

        # Collect all mesh node indices that have morph targets
        morph_nodes = set()
        if "nodes" in gltf and "meshes" in gltf:
            for node_idx, node in enumerate(gltf["nodes"]):
                mesh_idx = node.get("mesh")
                if mesh_idx is not None and 0 <= mesh_idx < len(gltf["meshes"]):
                    mesh = gltf["meshes"][mesh_idx]
                    has_targets = any(
                        "targets" in prim and len(prim["targets"]) > 0
                        for prim in mesh.get("primitives", [])
                    )
                    if has_targets:
                        morph_nodes.add(node_idx)

        new_animations = []
        for anim in gltf["animations"]:
            channels = anim.get("channels", [])
            samplers = anim.get("samplers", [])
            new_channels = []
            used_sampler_indices = set()

            for chan in channels:
                target = chan.get("target", {})
                node_idx = target.get("node")
                path = target.get("path")

                is_valid = False
                if path in ("translation", "rotation", "scale"):
                    # For skinned models, TRS channels must target skeleton joint nodes
                    if node_idx in joint_nodes or not joint_nodes:
                        is_valid = True
                elif path == "weights":
                    # Weight channels must target nodes with morph targets
                    if node_idx in morph_nodes:
                        is_valid = True

                if not is_valid:
                    print(
                        f"[~] Cleaner: Stripped non-skeletal channel targeting path '{path}' on node {node_idx}"
                    )
                    modified = True
                    continue

                new_channels.append(chan)
                if "sampler" in chan:
                    used_sampler_indices.add(chan["sampler"])

            # Only retain animation track if it contains valid skeleton channels
            if len(new_channels) > 0:
                if len(new_channels) != len(channels):
                    anim["channels"] = new_channels

                    new_samplers = []
                    sampler_mapping = {}
                    for old_idx in sorted(list(used_sampler_indices)):
                        sampler_mapping[old_idx] = len(new_samplers)
                        new_samplers.append(samplers[old_idx])

                    for chan in new_channels:
                        chan["sampler"] = sampler_mapping[chan["sampler"]]

                    anim["samplers"] = new_samplers
                    modified = True
                new_animations.append(anim)
            else:
                print(
                    f"[~] Cleaner: Removed non-skeletal animation track '{anim.get('name', '')}'"
                )
                modified = True

        gltf["animations"] = new_animations

    if not modified:
        return True

    try:
        # Repack binary GLB container
        new_json_bytes = json.dumps(gltf, separators=(",", ":")).encode("utf-8")
        padding_len = (4 - (len(new_json_bytes) % 4)) % 4
        new_json_bytes += b" " * padding_len
        new_chunk0_len = len(new_json_bytes)

        bin_chunk_offset = 20 + chunk0_len
        bin_bytes = b""
        bin_chunk_len = 0
        if bin_chunk_offset < len(data):
            bin_chunk_len, bin_chunk_type = struct.unpack_from(
                "<II", data, bin_chunk_offset
            )
            if bin_chunk_type == 0x004E4942:
                bin_bytes = data[
                    bin_chunk_offset + 8 : bin_chunk_offset + 8 + bin_chunk_len
                ]

        total_length = 12 + 8 + new_chunk0_len
        if bin_chunk_len > 0:
            total_length += 8 + bin_chunk_len

        header = struct.pack("<III", 0x46546C67, 2, total_length)
        chunk0_header = struct.pack("<II", new_chunk0_len, 0x4E4F534A)

        output_data = header + chunk0_header + new_json_bytes
        if bin_chunk_len > 0:
            chunk1_header = struct.pack("<II", bin_chunk_len, 0x004E4942)
            output_data += chunk1_header + bin_bytes

        with open(glb_path, "wb") as f:
            f.write(output_data)
        return True
    except Exception as e:
        print(f"[-] Cleaner Error: Exception occurred while repacking: {e}")
        return False
