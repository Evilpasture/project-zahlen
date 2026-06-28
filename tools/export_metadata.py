# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# tools/export_metadata.py
import os
import struct
import shutil
import sys
import bpy
from mathutils import Matrix
import math

# Set recursion limit for large hierarchical models
sys.setrecursionlimit(15000)

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
# Core Math, String & Search Helpers
# ============================================================================


def discover_blend_files(search_path):
    """Recursively scans directories to discover valid Blend files."""
    blend_files = []
    for root, _, files in os.walk(search_path):
        norm_root = root.replace("\\", "/").lower()
        if any(p in norm_root for p in ["resources", "exported_assets"]):
            continue
        for file in files:
            if file.endswith(".blend") and not file.startswith("."):
                if "void" in file.lower():
                    continue
                blend_files.append(os.path.join(root, file))
    return blend_files


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


def remove_scale_from_matrix(matrix):
    """Returns a copy of the matrix with uniform/non-uniform scaling removed (axes normalized)."""
    m = matrix.copy()
    col0 = m.col[0].to_3d().normalized()
    col1 = m.col[1].to_3d().normalized()
    col2 = m.col[2].to_3d().normalized()
    m.col[0] = (col0.x, col0.y, col0.z, m.col[0].w)
    m.col[1] = (col1.x, col1.y, col1.z, m.col[1].w)
    m.col[2] = (col2.x, col2.y, col2.z, m.col[2].w)
    return m


def scalar_to_rgb(t):
    """Procedurally maps a single scalar float to a 3D RGB color wheel."""
    r = 0.65 + 0.35 * math.cos(2.0 * math.pi * (t + 0.0))
    g = 0.65 + 0.35 * math.cos(2.0 * math.pi * (t + 0.33))
    b = 0.65 + 0.35 * math.cos(2.0 * math.pi * (t + 0.67))
    return (round(r, 4), round(g, 4), round(b, 4), 1.0)


def unpack_color(val):
    if val is None:
        return (1.0, 1.0, 1.0, 1.0)
    if not hasattr(val, "__len__"):
        try:
            f = float(val)
            return scalar_to_rgb(f)
        except Exception:
            return (1.0, 1.0, 1.0, 1.0)
    length = len(val)
    if length == 0:
        return (1.0, 1.0, 1.0, 1.0)
    elif length == 1:
        try:
            f = float(val[0])
            return scalar_to_rgb(f)
        except Exception:
            return (1.0, 1.0, 1.0, 1.0)
    elif length == 2:
        return (float(val[0]), float(val[1]), 0.0, 1.0)
    elif length == 3:
        return (float(val[0]), float(val[1]), float(val[2]), 1.0)
    else:
        return (float(val[0]), float(val[1]), float(val[2]), float(val[3]))


def unpack_color_from_datum(datum):
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


def get_scalar_value_from_datum(datum):
    if not datum:
        return 0.0
    if hasattr(datum, "value"):
        return float(datum.value)
    if hasattr(datum, "color"):
        return float(datum.color[0])
    if hasattr(datum, "vector"):
        return float(datum.vector[0])
    return 0.0


def is_valid_color_layer(attr):
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


def find_root_shader_node(material):
    """Resolves the root shader node driving the active surface output."""
    if not material.node_tree:
        return None

    output_node = next(
        (
            n
            for n in material.node_tree.nodes
            if n.type == "OUTPUT_MATERIAL" and n.is_active_output
        ),
        None,
    )
    if not output_node:
        output_node = next(
            (n for n in material.node_tree.nodes if n.type == "OUTPUT_MATERIAL"), None
        )

    if output_node:
        surface_input = output_node.inputs.get("Surface")
        if surface_input and surface_input.is_linked:
            return surface_input.links[0].from_node
    return None


# ============================================================================
# Shader Node Graph Bytecode Compiler (Python Front-End)
# ============================================================================


def build_procedural_instructions(material):
    root = find_root_shader_node(material)
    if not root:
        return None

    # Guard: Ensure we only compile actual procedural patterns (no image-textured materials)
    has_image_texture = False
    has_procedural_nodes = False
    if material.node_tree:
        for node in material.node_tree.nodes:
            if node.type == "TEX_IMAGE":
                has_image_texture = True
            if node.type in {"TEX_VORONOI", "TEX_NOISE", "TEX_MUSGRAVE"}:
                has_procedural_nodes = True

    if has_image_texture or not has_procedural_nodes:
        return None

    base_color_node = None
    if root.type == "BSDF_PRINCIPLED":
        socket = root.inputs.get("Base Color")
        if socket and socket.is_linked:
            base_color_node = socket.links[0].from_node
    else:
        base_color_node = root

    if not base_color_node:
        return None

    # Topological Sort via DFS
    visited = set()
    ordered = []

    def dfs(node):
        if node.name in visited:
            return
        visited.add(node.name)
        for inp in node.inputs:
            if inp.is_linked:
                for link in inp.links:
                    dfs(link.from_node)
        ordered.append(node)

    dfs(base_color_node)

    node_to_idx = {}
    instructions = []

    for idx, node in enumerate(ordered):
        node_to_idx[node.name] = idx

        def get_inp(name, fallback_idx=0):
            inp = node.inputs.get(name)
            if not inp and len(node.inputs) > fallback_idx:
                inp = node.inputs[fallback_idx]
            if inp and inp.is_linked:
                return node_to_idx[inp.links[0].from_node.name]
            return -1

        floats = []

        # Opcode 0: TEX_COORD
        if node.type in {"TEX_COORD", "NEW_GEOMETRY"}:
            floats.append(0)

        # Opcode 1: MAPPING
        elif node.type == "MAPPING":
            floats.append(1)
            floats.append(get_inp("Vector", 0))
            loc = (
                node.inputs["Location"].default_value
                if "Location" in node.inputs
                else [0, 0, 0]
            )
            rot = (
                node.inputs["Rotation"].default_value
                if "Rotation" in node.inputs
                else [0, 0, 0]
            )
            sca = (
                node.inputs["Scale"].default_value
                if "Scale" in node.inputs
                else [1, 1, 1]
            )
            floats.extend([clean_float(x) for x in loc])
            floats.extend([clean_float(x) for x in rot])
            floats.extend([clean_float(x) for x in sca])

        # Opcode 2: TEX_VORONOI
        elif node.type == "TEX_VORONOI":
            floats.append(2)
            floats.append(get_inp("Vector", 0))
            scale = (
                node.inputs["Scale"].default_value if "Scale" in node.inputs else 5.0
            )
            rand = (
                node.inputs["Randomness"].default_value
                if "Randomness" in node.inputs
                else 1.0
            )
            floats.extend([clean_float(scale), clean_float(rand)])

        # Opcode 3: VALTORGB (Color Ramp)
        elif node.type == "VALTORGB":
            floats.append(3)
            floats.append(get_inp("Fac", 0))
            floats.append(0 if node.color_ramp.interpolation == "CONSTANT" else 1)
            floats.append(len(node.color_ramp.elements))
            for el in node.color_ramp.elements:
                floats.append(clean_float(el.position))
                floats.extend([clean_float(c) for c in el.color])

        # Opcode 4: MIX / MIX_RGB
        elif node.type in {"MIX", "MIX_RGB"}:
            floats.append(4)
            fac_inp = node.inputs.get("Factor") or node.inputs.get("Fac")
            a_inp = node.inputs.get("A") or node.inputs.get("Color1")
            b_inp = node.inputs.get("B") or node.inputs.get("Color2")

            floats.append(get_inp(fac_inp.name if fac_inp else "Fac", 0))
            floats.append(get_inp(a_inp.name if a_inp else "A", 1))
            floats.append(get_inp(b_inp.name if b_inp else "B", 2))

            blend_map = {
                "MIX": 0,
                "LIGHTEN": 1,
                "MULTIPLY": 2,
                "ADD": 3,
                "SCREEN": 4,
                "DARKEN": 5,
            }
            btype = blend_map.get(getattr(node, "blend_type", "MIX"), 0)
            floats.append(btype)

            floats.append(
                clean_float(fac_inp.default_value)
                if fac_inp and not hasattr(fac_inp.default_value, "__len__")
                else 0.5
            )
            def_a = a_inp.default_value if a_inp else [1, 1, 1, 1]
            def_b = b_inp.default_value if b_inp else [1, 1, 1, 1]
            floats.extend(
                [
                    clean_float(x)
                    for x in (
                        def_a if hasattr(def_a, "__len__") else [def_a, def_a, def_a, 1]
                    )
                ]
            )
            floats.extend(
                [
                    clean_float(x)
                    for x in (
                        def_b if hasattr(def_b, "__len__") else [def_b, def_b, def_b, 1]
                    )
                ]
            )
        else:
            floats.append(-1)

        instructions.append((f"inst_{idx}", floats))

    return {"type": "NODE_GRAPH", "parameters": dict(instructions)}


def serialize_instruction_to_floats(inst):
    t = inst["type"]
    inputs = inst["inputs"]
    p = inst["params"]

    floats = []
    if t == 0:
        floats.append(float(p.get("coord_type", 1)))
    elif t == 1:
        floats.extend(p.get("translation", [0, 0, 0]))
        floats.extend(p.get("rotation", [0, 0, 0]))
        floats.extend(p.get("scale", [1, 1, 1]))
    elif t == 2:
        floats.append(p.get("scale", 5.0))
        floats.append(p.get("randomness", 1.0))
    elif t == 3:
        floats.append(float(p.get("interpolation", 1)))
        elements = p.get("elements", [])
        floats.append(float(len(elements)))
        for pos, col in elements:
            floats.append(pos)
            floats.extend(col)
    elif t == 4:
        floats.append(float(p.get("blend_type", 0)))
        floats.append(p.get("factor", 1.0))
        floats.extend(p.get("color1", [1, 1, 1, 1]))
        floats.extend(p.get("color2", [1, 1, 1, 1]))
    elif t == 5:
        floats.append(p.get("scale", 5.0))

    return t, inputs, floats


class BinaryMetadataWriter:
    """Serializes the extracted scene manifest directly to a fast, zero-parsing binary stream."""

    def __init__(self, filepath):
        self.f = open(filepath, "wb")
        # Write Magic header "ZMET" (Zahlen Metadata) and version 1
        self.f.write(b"ZMET")
        self.f.write(struct.pack("<I", 1))

    def close(self):
        self.f.close()

    def write_string(self, s):
        """Writes length-prefixed UTF-8 string."""
        s = s or ""
        b = s.encode("utf-8")
        self.f.write(struct.pack("<I", len(b)) + b)

    def write_fmt(self, fmt, *args):
        """Utility for packing flat types. '<' enforces little-endian."""
        self.f.write(struct.pack("<" + fmt, *args))

    def write_floats(self, lst):
        """Helper to write variable-length arrays of floats."""
        if not lst:
            return
        self.write_fmt(f"{len(lst)}f", *lst)

    def serialize(self, manifest):
        # 1. Level Info
        self.write_string(manifest["scene_info"]["name"])

        # 2. Materials
        self.write_fmt("I", len(manifest["materials"]))
        for mat in manifest["materials"]:
            self.write_string(mat["id"])
            self.write_floats(mat["pbr"]["base_color"])  # 4 floats
            self.write_fmt("f", mat["pbr"]["metallic"])
            self.write_fmt("f", mat["pbr"]["roughness"])
            self.write_floats(mat["pbr"]["emissive_factor"])  # 3 floats
            self.write_fmt("f", mat["pbr"]["emissive_strength"])
            self.write_fmt("B", 1 if mat["double_sided"] else 0)
            self.write_string(mat["maps"]["albedo"])
            self.write_string(mat["maps"]["normal"])
            self.write_string(mat["maps"]["metallic_roughness"])
            self.write_string(mat["maps"]["emissive"])

            proc = mat.get("procedural")
            if proc:
                self.write_fmt("B", 1)
                self.write_string(proc["type"])
                self.write_fmt("I", len(proc["parameters"]))
                for key, val in proc["parameters"].items():
                    self.write_string(key)
                    if isinstance(val, list):
                        self.write_fmt("B", len(val))  # float count
                        self.write_floats(val)
                    else:
                        self.write_fmt("B", 1)
                        self.write_fmt("f", val)
            else:
                self.write_fmt("B", 0)

        # 3. Meshes
        self.write_fmt("I", len(manifest["meshes"]))
        for mesh in manifest["meshes"]:
            self.write_string(mesh["id"])
            self.write_string(mesh["layout"])
            self.write_string(mesh["buffers"]["bin_file"])
            self.write_fmt(
                "II",
                mesh["buffers"]["vertex_buffer"]["byte_offset"],
                mesh["buffers"]["vertex_buffer"]["byte_length"],
            )
            # Primitives
            self.write_fmt("I", len(mesh["primitives"]))
            for prim in mesh["primitives"]:
                self.write_string(prim["material_id"])
                self.write_fmt("II", prim["vertex_offset"], prim["vertex_count"])
            # Morph targets
            self.write_fmt("I", len(mesh["morph_targets"]))
            for target in mesh["morph_targets"]:
                self.write_string(target["name"])
                self.write_string(target["bin_file"])
                self.write_fmt("II", target["byte_offset"], target["byte_length"])

        # 4. Nodes
        self.write_fmt("I", len(manifest["nodes"]))
        for node in manifest["nodes"]:
            self.write_string(node["id"])
            self.write_string(node["parent_id"])
            self.write_fmt("B", 1 if node["visible"] else 0)
            self.write_floats(node["transform"]["local"])  # 16 floats
            self.write_floats(node["transform"]["world"])  # 16 floats
            self.write_string(node["refs"]["mesh_id"])
            self.write_string(node["refs"]["skin_id"])
            self.write_string(node["refs"]["light_id"])

        # 5. Lights
        self.write_fmt("I", len(manifest["lights"]))
        for light in manifest["lights"]:
            self.write_string(light["id"])
            self.write_string(light["type"])
            self.write_floats(light["color"])  # 3 floats
            self.write_fmt("f", light["intensity"])

        # 6. Skins
        self.write_fmt("I", len(manifest["skins"]))
        for skin in manifest["skins"]:
            self.write_string(skin["id"])
            self.write_string(skin["name"])

            # Joints (list of strings)
            self.write_fmt("I", len(skin["joints"]))
            for joint in skin["joints"]:
                self.write_string(joint)

            # Parents (list of strings)
            self.write_fmt("I", len(skin["parents"]))
            for parent in skin["parents"]:
                self.write_string(parent)

            # Inverse Bind Matrices (flat floats)
            self.write_fmt("I", len(skin["inverse_bind_matrices"]))
            self.write_floats(skin["inverse_bind_matrices"])

            # Rest Pose (flat floats)
            self.write_fmt("I", len(skin["rest_pose"]))
            self.write_floats(skin["rest_pose"])

        # 7. Animations
        self.write_fmt("I", len(manifest["animations"]))
        for anim in manifest["animations"]:
            self.write_string(anim["id"])
            self.write_string(anim["name"])
            self.write_fmt("fB", anim["duration"], 1 if anim["loop"] else 0)

            # Channels
            self.write_fmt("I", len(anim["channels"]))
            for chan in anim["channels"]:
                self.write_string(chan["target_node_id"])
                self.write_string(chan["target_path"])
                self.write_fmt("I", chan["sampler_id"])

            # Samplers
            self.write_fmt("I", len(anim["samplers"]))
            for samp in anim["samplers"]:
                self.write_string(samp["interpolation"])
                self.write_fmt(
                    "IIII",
                    samp["input_offset"],
                    samp["input_length"],
                    samp["output_offset"],
                    samp["output_length"],
                )
                self.write_string(samp["bin_file"])


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
            except Exception:
                pass
            finally:
                image.filepath_raw = old_path
        else:
            src_path = bpy.path.abspath(image.filepath)
            if os.path.exists(src_path):
                try:
                    shutil.copy2(src_path, dest_path)
                except Exception:
                    pass
            else:
                try:
                    image.save_render(dest_path)
                except Exception:
                    pass


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


def extract_skins(depsgraph, node_id_map, world_yup_map):
    """Extracts skin bindings, inverse matrices, and rest poses."""
    skins = []
    for obj in depsgraph.scene.objects:
        if obj.IsA("ARMATURE"):
            joints_list = []
            parents_list = []
            ibms_list = []
            rest_pose_list = []
            pose_bone_ids = []

            # Precompute Armature basis
            arm_world_yup = c_basis @ obj.matrix_world @ c_basis_inv

            for bone in obj.data.bones:
                bone_key = f"bone_{obj.name}_{bone.name}"
                bone_node_id = node_id_map.get(bone_key)

                joints_list.append(bone_node_id)
                pose_bone_ids.append(bone_node_id)

                bone_world_zup = obj.matrix_world @ bone.matrix_local
                bone_world_yup = c_basis @ bone_world_zup @ c_basis_inv

                # Calculate parent-relative local rest pose matrix
                if bone.parent:
                    parent_key = f"bone_{obj.name}_{bone.parent.name}"
                    parents_list.append(node_id_map.get(parent_key))

                    parent_world_zup = obj.matrix_world @ bone.parent.matrix_local
                    parent_world_yup = c_basis @ parent_world_zup @ c_basis_inv
                    local_rest_yup = safe_invert(parent_world_yup) @ bone_world_yup
                else:
                    parents_list.append(make_id("node", obj.name))
                    local_rest_yup = safe_invert(arm_world_yup) @ bone_world_yup

                ibm_yup = safe_invert(bone_world_yup)

                ibms_list.extend(serialize_matrix_col_major(ibm_yup))
                rest_pose_list.extend(serialize_matrix_col_major(local_rest_yup))

            skins.append(
                {
                    "id": make_id("skin", obj.name),
                    "name": f"{obj.name} Skin",
                    "joints": joints_list,
                    "parents": parents_list,
                    "inverse_bind_matrices": ibms_list,
                    "rest_pose": rest_pose_list,
                    "pose_bone_ids": pose_bone_ids,
                }
            )
    return skins


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

    if is_skinned:
        local_yup_final = Matrix.Identity(4)
        world_yup_final = Matrix.Identity(4)
    else:
        local_yup_final = local_yup
        world_yup_final = world_yup

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
            "local": serialize_matrix_col_major(local_yup_final),
            "world": serialize_matrix_col_major(world_yup_final),
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

        if is_visible and mesh_id in exported_meshes:
            node_info["refs"]["mesh_id"] = mesh_id
        else:
            node_info["refs"]["mesh_id"] = None

        node_info["refs"]["material_ids"] = [
            make_id("mat", s.material.name) for s in obj.material_slots if s.material
        ]

        # --- OPTIMIZED OUT: MOVED TO C++ ---
        # We write simple dummy bounds [0,0,0] in the metadata.bin JSON,
        # because the engine reads the true bounds directly from the compiled .zmesh header.
        node_info["extras"]["bounds"] = {"min": [0.0, 0.0, 0.0], "max": [0.0, 0.0, 0.0]}

    return node_info


def extract_nodes(scene_objects, depsgraph, node_id_map, exported_meshes):
    """Assembles node structures, separating standard objects from instances."""
    nodes = []
    for obj in scene_objects:
        if obj.parent and obj.parent_type == "BONE" and obj.parent_bone:
            parent_key = f"bone_{obj.parent.name}_{obj.parent_bone}"
            parent_node_id = node_id_map.get(parent_key)
            parent_bone = obj.parent.data.bones.get(obj.parent_bone)
            if parent_bone:
                parent_world_zup = obj.parent.matrix_world @ parent_bone.matrix_local
                parent_world_yup = c_basis @ parent_world_zup @ c_basis_inv
            else:
                parent_world_yup = c_basis @ obj.parent.matrix_world @ c_basis_inv
        else:
            parent_key = obj.parent.name if obj.parent else None
            parent_node_id = node_id_map.get(parent_key) if parent_key else None
            parent_world_yup = (
                c_basis @ obj.parent.matrix_world @ c_basis_inv if obj.parent else None
            )

        world_yup = c_basis @ obj.matrix_world @ c_basis_inv
        local_yup = (
            safe_invert(parent_world_yup) @ world_yup
            if parent_node_id and parent_world_yup
            else world_yup
        )

        nodes.append(
            build_node_info(
                obj,
                node_id_map.get(obj.name),
                obj.name,
                parent_node_id,
                local_yup,
                world_yup,
                not obj.hide_viewport,
                exported_meshes,
            )
        )

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

        nodes.append(
            build_node_info(
                obj,
                node_id,
                node_name,
                parent_node_id,
                local_yup,
                world_yup,
                instance.show_self,
                exported_meshes,
            )
        )

    return nodes


def extract_materials():
    """Extracts all active materials, compiling dynamic shader node configurations."""
    materials = []
    for mat in bpy.data.materials:
        mat_id = make_id("mat", mat.name)
        default_color = [clean_float(c) for c in mat.diffuse_color]

        mat_info = {
            "id": mat_id,
            "name": mat.name,
            "double_sided": not mat.use_backface_culling,
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
                "emissive": None,
                "ao": None,
            },
            "sampler": {"wrap": "REPEAT", "filter": "LINEAR"},
        }

        # Read the raw basic properties dynamically so they are still present as fallbacks
        root = find_root_shader_node(mat)
        if root and root.type == "BSDF_PRINCIPLED":
            # --- ALBEDO MAP (Base Color) ---
            def_col = root.inputs.get("Base Color")
            if def_col:
                if not def_col.is_linked:
                    mat_info["pbr"]["base_color"] = [
                        clean_float(c) for c in def_col.default_value
                    ]
                else:
                    img = get_texture_node_image_block(def_col)
                    if img:
                        mat_info["maps"]["albedo"] = os.path.join(
                            "textures", get_image_filename(img)
                        ).replace("\\", "/")

            # --- METALLIC MAP ---
            def_met = root.inputs.get("Metallic")
            if def_met and not def_met.is_linked:
                mat_info["pbr"]["metallic"] = clean_float(def_met.default_value)

            # --- ROUGHNESS MAP ---
            def_rgh = root.inputs.get("Roughness")
            if def_rgh:
                if not def_rgh.is_linked:
                    mat_info["pbr"]["roughness"] = clean_float(def_rgh.default_value)
                else:
                    img = get_texture_node_image_block(def_rgh)
                    if img:
                        mat_info["maps"]["metallic_roughness"] = os.path.join(
                            "textures", get_image_filename(img)
                        ).replace("\\", "/")

            # --- NORMAL MAP ---
            def_nrm = root.inputs.get("Normal")
            if def_nrm and def_nrm.is_linked:
                img = get_texture_node_image_block(def_nrm)
                if img:
                    mat_info["maps"]["normal"] = os.path.join(
                        "textures", get_image_filename(img)
                    ).replace("\\", "/")

            # --- EMISSIVE MAP & FACTOR ---
            def_ems = root.inputs.get("Emission Color") or root.inputs.get("Emission")
            if def_ems:
                if not def_ems.is_linked:
                    mat_info["pbr"]["emissive_factor"] = [
                        clean_float(c) for c in def_ems.default_value[:3]
                    ]
                else:
                    img = get_texture_node_image_block(def_ems)
                    if img:
                        mat_info["maps"]["emissive"] = os.path.join(
                            "textures", get_image_filename(img)
                        ).replace("\\", "/")

            # --- EMISSIVE STRENGTH ---
            def_str = root.inputs.get("Emission Strength")
            if def_str and not def_str.is_linked:
                mat_info["pbr"]["emissive_strength"] = clean_float(
                    def_str.default_value
                )

        # Inject the Graph Bytecode Compiler!
        proc = build_procedural_instructions(mat)
        if proc:
            mat_info["procedural"] = proc

        materials.append(mat_info)
    return materials


def extract_meshes(geometry_sources, depsgraph, asset_dir, bin_dir, exported_meshes):
    """Packs fast contiguous loop arrays (offloading index deduplication to the C++ compiler)."""
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

        armature_mods_state = []
        has_armature = False
        for mod in obj.modifiers:
            if mod.type == "ARMATURE":
                armature_mods_state.append((mod, mod.show_viewport))
                mod.show_viewport = False
                has_armature = True

        if has_armature:
            depsgraph.update()
        mesh_data = get_evaluated_mesh_safely(obj, depsgraph)

        if not mesh_data:
            if has_armature:
                for mod, state in armature_mods_state:
                    mod.show_viewport = state
                depsgraph.update()
            continue

        if len(mesh_data.vertices) == 0 or len(mesh_data.polygons) == 0:
            if has_armature:
                for mod, state in armature_mods_state:
                    mod.show_viewport = state
                depsgraph.update()
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
        active_color_attr = None
        if hasattr(mesh_data, "attributes"):
            candidates_multi = []
            candidates_scalar = []
            for attr in mesh_data.attributes:
                attr_name_lower = attr.name.lower()
                if attr_name_lower in {"position", "normal", "tangent", "uvmap", "uv"}:
                    continue
                has_color_name = any(
                    hint in attr_name_lower
                    for hint in ["col", "color", "tint", "hue", "rgb"]
                )
                if attr.data_type in {"FLOAT_COLOR", "BYTE_COLOR", "FLOAT_VECTOR"}:
                    if is_valid_color_layer(attr):
                        candidates_multi.append((attr, has_color_name))
                elif attr.data_type == "FLOAT" and has_color_name:
                    candidates_scalar.append(attr)

            if candidates_scalar:
                active_color_attr = candidates_scalar[0]
            elif candidates_multi:
                named_candidates = [c[0] for c in candidates_multi if c[1]]
                if named_candidates:
                    active_color_attr = named_candidates[0]
                else:
                    active_color_attr = candidates_multi[0][0]

        group_to_joint_idx = {}
        if is_skinned:
            arm_obj = armature_mod.object
            bone_names = [b.name for b in arm_obj.data.bones]
            group_to_joint_idx = {
                idx: bone_names.index(g.name)
                for idx, g in enumerate(obj.vertex_groups)
                if g.name in bone_names
            }

        flat_vbo = []
        primitives = []
        current_offset = 0

        mesh_data.calc_loop_triangles()
        triangles_by_mat = {}
        for tri in mesh_data.loop_triangles:
            mat_idx = tri.material_index
            if mat_idx not in triangles_by_mat:
                triangles_by_mat[mat_idx] = []
            triangles_by_mat[mat_idx].append(tri)

        for mat_idx, tris in triangles_by_mat.items():
            loop_count = 0
            for tri in tris:
                for loop_idx, v_idx in zip(tri.loops, tri.vertices):
                    co = mesh_data.vertices[v_idx].co
                    normal = (
                        mesh_data.loops[loop_idx].normal
                        if hasattr(mesh_data.loops[loop_idx], "normal")
                        else mesh_data.vertices[v_idx].normal
                    )

                    if is_skinned:
                        co_world = obj.matrix_world @ co
                        cx, cy, cz = co_world[0], co_world[2], -co_world[1]
                        normal_world = (
                            safe_invert(obj.matrix_world).to_3x3().transposed() @ normal
                        ).normalized()
                        nx, ny, nz = normal_world[0], normal_world[2], -normal_world[1]
                    else:
                        cx, cy, cz = co[0], co[2], -co[1]
                        nx, ny, nz = normal[0], normal[2], -normal[1]

                    uv = active_uv_layer[loop_idx].uv if active_uv_layer else (0.0, 0.0)

                    # --- OPTIMIZED OUT: MOVED TO C++ ---
                    # Ignore joint limits, weight normalization, and sorting.
                    # We just pack the coordinates, UVs and raw vertex groups; zcook will parse
                    # and normalize them natively.
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

                    flat_vbo.extend(
                        [
                            float(v_idx),
                            cx,
                            cy,
                            cz,
                            nx,
                            ny,
                            nz,
                            uv[0],
                            1.0 - uv[1],
                            color[0],
                            color[1],
                            color[2],
                            color[3],
                        ]
                    )

                    if is_skinned:
                        v = mesh_data.vertices[v_idx]
                        v_influences = [
                            (group_to_joint_idx[g.group], g.weight)
                            for g in v.groups
                            if g.group in group_to_joint_idx
                        ]

                        # Optimization: Avoid sorting if we have 4 or fewer influences
                        if len(v_influences) > 4:
                            v_influences = sorted(
                                v_influences, key=lambda x: x[1], reverse=True
                            )[:4]
                        while len(v_influences) < 4:
                            v_influences.append((0, 0.0))

                        total_w = (
                            v_influences[0][1]
                            + v_influences[1][1]
                            + v_influences[2][1]
                            + v_influences[3][1]
                        )
                        if total_w > 0.0:
                            v_influences = [(j, w / total_w) for j, w in v_influences]
                        else:
                            v_influences = [(0, 1.0), (0, 0.0), (0, 0.0), (0, 0.0)]

                        for j, w in v_influences:
                            flat_vbo.append(float(j))
                        for j, w in v_influences:
                            flat_vbo.append(float(w))

                    loop_count += 1

            mat_id = (
                make_id("mat", obj.material_slots[mat_idx].material.name)
                if mat_idx < len(obj.material_slots)
                and obj.material_slots[mat_idx].material
                else None
            )
            primitives.append(
                {
                    "material_id": mat_id,
                    "vertex_offset": current_offset,
                    "vertex_count": loop_count,
                }
            )
            current_offset += loop_count

        vbo_binary = struct.pack(f"{len(flat_vbo)}f", *flat_vbo)
        bin_path = os.path.join(bin_dir, f"{mesh_id}.bin")
        with open(bin_path, "wb") as f:
            f.write(vbo_binary)

        morph_targets = []
        if obj.data.shape_keys:
            basis_kb = obj.data.shape_keys.key_blocks.get("Basis")
            if basis_kb:
                for kb in obj.data.shape_keys.key_blocks:
                    if kb.name == "Basis":
                        continue
                    target_offsets = []
                    for v_idx, v in enumerate(obj.data.vertices):
                        offset = kb.data[v_idx].co - basis_kb.data[v_idx].co
                        cx, cy, cz = offset[0], offset[2], -offset[1]
                        target_offsets.extend([cx, cy, cz])
                    target_bin_data = struct.pack(
                        f"{len(target_offsets)}f", *target_offsets
                    )
                    clean_kb_name = "".join(
                        [c if c.isalnum() or c in "._-" else "_" for c in kb.name]
                    )
                    target_bin_name = f"{mesh_id}_target_{clean_kb_name}.bin"
                    target_bin_path = os.path.join(bin_dir, target_bin_name)
                    with open(target_bin_path, "wb") as f:
                        f.write(target_bin_data)
                    morph_targets.append(
                        {
                            "name": kb.name,
                            "bin_file": os.path.relpath(target_bin_path, asset_dir),
                            "byte_offset": 0,
                            "byte_length": len(target_bin_data),
                        }
                    )

        layout = "RAW_P3N3U2C4_J4W4" if is_skinned else "RAW_P3N3U2C4"
        mesh_info = {
            "id": mesh_id,
            "name": f"{obj.data.name} Mesh",
            "layout": layout,
            "buffers": {
                "bin_file": os.path.relpath(bin_path, asset_dir),
                "vertex_buffer": {"byte_offset": 0, "byte_length": len(vbo_binary)},
            },
            "primitives": primitives,
            "morph_targets": morph_targets,
        }
        meshes.append(mesh_info)
        exported_meshes.add(mesh_id)

        if has_armature:
            for mod, state in armature_mods_state:
                mod.show_viewport = state
            depsgraph.update()

        try:
            obj.evaluated_get(depsgraph).to_mesh_clear()
        except Exception:
            try:
                bpy.data.meshes.remove(mesh_data)
            except Exception:
                pass
    return meshes


def get_node_ancestry(node_info):
    ancestors = []
    mode, n, arm_or_obj_name, bone_name = node_info
    current_mode, current_n, current_arm_obj, current_bone = (
        mode,
        n,
        arm_or_obj_name,
        bone_name,
    )

    while True:
        parent_key = None
        p_mode, p_arm_obj, p_bone = None, None, None
        if current_mode == "BONE":
            arm = bpy.data.objects.get(current_arm_obj)
            if arm:
                bone = arm.data.bones.get(current_bone)
                if bone and bone.parent:
                    parent_key = f"bone_{current_arm_obj}_{bone.parent.name}"
                    p_mode = "BONE"
                    p_arm_obj = current_arm_obj
                    p_bone = bone.parent.name
                else:
                    parent_key = current_arm_obj
                    p_mode = "OBJECT"
                    p_arm_obj = current_arm_obj
                    p_bone = None
        else:
            obj = bpy.data.objects.get(current_arm_obj)
            if obj and obj.parent:
                if obj.parent_type == "BONE" and obj.parent_bone:
                    parent_key = f"bone_{obj.parent.name}_{obj.parent_bone}"
                    p_mode = "BONE"
                    p_arm_obj = obj.parent.name
                    p_bone = obj.parent_bone
                else:
                    parent_key = obj.parent.name
                    p_mode = "OBJECT"
                    p_arm_obj = obj.parent.name
                    p_bone = None

        if parent_key:
            ancestors.append((p_mode, parent_key, p_arm_obj, p_bone))
            current_mode, current_n, current_arm_obj, current_bone = (
                p_mode,
                parent_key,
                p_arm_obj,
                p_bone,
            )
        else:
            break
    return ancestors


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


def extract_animations(
    depsgraph, node_id_map, bone_to_armature, bin_dir, asset_dir, fps
):
    animations = []
    original_frame = bpy.context.scene.frame_current

    original_actions = {}
    for obj in bpy.data.objects:
        if obj.animation_data:
            original_actions[obj] = obj.animation_data.action

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

        animated_armatures = set()
        animated_objects = set()
        animated_meshes_with_shape_keys = set()
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
                    animated_armatures.add(arm_obj)
            elif "key_blocks" in path:
                if isinstance(fc.id_data, bpy.types.Key):
                    for obj in bpy.data.objects:
                        if obj.type == "MESH" and obj.data.shape_keys == fc.id_data:
                            animated_meshes_with_shape_keys.add(obj)
            else:
                has_object_curves = True

        if has_object_curves and (getattr(action, "id_root", "OBJECT") == "OBJECT"):
            for obj in bpy.data.objects:
                if obj.animation_data and obj.animation_data.action == action:
                    if obj.IsA("ARMATURE"):
                        animated_armatures.add(obj)
                    else:
                        animated_objects.add(obj)

        target_nodes = set()
        for arm in animated_armatures:
            for bone in arm.data.bones:
                bone_key = f"bone_{arm.name}_{bone.name}"
                target_nodes.add(("BONE", bone_key, arm.name, bone.name))

        for obj in animated_objects:
            target_nodes.add(("OBJECT", obj.name, obj.name, None))

        for obj in animated_meshes_with_shape_keys:
            node_id = node_id_map.get(obj.name)
            if node_id:
                target_nodes.add(("WEIGHTS", node_id, obj.name, None))

        if not target_nodes:
            continue

        channels = []
        samplers = []
        sampler_index = 0

        for mode, name, arm_or_obj_name, bone_name in target_nodes:
            if mode == "WEIGHTS":
                continue
            obj = bpy.data.objects.get(arm_or_obj_name)
            if obj and obj.animation_data:
                obj.animation_data.action = action

        all_eval_nodes = set()
        for node_info in target_nodes:
            if node_info[0] != "WEIGHTS":
                all_eval_nodes.add(node_info)
                for ancestor in get_node_ancestry(node_info):
                    all_eval_nodes.add(ancestor)

        sampled_times = []
        sampled_transforms = {n: [] for _, n, _, _ in target_nodes}
        eval_dg = bpy.context.evaluated_depsgraph_get()

        for frame in range(start_frame, end_frame + 1):
            bpy.context.scene.frame_set(frame)
            eval_dg.update()
            sampled_times.append(clean_float((frame - start_frame) / fps))

            world_yup_eval = {}
            for mode, n, arm_or_obj, bone_name in all_eval_nodes:
                if mode == "BONE":
                    arm = bpy.data.objects.get(arm_or_obj)
                    if arm:
                        eval_arm = arm.evaluated_get(eval_dg)
                        pb = eval_arm.pose.bones.get(bone_name)
                        if pb:
                            b_world_zup = eval_arm.matrix_world @ pb.matrix
                            world_yup_eval[n] = c_basis @ b_world_zup @ c_basis_inv
                else:
                    obj = bpy.data.objects.get(arm_or_obj)
                    if obj:
                        eval_obj = obj.evaluated_get(eval_dg)
                        world_yup_eval[n] = (
                            c_basis @ eval_obj.matrix_world @ c_basis_inv
                        )

            for mode, n, arm_or_obj_name, bone_name in target_nodes:
                if mode == "WEIGHTS":
                    obj_mesh = bpy.data.objects.get(arm_or_obj_name)
                    weights = []
                    if obj_mesh and obj_mesh.data.shape_keys:
                        for kb in obj_mesh.data.shape_keys.key_blocks:
                            if kb.name != "Basis":
                                weights.append(kb.value)
                    sampled_transforms[n].append(weights)
                else:
                    parent_key = None
                    if mode == "BONE":
                        arm = bpy.data.objects.get(arm_or_obj_name)
                        bone = arm.data.bones.get(bone_name) if arm else None
                        if bone and bone.parent:
                            parent_key = f"bone_{arm_or_obj_name}_{bone.parent.name}"
                        else:
                            parent_key = arm_or_obj_name
                    else:
                        obj = bpy.data.objects.get(arm_or_obj_name)
                        if obj and obj.parent:
                            if obj.parent_type == "BONE" and obj.parent_bone:
                                parent_key = f"bone_{obj.parent.name}_{obj.parent_bone}"
                            else:
                                parent_key = obj.parent.name

                    world_yup = world_yup_eval.get(n, Matrix.Identity(4))

                    if mode == "BONE":
                        world_unscaled = remove_scale_from_matrix(world_yup)
                        if parent_key and parent_key in world_yup_eval:
                            parent_unscaled = remove_scale_from_matrix(
                                world_yup_eval[parent_key]
                            )
                            local_yup = safe_invert(parent_unscaled) @ world_unscaled
                        else:
                            local_yup = world_unscaled
                    else:
                        if parent_key and parent_key in world_yup_eval:
                            local_yup = (
                                safe_invert(world_yup_eval[parent_key]) @ world_yup
                            )
                        else:
                            local_yup = world_yup

                t, r, s = local_yup.decompose()
                sampled_transforms[n].append((t, r, s))

        if not sampled_times or not any(sampled_transforms.values()):
            continue

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
            if not node_id:
                continue

            is_weights_track = any(
                x[0] == "WEIGHTS" and x[1] == n for x in target_nodes
            )

            if is_weights_track:
                weights_flat = []
                for w_list in transforms:
                    weights_flat.extend(w_list)
                w_bin = struct.pack(f"{len(weights_flat)}f", *weights_flat)
                channels.append(
                    {
                        "target_node_id": node_id,
                        "target_path": "weights",
                        "sampler_id": sampler_index,
                    }
                )
                samplers.append(
                    {
                        "interpolation": "LINEAR",
                        "input_offset": times_offset,
                        "input_length": times_length,
                        "output_offset": anim_bin_offset,
                        "output_length": len(w_bin),
                    }
                )
                anim_bin_data += w_bin
                anim_bin_offset += len(w_bin)
                sampler_index += 1
            else:
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

    for obj, orig_act in original_actions.items():
        if obj.animation_data:
            obj.animation_data.action = orig_act

    bpy.context.scene.frame_set(original_frame)
    return animations


def export_raw_scene_data(blend_path, source_dir=None):
    if source_dir is None:
        source_dir = input_dir

    abs_blend = os.path.abspath(blend_path)
    abs_source = os.path.abspath(source_dir)
    rel_path = os.path.relpath(abs_blend, abs_source)
    namespace_dir = os.path.splitext(rel_path)[0].replace("\\", "/").replace("/", "_")

    asset_dir = os.path.join(output_parent, namespace_dir)
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

    # ============================================================================
    # AUTOMATED PIPELINE FIX: PROCEDURAL SHADER MAPS & NON-UNIFORM STRETCH FIX
    # ============================================================================
    procedural_materials = set()
    for mat in bpy.data.materials:
        if not mat.node_tree:
            continue

        # Guard: Check if the material is actually a procedural node-graph
        has_image_texture = False
        has_procedural_nodes = False
        for node in mat.node_tree.nodes:
            if node.type == "TEX_IMAGE":
                has_image_texture = True
            if node.type in {"TEX_VORONOI", "TEX_NOISE", "TEX_MUSGRAVE"}:
                has_procedural_nodes = True

        # ONLY apply changes to materials that are strictly procedural (no image textures)
        if has_procedural_nodes and not has_image_texture:
            procedural_materials.add(mat)

            # Make mapping scales uniform so procedural dots bake as circular patterns
            for node in mat.node_tree.nodes:
                if node.type == "MAPPING":
                    scale_inp = node.inputs.get("Scale")
                    if scale_inp:
                        val = scale_inp.default_value
                        avg_scale = (val[0] + val[1]) / 2.0

                        # Find reference object dimensions if present to scale our mapping factor
                        max_dim = 10.0
                        for other_node in mat.node_tree.nodes:
                            if other_node.type == "TEX_COORD" and other_node.object:
                                max_dim = max(other_node.object.dimensions)
                                break
                        if max_dim == 0:
                            max_dim = 10.0

                        # Normalize the scale parameter by dividing by the Lattice's maximum dimension
                        scale_inp.default_value = (
                            avg_scale / max_dim,
                            avg_scale / max_dim,
                            val[2] / max_dim,
                        )

            # Safely reroute standard coordinates to UV space for the 2D custom baker
            for node in mat.node_tree.nodes:
                if node.type == "TEX_COORD":
                    obj_socket = node.outputs.get("Object")
                    gen_socket = node.outputs.get("Generated")
                    uv_socket = node.outputs.get("UV")
                    if uv_socket:
                        for socket in [obj_socket, gen_socket]:
                            if socket and socket.is_linked:
                                links = list(socket.links)
                                for link in links:
                                    mat.node_tree.links.new(uv_socket, link.to_socket)

    # Apply scaling on any mesh utilizing a procedural material via low-level API
    for obj in list(bpy.data.objects):
        if obj.type == "MESH":
            uses_proc = any(
                slot.material in procedural_materials for slot in obj.material_slots
            )
            if uses_proc:
                try:
                    # 1. Identify if the texture coordinates target a reference object (e.g. Lattice)
                    reference_object = None
                    for slot in obj.material_slots:
                        if slot.material and slot.material.node_tree:
                            for node in slot.material.node_tree.nodes:
                                if node.type == "TEX_COORD" and node.object:
                                    reference_object = node.object
                                    break
                            if reference_object:
                                break

                    orig_matrix_world = obj.matrix_world.copy()

                    # Find scene/logo bounds to normalize the projection space
                    max_dim = 10.0
                    if reference_object:
                        max_dim = max(reference_object.dimensions)
                        if max_dim == 0:
                            max_dim = 10.0

                    # 2. Direct programmatic Triplanar/Box UV projection
                    if not obj.data.uv_layers:
                        obj.data.uv_layers.new(name="UVMap")
                    uv_layer = obj.data.uv_layers.active

                    for poly in obj.data.polygons:
                        normal = poly.normal
                        abs_nx = abs(normal.x)
                        abs_ny = abs(normal.y)
                        abs_nz = abs(normal.z)

                        # Project onto the dominant normal plane to generate uniform 3D coordinates
                        if abs_nz >= abs_nx and abs_nz >= abs_ny:
                            # Front / Back faces (project on X-Y plane)
                            proj_u = 0
                            proj_v = 1
                        elif abs_nx >= abs_ny and abs_nx >= abs_nz:
                            # Left / Right faces (project on Y-Z plane)
                            proj_u = 1
                            proj_v = 2
                        else:
                            # Top / Bottom faces (project on X-Z plane)
                            proj_u = 0
                            proj_v = 2

                        for loop_idx in poly.loop_indices:
                            v_idx = obj.data.loops[loop_idx].vertex_index
                            v_co = obj.data.vertices[v_idx].co

                            if reference_object:
                                # Transform coordinates into the unscaled local space of the reference object
                                p_world = orig_matrix_world @ v_co
                                unscaled_target_matrix = remove_scale_from_matrix(
                                    reference_object.matrix_world
                                )
                                v_co_eval = unscaled_target_matrix.inverted() @ p_world
                            else:
                                # Fallback: scale local coordinates directly
                                v_co_eval = v_co.copy()
                                v_co_eval[0] *= obj.scale[0]
                                v_co_eval[1] *= obj.scale[1]
                                v_co_eval[2] *= obj.scale[2]

                            # Normalize coordinates into the [0, 1] range based on the reference object's dimensions
                            u = (v_co_eval[proj_u] / max_dim) + 0.5
                            v = (v_co_eval[proj_v] / max_dim) + 0.5

                            uv_layer.data[loop_idx].uv = (u, v)

                    # 3. Resolve Multi-User Data Block issue by copying shared data
                    if obj.data and obj.data.users > 1:
                        obj.data = obj.data.copy()

                    # 4. Apply scale directly to mesh vertices using Python API
                    scale_matrix = Matrix.Diagonal(obj.scale).to_4x4()
                    obj.data.transform(scale_matrix)

                    # 5. Freeze the transform scale of the object back to 1.0
                    obj.scale = (1.0, 1.0, 1.0)

                except Exception as e:
                    print(
                        f"      [Warning] Non-uniform scale fix skipped for '{obj.name}': {e}"
                    )
    # ============================================================================

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
        "scene_info": {"name": namespace_dir},
        "materials": materials,
        "meshes": meshes,
        "nodes": nodes,
        "lights": lights,
        "skins": skins,
        "animations": animations,
    }

    binary_path = os.path.join(asset_dir, "metadata.bin")
    writer = BinaryMetadataWriter(binary_path)
    writer.serialize(scene_manifest)
    writer.close()

    print(
        f"      [Success] Extracted raw metadata & binary geometry for: {namespace_dir}"
    )


if "--" in sys.argv:
    args = sys.argv[sys.argv.index("--") + 1 :]
    if len(args) >= 2:
        blend_path = args[0]
        output_parent = args[1]
        try:
            source_dir = os.path.abspath(os.path.join(output_parent, "..", ".."))
            export_raw_scene_data(blend_path, source_dir)
        except Exception as e:
            print(f"      [Error] Extraction failed for {blend_path}: {e}")
            raise e
else:
    blend_files = discover_blend_files(input_dir)
    print(f"Discovered {len(blend_files)} levels for raw metadata extraction.\n")
    for idx, blend_path in enumerate(blend_files, start=1):
        print(
            f"[{idx}/{len(blend_files)}] Extracting: {os.path.relpath(blend_path, input_dir)}"
        )
        try:
            export_raw_scene_data(blend_path, input_dir)
        except Exception as e:
            print(f"      [Error] Extraction failed: {e}")
