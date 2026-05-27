# tools/analyze_vertices.py
import struct
import json
import os
import sys
import numpy as np
import matplotlib
import matplotlib.pyplot as plt
from mpl_toolkits.mplot3d import Axes3D

def load_gltf_vertices(glb_path):
    with open(glb_path, 'rb') as f:
        data = f.read()

    # Read Header
    magic, version, length = struct.unpack_from('<III', data, 0)
    if magic != 0x46546C67:
        raise ValueError("Not a valid binary glTF file.")

    # Read JSON Chunk
    chunk0_len, chunk0_type = struct.unpack_from('<II', data, 12)
    json_bytes = data[20 : 20 + chunk0_len]
    gltf = json.loads(json_bytes.decode('utf-8'))

    # Read BIN Chunk
    bin_offset = 20 + chunk0_len
    bin_len, bin_type = struct.unpack_from('<II', data, bin_offset)
    bin_data = data[bin_offset + 8 : bin_offset + 8 + bin_len]

    positions, normals, weights = [], [], []

    dtype_map = {
        5120: np.int8,
        5121: np.uint8,
        5122: np.int16,
        5123: np.uint16,
        5125: np.uint32,
        5126: np.float32
    }

    def decode_accessor(accessor_idx):
        if accessor_idx is None:
            return None
        acc = gltf['accessors'][accessor_idx]
        bv_idx = acc.get('bufferView')
        if bv_idx is None:
            return None
        bv = gltf['bufferViews'][bv_idx]

        offset = acc.get('byteOffset', 0) + bv.get('byteOffset', 0)
        count = acc['count']
        comp_type = acc['componentType']
        type_str = acc['type']

        dtype = dtype_map[comp_type]
        num_comps = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}[type_str]
        stride = bv.get('byteStride', dtype().itemsize * num_comps)

        raw_bytes = bin_data[offset : offset + count * stride]

        if stride == dtype().itemsize * num_comps:
            arr = np.frombuffer(raw_bytes, dtype=dtype, count=count * num_comps)
            return arr.reshape((count, num_comps))
        else:
            # Handle strided layouts
            out = []
            for i in range(count):
                start = i * stride
                val = np.frombuffer(raw_bytes[start : start + dtype().itemsize * num_comps], dtype=dtype, count=num_comps)
                out.append(val)
            return np.array(out)

    for mesh in gltf.get('meshes', []):
        for prim in mesh.get('primitives', []):
            attrs = prim.get('attributes', {})
            
            p = decode_accessor(attrs.get('POSITION'))
            n = decode_accessor(attrs.get('NORMAL'))
            w = decode_accessor(attrs.get('WEIGHTS_0'))

            if p is not None: positions.append(p)
            if n is not None: normals.append(n)
            if w is not None: weights.append(w)

    return (np.vstack(positions) if positions else np.array([]),
            np.vstack(normals) if normals else np.array([]),
            np.vstack(weights) if weights else np.array([]))

def plot_diagnostics(positions, normals, weights):
    fig = plt.figure(figsize=(18, 6))

    # --- Plot 1: 3D Point Cloud ---
    ax1 = fig.add_subplot(131, projection='3d')
    if positions.size > 0:
        # Sample points if the model is extremely dense to preserve performance
        step = max(1, len(positions) // 5000)
        p_sampled = positions[::step]
        ax1.scatter(p_sampled[:, 0], p_sampled[:, 1], p_sampled[:, 2], c=p_sampled[:, 1], cmap='viridis', s=1)
        ax1.set_title("Vertex Positions (3D Space)")
        ax1.set_xlabel("X")
        ax1.set_ylabel("Y")
        ax1.set_zlabel("Z")
    else:
        ax1.text(0.5, 0.5, "No Position Data", ha='center')

    # --- Plot 2: Normals on a Unit Sphere ---
    ax2 = fig.add_subplot(132, projection='3d')
    if normals.size > 0:
        step = max(1, len(normals) // 2000)
        n_sampled = normals[::step]
        
        # Plot a wireframe unit sphere for reference
        u, v = np.mgrid[0:2*np.pi:20j, 0:np.pi:10j]
        xs = np.cos(u)*np.sin(v)
        ys = np.sin(u)*np.sin(v)
        zs = np.cos(v)
        ax2.plot_wireframe(xs, ys, zs, color="gray", alpha=0.15)
        
        # Scatter actual normals
        ax2.scatter(n_sampled[:, 0], n_sampled[:, 1], n_sampled[:, 2], color='red', s=2, alpha=0.6)
        ax2.set_title("Normal Vectors Projected on Unit Sphere")
        ax2.set_xlim([-1, 1])
        ax2.set_ylim([-1, 1])
        ax2.set_zlim([-1, 1])
    else:
        ax2.text(0.5, 0.5, "No Normal Data", ha='center')

    # --- Plot 3: Joint Weight Sum Distribution ---
    ax3 = fig.add_subplot(133)
    if weights.size > 0:
        # Calculate sum of influence per vertex (Should be 1.0)
        weight_sums = np.sum(weights, axis=1)
        ax3.hist(weight_sums, bins=30, range=(0.8, 1.2), color='purple', edgecolor='black', alpha=0.7)
        ax3.axvline(1.0, color='red', linestyle='dashed', linewidth=2, label="Perfect Sum (1.0)")
        ax3.set_title("Skeletal Weights Sum per Vertex")
        ax3.set_xlabel("Total Combined Weight")
        ax3.set_ylabel("Vertex Count")
        ax3.legend()
    else:
        ax3.text(0.5, 0.5, "No Skeletal Weight Data", ha='center')

    plt.tight_layout()
    plt.show()

def main():
    if len(sys.argv) < 2:
        print("Usage: python analyze_vertices.py <model.glb>")
        sys.exit(1)

    glb_path = sys.argv[1]
    print(f"[*] Analyzing mesh data from: {glb_path}")
    
    positions, normals, weights = load_gltf_vertices(glb_path)
    print(f"[+] Decoded {len(positions)} vertices successfully.")
    
    if weights.size > 0:
        weight_sums = np.sum(weights, axis=1)
        invalid_vertices = np.sum(~np.isclose(weight_sums, 1.0, atol=1e-3))
        print(f"[!] Diagnostics: {invalid_vertices} vertices do not have normalized weights (sum != 1.0).")
    
    plot_diagnostics(positions, normals, weights)

if __name__ == "__main__":
    main()