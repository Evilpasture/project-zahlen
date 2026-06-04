# tools/export_metadata.py
import os
import json
import struct
import shutil
import bpy
from mathutils import Matrix

# Setup output paths
input_dir = os.getcwd()
output_parent = os.path.join(input_dir, "resources", "intermediate")
os.makedirs(output_parent, exist_ok=True)

# glTF/OpenGL Basis Transform: Blender Z-up right-handed to Y-up right-handed
# preserving winding order (CCW remains CCW)
c_basis = Matrix(
    (
        (1.0, 0.0, 0.0, 0.0),
        (0.0, 0.0, 1.0, 0.0),
        (0.0, -1.0, 0.0, 0.0),
        (0.0, 0.0, 0.0, 1.0),
    )
)
c_basis_inv = c_basis.inverted()

# Discover levels for metadata extraction
blend_files = []
for root, _, files in os.walk(input_dir):
    norm_root = root.replace("\\", "/").lower()
    if any(
        p in norm_root
        for p in ["resources", "exported_assets", "tadc_models", "asset_library"]
    ):
        continue
    for file in files:
        if file.endswith(".blend") and not file.startswith("."):
            blend_files.append(os.path.join(root, file))

print(f"Discovered {len(blend_files)} levels for raw metadata extraction.\n")


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
    """Safely inverts a matrix, falling back to identity if singular (scale of 0)."""
    try:
        return matrix.inverted()
    except ValueError:
        return Matrix.Identity(4)


def serialize_matrix_col_major(matrix):
    """Converts a mathutils.Matrix to a flat column-major list of 16 rounded floats."""
    return [clean_float(val) for col in matrix.transposed() for val in col]


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


def get_texture_node_image_block(input_socket):
    """Recursively walks node trees to extract the actual image object, resolving normal wrappers."""
    if input_socket and input_socket.is_linked:
        link = input_socket.links[0]
        from_node = link.from_node
        if from_node.type == "TEX_IMAGE" and from_node.image:
            return from_node.image
        if from_node.type == "NORMAL_MAP":
            color_input = from_node.inputs.get("Color")
            if color_input and color_input.is_linked:
                norm_link = color_input.links[0]
                norm_node = norm_link.from_node
                if norm_node.type == "TEX_IMAGE" and norm_node.image:
                    return norm_node.image
    return None


def export_and_copy_textures(asset_dir):
    """Safely extracts and copies packed/unpacked textures into the intermediate asset directory."""
    textures_dir = os.path.join(asset_dir, "textures")
    os.makedirs(textures_dir, exist_ok=True)

    for image in bpy.data.images:
        # Skip procedural render results and viewer nodes
        if image.type in {"COMPOSITER", "VIEWER"}:
            continue

        # Ensure it's a file-backed image or has pixels we can output
        if image.source not in {"FILE", "GENERATED", "TILED"}:
            continue

        dest_filename = get_image_filename(image)
        dest_path = os.path.join(textures_dir, dest_filename)

        # 1. Handle Packed Files
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
            # 2. Handle External Files on Disk
            src_path = bpy.path.abspath(image.filepath)
            if os.path.exists(src_path):
                try:
                    shutil.copy2(src_path, dest_path)
                except Exception as e:
                    print(
                        f"      [Warning] Failed to copy texture '{image.name}' from '{src_path}': {e}"
                    )
            else:
                # 3. Fallback: Save render if the source path is missing but pixels exist
                try:
                    image.save_render(dest_path)
                except Exception as e:
                    print(
                        f"      [Warning] Source missing and fallback failed for '{image.name}': {e}"
                    )


def export_raw_scene_data(blend_path):
    name_we = os.path.splitext(os.path.basename(blend_path))[0]
    asset_dir = os.path.join(output_parent, name_we)
    bin_dir = os.path.join(asset_dir, "geometry")
    os.makedirs(bin_dir, exist_ok=True)

    # Load Blender file headlessly
    bpy.ops.wm.open_mainfile(filepath=blend_path)

    # Prevent external library lookup issues using contextual overrides
    try:
        with bpy.context.temp_override(selected_objects=list(bpy.data.objects)):
            bpy.ops.object.make_all_local(type="ALL")
    except Exception:
        try:
            bpy.ops.object.make_all_local()
        except Exception:
            pass

    # Export and copy all scene textures locally (Fixed headless lookup check)
    export_and_copy_textures(asset_dir)

    depsgraph = bpy.context.evaluated_depsgraph_get()

    # Pre-map all node IDs in the scene graph
    node_id_map = {}
    for instance in depsgraph.object_instances:
        obj = instance.object
        node_id_map[obj.name] = make_id("node", obj.name)
        if obj.type == "ARMATURE":
            for bone in obj.data.bones:
                node_id_map[f"bone_{obj.name}_{bone.name}"] = make_id(
                    "node_bone", f"{obj.name}_{bone.name}"
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
        "materials": [],
        "meshes": [],
        "nodes": [],
        "skins": [],
        "animations": [],
        "extras": {},
    }

    # 1. Pre-calculate Y-Up World Matrices for all objects & bones
    world_yup_map = {}
    for instance in depsgraph.object_instances:
        obj = instance.object
        world_yup_map[obj.name] = c_basis @ instance.matrix_world @ c_basis_inv

    for obj in depsgraph.scene.objects:
        if obj.type == "ARMATURE":
            for bone in obj.data.bones:
                bone_key = f"bone_{obj.name}_{bone.name}"
                bone_world_zup = obj.matrix_world @ bone.matrix_local
                world_yup_map[bone_key] = c_basis @ bone_world_zup @ c_basis_inv

    # 2. Extract PBR Materials
    for mat in bpy.data.materials:
        mat_id = make_id("mat", mat.name)
        mat_info = {
            "id": mat_id,
            "name": mat.name,
            "pbr": {
                "base_color": [0.8, 0.8, 0.8, 1.0],
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

        if hasattr(mat, "node_tree") and mat.node_tree:
            principled = next(
                (n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None
            )
            if principled:
                mat_info["pbr"]["base_color"] = [
                    clean_float(c)
                    for c in principled.inputs["Base Color"].default_value
                ]
                mat_info["pbr"]["metallic"] = clean_float(
                    principled.inputs["Metallic"].default_value
                )
                mat_info["pbr"]["roughness"] = clean_float(
                    principled.inputs["Roughness"].default_value
                )

                em_col = principled.inputs.get(
                    "Emission Color"
                ) or principled.inputs.get("Emission")
                em_str = principled.inputs.get("Emission Strength")
                if em_col:
                    mat_info["pbr"]["emissive_factor"] = [
                        clean_float(c) for c in em_col.default_value[:3]
                    ]
                if em_str:
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

        scene_manifest["materials"].append(mat_info)

    # 3. Extract Unique Meshes, Primitives & Package Binary Buffers
    exported_meshes = set()

    for instance in depsgraph.object_instances:
        obj = instance.object
        if obj.type != "MESH":
            continue

        # Check for skeletal rigging modifier
        armature_mod = next((m for m in obj.modifiers if m.type == "ARMATURE"), None)
        is_skinned = armature_mod is not None and armature_mod.object is not None

        # Suffix ID to prevent data sharing conflicts
        mesh_suffix = "_skinned" if is_skinned else ""
        mesh_id = make_id("mesh", f"{obj.data.name}{mesh_suffix}")

        if mesh_id in exported_meshes:
            continue

        mesh_data = get_evaluated_mesh_safely(obj, depsgraph)
        if not mesh_data:
            continue

        # Initialize split normals
        try:
            mesh_data.calc_normals_split()
        except Exception:
            pass

        # Retrieve active split geometry layers
        active_uv_layer = (
            mesh_data.uv_layers.active.data if mesh_data.uv_layers else None
        )
        has_tangents = False
        try:
            mesh_data.calc_tangents()
            has_tangents = True
        except Exception:
            pass

        # Skinning lookup structures
        group_to_joint_idx = {}
        if is_skinned:
            arm_obj = armature_mod.object
            bone_names = [b.name for b in arm_obj.data.bones]
            group_to_joint_idx = {
                idx: bone_names.index(g.name)
                for idx, g in enumerate(obj.vertex_groups)
                if g.name in bone_names
            }

        # Build loop-indexed split-vertex table (Corrects normal/tangent/UV seam artifacts)
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

                    # NOTE: The stride is strictly 48 bytes (12 floats * 4 bytes) matching P3N3T4U2.
                    # If the mesh has no tangents, we manually write (1.0, 0.0, 0.0, 1.0) as a fallback
                    # so the binary buffer layout remains completely aligned and consistent for the compiler.
                    if has_tangents:
                        tangent = mesh_data.loops[loop_idx].tangent
                        sign = mesh_data.loops[loop_idx].bitangent_sign
                    else:
                        tangent = (1.0, 0.0, 0.0)
                        sign = 1.0

                    # Standard unique combination key
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
                    )

                    if key in unique_vertices:
                        idx = unique_vertices[key]
                    else:
                        idx = vertex_count
                        unique_vertices[key] = idx
                        vertex_count += 1

                        # Align attributes to column-major right-handed Y-up system (X, Z, -Y)
                        flat_vbo.extend([co[0], co[2], -co[1]])
                        flat_vbo.extend([normal[0], normal[2], -normal[1]])
                        flat_vbo.extend([tangent[0], tangent[2], -tangent[1], sign])
                        flat_vbo.extend([uv[0], 1.0 - uv[1]])  # Flip V for glTF

                        # Skin weight collection corresponding to unique split indices
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

        # Morph Targets (Shape Keys) aligned to split indices (delta normals omitted)
        morph_targets = []
        blend_weights = []
        morph_binary = b""
        if mesh_data.shape_keys:
            key_blocks = mesh_data.shape_keys.key_blocks
            basis_key = key_blocks[0]
            basis_cos = [0.0] * (len(mesh_data.vertices) * 3)
            basis_key.data.foreach_get("co", basis_cos)

            morph_offset = 0
            for idx, kb in enumerate(key_blocks[1:]):  # Skip Basis key
                target_cos = [0.0] * (len(mesh_data.vertices) * 3)
                kb.data.foreach_get("co", target_cos)

                deltas_pos = [0.0] * (vertex_count * 3)

                # Propagate basic vertex deltas into duplicate split spaces
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

        # Save buffer bytes to unified .bin file
        bin_path = os.path.join(bin_dir, f"{mesh_id}.bin")
        with open(bin_path, "wb") as f:
            f.write(vbo_binary)
            f.write(ibo_binary)
            if is_skinned:
                f.write(joints_binary)
                f.write(weights_binary)
            if morph_binary:
                f.write(morph_binary)

        # Set relative byte bounds
        v_offset = 0
        v_len = len(vbo_binary)
        i_offset = v_len
        i_len = len(ibo_binary)

        # Compose the combined layout description string dynamically
        layout = "P3N3T4U2"
        if is_skinned:
            layout += "_J4W4"
        if morph_targets:
            layout += f"_M{len(morph_targets)}"

        mesh_info = {
            "id": mesh_id,
            "name": f"{obj.data.name} Mesh",
            "vertex_count": vertex_count,  # The number of unique split-VBO entries (used by the engine's loader)
            "source_vertex_count": len(
                mesh_data.vertices
            ),  # Debug: Raw vertex count in Blender before UV splitting
            "layout": layout,
            "buffers": {
                "bin_file": os.path.relpath(bin_path, asset_dir),
                "vertex_buffer": {
                    "byte_offset": v_offset,
                    "byte_length": v_len,
                    "stride": 48,
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

        scene_manifest["meshes"].append(mesh_info)
        exported_meshes.add(mesh_id)

        try:
            obj.evaluated_get(depsgraph).to_mesh_clear()
        except Exception:
            try:
                bpy.data.meshes.remove(mesh_data)
            except Exception:
                pass

    # 4. Extract Nodes (Object and Bone Hierarchies)
    for instance in depsgraph.object_instances:
        obj = instance.object

        # Parent mapping
        if obj.parent:
            if obj.parent_type == "BONE" and obj.parent_bone:
                parent_key = f"bone_{obj.parent.name}_{obj.parent_bone}"
            else:
                parent_key = obj.parent.name
        else:
            parent_key = None

        parent_node_id = node_id_map.get(parent_key) if parent_key else None

        # Fetch local transform matrix securely via .get fallbacks
        world_yup = world_yup_map.get(obj.name, Matrix.Identity(4))
        if parent_key and parent_key in world_yup_map:
            local_yup = (
                safe_invert(world_yup_map.get(parent_key, Matrix.Identity(4)))
                @ world_yup
            )
        else:
            local_yup = world_yup

        # Compute modifier stack
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
            "id": node_id_map.get(obj.name),
            "name": obj.name,
            "type": obj.type,
            "parent_id": parent_node_id,
            "transform": {
                "local": serialize_matrix_col_major(local_yup),
                "world": serialize_matrix_col_major(world_yup),
            },
            "refs": {
                "mesh_id": None,
                "skin_id": None,
                "material_ids": [],
                "morph_weights": [],
            },
            "extras": {},
            "modifier_stack": modifier_stack,
        }

        # Armature skin pointer
        armature_mod = next((m for m in obj.modifiers if m.type == "ARMATURE"), None)
        if armature_mod and armature_mod.object:
            node_info["refs"]["skin_id"] = make_id("skin", armature_mod.object.name)

        # Build extras
        if obj.type == "LIGHT":
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
        elif obj.type == "MESH":
            is_skinned = armature_mod is not None and armature_mod.object is not None
            mesh_suffix = "_skinned" if is_skinned else ""

            node_info["refs"]["mesh_id"] = make_id(
                "mesh", f"{obj.data.name}{mesh_suffix}"
            )
            node_info["refs"]["material_ids"] = [
                make_id("mat", s.material.name)
                for s in obj.material_slots
                if s.material
            ]

            # Active Shape Key weights on this node instance
            if obj.data.shape_keys:
                node_info["refs"]["morph_weights"] = [
                    clean_float(kb.value) for kb in obj.data.shape_keys.key_blocks[1:]
                ]

            # Generate right-handed local bounding bounds
            try:
                coords = [c_basis @ v.co for v in obj.data.vertices]
                min_bound = [clean_float(min(c[i] for c in coords)) for i in range(3)]
                max_bound = [clean_float(max(c[i] for c in coords)) for i in range(3)]
            except Exception:
                min_bound = [0.0, 0.0, 0.0]
                max_bound = [0.0, 0.0, 0.0]

            node_info["extras"]["bounds"] = {"min": min_bound, "max": max_bound}

        scene_manifest["nodes"].append(node_info)

        # Pose bone scene node instantiation
        if obj.type == "ARMATURE":
            for bone in obj.data.bones:
                bone_key = f"bone_{obj.name}_{bone.name}"
                bone_world_yup = world_yup_map.get(bone_key, Matrix.Identity(4))

                if bone.parent:
                    bone_parent_key = f"bone_{obj.name}_{bone.parent.name}"
                else:
                    bone_parent_key = obj.name

                bone_parent_world_yup = world_yup_map.get(
                    bone_parent_key, Matrix.Identity(4)
                )
                bone_local_yup = safe_invert(bone_parent_world_yup) @ bone_world_yup

                bone_node_info = {
                    "id": node_id_map.get(bone_key),
                    "name": bone.name,
                    "type": "BONE",
                    "parent_id": node_id_map.get(bone_parent_key),
                    "transform": {
                        "local": serialize_matrix_col_major(bone_local_yup),
                        "world": serialize_matrix_col_major(bone_world_yup),
                    },
                    "refs": {
                        "mesh_id": None,
                        "skin_id": None,
                        "material_ids": [],
                        "morph_weights": [],
                    },
                    "extras": {},
                    "modifier_stack": [],
                }
                scene_manifest["nodes"].append(bone_node_info)

    # 5. Extract Skins
    for obj in depsgraph.scene.objects:
        if obj.type == "ARMATURE":
            joints_list = []
            ibms_list = []
            rest_pose_list = []
            pose_bone_ids = []

            for bone in obj.data.bones:
                bone_key = f"bone_{obj.name}_{bone.name}"
                bone_node_id = node_id_map.get(bone_key)

                joints_list.append(bone_node_id)
                pose_bone_ids.append(bone_node_id)

                # Inverse Bind Matrix calculation
                bone_world_zup = obj.matrix_world @ bone.matrix_local
                bone_world_yup = c_basis @ bone_world_zup @ c_basis_inv
                ibm_yup = safe_invert(bone_world_yup)

                ibms_list.extend(serialize_matrix_col_major(ibm_yup))

                # Rest Pose matrix (bone.matrix_local in Blender is rest space transform)
                rest_pose_yup = c_basis @ bone.matrix_local @ c_basis_inv
                rest_pose_list.extend(serialize_matrix_col_major(rest_pose_yup))

            scene_manifest["skins"].append(
                {
                    "id": make_id("skin", obj.name),
                    "name": f"{obj.name} Skin",
                    "joints": joints_list,
                    "inverse_bind_matrices": ibms_list,
                    "rest_pose": rest_pose_list,
                    "pose_bone_ids": pose_bone_ids,
                }
            )

    # 6. Extract Animations (Bakes actions into TRS channels)
    fps = bpy.context.scene.render.fps / bpy.context.scene.render.fps_base
    original_frame = bpy.context.scene.frame_current

    # Optimization: pre-calculate global bone -> armature mapping once to avoid O(N^2) loops
    bone_to_armature = {}
    for obj in bpy.data.objects:
        if obj.type == "ARMATURE":
            for bone in obj.data.bones:
                bone_to_armature[(obj.name, bone.name)] = obj

    for action in bpy.data.actions:
        action_id = make_id("anim", action.name)
        start_frame, end_frame = int(action.frame_range[0]), int(action.frame_range[1])

        # Safe length guard
        if end_frame <= start_frame:
            continue

        duration = clean_float((end_frame - start_frame) / fps)

        # Safely collect fcurves for Blender 5.0+ Slotted/Layered Animations
        fcurves = []
        if hasattr(action, "is_action_layered") and action.is_action_layered:
            for layer in action.layers:
                for strip in layer.strips:
                    for channelbag in strip.channelbags:
                        fcurves.extend(channelbag.fcurves)
        elif hasattr(action, "fcurves") and action.fcurves is not None:
            fcurves = list(action.fcurves)

        # Identify channels modified by curves securely
        target_nodes = set()
        has_object_curves = False

        for fc in fcurves:
            path = fc.data_path
            if path.startswith("pose.bones["):
                bone_name = path.split('"')[1]
                # Match bone to containing armature
                arm_obj = None
                for obj in bpy.data.objects:
                    if (
                        obj.type == "ARMATURE"
                        and obj.animation_data
                        and obj.animation_data.action == action
                    ):
                        if bone_name in obj.data.bones:
                            arm_obj = obj
                            break
                if not arm_obj:
                    # Fallback to any bone-to-armature registry match
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
            # Sample only objects explicitly associated with the active action
            for obj in bpy.data.objects:
                if obj.animation_data and obj.animation_data.action == action:
                    target_nodes.add(("OBJECT", obj.name, obj.name, None))

        if not target_nodes:
            continue

        channels = []
        samplers = []
        sampler_index = 0

        # Temporarily link action to active object hierarchy to sample
        for mode, name, arm_or_obj_name, bone_name in target_nodes:
            if mode == "OBJECT":
                obj = bpy.data.objects.get(arm_or_obj_name)
                if obj and obj.animation_data:
                    obj.animation_data.action = action
            elif mode == "BONE":
                obj = bpy.data.objects.get(arm_or_obj_name)
                if obj and obj.animation_data:
                    obj.animation_data.action = action

        # Sample frames
        sampled_times = []
        sampled_transforms = {n: [] for _, n, _, _ in target_nodes}

        # Cache the evaluated depsgraph once before the loop for high-performance updates
        eval_dg = bpy.context.evaluated_depsgraph_get()

        for frame in range(start_frame, end_frame + 1):
            bpy.context.scene.frame_set(frame)
            eval_dg.update()  # Update the active depsgraph state

            sampled_times.append(clean_float((frame - start_frame) / fps))

            # Recalculate evaluated world transforms
            world_yup_eval = {}
            for instance in eval_dg.object_instances:
                o = instance.object
                world_yup_eval[o.name] = c_basis @ instance.matrix_world @ c_basis_inv

            # Bone Animation Sampling using evaluated pose-bone and evaluated-armature world matrices
            for o in eval_dg.scene.objects:
                if o.type == "ARMATURE":
                    eval_arm = o.evaluated_get(eval_dg)
                    for pb in eval_arm.pose.bones:
                        b_key = f"bone_{o.name}_{pb.name}"
                        b_world_zup = eval_arm.matrix_world @ pb.matrix
                        world_yup_eval[b_key] = c_basis @ b_world_zup @ c_basis_inv

            # Extract active transforms safely avoiding null objects and KeyErrors
            for mode, n, arm_or_obj_name, bone_name in target_nodes:
                if mode == "OBJECT":
                    obj = bpy.data.objects.get(arm_or_obj_name)
                    parent_key = obj.parent.name if (obj and obj.parent) else None
                else:  # BONE
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
                            if bone:
                                parent_key = (
                                    f"bone_{arm_name}_{bone.parent.name}"
                                    if bone.parent
                                    else arm_name
                                )
                            else:
                                parent_key = arm_name
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

        # Pack binary files per Action
        anim_bin_data = b""
        anim_bin_offset = 0

        # Pack time array once
        times_bin = struct.pack(f"{len(sampled_times)}f", *sampled_times)
        anim_bin_data += times_bin
        times_offset = anim_bin_offset
        times_length = len(times_bin)
        anim_bin_offset += times_length

        for n in sampled_transforms:
            transforms = sampled_transforms[n]
            node_id = node_id_map.get(n)

            # Translation, Rotation, and Scale separate packaging
            translations_flat = []
            rotations_flat = []
            scales_flat = []

            for t, r, s in transforms:
                translations_flat.extend([t.x, t.y, t.z])
                # Convert quaternion from [w, x, y, z] to glTF compliant [x, y, z, w]
                rotations_flat.extend([r.x, r.y, r.z, r.w])
                scales_flat.extend([s.x, s.y, s.z])

            t_bin = struct.pack(f"{len(translations_flat)}f", *translations_flat)
            r_bin = struct.pack(f"{len(rotations_flat)}f", *rotations_flat)
            s_bin = struct.pack(f"{len(scales_flat)}f", *scales_flat)

            # 1. Translation Channel
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

            # 2. Rotation Channel
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

            # 3. Scale Channel
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

        # Write Action binary file
        anim_bin_path = os.path.join(bin_dir, f"{action_id}_anim.bin")
        with open(anim_bin_path, "wb") as f:
            f.write(anim_bin_data)

        # Assign binary path inside samplers
        rel_bin_path = os.path.relpath(anim_bin_path, asset_dir)
        for s in samplers:
            s["bin_file"] = rel_bin_path

        scene_manifest["animations"].append(
            {
                "id": action_id,
                "name": action.name,
                "duration": duration,
                "loop": bool(
                    action.get("loop", False)
                ),  # Default animation loop is False for safety (e.g. cutscenes/one-shots)
                "channels": channels,
                "samplers": samplers,
            }
        )

    # Restore scene state
    bpy.context.scene.frame_set(original_frame)

    # Save Metadata JSON
    json_path = os.path.join(asset_dir, "metadata.json")
    with open(json_path, "w") as f:
        json.dump(scene_manifest, f, indent=2)

    print(f"      [Success] Extracted raw metadata & binary geometry for: {name_we}")


# Run extractor
for idx, blend_path in enumerate(blend_files, start=1):
    print(
        f"[{idx}/{len(blend_files)}] Extracting: {os.path.relpath(blend_path, input_dir)}"
    )
    try:
        export_raw_scene_data(blend_path)
    except Exception as e:
        print(f"      [Error] Extraction failed: {e}")
