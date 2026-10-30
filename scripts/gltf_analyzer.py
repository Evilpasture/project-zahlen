# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


import argparse
import sys
import math
from pygltflib import GLTF2

def format_trs(node):
    """Formats Translation, Rotation, Scale for display."""
    t = node.translation if node.translation is not None else [0, 0, 0]
    r = node.rotation if node.rotation is not None else [0, 0, 0, 1]
    s = node.scale if node.scale is not None else [1, 1, 1]
    
    # Check if it has a matrix instead of TRS
    if node.matrix is not None:
        return "Matrix defined (complex transform)"
    
    # Check if it's actually at origin
    is_at_origin = all(v == 0 for v in t)
    origin_str = " [AT ORIGIN]" if is_at_origin else f" Pos: {t}"
    
    return f"{origin_str} | Rot: {r} | Scale: {s}"

def analyze_gltf_expanded(file_path, verbose=False):
    try:
        gltf = GLTF2.load(file_path)
    except Exception as e:
        print(f"Error loading file: {e}")
        sys.exit(1)

    print(f"\n{'='*80}")
    print(f"DEEP ANALYSIS: {file_path}")
    print(f"{'='*80}")

    # --- 1. SCENE GRAPH & TRANSFORMS ---
    # This addresses the "everything at 0,0,0" concern
    print(f"\n[ NODE HIERARCHY & PLACEMENT ]")
    if not gltf.nodes:
        print("No nodes found.")
    else:
        for i, node in enumerate(gltf.nodes):
            indent = "  "
            mesh_info = f" -> Mesh Index: {node.mesh}" if node.mesh is not None else ""
            skin_info = f" -> Skin Index: {node.skin}" if node.skin is not None else ""
            
            print(f"({i}) Node: '{node.name or 'Unnamed'}'{mesh_info}{skin_info}")
            print(f"{indent}Transform: {format_trs(node)}")
            
            if node.children:
                print(f"{indent}Children IDs: {node.children}")

    # --- 2. ANIMATIONS ---
    print(f"\n[ ANIMATIONS: {len(gltf.animations)} ]")
    for i, anim in enumerate(gltf.animations):
        print(f"({i}) Animation Name: '{anim.name or 'N/A'}'")
        channels = len(anim.channels)
        
        # Calculate duration by checking the input accessor of the first sampler
        duration = 0
        if anim.samplers:
            accessor_idx = anim.samplers[0].input
            accessor = gltf.accessors[accessor_idx]
            duration = accessor.max[0] if accessor.max else "Unknown"
            
        print(f"    - Channels: {channels}")
        print(f"    - Duration: {duration} seconds")
        
        if verbose:
            for c_idx, channel in enumerate(anim.channels):
                target = channel.target
                path = target.path # translation, rotation, scale, or weights
                node_name = gltf.nodes[target.node].name if target.node is not None else "Root"
                print(f"      Channel {c_idx}: Affects '{node_name}' -> {path}")

    # --- 3. SKINS (Skeletal Data) ---
    if gltf.skins:
        print(f"\n[ SKINS: {len(gltf.skins)} ]")
        for i, skin in enumerate(gltf.skins):
            print(f"({i}) Skin Name: '{skin.name or 'N/A'}'")
            print(f"    - Joints Count: {len(skin.joints)}")
            if skin.skeleton is not None:
                print(f"    - Skeleton Root Node: {skin.skeleton}")

    # --- 4. MATERIALS & TEXTURES ---
    print(f"\n[ MATERIALS: {len(gltf.materials)} ]")
    for i, mat in enumerate(gltf.materials):
        pbr = mat.pbrMetallicRoughness
        print(f"({i}) '{mat.name or 'N/A'}'")
        if pbr:
            print(f"    - Base Color: {pbr.baseColorFactor}")
            if pbr.baseColorTexture:
                # Find the actual image source
                tex_idx = pbr.baseColorTexture.index
                img_idx = gltf.textures[tex_idx].source
                img = gltf.images[img_idx]
                print(f"    - Texture: {img.uri or 'Embedded/BufferView'}")

    # --- 5. LIGHTS & CAMERAS ---
    if gltf.cameras:
        print(f"\n[ CAMERAS: {len(gltf.cameras)} ]")
        for i, cam in enumerate(gltf.cameras):
            print(f"({i}) Type: {cam.type}")

    # Check for KHR_lights_punctual extension
    if "KHR_lights_punctual" in (gltf.extensionsUsed or []):
        try:
            lights = gltf.extensions["KHR_lights_punctual"]["lights"]
            print(f"\n[ LIGHTS (KHR_lights_punctual): {len(lights)} ]")
            for i, light in enumerate(lights):
                print(f"({i}) Name: {light.get('name', 'N/A')} | Type: {light.get('type')} | Color: {light.get('color')}")
        except:
            print("\n[ LIGHTS ] Extension present but data unreadable.")

    # --- 6. FILE METADATA ---
    print(f"\n[ METADATA ]")
    print(f"Generator : {gltf.asset.generator}")
    print(f"Version   : {gltf.asset.version}")
    if gltf.extensionsUsed:
        print(f"Extensions Used: {', '.join(gltf.extensionsUsed)}")
    if gltf.extensionsRequired:
        print(f"Extensions Req : {', '.join(gltf.extensionsRequired)}")

def main():
    parser = argparse.ArgumentParser(description="Advanced glTF Assembly & Animation Inspector")
    parser.add_argument("input", help="Path to the .gltf or .glb file")
    parser.add_argument("-v", "--verbose", action="store_true", help="Show channel-level animation details")

    args = parser.parse_args()
    analyze_gltf_expanded(args.input, args.verbose)

if __name__ == "__main__":
    main()