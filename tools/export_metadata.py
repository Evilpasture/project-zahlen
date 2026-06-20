# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# tools/export_metadata.py
import os
import json
import struct
import shutil
import sys
import bpy
from mathutils import Matrix

# Set recursion limit for large hierarchical models
sys.setrecursionlimit(15000)

# ============================================================================
# Core Type-Validation Helpers (OOP Injection)
# ============================================================================


def is_a(self, type_string):
    """Injected type checking supporting both spatial Objects and generic mesh Attributes."""
    try:
        if hasattr(self, "type"):
            return self.type.upper() == type_string.upper()
        if hasattr(self, "data_type") and hasattr(self, "domain"):
            mappings = {
                "UVMAP": ("CORNER", {"FLOAT_2D_VECTOR", "FLOAT_2"}),
                "VERTEXCOLOR": (
                    "CORNER",
                    {"FLOAT_COLOR", "BYTE_COLOR", "FLOAT_VECTOR"},
                ),
                "NORMAL": ("POINT", {"FLOAT_VECTOR", "FLOAT_3D_VECTOR"}),
            }
            target = type_string.upper()
            if target in mappings:
                domain, allowed_types = mappings[target]
                return self.domain == domain and self.data_type in allowed_types
    except ReferenceError:
        return False
    return False


bpy.types.Object.IsA = is_a
bpy.types.Attribute.IsA = is_a

# ============================================================================
# Setup Global Configurations & Output Paths
# ============================================================================

input_dir = os.getcwd()
output_parent = os.path.join(input_dir, "resources", "intermediate")
os.makedirs(output_parent, exist_ok=True)

# glTF/OpenGL Basis Transform: Blender Z-up right-handed to Y-up right-handed
c_basis = Matrix(
    (
        (1.0, 0.0, 0.0, 0.0),
        (0.0, 0.0, 1.0, 0.0),
        (0.0, -1.0, 0.0, 0.0),
        (0.0, 0.0, 0.0, 1.0),
    )
)
c_basis_inv = c_basis.inverted()


def discover_blend_files(search_path):
    """Recursively scans directories to discover valid level Blend files, ignoring asset libraries."""
    blend_files = []
    for root, _, files in os.walk(search_path):
        norm_root = root.replace("\\", "/").lower()
        if any(
            p in norm_root
            for p in ["resources", "exported_assets", "tadc_models", "asset_library"]
        ):
            continue
        for file in files:
            if file.endswith(".blend") and not file.startswith("."):
                if "void" in file.lower():
                    continue
                blend_files.append(os.path.join(root, file))
    return blend_files


blend_files = discover_blend_files(input_dir)
print(f"Discovered {len(blend_files)} levels for raw metadata extraction.\n")

# ============================================================================
# Core Math & Geometry Unpacking Helpers
# ============================================================================


def make_id(prefix, name):
    """Converts a Blender name to a clean, unique, lowercase ID string."""
    clean_name = "".join([c.lower() if c.isalnum() else "_" for c in name])
    while "__" in clean_name:
        clean_name = clean_name.replace("__", "_")
    return f"{prefix}_{clean_name.strip('_')}"


def clean_float(val):
    """Rounds floats to 4 decimal places for clean serialization."""
    return round(val, 4)


def safe_invert(matrix):
    """Safely inverts a matrix, falling back to identity if singular."""
    try:
        return matrix.inverted()
    except ValueError:
        return Matrix.Identity(4)


def serialize_matrix_col_major(matrix):
    """Converts a mathutils.Matrix to a flat column-major list of 16 rounded floats."""
    return [clean_float(val) for col in matrix.transposed() for val in col]


def unpack_color(val):
    """Safely unpacks color/vector data of any dimension into a 4-float RGBA tuple."""
    if val is None:
        return (1.0, 1.0, 1.0, 1.0)
    if not hasattr(val, "__len__"):
        try:
            f = float(val)
            return (f, f, f, 1.0)
        except Exception:
            return (1.0, 1.0, 1.0, 1.0)
    length = len(val)
    if length == 0:
        return (1.0, 1.0, 1.0, 1.0)
    elif length == 1:
        try:
            f = float(val[0])
            return (f, f, f, 1.0)
        except Exception:
            return (1.0, 1.0, 1.0, 1.0)
    elif length == 2:
        return (float(val[0]), float(val[1]), 0.0, 1.0)
    elif length == 3:
        return (float(val[0]), float(val[1]), float(val[2]), 1.0)
    else:
        return (float(val[0]), float(val[1]), float(val[2]), float(val[3]))


def unpack_color_from_datum(datum):
    """Helper to resolve the color robustly from any generic attribute layer payload."""
    if not datum:
        return (1.0, 1.0, 1.0, 1.0)

    vals = None
    if hasattr(datum, "color"):
        vals = datum.color
    elif hasattr(datum, "vector"):
        vals = datum.vector
    elif hasattr(datum, "value"):
        vals = datum.value
        if not hasattr(vals, "__len__"):
            vals = [vals]

    return unpack_color(vals)


def is_valid_color_layer(attr):
    """Deterministically verifies if an attribute is a true color layer by checking value ranges."""
    if not attr or not attr.data:
        return False

    sample_size = min(len(attr.data), 100)
    for i in range(sample_size):
        datum = attr.data[i]

        if hasattr(datum, "color"):
            vals = datum.color
        elif hasattr(datum, "vector"):
            vals = datum.vector
        elif hasattr(datum, "value"):
            vals = datum.value
            if not hasattr(vals, "__len__"):
                vals = [vals]
        else:
            continue

        for v in vals:
            if v < -0.001 or v > 1.001:
                return False

    return True


def get_evaluated_mesh_safely(obj, depsgraph):
    """Safely extracts fully evaluated mesh data directly from the dependency graph."""
    try:
        eval_obj = obj.evaluated_get(depsgraph)
        return eval_obj.to_mesh()
    except Exception:
        return obj.data.copy()


def get_image_filename(image):
    """Gets a clean target filename for a Blender Image block, falling back to PNG."""
    if not image:
        return None
    filename = os.path.basename(image.filepath)
    if not filename:
        filename = f"{image.name}.png"
    return filename


def find_upstream_texture_or_attribute(socket):
    """Recursively traverses linked node tree sockets to find if any image texture or attribute is connected."""
    if not socket or not socket.is_linked:
        return None

    for link in socket.links:
        from_node = link.from_node
        if from_node.type in {"TEX_IMAGE", "ATTRIBUTE", "VERTEX_COLOR"}:
            return from_node

        for input_socket in from_node.inputs:
            found = find_upstream_texture_or_attribute(input_socket)
            if found:
                return found
    return None


def get_texture_node_image_block(input_socket):
    """Recursively walks node trees to extract the actual image object, supporting linked networks."""
    image_node = find_upstream_texture_or_attribute(input_socket)
    if image_node and image_node.type == "TEX_IMAGE" and image_node.image:
        return image_node.image
    return None


# ============================================================================
# Modular Pipeline Exporters
# ============================================================================


def export_and_copy_textures(asset_dir):
    """Extracts and copies packed/unpacked textures into the level folder."""
    textures_dir = os.path.join(asset_dir, "textures")
    os.makedirs(textures_dir, exist_ok=True)

    for image in bpy.data.images:
        if image.type in {"COMPOSITER", "VIEWER"} or image.source not in {
            "FILE",
            "GENERATED",
            "TILED",
        }:
            continue

        dest_filename = get_image_filename(image)
        if not dest_filename:
            continue
        dest_path = os.path.join(textures_dir, dest_filename)

        if image.packed_file:
            old_path = image.filepath_raw
            try:
                image.filepath_raw = dest_path
                image.save()
            except Exception as e:
                print(
                    f"      [Warning] Failed to save packed image '{image.name}': {e}"
                )
            finally:
                image.filepath_raw = old_path
        else:
            src_path = bpy.path.abspath(image.filepath)
            if os.path.exists(src_path):
                try:
                    shutil.copy2(src_path, dest_path)
                except Exception as e:
                    print(
                        f"      [Warning] Failed to copy texture '{image.name}' from '{src_path}': {e}"
                    )
            else:
                try:
                    image.save_render(dest_path)
                except Exception as e:
                    print(
                        f"      [Warning] Source missing and fallback failed for '{image.name}': {e}"
                    )


def precalculate_world_matrices(scene_objects, depsgraph):
    """Calculates all relative coordinate matrices across standard objects and armatures."""
    world_yup_map = {}
    for obj in scene_objects:
        world_yup_map[obj.name] = c_basis @ obj.matrix_world @ c_basis_inv

    for obj in depsgraph.scene.objects:
        if obj.IsA("ARMATURE"):
            for bone in obj.data.bones:
                bone_key = f"bone_{obj.name}_{bone.name}"
                bone_world_zup = obj.matrix_world @ bone.matrix_local
                world_yup_map[bone_key] = c_basis @ bone_world_zup @ c_basis_inv
    return world_yup_map


def resolve_base_color(principled):
    """Resolves the solid base color of a Principled BSDF node, tracing simple value links if necessary."""
    base_color_input = principled.inputs.get("Base Color")
    if not base_color_input:
        return [1.0, 1.0, 1.0, 1.0]

    if not base_color_input.is_linked:
        return [clean_float(c) for c in base_color_input.default_value]

    # If the base color is driven by an image texture or vertex color attribute,
    # default the tint factor to white to prevent stray node values from darkening the export.
    img_node = find_upstream_texture_or_attribute(base_color_input)
    if img_node:
        return [1.0, 1.0, 1.0, 1.0]

    link = base_color_input.links[0]
    from_node = link.from_node

    if from_node.type == "RGB" and hasattr(from_node, "outputs") and from_node.outputs:
        return [clean_float(c) for c in from_node.outputs[0].default_value]

    if from_node.type == "VALTORGB" and len(from_node.color_ramp.elements) > 0:
        return [clean_float(c) for c in from_node.color_ramp.elements[0].color]

    if from_node.type in {"MIX", "MIX_RGB"}:
        col_input = from_node.inputs.get("Color1") or from_node.inputs.get("A")
        if col_input:
            return [clean_float(c) for c in col_input.default_value]

    return [clean_float(c) for c in base_color_input.default_value]


def extract_materials():
    """Extracts all active materials, resolving node configurations & viewports."""
    materials = []
    for mat in bpy.data.materials:
        mat_id = make_id("mat", mat.name)
        default_color = [clean_float(c) for c in mat.diffuse_color]

        mat_info = {
            "id": mat_id,
            "name": mat.name,
            "pbr": {
                "base_color": default_color,
                "metallic": 0.0,
                "roughness": 0.5,
                "emissive_factor": [0.0, 0.0, 0.0],
                "emissive_strength": 0.0,
            },
            "maps": {
                "albedo": None,
                "normal": None,
                "metallic_roughness": None,
                "ao": None,
            },
            "sampler": {"wrap": "REPEAT", "filter": "LINEAR"},
        }

        if mat.use_nodes and mat.node_tree:
            principled = next(
                (n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None
            )
            if principled:
                mat_info["pbr"]["base_color"] = resolve_base_color(principled)

                metallic_input = principled.inputs.get("Metallic")
                if metallic_input and hasattr(metallic_input, "default_value"):
                    mat_info["pbr"]["metallic"] = clean_float(
                        metallic_input.default_value
                    )

                roughness_input = principled.inputs.get("Roughness")
                if roughness_input and hasattr(roughness_input, "default_value"):
                    mat_info["pbr"]["roughness"] = clean_float(
                        roughness_input.default_value
                    )

                em_col = principled.inputs.get(
                    "Emission Color"
                ) or principled.inputs.get("Emission")
                em_str = principled.inputs.get("Emission Strength")
                if em_col and hasattr(em_col, "default_value"):
                    mat_info["pbr"]["emissive_factor"] = [
                        clean_float(c) for c in em_col.default_value[:3]
                    ]
                if em_str and hasattr(em_str, "default_value"):
                    mat_info["pbr"]["emissive_strength"] = clean_float(
                        em_str.default_value
                    )

                albedo_img = get_texture_node_image_block(
                    principled.inputs.get("Base Color")
                )
                normal_img = get_texture_node_image_block(
                    principled.inputs.get("Normal")
                )
                mr_img = get_texture_node_image_block(
                    principled.inputs.get("Roughness")
                )

                albedo_file = get_image_filename(albedo_img)
                normal_file = get_image_filename(normal_img)
                mr_file = get_image_filename(mr_img)

                mat_info["maps"]["albedo"] = (
                    f"textures/{albedo_file}" if albedo_file else None
                )
                mat_info["maps"]["normal"] = (
                    f"textures/{normal_file}" if normal_file else None
                )
                mat_info["maps"]["metallic_roughness"] = (
                    f"textures/{mr_file}" if mr_file else None
                )

        if not mat_info["maps"]["albedo"]:
            mat_info["pbr"]["base_color"] = default_color

        materials.append(mat_info)
    return materials


def extract_meshes(geometry_sources, depsgraph, asset_dir, bin_dir, exported_meshes):
    """Packs local geometry buffers, split-vertex coordinates, and morph keys."""
    meshes = []

    for obj in geometry_sources:
        if not obj or not obj.IsA("MESH"):
            continue

        armature_mod = next((m for m in obj.modifiers if m.type == "ARMATURE"), None)
        is_skinned = armature_mod is not None and armature_mod.object is not None
        mesh_suffix = "_skinned" if is_skinned else ""
        mesh_id = make_id("mesh", f"{obj.data.name}{mesh_suffix}")

        if mesh_id in exported_meshes:
            continue

        mesh_data = get_evaluated_mesh_safely(obj, depsgraph)
        if not mesh_data:
            continue

        # Prevent empty mesh generation to preserve glTF spec compliance
        if len(mesh_data.vertices) == 0 or len(mesh_data.polygons) == 0:
            try:
                obj.evaluated_get(depsgraph).to_mesh_clear()
            except Exception:
                pass
            continue

        try:
            mesh_data.calc_normals_split()
        except Exception:
            pass

        active_uv_layer = (
            mesh_data.uv_layers.active.data if mesh_data.uv_layers else None
        )

        # Robust extraction of generic Geometry Node / Attribute colors
        active_color_attr = None
        if hasattr(mesh_data, "attributes"):
            for attr in mesh_data.attributes:
                if attr.data_type in {"FLOAT_COLOR", "BYTE_COLOR", "FLOAT_VECTOR"}:
                    if is_valid_color_layer(attr):
                        active_color_attr = attr
                        if "color" in attr.name.lower():
                            break

        has_tangents = False
        try:
            mesh_data.calc_tangents()
            has_tangents = True
        except Exception:
            pass

        group_to_joint_idx = {}
        if is_skinned:
            arm_obj = armature_mod.object
            bone_names = [b.name for b in arm_obj.data.bones]
            group_to_joint_idx = {
                idx: bone_names.index(g.name)
                for idx, g in enumerate(obj.vertex_groups)
                if g.name in bone_names
            }

        unique_vertices = {}
        flat_vbo = []
        vertex_count = 0
        joints_data = []
        weights_data = []
        primitives = []
        ibo_binary = b""
        current_offset = 0

        mesh_data.calc_loop_triangles()
        triangles_by_mat = {}
        for tri in mesh_data.loop_triangles:
            mat_idx = tri.material_index
            if mat_idx not in triangles_by_mat:
                triangles_by_mat[mat_idx] = []
            triangles_by_mat[mat_idx].append(tri)

        for mat_idx, tris in triangles_by_mat.items():
            prim_indices = []
            for tri in tris:
                for loop_idx, v_idx in zip(tri.loops, tri.vertices):
                    co = mesh_data.vertices[v_idx].co
                    normal = (
                        mesh_data.loops[loop_idx].normal
                        if hasattr(mesh_data.loops[loop_idx], "normal")
                        else mesh_data.vertices[v_idx].normal
                    )
                    uv = active_uv_layer[loop_idx].uv if active_uv_layer else (0.0, 0.0)

                    color = (1.0, 1.0, 1.0, 1.0)
                    if active_color_attr and active_color_attr.data:
                        if active_color_attr.domain == "CORNER":
                            datum = active_color_attr.data[loop_idx]
                        elif active_color_attr.domain == "POINT":
                            datum = active_color_attr.data[v_idx]
                        elif active_color_attr.domain == "FACE":
                            datum = active_color_attr.data[tri.polygon_index]
                        else:
                            datum = None
                        color = unpack_color_from_datum(datum)

                    if has_tangents:
                        tangent = mesh_data.loops[loop_idx].tangent
                        sign = mesh_data.loops[loop_idx].bitangent_sign
                    else:
                        tangent = (1.0, 0.0, 0.0)
                        sign = 1.0

                    key = (
                        v_idx,
                        round(uv[0], 5),
                        round(uv[1], 5),
                        round(normal[0], 5),
                        round(normal[1], 5),
                        round(normal[2], 5),
                        round(tangent[0], 5),
                        round(tangent[1], 5),
                        round(tangent[2], 5),
                        round(sign, 5),
                        round(color[0], 5),
                        round(color[1], 5),
                        round(color[2], 5),
                        round(color[3], 5),
                    )

                    if key in unique_vertices:
                        idx = unique_vertices[key]
                    else:
                        idx = vertex_count
                        unique_vertices[key] = idx
                        vertex_count += 1

                        flat_vbo.extend([co[0], co[2], -co[1]])
                        flat_vbo.extend([normal[0], normal[2], -normal[1]])
                        flat_vbo.extend([tangent[0], tangent[2], -tangent[1], sign])
                        flat_vbo.extend([uv[0], 1.0 - uv[1]])
                        flat_vbo.extend([color[0], color[1], color[2], color[3]])

                        if is_skinned:
                            v = mesh_data.vertices[v_idx]
                            v_influences = []
                            for g in v.groups:
                                j_idx = group_to_joint_idx.get(g.group)
                                if j_idx is not None:
                                    v_influences.append((j_idx, g.weight))

                            v_influences = sorted(
                                v_influences, key=lambda x: x[1], reverse=True
                            )[:4]
                            while len(v_influences) < 4:
                                v_influences.append((0, 0.0))

                            total_w = sum(w for _, w in v_influences)
                            if total_w > 0.0:
                                v_influences = [
                                    (j, w / total_w) for j, w in v_influences
                                ]
                            else:
                                v_influences = [(0, 1.0)] + [(0, 0.0)] * 3

                            for j, w in v_influences:
                                joints_data.append(j)
                                weights_data.append(w)

                    prim_indices.append(idx)

            prim_bin = struct.pack(f"{len(prim_indices)}I", *prim_indices)
            ibo_binary += prim_bin

            if (
                mat_idx < len(obj.material_slots)
                and obj.material_slots[mat_idx].material
            ):
                mat_id = make_id("mat", obj.material_slots[mat_idx].material.name)
            else:
                mat_id = None

            primitives.append(
                {
                    "material_id": mat_id,
                    "draw_mode": "TRIANGLES",
                    "index_offset": current_offset,
                    "index_count": len(prim_indices),
                }
            )
            current_offset += len(prim_bin)

        vbo_binary = struct.pack(f"{len(flat_vbo)}f", *flat_vbo)
        joints_binary = b""
        weights_binary = b""
        if is_skinned:
            joints_binary = struct.pack(f"{len(joints_data)}H", *joints_data)
            weights_binary = struct.pack(f"{len(weights_data)}f", *weights_data)

        morph_targets = []
        blend_weights = []
        morph_binary = b""
        if mesh_data.shape_keys:
            key_blocks = mesh_data.shape_keys.key_blocks
            basis_key = key_blocks[0]
            basis_cos = [0.0] * (len(mesh_data.vertices) * 3)
            basis_key.data.foreach_get("co", basis_cos)

            morph_offset = 0
            for idx, kb in enumerate(key_blocks[1:]):
                target_cos = [0.0] * (len(mesh_data.vertices) * 3)
                kb.data.foreach_get("co", target_cos)
                deltas_pos = [0.0] * (vertex_count * 3)

                for key, split_idx in unique_vertices.items():
                    orig_v_idx = key[0]
                    dx = target_cos[orig_v_idx * 3] - basis_cos[orig_v_idx * 3]
                    dy = target_cos[orig_v_idx * 3 + 1] - basis_cos[orig_v_idx * 3 + 1]
                    dz = target_cos[orig_v_idx * 3 + 2] - basis_cos[orig_v_idx * 3 + 2]

                    deltas_pos[split_idx * 3] = dx
                    deltas_pos[split_idx * 3 + 1] = dz
                    deltas_pos[split_idx * 3 + 2] = -dy

                target_bin = struct.pack(f"{len(deltas_pos)}f", *deltas_pos)
                morph_binary += target_bin

                morph_targets.append(
                    {
                        "name": kb.name,
                        "weight": clean_float(kb.value),
                        "delta_positions": {
                            "byte_offset": morph_offset,
                            "byte_length": len(deltas_pos) * 4,
                        },
                    }
                )
                morph_offset += len(target_bin)

                blend_weights.append(
                    {
                        "index": idx,
                        "name": kb.name,
                        "default_weight": clean_float(kb.value),
                        "range": [
                            clean_float(kb.slider_min),
                            clean_float(kb.slider_max),
                        ],
                    }
                )

        bin_path = os.path.join(bin_dir, f"{mesh_id}.bin")
        with open(bin_path, "wb") as f:
            f.write(vbo_binary)
            f.write(ibo_binary)
            if is_skinned:
                f.write(joints_binary)
                f.write(weights_binary)
            if morph_binary:
                f.write(morph_binary)

        v_offset = 0
        v_len = len(vbo_binary)
        i_offset = v_len
        i_len = len(ibo_binary)

        layout = "P3N3T4U2C4"
        if is_skinned:
            layout += "_J4W4"
        if morph_targets:
            layout += f"_M{len(morph_targets)}"

        mesh_info = {
            "id": mesh_id,
            "name": f"{obj.data.name} Mesh",
            "vertex_count": vertex_count,
            "source_vertex_count": len(mesh_data.vertices),
            "layout": layout,
            "buffers": {
                "bin_file": os.path.relpath(bin_path, asset_dir),
                "vertex_buffer": {
                    "byte_offset": v_offset,
                    "byte_length": v_len,
                    "stride": 64,
                },
                "index_buffer": {"byte_offset": i_offset, "byte_length": i_len},
            },
            "primitives": primitives,
            "morph_targets": morph_targets,
            "blend_weights": blend_weights,
        }

        if is_skinned:
            j_offset = i_offset + i_len
            j_len = len(joints_binary)
            w_offset = j_offset + j_len
            w_len = len(weights_binary)

            mesh_info["buffers"]["joints"] = {
                "byte_offset": j_offset,
                "byte_length": j_len,
            }
            mesh_info["buffers"]["weights"] = {
                "byte_offset": w_offset,
                "byte_length": w_len,
            }

        if morph_binary:
            target_start = i_offset + i_len + (len(joints_binary) + len(weights_binary))
            for target in mesh_info["morph_targets"]:
                target["delta_positions"]["byte_offset"] += target_start

        meshes.append(mesh_info)
        exported_meshes.add(mesh_id)

        try:
            obj.evaluated_get(depsgraph).to_mesh_clear()
        except Exception:
            try:
                bpy.data.meshes.remove(mesh_data)
            except Exception:
                pass
    return meshes


def build_node_info(
    obj,
    node_id,
    node_name,
    parent_node_id,
    local_yup,
    world_yup,
    is_visible,
    exported_meshes,
):
    """Factory helper to build consistent glTF node manifest descriptions."""
    armature_mod = next((m for m in obj.modifiers if m.type == "ARMATURE"), None)
    is_skinned = armature_mod is not None and armature_mod.object is not None
    mesh_suffix = "_skinned" if is_skinned else ""

    modifier_stack = []
    for mod in obj.modifiers:
        params = {}
        if mod.type == "SUBDIV":
            params = {"levels": mod.levels, "render_levels": mod.render_levels}
        elif mod.type == "ARMATURE" and mod.object:
            params = {"armature_id": make_id("node", mod.object.name)}
        elif mod.type == "DECIMATE":
            params = {
                "ratio": clean_float(mod.ratio),
                "decimate_type": mod.decimate_type,
            }
        elif mod.type == "NODES" and mod.node_group:
            params = {"node_group": mod.node_group.name}

        modifier_stack.append(
            {
                "type": mod.type,
                "name": mod.name,
                "params": params,
                "eval_order": len(modifier_stack),
                "gn_hash": str(hash(mod.node_group.name))
                if (mod.type == "NODES" and mod.node_group)
                else None,
            }
        )

    node_info = {
        "id": node_id,
        "name": node_name,
        "type": obj.type,
        "visible": is_visible,
        "parent_id": parent_node_id,
        "transform": {
            "local": serialize_matrix_col_major(local_yup),
            "world": serialize_matrix_col_major(world_yup),
        },
        "refs": {
            "mesh_id": None,
            "skin_id": None,
            "light_id": None,
            "material_ids": [],
            "morph_weights": [],
        },
        "extras": {},
        "modifier_stack": modifier_stack,
    }

    if armature_mod and armature_mod.object:
        node_info["refs"]["skin_id"] = make_id("skin", armature_mod.object.name)

    if obj.type == "LIGHT":
        # Only attach light definitions if the object is strictly visible
        if is_visible:
            node_info["refs"]["light_id"] = make_id("light", obj.name)
            light_data = obj.data
            node_info["extras"]["light"] = {
                "type": light_data.type,
                "color": [clean_float(c) for c in light_data.color],
                "energy": clean_float(light_data.energy),
                "spot_size": clean_float(light_data.spot_size)
                if light_data.type == "SPOT"
                else 0.0,
            }
    elif obj.type == "CAMERA":
        cam_data = obj.data
        node_info["extras"]["camera"] = {
            "type": "PERSP" if cam_data.type == "PERSP" else "ORTHO",
            "fov": clean_float(cam_data.angle),
            "clip_start": clean_float(cam_data.clip_start),
            "clip_end": clean_float(cam_data.clip_end),
        }
    elif obj.IsA("MESH"):
        mesh_id = make_id("mesh", f"{obj.data.name}{mesh_suffix}")

        # Only attach mesh configurations if the object is strictly visible
        if is_visible and mesh_id in exported_meshes:
            node_info["refs"]["mesh_id"] = mesh_id
        else:
            node_info["refs"]["mesh_id"] = None

        node_info["refs"]["material_ids"] = [
            make_id("mat", s.material.name) for s in obj.material_slots if s.material
        ]
        if obj.data.shape_keys:
            node_info["refs"]["morph_weights"] = [
                clean_float(kb.value) for kb in obj.data.shape_keys.key_blocks[1:]
            ]
        try:
            coords = [c_basis @ v.co for v in obj.data.vertices]
            min_bound = [clean_float(min(c[i] for c in coords)) for i in range(3)]
            max_bound = [clean_float(max(c[i] for c in coords)) for i in range(3)]
        except Exception:
            min_bound = [0.0, 0.0, 0.0]
            max_bound = [0.0, 0.0, 0.0]

        node_info["extras"]["bounds"] = {"min": min_bound, "max": max_bound}

    return node_info


def extract_nodes(scene_objects, depsgraph, node_id_map, exported_meshes):
    """Assembles node structures, separating standard objects from instances."""
    nodes = []

    for obj in scene_objects:
        parent_key = obj.parent.name if obj.parent else None
        parent_node_id = node_id_map.get(parent_key) if parent_key else None

        world_yup = c_basis @ obj.matrix_world @ c_basis_inv
        local_yup = (
            c_basis @ obj.matrix_local @ c_basis_inv if obj.parent else world_yup
        )

        node_info = build_node_info(
            obj,
            node_id_map.get(obj.name),
            obj.name,
            parent_node_id,
            local_yup,
            world_yup,
            not obj.hide_viewport,
            exported_meshes,
        )
        nodes.append(node_info)

    for idx, instance in enumerate(depsgraph.object_instances):
        if not instance.is_instance:
            continue

        obj = instance.instance_object
        if not obj or not obj.IsA("MESH"):
            continue

        node_id = f"node_{make_id('', instance.parent.name)}_inst_{idx}"
        node_name = f"{instance.parent.name}_instance_{idx}"
        parent_node_id = (
            node_id_map.get(instance.parent.name) if instance.parent else None
        )

        world_yup = c_basis @ instance.matrix_world @ c_basis_inv
        instancer_world_yup = c_basis @ instance.parent.matrix_world @ c_basis_inv
        local_yup = safe_invert(instancer_world_yup) @ world_yup

        node_info = build_node_info(
            obj,
            node_id,
            node_name,
            parent_node_id,
            local_yup,
            world_yup,
            instance.show_self,
            exported_meshes,
        )
        nodes.append(node_info)

    return nodes


def extract_lights(scene_objects):
    """Extracts KHR_lights_punctual compliant properties."""
    lights = []
    for obj in scene_objects:
        if obj.type == "LIGHT":
            ld = obj.data
            l_type = (
                "directional"
                if ld.type == "SUN"
                else ("spot" if ld.type == "SPOT" else "point")
            )

            light_info = {
                "id": make_id("light", obj.name),
                "type": l_type,
                "color": [clean_float(c) for c in ld.color],
                "intensity": clean_float(ld.energy),
            }
            if l_type == "spot":
                light_info["spot"] = {
                    "innerConeAngle": 0.0,
                    "outerConeAngle": clean_float(ld.spot_size / 2.0),
                }
            lights.append(light_info)
    return lights


def extract_skins(depsgraph, node_id_map, world_yup_map):
    """Extracts skin bindings, inverse matrices, and rest poses."""
    skins = []
    for obj in depsgraph.scene.objects:
        if obj.IsA("ARMATURE"):
            joints_list = []
            ibms_list = []
            rest_pose_list = []
            pose_bone_ids = []

            for bone in obj.data.bones:
                bone_key = f"bone_{obj.name}_{bone.name}"
                bone_node_id = node_id_map.get(bone_key)

                joints_list.append(bone_node_id)
                pose_bone_ids.append(bone_node_id)

                bone_world_zup = obj.matrix_world @ bone.matrix_local
                bone_world_yup = c_basis @ bone_world_zup @ c_basis_inv
                ibm_yup = safe_invert(bone_world_yup)

                ibms_list.extend(serialize_matrix_col_major(ibm_yup))

                rest_pose_yup = c_basis @ bone.matrix_local @ c_basis_inv
                rest_pose_list.extend(serialize_matrix_col_major(rest_pose_yup))

            skins.append(
                {
                    "id": make_id("skin", obj.name),
                    "name": f"{obj.name} Skin",
                    "joints": joints_list,
                    "inverse_bind_matrices": ibms_list,
                    "rest_pose": rest_pose_list,
                    "pose_bone_ids": pose_bone_ids,
                }
            )
    return skins


def extract_animations(
    depsgraph, node_id_map, bone_to_armature, bin_dir, asset_dir, fps
):
    """Saves animated keyframe tracks, packing bone and object channels to .bin."""
    animations = []
    original_frame = bpy.context.scene.frame_current

    for action in bpy.data.actions:
        action_id = make_id("anim", action.name)
        start_frame, end_frame = int(action.frame_range[0]), int(action.frame_range[1])

        if end_frame <= start_frame:
            continue

        duration = clean_float((end_frame - start_frame) / fps)

        fcurves = []
        if hasattr(action, "is_action_layered") and action.is_action_layered:
            for layer in action.layers:
                for strip in layer.strips:
                    for channelbag in strip.channelbags:
                        fcurves.extend(channelbag.fcurves)
        elif hasattr(action, "fcurves") and action.fcurves is not None:
            fcurves = list(action.fcurves)

        target_nodes = set()
        has_object_curves = False

        for fc in fcurves:
            path = fc.data_path
            if path.startswith("pose.bones["):
                bone_name = path.split('"')[1]
                arm_obj = None
                for obj in bpy.data.objects:
                    if (
                        obj.IsA("ARMATURE")
                        and obj.animation_data
                        and obj.animation_data.action == action
                    ):
                        if bone_name in obj.data.bones:
                            arm_obj = obj
                            break
                if not arm_obj:
                    for (arm_name, b_name), o in bone_to_armature.items():
                        if b_name == bone_name:
                            arm_obj = o
                            break

                if arm_obj:
                    bone_key = f"bone_{arm_obj.name}_{bone_name}"
                    target_nodes.add(("BONE", bone_key, arm_obj.name, bone_name))
            else:
                has_object_curves = True

        if has_object_curves and (getattr(action, "id_root", "OBJECT") == "OBJECT"):
            for obj in bpy.data.objects:
                if obj.animation_data and obj.animation_data.action == action:
                    target_nodes.add(("OBJECT", obj.name, obj.name, None))

        if not target_nodes:
            continue

        channels = []
        samplers = []
        sampler_index = 0

        for mode, name, arm_or_obj_name, bone_name in target_nodes:
            obj = bpy.data.objects.get(arm_or_obj_name)
            if obj and obj.animation_data:
                obj.animation_data.action = action

        sampled_times = []
        sampled_transforms = {n: [] for _, n, _, _ in target_nodes}
        eval_dg = bpy.context.evaluated_depsgraph_get()

        for frame in range(start_frame, end_frame + 1):
            bpy.context.scene.frame_set(frame)
            eval_dg.update()

            sampled_times.append(clean_float((frame - start_frame) / fps))

            world_yup_eval = {}
            for instance in eval_dg.object_instances:
                o = instance.object
                world_yup_eval[o.name] = c_basis @ instance.matrix_world @ c_basis_inv

            for o in eval_dg.scene.objects:
                if o.IsA("ARMATURE"):
                    eval_arm = o.evaluated_get(eval_dg)
                    for pb in eval_arm.pose.bones:
                        b_key = f"bone_{o.name}_{pb.name}"
                        b_world_zup = eval_arm.matrix_world @ pb.matrix
                        world_yup_eval[b_key] = c_basis @ b_world_zup @ c_basis_inv

            for mode, n, arm_or_obj_name, bone_name in target_nodes:
                if mode == "OBJECT":
                    obj = bpy.data.objects.get(arm_or_obj_name)
                    parent_key = obj.parent.name if (obj and obj.parent) else None
                else:
                    parts = n.split("_", 2)
                    if len(parts) >= 3:
                        arm_name = parts[1]
                        bone_name = parts[2]
                        obj_arm = bpy.data.objects.get(arm_name)
                        if (
                            obj_arm
                            and hasattr(obj_arm, "data")
                            and hasattr(obj_arm.data, "bones")
                        ):
                            bone = obj_arm.data.bones.get(bone_name)
                            parent_key = (
                                f"bone_{arm_name}_{bone.parent.name}"
                                if (bone and bone.parent)
                                else arm_name
                            )
                        else:
                            parent_key = arm_name
                    else:
                        parent_key = None

                world_yup = world_yup_eval.get(n, Matrix.Identity(4))
                if parent_key and parent_key in world_yup_eval:
                    local_yup = (
                        safe_invert(world_yup_eval.get(parent_key, Matrix.Identity(4)))
                        @ world_yup
                    )
                else:
                    local_yup = world_yup

                t, r, s = local_yup.decompose()
                sampled_transforms[n].append((t, r, s))

        anim_bin_data = b""
        anim_bin_offset = 0

        times_bin = struct.pack(f"{len(sampled_times)}f", *sampled_times)
        anim_bin_data += times_bin
        times_offset = anim_bin_offset
        times_length = len(times_bin)
        anim_bin_offset += times_length

        for n in sampled_transforms:
            transforms = sampled_transforms[n]
            node_id = node_id_map.get(n)

            translations_flat = []
            rotations_flat = []
            scales_flat = []

            for t, r, s in transforms:
                translations_flat.extend([t.x, t.y, t.z])
                rotations_flat.extend([r.x, r.y, r.z, r.w])
                scales_flat.extend([s.x, s.y, s.z])

            t_bin = struct.pack(f"{len(translations_flat)}f", *translations_flat)
            r_bin = struct.pack(f"{len(rotations_flat)}f", *rotations_flat)
            s_bin = struct.pack(f"{len(scales_flat)}f", *scales_flat)

            channels.append(
                {
                    "target_node_id": node_id,
                    "target_path": "translation",
                    "sampler_id": sampler_index,
                }
            )
            samplers.append(
                {
                    "interpolation": "LINEAR",
                    "input_offset": times_offset,
                    "input_length": times_length,
                    "output_offset": anim_bin_offset,
                    "output_length": len(t_bin),
                }
            )
            anim_bin_data += t_bin
            anim_bin_offset += len(t_bin)
            sampler_index += 1

            channels.append(
                {
                    "target_node_id": node_id,
                    "target_path": "rotation",
                    "sampler_id": sampler_index,
                }
            )
            samplers.append(
                {
                    "interpolation": "LINEAR",
                    "input_offset": times_offset,
                    "input_length": times_length,
                    "output_offset": anim_bin_offset,
                    "output_length": len(r_bin),
                }
            )
            anim_bin_data += r_bin
            anim_bin_offset += len(r_bin)
            sampler_index += 1

            channels.append(
                {
                    "target_node_id": node_id,
                    "target_path": "scale",
                    "sampler_id": sampler_index,
                }
            )
            samplers.append(
                {
                    "interpolation": "LINEAR",
                    "input_offset": times_offset,
                    "input_length": times_length,
                    "output_offset": anim_bin_offset,
                    "output_length": len(s_bin),
                }
            )
            anim_bin_data += s_bin
            anim_bin_offset += len(s_bin)
            sampler_index += 1

        anim_bin_path = os.path.join(bin_dir, f"{action_id}_anim.bin")
        with open(anim_bin_path, "wb") as f:
            f.write(anim_bin_data)

        rel_bin_path = os.path.relpath(anim_bin_path, asset_dir)
        for s in samplers:
            s["bin_file"] = rel_bin_path

        animations.append(
            {
                "id": action_id,
                "name": action.name,
                "duration": duration,
                "loop": bool(action.get("loop", False)),
                "channels": channels,
                "samplers": samplers,
            }
        )

    bpy.context.scene.frame_set(original_frame)
    return animations


# ============================================================================
# Core Execution Pipeline
# ============================================================================


def export_raw_scene_data(blend_path):
    name_we = os.path.splitext(os.path.basename(blend_path))[0]
    asset_dir = os.path.join(output_parent, name_we)
    bin_dir = os.path.join(asset_dir, "geometry")
    os.makedirs(bin_dir, exist_ok=True)

    bpy.ops.wm.open_mainfile(filepath=blend_path)
    try:
        with bpy.context.temp_override(selected_objects=list(bpy.data.objects)):
            bpy.ops.object.make_all_local(type="ALL")
    except Exception:
        try:
            bpy.ops.object.make_all_local()
        except Exception:
            pass

    export_and_copy_textures(asset_dir)

    depsgraph = bpy.context.evaluated_depsgraph_get()
    scene_objects = [obj for obj in bpy.context.scene.objects]

    node_id_map = {}
    for obj in scene_objects:
        node_id_map[obj.name] = make_id("node", obj.name)
        if obj.IsA("ARMATURE"):
            for bone in obj.data.bones:
                node_id_map[f"bone_{obj.name}_{bone.name}"] = make_id(
                    "node_bone", f"{obj.name}_{bone.name}"
                )

    bone_to_armature = {}
    for obj in bpy.data.objects:
        if obj.IsA("ARMATURE"):
            for bone in obj.data.bones:
                bone_to_armature[(obj.name, bone.name)] = obj

    world_yup_map = precalculate_world_matrices(scene_objects, depsgraph)

    materials = extract_materials()

    geometry_sources = []
    for obj in scene_objects:
        if obj.IsA("MESH"):
            geometry_sources.append(obj)
    for instance in depsgraph.object_instances:
        if instance.is_instance and instance.instance_object:
            geometry_sources.append(instance.instance_object)

    exported_meshes = set()
    meshes = extract_meshes(
        geometry_sources, depsgraph, asset_dir, bin_dir, exported_meshes
    )
    nodes = extract_nodes(scene_objects, depsgraph, node_id_map, exported_meshes)
    lights = extract_lights(scene_objects)
    skins = extract_skins(depsgraph, node_id_map, world_yup_map)

    fps = bpy.context.scene.render.fps / bpy.context.scene.render.fps_base
    animations = extract_animations(
        depsgraph, node_id_map, bone_to_armature, bin_dir, asset_dir, fps
    )

    scene_manifest = {
        "version": "1.2",
        "scene_info": {
            "name": name_we,
            "up_axis": "Y",
            "coordinate_system": "Right-Handed",
            "winding_order": "CCW",
            "matrix_layout": "Column-Major",
        },
        "materials": materials,
        "meshes": meshes,
        "nodes": nodes,
        "lights": lights,
        "skins": skins,
        "animations": animations,
        "extras": {},
    }

    json_path = os.path.join(asset_dir, "metadata.json")
    with open(json_path, "w") as f:
        json.dump(scene_manifest, f, indent=2)

    print(f"      [Success] Extracted raw metadata & binary geometry for: {name_we}")


for idx, blend_path in enumerate(blend_files, start=1):
    print(
        f"[{idx}/{len(blend_files)}] Extracting: {os.path.relpath(blend_path, input_dir)}"
    )
    try:
        export_raw_scene_data(blend_path)
    except Exception as e:
        print(f"      [Error] Extraction failed: {e}")
