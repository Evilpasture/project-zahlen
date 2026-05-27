# tools/asset_utils/cleaner.py
import struct
import json
import os

def sanitize_glb(glb_path: str) -> bool:
    """Clamps baseColorFactors and reparents skinned meshes to resolve validation errors."""
    if not os.path.exists(glb_path):
        print(f"[-] Cleaner Error: GLB file does not exist at '{glb_path}'. (Did Blender fail to export?)")
        return False

    try:
        with open(glb_path, 'rb') as f:
            data = f.read()

        if len(data) < 20:
            print("[-] Cleaner Error: File is too small to contain a valid GLB header.")
            return False

        magic, version, length = struct.unpack_from('<III', data, 0)
        if magic != 0x46546C67:  # Lowercase 'l' (0x6c) is correct
            print(f"[-] Cleaner Error: Invalid GLB magic signature. Expected 0x46546C67, got 0x{magic:08X}.")
            return False

        chunk0_len, chunk0_type = struct.unpack_from('<II', data, 12)
        if chunk0_type != 0x4E4F534A:
            print(f"[-] Cleaner Error: Chunk 0 is not JSON. Expected 0x4E4F534A, got 0x{chunk0_type:08X}.")
            return False

        json_bytes = data[20 : 20 + chunk0_len]
        gltf = json.loads(json_bytes.decode('utf-8'))

    except Exception as e:
        print(f"[-] Cleaner Error: Exception occurred while reading binary data: {e}")
        return False

    modified = False

    # 1. Clamp alpha and color ranges to [0.0, 1.0]
    if 'materials' in gltf:
        for idx, mat in enumerate(gltf['materials']):
            pbr = mat.get('pbrMetallicRoughness', {})
            if 'baseColorFactor' in pbr:
                factor = pbr['baseColorFactor']
                clamped = [max(0.0, min(1.0, val)) for val in factor]
                if factor != clamped:
                    pbr['baseColorFactor'] = clamped
                    modified = True

    # 2. Re-parent Skinned Meshes to Scene Root
    if 'nodes' in gltf and 'scenes' in gltf:
        parent_map = {}
        for node_idx, node in enumerate(gltf['nodes']):
            for child in node.get('children', []):
                parent_map[child] = node_idx

        skinned_nodes = [i for i, n in enumerate(gltf['nodes']) if 'skin' in n and 'mesh' in n]

        for node_idx in skinned_nodes:
            node = gltf['nodes'][node_idx]
            parent_idx = parent_map.get(node_idx)

            if parent_idx is not None:
                parent_node = gltf['nodes'][parent_idx]
                if 'children' in parent_node and node_idx in parent_node['children']:
                    parent_node['children'].remove(node_idx)

                for transform in ['translation', 'rotation', 'scale', 'matrix']:
                    if transform in node:
                        del node[transform]

                for scene in gltf.get('scenes', []):
                    if 'nodes' not in scene:
                        scene['nodes'] = []
                    if node_idx not in scene['nodes']:
                        scene['nodes'].append(node_idx)
                modified = True

    if not modified:
        return True

    try:
        # Repack the binary container
        new_json_bytes = json.dumps(gltf, separators=(',', ':')).encode('utf-8')
        padding_len = (4 - (len(new_json_bytes) % 4)) % 4
        new_json_bytes += b' ' * padding_len
        new_chunk0_len = len(new_json_bytes)

        bin_chunk_offset = 20 + chunk0_len
        bin_bytes = b''
        bin_chunk_len = 0
        if bin_chunk_offset < len(data):
            bin_chunk_len, bin_chunk_type = struct.unpack_from('<II', data, bin_chunk_offset)
            if bin_chunk_type == 0x004E4942:
                bin_bytes = data[bin_chunk_offset + 8 : bin_chunk_offset + 8 + bin_chunk_len]

        total_length = 12 + 8 + new_chunk0_len
        if bin_chunk_len > 0:
            total_length += 8 + bin_chunk_len

        header = struct.pack('<III', 0x46546C67, 2, total_length)
        chunk0_header = struct.pack('<II', new_chunk0_len, 0x4E4F534A)

        output_data = header + chunk0_header + new_json_bytes
        if bin_chunk_len > 0:
            chunk1_header = struct.pack('<II', bin_chunk_len, 0x004E4942)
            output_data += chunk1_header + bin_bytes

        with open(glb_path, 'wb') as f:
            f.write(output_data)
        return True
    except Exception as e:
        print(f"[-] Cleaner Error: Exception occurred while repacking: {e}")
        return False