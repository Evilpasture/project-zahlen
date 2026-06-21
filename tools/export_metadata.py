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
import math

# Set recursion limit for large hierarchical models
sys.setrecursionlimit(15000)


def scalar_to_rgb(t):
    """Procedurally maps a single scalar float to a 3D RGB color wheel.
    Adjust the cosine parameters below to customize the visual aesthetic.
    """
    r = 0.65 + 0.35 * math.cos(2.0 * math.pi * (t + 0.0))
    g = 0.65 + 0.35 * math.cos(2.0 * math.pi * (t + 0.33))
    b = 0.65 + 0.35 * math.cos(2.0 * math.pi * (t + 0.67))

    return (round(r, 4), round(g, 4), round(b, 4), 1.0)


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
            return scalar_to_rgb(f)  # Map scalar floats to RGB
        except Exception:
            return (1.0, 1.0, 1.0, 1.0)
    length = len(val)
    if length == 0:
        return (1.0, 1.0, 1.0, 1.0)
    elif length == 1:
        try:
            f = float(val[0])
            return scalar_to_rgb(f)  # Map list-wrapped scalar floats to RGB
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


def get_scalar_value_from_datum(datum):
    """Extracts a singular scalar float value safely from any generic attribute datum."""
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


# ============================================================================
# Shader Tree Compilation Engine (Support for Mix, Add & Transparent Shaders)
# ============================================================================


def find_root_shader_node(material):
    """Resolves the root shader node driving the active surface output."""
    if not material.use_nodes or not material.node_tree:
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
            (n for n in material.node_tree.nodes if n.type == "OUTPUT_MATERIAL"),
            None,
        )

    if output_node:
        surface_input = output_node.inputs.get("Surface")
        if surface_input and surface_input.is_linked:
            return surface_input.links[0].from_node
    return None


def evaluate_shader(node, depth=0):
    """Recursively evaluates the shader node tree up to a defined depth,
    compiling combined and mixed networks into unified PBR material values.
    """
    default_props = {
        "base_color": [1.0, 1.0, 1.0, 1.0],
        "metallic": 0.0,
        "roughness": 0.5,
        "emissive_factor": [0.0, 0.0, 0.0],
        "emissive_strength": 0.0,
        "albedo_img": None,
        "normal_img": None,
        "mr_img": None,
        "emissive_img": None,
    }

    if depth > 10 or not node:
        return default_props

    if node.type == "BSDF_PRINCIPLED":
        base_color = resolve_base_color(node)

        metallic = 0.0
        metallic_input = node.inputs.get("Metallic")
        if metallic_input and not metallic_input.is_linked:
            metallic = clean_float(metallic_input.default_value)

        roughness = 0.5
        roughness_input = node.inputs.get("Roughness")
        if roughness_input and not roughness_input.is_linked:
            roughness = clean_float(roughness_input.default_value)

        em_col = [0.0, 0.0, 0.0]
        em_col_input = node.inputs.get("Emission Color") or node.inputs.get("Emission")
        emissive_img = None
        if em_col_input:
            if not em_col_input.is_linked:
                try:
                    em_col = [clean_float(c) for c in em_col_input.default_value[:3]]
                except Exception:
                    pass
            else:
                emissive_img = get_texture_node_image_block(em_col_input)
                em_col = [1.0, 1.0, 1.0]

        em_str = 1.0
        em_str_input = node.inputs.get("Emission Strength")
        if em_str_input and not em_str_input.is_linked:
            em_str = clean_float(em_str_input.default_value)

        albedo_img = get_texture_node_image_block(node.inputs.get("Base Color"))
        normal_img = get_texture_node_image_block(node.inputs.get("Normal"))
        mr_img = get_texture_node_image_block(node.inputs.get("Roughness"))

        return {
            "base_color": base_color,
            "metallic": metallic,
            "roughness": roughness,
            "emissive_factor": em_col,
            "emissive_strength": em_str,
            "albedo_img": albedo_img,
            "normal_img": normal_img,
            "mr_img": mr_img,
            "emissive_img": emissive_img,
        }

    elif node.type == "BSDF_DIFFUSE":
        col_input = node.inputs.get("Color")
        base_color = [1.0, 1.0, 1.0, 1.0]
        albedo_img = None
        if col_input:
            if not col_input.is_linked:
                base_color = [clean_float(c) for c in col_input.default_value]
            else:
                albedo_img = get_texture_node_image_block(col_input)

        roughness = 0.5
        rough_input = node.inputs.get("Roughness")
        if rough_input and not rough_input.is_linked:
            roughness = clean_float(rough_input.default_value)

        return {
            "base_color": base_color,
            "metallic": 0.0,
            "roughness": roughness,
            "emissive_factor": [0.0, 0.0, 0.0],
            "emissive_strength": 0.0,
            "albedo_img": albedo_img,
            "normal_img": get_texture_node_image_block(node.inputs.get("Normal")),
            "mr_img": None,
            "emissive_img": None,
        }

    elif node.type == "BSDF_GLOSSY":
        col_input = node.inputs.get("Color")
        base_color = [1.0, 1.0, 1.0, 1.0]
        albedo_img = None
        if col_input:
            if not col_input.is_linked:
                base_color = [clean_float(c) for c in col_input.default_value]
            else:
                albedo_img = get_texture_node_image_block(col_input)

        roughness = 0.5
        rough_input = node.inputs.get("Roughness")
        if rough_input and not rough_input.is_linked:
            roughness = clean_float(rough_input.default_value)

        return {
            "base_color": base_color,
            "metallic": 1.0,
            "roughness": roughness,
            "emissive_factor": [0.0, 0.0, 0.0],
            "emissive_strength": 0.0,
            "albedo_img": albedo_img,
            "normal_img": get_texture_node_image_block(node.inputs.get("Normal")),
            "mr_img": None,
            "emissive_img": None,
        }

    elif node.type == "BSDF_TRANSPARENT":
        col_input = node.inputs.get("Color")
        base_color = [1.0, 1.0, 1.0, 1.0]
        albedo_img = None
        if col_input:
            if not col_input.is_linked:
                c = col_input.default_value
                base_color = [
                    clean_float(c[0]),
                    clean_float(c[1]),
                    clean_float(c[2]),
                    0.0,
                ]
            else:
                albedo_img = get_texture_node_image_block(col_input)
                base_color = [1.0, 1.0, 1.0, 0.0]

        return {
            "base_color": base_color,
            "metallic": 0.0,
            "roughness": 0.0,
            "emissive_factor": [0.0, 0.0, 0.0],
            "emissive_strength": 0.0,
            "albedo_img": albedo_img,
            "normal_img": None,
            "mr_img": None,
            "emissive_img": None,
        }

    elif node.type in {"EMISSION", "EMISSION_BSDF"}:
        col_input = node.inputs.get("Color")
        em_col = [1.0, 1.0, 1.0]
        emissive_img = None
        if col_input:
            if not col_input.is_linked:
                em_col = [clean_float(c) for c in col_input.default_value[:3]]
            else:
                emissive_img = get_texture_node_image_block(col_input)

        strength = 1.0
        strength_input = node.inputs.get("Strength")
        if strength_input and not strength_input.is_linked:
            strength = clean_float(strength_input.default_value)

        return {
            "base_color": [0.0, 0.0, 0.0, 1.0],
            "metallic": 0.0,
            "roughness": 0.5,
            "emissive_factor": em_col,
            "emissive_strength": strength,
            "albedo_img": None,
            "normal_img": None,
            "mr_img": None,
            "emissive_img": emissive_img,
        }

    elif node.type == "MIX_SHADER":
        fac = 0.5
        fac_input = node.inputs.get("Fac")
        if fac_input and not fac_input.is_linked:
            fac = clean_float(fac_input.default_value)

        shader1_node = None
        shader2_node = None

        s1_input = node.inputs[1] if len(node.inputs) > 1 else None
        s2_input = node.inputs[2] if len(node.inputs) > 2 else None

        if s1_input and s1_input.is_linked:
            shader1_node = s1_input.links[0].from_node
        if s2_input and s2_input.is_linked:
            shader2_node = s2_input.links[0].from_node

        s1_props = evaluate_shader(shader1_node, depth + 1)
        s2_props = evaluate_shader(shader2_node, depth + 1)

        w1 = 1.0 - fac
        w2 = fac

        base_color = [
            clean_float(s1_props["base_color"][i] * w1 + s2_props["base_color"][i] * w2)
            for i in range(4)
        ]
        metallic = clean_float(s1_props["metallic"] * w1 + s2_props["metallic"] * w2)
        roughness = clean_float(s1_props["roughness"] * w1 + s2_props["roughness"] * w2)
        emissive_factor = [
            clean_float(
                s1_props["emissive_factor"][i] * w1
                + s2_props["emissive_factor"][i] * w2
            )
            for i in range(3)
        ]
        emissive_strength = clean_float(
            s1_props["emissive_strength"] * w1 + s2_props["emissive_strength"] * w2
        )

        albedo_img = s2_props["albedo_img"] if fac >= 0.5 else s1_props["albedo_img"]
        normal_img = s2_props["normal_img"] if fac >= 0.5 else s1_props["normal_img"]
        mr_img = s2_props["mr_img"] if fac >= 0.5 else s1_props["mr_img"]
        emissive_img = (
            s2_props.get("emissive_img") if fac >= 0.5 else s1_props.get("emissive_img")
        )

        if not albedo_img:
            albedo_img = (
                s1_props["albedo_img"] if fac >= 0.5 else s2_props["albedo_img"]
            )
        if not normal_img:
            normal_img = (
                s1_props["normal_img"] if fac >= 0.5 else s2_props["normal_img"]
            )
        if not mr_img:
            mr_img = s1_props["mr_img"] if fac >= 0.5 else s2_props["mr_img"]
        if not emissive_img:
            emissive_img = (
                s1_props.get("emissive_img")
                if fac >= 0.5
                else s2_props.get("emissive_img")
            )

        return {
            "base_color": base_color,
            "metallic": metallic,
            "roughness": roughness,
            "emissive_factor": emissive_factor,
            "emissive_strength": emissive_strength,
            "albedo_img": albedo_img,
            "normal_img": normal_img,
            "mr_img": mr_img,
            "emissive_img": emissive_img,
        }

    elif node.type == "ADD_SHADER":
        shader1_node = None
        shader2_node = None

        s1_input = node.inputs[0] if len(node.inputs) > 0 else None
        s2_input = node.inputs[1] if len(node.inputs) > 1 else None

        if s1_input and s1_input.is_linked:
            shader1_node = s1_input.links[0].from_node
        if s2_input and s2_input.is_linked:
            shader2_node = s2_input.links[0].from_node

        s1_props = evaluate_shader(shader1_node, depth + 1)
        s2_props = evaluate_shader(shader2_node, depth + 1)

        base_color = [
            clean_float(min(1.0, s1_props["base_color"][i] + s2_props["base_color"][i]))
            for i in range(4)
        ]
        metallic = clean_float(min(1.0, s1_props["metallic"] + s2_props["metallic"]))
        roughness = clean_float((s1_props["roughness"] + s2_props["roughness"]) * 0.5)
        emissive_factor = [
            clean_float(
                min(
                    1.0,
                    s1_props["emissive_factor"][i] + s2_props["emissive_factor"][i],
                )
            )
            for i in range(3)
        ]
        emissive_strength = clean_float(
            s1_props["emissive_strength"] + s2_props["emissive_strength"]
        )

        albedo_img = s1_props["albedo_img"] or s2_props["albedo_img"]
        normal_img = s1_props["normal_img"] or s2_props["normal_img"]
        mr_img = s1_props["mr_img"] or s2_props["mr_img"]
        emissive_img = s1_props.get("emissive_img") or s2_props.get("emissive_img")

        return {
            "base_color": base_color,
            "metallic": metallic,
            "roughness": roughness,
            "emissive_factor": emissive_factor,
            "emissive_strength": emissive_strength,
            "albedo_img": albedo_img,
            "normal_img": normal_img,
            "mr_img": mr_img,
            "emissive_img": emissive_img,
        }

    return default_props


def extract_materials():
    """Extracts all active materials, compiling dynamic shader node configurations."""
    materials = []
    for mat in bpy.data.materials:
        mat_id = make_id("mat", mat.name)
        default_color = [clean_float(c) for c in mat.diffuse_color]

        mat_info = {
            "id": mat_id,
            "name": mat.name,
            "double_sided": not mat.use_backface_culling,  # True if backface culling is OFF in Blender
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

        root_node = find_root_shader_node(mat)
        if root_node:
            props = evaluate_shader(root_node)
            mat_info["pbr"]["base_color"] = props["base_color"]
            mat_info["pbr"]["metallic"] = props["metallic"]
            mat_info["pbr"]["roughness"] = props["roughness"]
            mat_info["pbr"]["emissive_factor"] = props["emissive_factor"]
            mat_info["pbr"]["emissive_strength"] = props["emissive_strength"]

            albedo_file = get_image_filename(props["albedo_img"])
            normal_file = get_image_filename(props["normal_img"])
            mr_file = get_image_filename(props["mr_img"])
            emissive_file = get_image_filename(props.get("emissive_img"))

            mat_info["maps"]["albedo"] = (
                f"textures/{albedo_file}" if albedo_file else None
            )
            mat_info["maps"]["normal"] = (
                f"textures/{normal_file}" if normal_file else None
            )
            mat_info["maps"]["metallic_roughness"] = (
                f"textures/{mr_file}" if mr_file else None
            )
            mat_info["maps"]["emissive"] = (
                f"textures/{emissive_file}" if emissive_file else None
            )
        else:
            mat_info["pbr"]["base_color"] = default_color

        if not mat_info["maps"]["albedo"] and not root_node:
            mat_info["pbr"]["base_color"] = default_color

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

        mesh_data = get_evaluated_mesh_safely(obj, depsgraph)
        if not mesh_data:
            continue

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
            ramp_node = None
            ramp_attr = None
            if mat_idx < len(obj.material_slots):
                prim_mat = obj.material_slots[mat_idx].material
                if prim_mat and prim_mat.use_nodes:
                    ramp_node = next(
                        (n for n in prim_mat.node_tree.nodes if n.type == "VALTORGB"),
                        None,
                    )
                    if ramp_node:
                        fac_input = ramp_node.inputs.get("Fac")
                        attr_node = find_upstream_texture_or_attribute(fac_input)
                        if attr_node and attr_node.type == "ATTRIBUTE":
                            ramp_attr_name = attr_node.attribute_name
                            if hasattr(mesh_data, "attributes") and ramp_attr_name:
                                ramp_attr = mesh_data.attributes.get(ramp_attr_name)

            loop_count = 0
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
                    if ramp_node and ramp_attr and ramp_attr.data:
                        if ramp_attr.domain == "CORNER":
                            datum = ramp_attr.data[loop_idx]
                        elif ramp_attr.domain == "POINT":
                            datum = ramp_attr.data[v_idx]
                        elif ramp_attr.domain == "FACE":
                            datum = ramp_attr.data[tri.polygon_index]
                        else:
                            datum = None

                        val = get_scalar_value_from_datum(datum)
                        color = ramp_node.color_ramp.evaluate(val)
                    elif active_color_attr and active_color_attr.data:
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
                            co[0],
                            co[2],
                            -co[1],
                            normal[0],
                            normal[2],
                            -normal[1],
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
                            v_influences = [(j, w / total_w) for j, w in v_influences]
                        else:
                            v_influences = [(0, 1.0)] + [(0, 0.0)] * 3

                        for j, w in v_influences:
                            flat_vbo.append(float(j))
                        for j, w in v_influences:
                            flat_vbo.append(float(w))

                    loop_count += 1

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
                    "vertex_offset": current_offset,
                    "vertex_count": loop_count,
                }
            )
            current_offset += loop_count

        vbo_binary = struct.pack(f"{len(flat_vbo)}f", *flat_vbo)

        bin_path = os.path.join(bin_dir, f"{mesh_id}.bin")
        with open(bin_path, "wb") as f:
            f.write(vbo_binary)

        layout = "RAW_P3N3U2C4"
        if is_skinned:
            layout += "_J4W4"

        mesh_info = {
            "id": mesh_id,
            "name": f"{obj.data.name} Mesh",
            "layout": layout,
            "buffers": {
                "bin_file": os.path.relpath(bin_path, asset_dir),
                "vertex_buffer": {
                    "byte_offset": 0,
                    "byte_length": len(vbo_binary),
                },
            },
            "primitives": primitives,
        }

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

        if is_visible and mesh_id in exported_meshes:
            node_info["refs"]["mesh_id"] = mesh_id
        else:
            node_info["refs"]["mesh_id"] = None

        node_info["refs"]["material_ids"] = [
            make_id("mat", s.material.name) for s in obj.material_slots if s.material
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


# Check if executed headless for a single file from the Ninja pipeline
if "--" in sys.argv:
    args = sys.argv[sys.argv.index("--") + 1 :]
    if len(args) >= 2:
        blend_path = args[0]
        output_parent = args[1]
        try:
            export_raw_scene_data(blend_path)
            sys.exit(0)
        except Exception as e:
            print(f"      [Error] Extraction failed for {blend_path}: {e}")
            sys.exit(1)

# Fallback block: Scan directory if executed manually outside of Ninja
else:
    blend_files = discover_blend_files(input_dir)
    print(f"Discovered {len(blend_files)} levels for raw metadata extraction.\n")

    for idx, blend_path in enumerate(blend_files, start=1):
        print(
            f"[{idx}/{len(blend_files)}] Extracting: {os.path.relpath(blend_path, input_dir)}"
        )
        try:
            export_raw_scene_data(blend_path)
        except Exception as e:
            print(f"      [Error] Extraction failed: {e}")
