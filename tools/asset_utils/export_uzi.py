# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# tools/asset_utils/export_uzi.py
import json
import os
import subprocess
import sys


def export_uzi_to_glb(blend_path: str, glb_path: str) -> bool:
    blend_path = os.path.abspath(blend_path)
    glb_path = os.path.abspath(glb_path)

    if not os.path.exists(blend_path):
        print(f"[-] Source blend file not found: {blend_path}")
        return False

    with open(blend_path, "rb") as f:
        original_bytes = f.read()

    blend_dir = os.path.dirname(blend_path)
    blend_name = os.path.basename(blend_path)
    temp_blend_path = os.path.join(blend_dir, f".__tmp_{blend_name}")

    expr = """
import bpy
import mathutils
import os

FACIAL_HIDE_PATTERNS = [
    "eyelid", "eyebrow", "tongue", "blush", "expression", "mark",
    "anc-", "ctr-", "wgt-", "aux_", "icosphere.001",
    "mouthless", "display_frame", "marks_frame", "solver", "warning", "full_x",
    "x.l", "x.r"
]

HAND_FINGER_PATTERNS = [
    "hand", "finger", "thumb", "index", "middle", "pinky", 
    "roundcube", "plane.037", "plane.128", "claw"
]

def safe_str(val) -> str:
    try:
        if isinstance(val, bytes):
            return val.decode("utf-8", errors="replace")
        return str(val).encode("utf-8", errors="replace").decode("utf-8")
    except Exception:
        return "Unknown Error"

def should_hide_facial_element(obj_name):
    name_lower = obj_name.lower()
    return any(pat in name_lower for pat in FACIAL_HIDE_PATTERNS)

def is_hand_or_finger(obj):
    if not obj:
        return False
    o_name = obj.name.lower()
    p_name = obj.parent.name.lower() if obj.parent else ""
    return any(k in o_name or k in p_name for k in HAND_FINGER_PATTERNS)

def is_limb_object(obj):
    if not obj or is_hand_or_finger(obj):
        return False
    in_limbs_col = any(c.name.lower() in ["limbs", "limb_hooks"] for c in obj.users_collection)
    parent_limb = obj.parent and any(k in obj.parent.name.lower() for k in ["arm", "leg", "thigh", "shin", "forearm", "limb"])
    return in_limbs_col or parent_limb

def deselect_all_objects():
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        try:
            bpy.ops.object.mode_set(mode='OBJECT')
        except Exception:
            pass
    for obj in bpy.context.selected_objects:
        if obj:
            obj.select_set(False)

def setup_environment_and_drivers():
    print("[*] Enabling Python script auto-execution for rig UI...", flush=True)
    bpy.context.preferences.filepaths.use_scripts_auto_execute = True

    for txt in bpy.data.texts:
        if txt.name.endswith(".py") or any(k in txt.name.lower() for k in ["rig", "driver", "ui"]):
            try:
                exec(txt.as_string(), {"__name__": "__main__"})
                print(f"  [+] Executed embedded rig script: '{txt.name}'", flush=True)
            except Exception as e:
                print(f"  [~] Notice running embedded script '{txt.name}': {e}", flush=True)

    layer_collections = [bpy.context.view_layer.layer_collection]
    while layer_collections:
        l_c = layer_collections.pop(0)
        layer_collections.extend(l_c.children)
        l_c.exclude = False
        l_c.hide_viewport = False

    bpy.context.view_layer.update()

def setup_root_bone(main_rig):
    print("[*] Setting up floor 'Root' bone at (0, 0, 0)...", flush=True)
    if not main_rig or not main_rig.data:
        return

    deselect_all_objects()
    main_rig.select_set(True)
    bpy.context.view_layer.objects.active = main_rig
    bpy.ops.object.mode_set(mode='EDIT')
    ebs = main_rig.data.edit_bones

    root_bone = ebs.get("Root") or ebs.get("DEF-Root")
    if not root_bone:
        if "neutral_bone" in ebs:
            root_bone = ebs["neutral_bone"]
            root_bone.name = "Root"
        else:
            root_bone = ebs.new("Root")
            root_bone.head = (0.0, 0.0, 0.0)
            root_bone.tail = (0.0, 0.0, 0.1)

    root_bone.use_deform = True

    hips = ebs.get("DEF-Hips") or ebs.get("Hips")
    if hips and hips.parent is None:
        hips.parent = root_bone
        print("  [+] Parented 'DEF-Hips' under 'Root'.", flush=True)

    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.context.view_layer.update()

def setup_shoulder_bones(main_rig):
    print("[*] Configuring shoulder/clavicle deform bones...", flush=True)
    if not main_rig or not main_rig.data:
        return

    deselect_all_objects()
    main_rig.select_set(True)
    bpy.context.view_layer.objects.active = main_rig
    bpy.ops.object.mode_set(mode='EDIT')
    ebs = main_rig.data.edit_bones

    for side in ["L", "R", "l", "r"]:
        side_upper = side.upper()
        candidates = [
            f"DEF-shoulder.{side}", f"DEF-Shoulder.{side}", f"DEF-shoulder.{side_upper}", f"DEF-Shoulder.{side_upper}",
            f"DEF-Clavicle.{side_upper}", f"DEF-clavicle.{side}"
        ]
        for c in candidates:
            b = ebs.get(c)
            if b:
                standard_name = f"DEF-Shoulder.{side_upper}"
                old_name = b.name
                b.name = standard_name
                b.use_deform = True

                for m_obj in bpy.data.objects:
                    if m_obj.type == 'MESH' and old_name in m_obj.vertex_groups:
                        vg = m_obj.vertex_groups.get(old_name)
                        if vg:
                            vg.name = standard_name

                print(f"  [+] Configured deform shoulder bone: '{standard_name}'", flush=True)
                break

    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.context.view_layer.update()

def convert_hair_curves_to_mesh():
    print("[*] Converting hair curve strands to 3D meshes...", flush=True)
    deselect_all_objects()

    for obj in list(bpy.data.objects):
        if obj and getattr(obj, "type", None) in {'CURVE', 'SURFACE', 'FONT'} and not obj.hide_render:
            if is_limb_object(obj):
                continue

            is_hair = any("hair" in col.name.lower() for col in obj.users_collection) or \
                      any(slot.material and "hair" in slot.material.name.lower() for slot in obj.material_slots) or \
                      (hasattr(obj.data, "bevel_depth") and obj.data.bevel_depth > 0)

            if not is_hair and any(k.lower() in obj.name.lower() for k in ["curve_sock"]):
                continue

            deselect_all_objects()
            obj.select_set(True)
            bpy.context.view_layer.objects.active = obj
            try:
                bpy.ops.object.convert(target='MESH')
                obj.name = f"Hair_Mesh_{obj.name}"
                obj.hide_render = False
                obj.hide_viewport = False
                print(f"  [+] Converted hair curve to MESH: '{obj.name}'", flush=True)
            except Exception as e:
                print(f"  [-] Skipped '{obj.name}': {e}", flush=True)

    bpy.context.view_layer.update()

def setup_and_rename_hair_chains(main_rig):
    print("[*] Processing hair rig and parenting strand chains to DEF-Head...", flush=True)
    if not main_rig or not main_rig.data:
        return

    if main_rig.data:
        main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()

    hair_rigs = [
        o for o in bpy.data.objects 
        if o.type == 'ARMATURE' and o != main_rig and any(k in o.name.lower() for k in ["hair", "hair_rig", "armature."])
    ]

    for h_rig in hair_rigs:
        if h_rig.data:
            h_rig.data.pose_position = 'REST'
        bpy.context.view_layer.update()

        world_mat = h_rig.matrix_world.copy()
        h_rig.parent = None
        h_rig.constraints.clear()
        h_rig.matrix_world = world_mat
        bpy.context.view_layer.update()

        deselect_all_objects()
        h_rig.select_set(True)
        bpy.context.view_layer.objects.active = h_rig
        try:
            bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        except Exception as e:
            print(f"  [~] Notice applying transform on '{h_rig.name}': {e}", flush=True)

        deselect_all_objects()
        h_rig.select_set(True)
        main_rig.select_set(True)
        bpy.context.view_layer.objects.active = main_rig
        try:
            bpy.ops.object.join()
            print(f"  [+] Merged '{h_rig.name}' into main rig with world transforms intact.", flush=True)
        except Exception as e:
            print(f"  [~] Notice joining hair rig: {safe_str(e)}", flush=True)

    deselect_all_objects()
    main_rig.select_set(True)
    bpy.context.view_layer.objects.active = main_rig
    bpy.ops.object.mode_set(mode='EDIT')
    ebs = main_rig.data.edit_bones

    head_bone = ebs.get("DEF-Head") or ebs.get("Head")

    hair_root_bones = []
    for b in list(ebs):
        b_name = b.name
        if b_name.startswith("Bone") or b_name.startswith("Hair") or "hair" in b_name.lower():
            if b != head_bone:
                if b.parent is None or b.parent == head_bone or not (b.parent.name.startswith("Bone") or "hair" in b.parent.name.lower()):
                    hair_root_bones.append(b)

    print(f"  [+] Found {len(hair_root_bones)} hair strand root bones.", flush=True)

    strand_idx = 1
    for root_b in hair_root_bones:
        if head_bone:
            root_b.parent = head_bone
            root_b.use_connect = False

        curr = root_b
        depth = 1
        while curr:
            new_name = f"DEF-Hair_S{strand_idx:02d}_{depth:02d}"
            old_name = curr.name
            curr.name = new_name
            curr.use_deform = True

            for m_obj in bpy.data.objects:
                if m_obj.type == 'MESH' and old_name in m_obj.vertex_groups:
                    vg = m_obj.vertex_groups.get(old_name)
                    if vg:
                        vg.name = new_name

            children = [c for c in curr.children if c.name.startswith("Bone") or "hair" in c.name.lower() or c.name.startswith("DEF-Hair")]
            curr = children[0] if children else None
            depth += 1

        strand_idx += 1

    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.context.view_layer.update()

def preserve_dynamic_hair_skinning(main_rig):
    print("[*] Preserving vertex group skinning and world alignment on dynamic hair meshes...", flush=True)
    if not main_rig or not main_rig.data:
        return

    for obj in list(bpy.data.objects):
        if not (obj and getattr(obj, "type", None) == 'MESH' and not obj.hide_render):
            continue

        if is_limb_object(obj) or should_hide_facial_element(obj.name):
            continue

        has_hair_vgroups = any("DEF-Hair" in vg.name or "Bone." in vg.name or "hair" in vg.name.lower() for vg in obj.vertex_groups)
        is_in_hair_col = any("hair" in c.name.lower() for c in obj.users_collection)

        if has_hair_vgroups or is_in_hair_col:
            world_mat = obj.matrix_world.copy()
            obj.parent = None
            obj.constraints.clear()
            obj.matrix_world = world_mat
            bpy.context.view_layer.update()

            deselect_all_objects()
            obj.select_set(True)
            bpy.context.view_layer.objects.active = obj
            try:
                bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
            except Exception as e:
                print(f"  [~] Notice applying transform on hair mesh '{obj.name}': {e}", flush=True)

            obj.parent = main_rig
            obj.parent_type = 'OBJECT'
            obj.matrix_world = main_rig.matrix_world

            arm_mod = next((m for m in obj.modifiers if m.type == 'ARMATURE'), None)
            if not arm_mod:
                arm_mod = obj.modifiers.new(name="Armature", type='ARMATURE')
            arm_mod.object = main_rig

            obj.hide_render = False
            obj.hide_viewport = False
            print(f"  [+] Aligned and preserved skinning for hair mesh: '{obj.name}'", flush=True)

    bpy.context.view_layer.update()

def enable_all_deform_bones(main_rig):
    print("[*] Enabling 'use_deform = True' strictly on DEF-* and Root bones...", flush=True)
    if not (main_rig and main_rig.data):
        return

    for bone in main_rig.data.bones:
        b_name_lower = bone.name.lower()
        if b_name_lower.startswith("def-") or b_name_lower in ["root"]:
            bone.use_deform = True
        elif any(prefix in bone.name for prefix in ["CTR-", "MCH-", "FK-", "IK-", "WGT-", "AUX_"]):
            bone.use_deform = False
        else:
            bone.use_deform = False

    bpy.context.view_layer.update()

def setup_cycles_gpu():
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 16

    try:
        prefs = bpy.context.preferences.addons["cycles"].preferences
        prefs.get_devices()

        gpu_enabled = False
        for dev_type in ["OPTIX", "CUDA"]:
            try:
                prefs.compute_device_type = dev_type
                for device in prefs.devices:
                    device.use = True
                print(f"[+] Cycles GPU Acceleration active: {dev_type}", flush=True)
                gpu_enabled = True
                break
            except Exception:
                pass

        if gpu_enabled:
            scene.cycles.device = "GPU"
        else:
            print("[~] Notice: NVIDIA GPU not detected by Cycles API, falling back to CPU.", flush=True)
            scene.cycles.device = "CPU"

    except Exception as e:
        print(f"[~] Notice setting up Cycles GPU: {e}", flush=True)

def ensure_uv_unwrap(obj):
    if not (obj and getattr(obj, "data", None)):
        return
    if not obj.data.uv_layers:
        print(f"  [*] Generating Smart UV Project for '{obj.name}'...", flush=True)
        deselect_all_objects()
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj

        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.uv.smart_project(island_margin=0.02)
        bpy.ops.object.mode_set(mode="OBJECT")

def bake_procedural_material(mat_name, target_objects, resolution=2048):
    mat = bpy.data.materials.get(mat_name)
    if not mat or not mat.use_nodes:
        return False

    nodes = mat.node_tree.nodes
    links = mat.node_tree.links

    bsdf_node = next((n for n in nodes if n.type == "BSDF_PRINCIPLED"), None)
    if not bsdf_node:
        return False

    procedural_tex_types = {
        "TEX_NOISE", "TEX_WAVE", "TEX_VORONOI", "TEX_BRICK", "TEX_GRADIENT", "TEX_CHECKER"
    }

    bake_color = False
    bake_normal = False

    base_color_input = bsdf_node.inputs.get("Base Color")
    if base_color_input and base_color_input.is_linked:
        for n in nodes:
            if n.type in procedural_tex_types:
                bake_color = True
                break

    normal_input = bsdf_node.inputs.get("Normal")
    if normal_input and normal_input.is_linked:
        from_node = normal_input.links[0].from_node
        if from_node.type in (procedural_tex_types | {"GROUP", "BUMP"}):
            bake_normal = True

    if not (bake_color or bake_normal):
        return False

    print(f"[*] Starting Cycles Bake for '{mat_name}' (Bake Color: {bake_color}, Bake Normal: {bake_normal})...", flush=True)
    setup_cycles_gpu()

    target_obj = target_objects[0]
    ensure_uv_unwrap(target_obj)

    tmp_mesh = target_obj.data.copy()
    tmp_obj = bpy.data.objects.new(f"__tmp_bake_{mat_name}", tmp_mesh)
    bpy.context.collection.objects.link(tmp_obj)
    tmp_obj.data.materials.clear()
    tmp_obj.data.materials.append(mat)

    deselect_all_objects()
    tmp_obj.select_set(True)
    bpy.context.view_layer.objects.active = tmp_obj

    bake_node = nodes.new("ShaderNodeTexImage")
    bake_node.location = (-400, 300)

    diffuse_img = None
    if bake_color:
        diffuse_img_name = f"{mat_name}_Baked_Diffuse"
        diffuse_img = bpy.data.images.get(diffuse_img_name) or bpy.data.images.new(diffuse_img_name, width=resolution, height=resolution, alpha=True)

        print(f"  [*] Baking Diffuse map for '{mat_name}'...", flush=True)
        bake_node.image = diffuse_img
        nodes.active = bake_node
        bake_node.select = True

        scene = bpy.context.scene
        scene.cycles.bake_type = "DIFFUSE"
        scene.render.bake.use_pass_direct = False
        scene.render.bake.use_pass_indirect = False
        scene.render.bake.use_pass_color = True
        scene.render.bake.margin = 16

        bpy.ops.object.bake(type="DIFFUSE", save_mode="INTERNAL")
        diffuse_img.pack()

    normal_img = None
    if bake_normal:
        normal_img_name = f"{mat_name}_Baked_Normal"
        normal_img = bpy.data.images.get(normal_img_name) or bpy.data.images.new(normal_img_name, width=resolution, height=resolution, alpha=False)

        print(f"  [*] Baking Normal map for '{mat_name}'...", flush=True)
        normal_img.colorspace_settings.name = "Non-Color"
        bake_node.image = normal_img
        nodes.active = bake_node
        bake_node.select = True

        scene = bpy.context.scene
        scene.cycles.bake_type = "NORMAL"
        scene.render.bake.margin = 16

        bpy.ops.object.bake(type="NORMAL", save_mode="INTERNAL")
        normal_img.pack()

    bpy.data.objects.remove(tmp_obj, do_unlink=True)
    if tmp_mesh.users == 0:
        bpy.data.meshes.remove(tmp_mesh)

    nodes.remove(bake_node)

    if bake_color and diffuse_img:
        for link in list(bsdf_node.inputs["Base Color"].links):
            links.remove(link)
        tex_diffuse = nodes.new("ShaderNodeTexImage")
        tex_diffuse.location = (-400, 200)
        tex_diffuse.image = diffuse_img
        links.new(tex_diffuse.outputs["Color"], bsdf_node.inputs["Base Color"])

    if bake_normal and normal_img:
        for link in list(bsdf_node.inputs["Normal"].links):
            links.remove(link)
        tex_normal = nodes.new("ShaderNodeTexImage")
        tex_normal.location = (-400, -100)
        tex_normal.image = normal_img
        tex_normal.image.colorspace_settings.name = "Non-Color"

        normal_map_node = nodes.new("ShaderNodeNormalMap")
        normal_map_node.location = (-150, -100)
        normal_map_node.inputs["Strength"].default_value = 1.0

        links.new(tex_normal.outputs["Color"], normal_map_node.inputs["Color"])
        links.new(normal_map_node.outputs["Normal"], bsdf_node.inputs["Normal"])

    print(f"[+] Material '{mat_name}' successfully updated with baked PBR maps!\\n", flush=True)
    return True

def optimize_subsurf_modifiers(obj, max_level=1):
    if not (obj and getattr(obj, "type", None) == 'MESH'):
        return
    subsurf_mods = [m for m in obj.modifiers if m.type == 'SUBSURF']
    if not subsurf_mods:
        return
    for m in subsurf_mods[1:]:
        obj.modifiers.remove(m)
    first_subsurf = subsurf_mods[0]
    first_subsurf.levels = min(first_subsurf.levels, max_level)
    first_subsurf.render_levels = min(first_subsurf.render_levels, max_level)

def convert_particle_systems_to_real_mesh(main_rig):
    print("[*] Converting jacket/fur particle systems to real meshes...", flush=True)
    deselect_all_objects()

    if main_rig and main_rig.data:
        main_rig.data.pose_position = "REST"
    bpy.context.view_layer.update()

    ALLOWED_PARTICLE_OBJECTS = ["Cylinder.001", "Jacket", "Clothes", "Coat"]

    for obj in list(bpy.data.objects):
        if not (obj and getattr(obj, "type", None) == "MESH" and not obj.hide_render):
            continue

        psys_mods = [m for m in obj.modifiers if m.type == "PARTICLE_SYSTEM"]
        if not psys_mods:
            continue

        is_allowed = any(target.lower() in obj.name.lower() for target in ALLOWED_PARTICLE_OBJECTS)
        if not is_allowed:
            print(f"  [-] Stripped high-density particle system from '{obj.name}' to prevent mesh bloat.", flush=True)
            for m in psys_mods:
                obj.modifiers.remove(m)
            continue

        print(f"  [*] Baking particle instances on object: '{obj.name}' in REST pose...", flush=True)
        deselect_all_objects()
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj

        try:
            bpy.ops.object.duplicates_make_real(use_base_parent=False, use_hierarchy=False)
            spawned_objs = [o for o in bpy.context.selected_objects if o and o != obj and getattr(o, "type", None) == "MESH"]

            if spawned_objs:
                for inst in spawned_objs:
                    inst.hide_render = False
                    inst.hide_viewport = False

                deselect_all_objects()
                for inst in spawned_objs:
                    inst.select_set(True)
                bpy.context.view_layer.objects.active = spawned_objs[0]

                if bpy.ops.object.mode_set.poll():
                    bpy.ops.object.mode_set(mode="OBJECT")

                bpy.ops.object.join()
                joined_fur = bpy.context.view_layer.objects.active
                joined_fur.name = f"{obj.name}_Fur_Trim"
                joined_fur.hide_render = False
                joined_fur.hide_viewport = False

                if joined_fur.data and joined_fur.data.users > 1:
                    joined_fur.data = joined_fur.data.copy()

                bpy.ops.object.mode_set(mode="EDIT")
                bpy.ops.mesh.select_all(action="SELECT")
                bpy.ops.mesh.normals_make_consistent(inside=False)
                bpy.ops.object.mode_set(mode="OBJECT")

                for slot in joined_fur.material_slots:
                    if slot.material:
                        slot.material.use_backface_culling = False

                dt_mod = joined_fur.modifiers.new(name="WeightTransfer", type="DATA_TRANSFER")
                dt_mod.object = obj
                dt_mod.use_vert_data = True
                dt_mod.data_types_verts = {"VGROUP_WEIGHTS"}
                dt_mod.vert_mapping = "NEAREST"

                deselect_all_objects()
                joined_fur.select_set(True)
                bpy.context.view_layer.objects.active = joined_fur
                try:
                    bpy.ops.object.datalayout_transfer(modifier=dt_mod.name)
                except Exception:
                    pass

                bpy.ops.object.modifier_apply(modifier=dt_mod.name)

                deselect_all_objects()
                joined_fur.select_set(True)
                obj.select_set(True)
                bpy.context.view_layer.objects.active = obj
                bpy.ops.object.join()

                print(f"  [+] Seamlessly merged fur trim into jacket mesh: '{obj.name}'", flush=True)

            for m in list(obj.modifiers):
                if m.type == "PARTICLE_SYSTEM":
                    obj.modifiers.remove(m)

        except Exception as e:
            print(f"  [~] Notice converting particles on '{obj.name}': {safe_str(e)}", flush=True)

    if main_rig and main_rig.data:
        main_rig.data.pose_position = "REST"
    bpy.context.view_layer.update()

def patch_blender_52_gltf_cache_bug():
    try:
        from io_scene_gltf2.blender.exp.tree import VExportNode
        class DummyNode:
            def __init__(self):
                self.skin = None
                self.armature = None
                self.children = []
                self.matrix = None

        _dummy_singleton = DummyNode()
        if not hasattr(VExportNode, "_is_patched_for_b52"):
            def node_getter(self):
                val = getattr(self, "_node_internal_val", None)
                return val if val is not None else _dummy_singleton
            def node_setter(self, val):
                self._node_internal_val = val
            VExportNode.node = property(node_getter, node_setter)
            VExportNode._is_patched_for_b52 = True
        print("[+] Patched Blender 5.2 glTF exporter VExportNode.node property getter.", flush=True)
    except Exception as e:
        print(f"[~] Notice while patching glTF exporter: {e}", flush=True)

def get_socket_color(socket):
    if socket.is_linked:
        from_node = socket.links[0].from_node
        if from_node.type == "RGB":
            return list(from_node.outputs[0].default_value)
        elif hasattr(from_node, "outputs") and len(from_node.outputs) > 0:
            out = from_node.outputs[0]
            if hasattr(out, "default_value") and isinstance(out.default_value, (list, tuple)):
                return list(out.default_value)
    if hasattr(socket, "default_value"):
        val = socket.default_value
        if isinstance(val, (list, tuple)):
            return list(val)
    return [1.0, 1.0, 1.0, 1.0]

def convert_emission_shaders_to_pbr():
    print("[*] Converting Emission shader nodes to glTF-compatible PBR materials...", flush=True)
    for mat in bpy.data.materials:
        if not (mat and mat.use_nodes and mat.node_tree):
            continue
        output_node = next((n for n in mat.node_tree.nodes if n.type == "OUTPUT_MATERIAL" and n.is_active_output), None)
        if not output_node:
            continue
        surface_input = output_node.inputs.get("Surface")
        if not (surface_input and surface_input.is_linked):
            continue

        connected_node = surface_input.links[0].from_node
        if connected_node.type == "EMISSION":
            emission_node = connected_node
            color_val = [1.0, 1.0, 1.0, 1.0]
            if "Color" in emission_node.inputs:
                color_val = get_socket_color(emission_node.inputs["Color"])

            strength_val = 1.0
            if "Strength" in emission_node.inputs and not emission_node.inputs["Strength"].is_linked:
                strength_val = emission_node.inputs["Strength"].default_value

            eff_r = color_val[0] * strength_val
            eff_g = color_val[1] * strength_val
            eff_b = color_val[2] * strength_val

            base_r, base_g, base_b = max(0.0, min(1.0, eff_r)), max(0.0, min(1.0, eff_g)), max(0.0, min(1.0, eff_b))
            em_r, em_g, em_b = max(0.0, min(1.0, color_val[0])), max(0.0, min(1.0, color_val[1])), max(0.0, min(1.0, color_val[2]))

            bsdf_node = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
            if not bsdf_node:
                bsdf_node = mat.node_tree.nodes.new("ShaderNodeBsdfPrincipled")
                bsdf_node.location = (emission_node.location.x, emission_node.location.y)

            bsdf_node.inputs["Base Color"].default_value = (base_r, base_g, base_b, 1.0)
            bsdf_node.inputs["Roughness"].default_value = 0.15

            if "Emission Color" in bsdf_node.inputs:
                bsdf_node.inputs["Emission Color"].default_value = (em_r, em_g, em_b, 1.0)
            elif "Emission" in bsdf_node.inputs:
                bsdf_node.inputs["Emission"].default_value = (em_r, em_g, em_b, 1.0)

            if "Emission Strength" in bsdf_node.inputs:
                bsdf_node.inputs["Emission Strength"].default_value = strength_val

            mat.node_tree.links.new(bsdf_node.outputs["BSDF"], surface_input)

    bpy.context.view_layer.update()

def fix_visor_glass_materials():
    print("[*] Configuring outer visor glass materials ('Drone_Screen_Ext') for glTF transparency...", flush=True)
    for mat in bpy.data.materials:
        if mat and any(k.lower() in mat.name.lower() for k in ["screen_ext", "visorext", "glass_ext"]):
            mat.use_nodes = True
            if hasattr(mat, "blend_method"):
                mat.blend_method = 'BLEND'
            if mat.node_tree:
                for node in mat.node_tree.nodes:
                    if node.type == 'BSDF_PRINCIPLED':
                        if "Alpha" in node.inputs:
                            node.inputs["Alpha"].default_value = 0.35
                        if "Roughness" in node.inputs:
                            node.inputs["Roughness"].default_value = 0.05
                        if "Transmission Weight" in node.inputs:
                            node.inputs["Transmission Weight"].default_value = 0.8
                        elif "Transmission" in node.inputs:
                            node.inputs["Transmission"].default_value = 0.8

    bpy.context.view_layer.update()

def bind_mesh_to_head_deform_bone(obj, main_rig, bone_name="DEF-Head"):
    if not (obj and getattr(obj, "type", None) == 'MESH' and main_rig):
        return

    if main_rig.data:
        main_rig.data.pose_position = 'REST'

    if obj.parent != main_rig:
        world_mat = obj.matrix_world.copy()
        obj.parent = main_rig
        obj.parent_type = 'OBJECT'
        obj.matrix_world = world_mat

    arm_mod = next((m for m in obj.modifiers if m.type == 'ARMATURE'), None)
    if not arm_mod:
        arm_mod = obj.modifiers.new(name="Armature", type='ARMATURE')
    arm_mod.object = main_rig

    target_vgroup = bone_name if (main_rig.data and bone_name in main_rig.data.bones) else "Head"
    vgroup = obj.vertex_groups.get(target_vgroup) or obj.vertex_groups.new(name=target_vgroup)

    if obj.data and hasattr(obj.data, "vertices") and len(obj.data.vertices) > 0:
        all_vert_indices = [v.index for v in obj.data.vertices]
        vgroup.add(all_vert_indices, 1.0, 'REPLACE')

    obj.hide_render = False
    obj.hide_viewport = False

def fix_and_bind_necklace_components(main_rig):
    print("[*] Binding all separate Necklace components independently to 'DEF-Neck' without merging...", flush=True)
    if not main_rig or not main_rig.data:
        return

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()

    neck_bone = "DEF-Neck" if "DEF-Neck" in main_rig.data.bones else ("Neck" if "Neck" in main_rig.data.bones else "DEF-Head")

    necklace_objects = []

    # 1. Collect all objects residing in the Necklace collection
    for col in bpy.data.collections:
        if "necklace" in col.name.lower():
            for obj in col.all_objects:
                if obj and getattr(obj, "type", None) == 'MESH' and obj not in necklace_objects:
                    necklace_objects.append(obj)

    # 2. Collect root object Cylinder.013 and any children / related objects
    root_necklace_obj = bpy.data.objects.get("Cylinder.013")
    if root_necklace_obj and root_necklace_obj not in necklace_objects:
        necklace_objects.append(root_necklace_obj)

    for obj in list(bpy.data.objects):
        if not (obj and getattr(obj, "type", None) == 'MESH'):
            continue
        if obj.parent == root_necklace_obj or (obj.parent and "necklace" in obj.parent.name.lower()):
            if obj not in necklace_objects:
                necklace_objects.append(obj)
        elif any(k in obj.name.lower() for k in ["uzi_necklace", "necklace", "choker", "cylinder.015"]) or (obj.name == "Cylinder" and obj.parent == root_necklace_obj):
            if obj not in necklace_objects:
                necklace_objects.append(obj)

    if not necklace_objects:
        print("  [~] Notice: No necklace objects found in scene.", flush=True)
        return

    print(f"  [+] Found {len(necklace_objects)} separate necklace components: {[o.name for o in necklace_objects]}", flush=True)

    depsgraph = bpy.context.evaluated_depsgraph_get()

    # Bind each component individually to DEF-Neck without joining/merging them
    for obj in necklace_objects:
        obj.hide_render = False
        obj.hide_viewport = False

        # Capture exact current world matrix in REST pose
        world_mat = obj.matrix_world.copy()

        # Bake non-armature modifiers (e.g. Mirror, Array, Subsurf) individually ON THIS OBJECT ONLY
        non_arm_mods = [m for m in obj.modifiers if m.type != 'ARMATURE']
        if non_arm_mods:
            try:
                obj_eval = obj.evaluated_get(depsgraph)
                new_mesh = bpy.data.meshes.new_from_object(obj_eval, preserve_all_data_layers=True, depsgraph=depsgraph)
                old_mesh = obj.data
                obj.data = new_mesh
                if old_mesh and old_mesh.users == 0:
                    bpy.data.meshes.remove(old_mesh)

                for m in non_arm_mods:
                    obj.modifiers.remove(m)
            except Exception as e:
                print(f"  [~] Notice baking modifiers on '{obj.name}': {safe_str(e)}", flush=True)

        # Parent directly to main_rig in world space so glTF skeleton skinning functions cleanly
        obj.parent = main_rig
        obj.parent_type = 'OBJECT'
        obj.matrix_world = world_mat
        bpy.context.view_layer.update()

        # Add Armature modifier targeting main_rig
        arm_mod = next((m for m in obj.modifiers if m.type == 'ARMATURE'), None)
        if not arm_mod:
            arm_mod = obj.modifiers.new(name="Armature", type='ARMATURE')
        arm_mod.object = main_rig

        # Assign 100% rigid weight for DEF-Neck bone to keep component locked around neck
        vg = obj.vertex_groups.get(neck_bone) or obj.vertex_groups.new(name=neck_bone)
        if obj.data and hasattr(obj.data, "vertices") and len(obj.data.vertices) > 0:
            all_indices = list(range(len(obj.data.vertices)))
            vg.add(all_indices, 1.0, 'REPLACE')

        print(f"  [+] Bound separate component '{obj.name}' cleanly to bone '{neck_bone}'.", flush=True)

    # Bind auxiliary neck spheres (e.g. Sphere.016)
    for sphere_name in ["Sphere.016", "Neck", "Cylinder.016", "Cylinder.017", "Cylinder.018"]:
        s_obj = bpy.data.objects.get(sphere_name)
        if s_obj and getattr(s_obj, "type", None) == 'MESH' and s_obj not in necklace_objects:
            s_obj.hide_render = False
            s_obj.hide_viewport = False
            s_obj.vertex_groups.clear()
            bind_mesh_to_head_deform_bone(s_obj, main_rig, neck_bone)

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()

def bind_hands_and_fingers_safely(main_rig):
    print("[*] Converting author-parented hands and fingers to glTF-safe skinning...", flush=True)
    if not main_rig or not main_rig.data:
        return

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()

    for obj in list(bpy.data.objects):
        if obj and getattr(obj, "type", None) == 'MESH' and not obj.hide_render:
            if is_hand_or_finger(obj):
                bone_target = None
                if obj.parent == main_rig and obj.parent_type == 'BONE' and obj.parent_bone:
                    bone_target = obj.parent_bone

                if obj.parent != main_rig or obj.parent_type != 'OBJECT':
                    world_mat = obj.matrix_world.copy()
                    obj.parent = main_rig
                    obj.parent_type = 'OBJECT'
                    obj.matrix_world = world_mat

                arm_mod = next((m for m in obj.modifiers if m.type == 'ARMATURE'), None)
                if not arm_mod:
                    arm_mod = obj.modifiers.new(name="Armature", type='ARMATURE')
                arm_mod.object = main_rig

                if bone_target:
                    obj.vertex_groups.clear()
                    vgroup = obj.vertex_groups.get(bone_target) or obj.vertex_groups.new(name=bone_target)
                    if obj.data and hasattr(obj.data, "vertices") and len(obj.data.vertices) > 0:
                        all_vert_indices = [v.index for v in obj.data.vertices]
                        vgroup.add(all_vert_indices, 1.0, 'REPLACE')

                obj.hide_render = False
                obj.hide_viewport = False

    bpy.context.view_layer.update()

def bake_and_attach_teeth(main_rig):
    print("[*] Baking teeth geometry using creator's tuned modifiers and tucking behind screen...", flush=True)
    if not main_rig or not main_rig.data:
        return

    target_bone = "DEF-Head" if "DEF-Head" in main_rig.data.bones else "Head"

    for teeth_name in ["Teeth_Top", "Teeth_Bot", "Teeth_Bottom"]:
        obj = bpy.data.objects.get(teeth_name)
        if obj and getattr(obj, "type", None) == 'MESH':
            for m in obj.modifiers:
                if m.show_render:
                    m.show_viewport = True

            optimize_subsurf_modifiers(obj, max_level=1)
            depsgraph = bpy.context.evaluated_depsgraph_get()
            try:
                obj_eval = obj.evaluated_get(depsgraph)
                new_mesh = bpy.data.meshes.new_from_object(obj_eval, preserve_all_data_layers=True, depsgraph=depsgraph)
                old_mesh = obj.data
                obj.data = new_mesh
                if old_mesh and old_mesh.users == 0:
                    bpy.data.meshes.remove(old_mesh)

                for c in list(obj.constraints):
                    obj.constraints.remove(c)
                for m in list(obj.modifiers):
                    obj.modifiers.remove(m)

                bind_mesh_to_head_deform_bone(obj, main_rig, target_bone)
                obj.matrix_world.translation.y += 0.008
                if "bot" in teeth_name.lower():
                    obj.matrix_world.translation.z += 0.005
            except Exception as e:
                print(f"  [~] Notice baking teeth '{obj.name}': {safe_str(e)}", flush=True)

    bpy.context.view_layer.update()

def apply_facial_gui_visibility_and_hide_anchors(main_rig):
    print("[*] Hiding Facial_Rig control armature and GUI meshes...", flush=True)
    facial_rig = bpy.data.objects.get("Facial_Rig")
    if facial_rig:
        facial_rig.hide_render = True

    for obj in list(bpy.data.objects):
        if not obj:
            continue
        if should_hide_facial_element(obj.name):
            obj.hide_render = True

    bpy.context.view_layer.update()

def bake_clothing_modifiers_with_shapekeys():
    print("[*] Baking Subdivision, Shrinkwrap, Solidify, and Displace modifiers into clothing & visor...", flush=True)
    for arm in [o for o in bpy.data.objects if o and o.type == 'ARMATURE']:
        if arm.data:
            arm.data.pose_position = 'REST'
    bpy.context.view_layer.update()

    CLOTHING_EXACT_NAMES = [
        "Cylinder.001", "Cylinder.002", "Jacket", "Clothes", "Shirt", 
        "DD_Symbol", "WD_Symbol", "Symbol", 
        "Plane.130", "Plane.197", "Chestplate", "Shield",
        "VisorExt", "VisorInt", "Visor", "Screen"
    ]

    for obj in list(bpy.data.objects):
        if not (obj and getattr(obj, "type", None) == 'MESH' and not obj.hide_render):
            continue
        if not any(k.lower() == obj.name.lower() or k.lower() in obj.name.lower() for k in CLOTHING_EXACT_NAMES):
            continue
        if obj.name.startswith("Cylinder.") and obj.name not in ["Cylinder.001", "Cylinder.002"]:
            continue

        target_mods = [m for m in obj.modifiers if m.type in {'SUBSURF', 'SOLIDIFY', 'SHRINKWRAP', 'DISPLACE', 'CORRECTIVE_SMOOTH'}]
        if not target_mods:
            continue

        arm_mods = [m for m in obj.modifiers if m.type == 'ARMATURE']
        for m in arm_mods:
            m.show_viewport = False
        bpy.context.view_layer.update()

        try:
            if obj.data and obj.data.shape_keys:
                shape_keys = obj.data.shape_keys.key_blocks
                key_names = [k.name for k in shape_keys]
                orig_values = {k.name: k.value for k in shape_keys}
                for k in shape_keys:
                    k.value = 0.0

                eval_objs = []
                for k_name in key_names:
                    k = shape_keys.get(k_name)
                    if k is not None:
                        k.value = 1.0
                    bpy.context.view_layer.update()
                    depsgraph = bpy.context.evaluated_depsgraph_get()
                    obj_eval = obj.evaluated_get(depsgraph)
                    mesh_eval = bpy.data.meshes.new_from_object(obj_eval, preserve_all_data_layers=True, depsgraph=depsgraph)
                    tmp_obj = bpy.data.objects.new(f"__tmp_sk_{k_name}", mesh_eval)
                    bpy.context.collection.objects.link(tmp_obj)
                    eval_objs.append((k_name, tmp_obj))
                    if k is not None:
                        k.value = 0.0

                if eval_objs:
                    base_tmp_obj = eval_objs[0][1]
                    deselect_all_objects()
                    base_tmp_obj.select_set(True)
                    bpy.context.view_layer.objects.active = base_tmp_obj
                    for _, tmp_obj in eval_objs:
                        tmp_obj.select_set(True)

                    if len(eval_objs) > 1:
                        bpy.ops.object.join_shapes()

                    if base_tmp_obj.data.shape_keys:
                        for (k_name, _), sk_block in zip(eval_objs, base_tmp_obj.data.shape_keys.key_blocks):
                            sk_block.name = k_name

                    new_mesh = base_tmp_obj.data
                    old_mesh = obj.data
                    for mat in old_mesh.materials:
                        new_mesh.materials.append(mat)
                    obj.data = new_mesh

                    for _, tmp_obj in eval_objs:
                        bpy.data.objects.remove(tmp_obj, do_unlink=True)
                    if old_mesh.users == 0:
                        bpy.data.meshes.remove(old_mesh)

                    for m in list(obj.modifiers):
                        if m.type in {'SUBSURF', 'SOLIDIFY', 'SHRINKWRAP', 'DISPLACE', 'CORRECTIVE_SMOOTH'}:
                            obj.modifiers.remove(m)

                    if obj.data.shape_keys:
                        for k_name, val in orig_values.items():
                            if k_name in obj.data.shape_keys.key_blocks:
                                obj.data.shape_keys.key_blocks[k_name].value = val
                    print(f"  [+] Baked modifiers into shape keys for: '{obj.name}'", flush=True)

            else:
                depsgraph = bpy.context.evaluated_depsgraph_get()
                obj_eval = obj.evaluated_get(depsgraph)
                new_mesh = bpy.data.meshes.new_from_object(obj_eval, preserve_all_data_layers=True, depsgraph=depsgraph)
                old_mesh = obj.data
                obj.data = new_mesh
                if old_mesh == 0:
                    bpy.data.meshes.remove(old_mesh)
                for m in list(obj.modifiers):
                    if m.type in {'SUBSURF', 'SOLIDIFY', 'SHRINKWRAP', 'DISPLACE', 'CORRECTIVE_SMOOTH'}:
                        obj.modifiers.remove(m)
        except Exception as e:
            print(f"  [~] Notice while baking '{obj.name}': {safe_str(e)}", flush=True)
        finally:
            for m in arm_mods:
                m.show_viewport = True

    bpy.context.view_layer.update()

def bake_limb_modifiers():
    print("[*] Baking Array, Curve, Bevel, and Subdivision modifiers for metal limbs, wrists, and ankles...", flush=True)
    for arm in [o for o in bpy.data.objects if o and o.type == 'ARMATURE']:
        if arm.data:
            arm.data.pose_position = 'REST'
    bpy.context.view_layer.update()

    limb_objects = set()
    limbs_collection = bpy.data.collections.get("Limbs")
    if limbs_collection:
        for obj in limbs_collection.all_objects:
            if obj and getattr(obj, "type", None) == 'MESH' and not obj.hide_render:
                if not is_hand_or_finger(obj):
                    limb_objects.add(obj)

    for obj in bpy.data.objects:
        if obj and getattr(obj, "type", None) == 'MESH' and not obj.hide_render:
            if is_hand_or_finger(obj):
                continue
            name_lower = obj.name.lower()
            if is_limb_object(obj) or any(k in name_lower for k in [
                "cylinder.004", "cylinder.006", "cylinder.019", "cylinder.020",
                "cylinder.021", "cylinder.024", "cylinder.025", "cylinder.026",
                "cylinder.029", "cylinder.042", "cylinder.044", "cylinder.046"
            ]):
                limb_objects.add(obj)

    for obj in limb_objects:
        target_mods = [m for m in obj.modifiers if m.type in {'ARRAY', 'CURVE', 'BEVEL', 'SUBSURF', 'SOLIDIFY'}]
        if not target_mods:
            continue
        try:
            depsgraph = bpy.context.evaluated_depsgraph_get()
            obj_eval = obj.evaluated_get(depsgraph)
            new_mesh = bpy.data.meshes.new_from_object(obj_eval, preserve_all_data_layers=True, depsgraph=depsgraph)
            old_mesh = obj.data
            obj.data = new_mesh
            if old_mesh and old_mesh.users == 0:
                bpy.data.meshes.remove(old_mesh)
            for m in list(obj.modifiers):
                if m.type in {'ARRAY', 'CURVE', 'BEVEL', 'SUBSURF', 'SOLIDIFY'}:
                    obj.modifiers.remove(m)
            if obj.data and hasattr(obj.data, "polygons"):
                for poly in obj.data.polygons:
                    poly.use_smooth = True
        except Exception as e:
            print(f"  [~] Notice while baking limb '{obj.name}': {safe_str(e)}", flush=True)

    bpy.context.view_layer.update()

def bake_socks_modifiers():
    print("[*] Baking Displace, Solidify, and Curve modifiers for socks...", flush=True)
    socks_objects = []
    socks_collection = bpy.data.collections.get("Socks")
    if socks_collection:
        for obj in socks_collection.all_objects:
            if obj and getattr(obj, "type", None) == 'MESH':
                socks_objects.append(obj)

    for obj in bpy.data.objects:
        if obj and getattr(obj, "type", None) == 'MESH':
            if any(k in obj.name for k in ["Cylinder.003", "Cylinder.060", "Sock"]):
                if obj not in socks_objects:
                    socks_objects.append(obj)

    for obj in socks_objects:
        obj.hide_render = False
        obj.hide_viewport = False
        if obj.parent and "AUX" in obj.parent.name:
            world_mat = obj.matrix_world.copy()
            obj.parent = None
            obj.matrix_world = world_mat

        for m in list(obj.modifiers):
            if m.type == 'MASK':
                obj.modifiers.remove(m)

        deselect_all_objects()
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        try:
            bpy.ops.object.convert(target='MESH')
        except Exception as e:
            print(f"  [~] Notice converting '{obj.name}': {safe_str(e)}", flush=True)

    bpy.context.view_layer.update()

def attach_socks_to_bones(main_rig):
    print("[*] Binding baked socks to Armature with smooth leg skinning...", flush=True)
    if not main_rig or not main_rig.data:
        return

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()

    for sock_name in ["Cylinder.003", "Cylinder.060"]:
        sock_obj = bpy.data.objects.get(sock_name)
        if not sock_obj:
            continue

        sock_obj.hide_render = False
        sock_obj.hide_viewport = False
        world_mat = sock_obj.matrix_world.copy()
        sock_obj.parent = None
        sock_obj.matrix_world = world_mat
        bpy.context.view_layer.update()

        deselect_all_objects()
        sock_obj.select_set(True)
        main_rig.select_set(True)
        bpy.context.view_layer.objects.active = main_rig

        try:
            bpy.ops.object.parent_set(type='ARMATURE_AUTO')
        except Exception:
            arm_mod = next((m for m in sock_obj.modifiers if m.type == 'ARMATURE'), None)
            if not arm_mod:
                arm_mod = sock_obj.modifiers.new(name="Armature", type='ARMATURE')
                arm_mod.object = main_rig

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()

def skin_limbs_and_boots_safely(main_rig):
    print("[*] Binding metal limb meshes and solidifying boots (preventing loose lace tearing)...", flush=True)
    if not main_rig or not main_rig.data:
        return

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()

    limb_objects = []
    limbs_collection = bpy.data.collections.get("Limbs")
    if limbs_collection:
        for obj in limbs_collection.all_objects:
            if obj and getattr(obj, "type", None) == 'MESH' and not obj.hide_render:
                if not is_hand_or_finger(obj):
                    limb_objects.append(obj)

    for obj in bpy.data.objects:
        if obj and getattr(obj, "type", None) == 'MESH' and not obj.hide_render:
            name_lower = obj.name.lower()
            if is_limb_object(obj) or any(k in name_lower for k in [
                "cylinder.004", "cylinder.005", "cylinder.006", "cylinder.019", "cylinder.020",
                "cylinder.021", "cylinder.024", "cylinder.025", "cylinder.026",
                "cylinder.029", "cylinder.042", "cylinder.044", "cylinder.046"
            ]):
                if obj not in limb_objects:
                    limb_objects.append(obj)

    for obj in limb_objects:
        bone_target = None
        if obj.parent == main_rig and obj.parent_type == 'BONE' and obj.parent_bone:
            bone_target = obj.parent_bone

        world_mat = obj.matrix_world.copy()
        obj.parent = main_rig
        obj.parent_type = 'OBJECT'
        obj.matrix_world = world_mat

        arm_mod = next((m for m in obj.modifiers if m.type == 'ARMATURE'), None)
        if not arm_mod:
            arm_mod = obj.modifiers.new(name="Armature", type='ARMATURE')
        arm_mod.object = main_rig

        if bone_target:
            obj.vertex_groups.clear()
            vg = obj.vertex_groups.new(name=bone_target)
            if len(obj.data.vertices) > 0:
                vg.add(list(range(len(obj.data.vertices))), 1.0, 'REPLACE')
        elif not obj.vertex_groups:
            deselect_all_objects()
            obj.select_set(True)
            main_rig.select_set(True)
            bpy.context.view_layer.objects.active = main_rig
            try:
                bpy.ops.object.parent_set(type='ARMATURE_AUTO')
            except Exception:
                pass

        obj.hide_render = False
        obj.hide_viewport = False

    # Fix Left Boot (Cylinder.004) loose lace islands to prevent tearing
    left_boot = bpy.data.objects.get("Cylinder.004")
    if left_boot and getattr(left_boot, "type", None) == 'MESH':
        foot_bone = "DEF-Foot.L" if "DEF-Foot.L" in main_rig.data.bones else ("Foot.L" if "Foot.L" in main_rig.data.bones else "DEF-Shin.L")
        shin_bone = "DEF-Shin.L" if "DEF-Shin.L" in main_rig.data.bones else "Shin.L"

        for vg in list(left_boot.vertex_groups):
            vg_lower = vg.name.lower()
            if any(k in vg_lower for k in [".r", "_r", "right", "head", "hips", "root"]):
                left_boot.vertex_groups.remove(vg)

        vg_foot = left_boot.vertex_groups.get(foot_bone) or left_boot.vertex_groups.new(name=foot_bone)
        for v in left_boot.data.vertices:
            v_weights = [g.weight for g in v.groups if g.group < len(left_boot.vertex_groups)]
            if not v_weights or max(v_weights) < 0.2:
                vg_foot.add([v.index], 1.0, 'REPLACE')

        deselect_all_objects()
        left_boot.select_set(True)
        bpy.context.view_layer.objects.active = left_boot
        try:
            bpy.ops.object.vertex_group_clean(group_select_mode='ALL', limit=0.01)
            bpy.ops.object.vertex_group_limit_total(group_select_mode='ALL', limit=4)
            bpy.ops.object.vertex_group_normalize_all(group_select_mode='ALL', lock_active=False)
        except Exception:
            pass

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()

def fix_head_hair_and_accessories_parenting(main_rig):
    print("[*] Re-parenting active head, beanie, and accessories directly to 'DEF-Head'...", flush=True)
    if not main_rig or not main_rig.data:
        return
    target_bone = "DEF-Head" if "DEF-Head" in main_rig.data.bones else "Head"
    RIGID_HEAD_NAMES = [
        "HeadTop", "VisorExt", "VisorInt", "AUX_SCREEN", "Sphere.014", "Sphere.015",
        "Cylinder.017", "Beanie", "Hat", "Cap"
    ]

    for name in RIGID_HEAD_NAMES:
        for obj in bpy.data.objects:
            if not (obj and getattr(obj, "type", None) == 'MESH' and not obj.hide_render):
                continue
            if name.lower() in obj.name.lower() and not should_hide_facial_element(obj.name):
                if not any("DEF-Hair" in vg.name for vg in obj.vertex_groups):
                    bind_mesh_to_head_deform_bone(obj, main_rig, target_bone)

    bpy.context.view_layer.update()

def fix_and_bake_mouth_shrink(main_rig):
    print("[*] Baking 'Mouth_Shrink' native modifier stack onto visor...", flush=True)
    if not main_rig or not main_rig.data:
        return
    target_bone = "DEF-Head" if "DEF-Head" in main_rig.data.bones else "Head"
    mouth_obj = bpy.data.objects.get("Mouth_Shrink")

    if mouth_obj and getattr(mouth_obj, "type", None) == "MESH":
        for aux_name in ["AUX_MOUTH", "AUX_SCREEN", "VisorInt", "VisorExt"]:
            aux = bpy.data.objects.get(aux_name)
            if aux:
                aux.hide_viewport = False
                aux.hide_render = False

        mouth_obj.hide_viewport = False
        mouth_obj.hide_render = False

        subsurf_count = 0
        for m in list(mouth_obj.modifiers):
            m_name_lower = m.name.lower()
            if "(render)" in m_name_lower:
                perf_name = m.name.lower().replace("(render)", "(performance)").strip()
                if any(other.name.lower().strip() == perf_name for other in mouth_obj.modifiers):
                    mouth_obj.modifiers.remove(m)
                    continue

            if m.type == "SUBSURF":
                subsurf_count += 1
                if subsurf_count > 1:
                    mouth_obj.modifiers.remove(m)
                else:
                    m.show_viewport = True
                    m.show_render = True
                    m.levels = 3
                    m.render_levels = 3
            elif m.type == "SHRINKWRAP":
                m.show_viewport = True
                m.show_render = True
                m.wrap_method = "TARGET_PROJECT"

        bpy.context.view_layer.update()
        depsgraph = bpy.context.evaluated_depsgraph_get()
        try:
            mouth_eval = mouth_obj.evaluated_get(depsgraph)
            new_mesh = bpy.data.meshes.new_from_object(mouth_eval, preserve_all_data_layers=True, depsgraph=depsgraph)
            old_mesh = mouth_obj.data
            mouth_obj.data = new_mesh
            if old_mesh and old_mesh.users == 0:
                bpy.data.meshes.remove(old_mesh)
            mouth_obj.modifiers.clear()
        except Exception as e:
            print(f"  [~] Notice converting Mouth_Shrink: {safe_str(e)}", flush=True)

        bind_mesh_to_head_deform_bone(mouth_obj, main_rig, target_bone)

    internal_obj = bpy.data.objects.get("Internal")
    if internal_obj and getattr(internal_obj, "type", None) == 'MESH':
        bind_mesh_to_head_deform_bone(internal_obj, main_rig, target_bone)
        internal_obj.matrix_world.translation.y += 0.015
        internal_obj.scale.x *= 0.92
        internal_obj.scale.z *= 0.92

    bpy.context.view_layer.update()

def bake_eyes_onto_visor(main_rig):
    print("[*] Baking visual eye projection directly onto the visor screen in REST pose...", flush=True)
    if not main_rig or not main_rig.data:
        return

    head_bone = "DEF-Head" if (main_rig.data and "DEF-Head" in main_rig.data.bones) else "Head"

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()

    for eye_name in ["Eye.L", "Eye.R"]:
        obj = bpy.data.objects.get(eye_name)
        if not (obj and obj.type == 'MESH'):
            continue

        obj.hide_render = False
        obj.hide_viewport = False

        try:
            obj_eval = obj.evaluated_get(depsgraph)
            new_mesh = bpy.data.meshes.new_from_object(obj_eval, preserve_all_data_layers=True, depsgraph=depsgraph)
            world_mat = obj_eval.matrix_world.copy()

            to_rig_mat = main_rig.matrix_world.inverted() @ world_mat
            new_mesh.transform(to_rig_mat)

            old_mesh = obj.data
            obj.data = new_mesh

            if old_mesh and old_mesh.users == 0:
                bpy.data.meshes.remove(old_mesh)

            if obj.animation_data:
                obj.animation_data_clear()
            obj.constraints.clear()
            obj.modifiers.clear()

            obj.parent = main_rig
            obj.parent_type = 'OBJECT'
            obj.matrix_world = main_rig.matrix_world

            arm_mod = obj.modifiers.new(name="Armature", type='ARMATURE')
            arm_mod.object = main_rig

            obj.vertex_groups.clear()
            vg = obj.vertex_groups.new(name=head_bone)
            if len(obj.data.vertices) > 0:
                vg.add(list(range(len(obj.data.vertices))), 1.0, 'REPLACE')

            print(f"  [+] Baked and locked '{eye_name}' flush against visor under bone '{head_bone}'.", flush=True)
        except Exception as e:
            print(f"  [~] Error baking '{eye_name}': {e}", flush=True)

    bpy.context.view_layer.update()

def hide_non_character_widgets_and_symbols():
    print("[*] Marking viewport widgets, guide curves, and limb NurbsPaths as hidden from export...", flush=True)
    HIDE_PATTERNS = [
        "WGT", "Widget", "Solver", "Warning", "X.L", "X.R", "Full_X", 
        "WD_Symbol", "Display_Frame", "Marks_Frame", "Curve_Sock",
        "Mouthless", "Hide_Mouth", "ANC-Teeth"
    ]

    for obj in list(bpy.data.objects):
        if not obj:
            continue
        name_lower = obj.name.lower()
        if "nurbspath" in name_lower:
            is_head_hair = any(c.name.lower() in ["hair", "head", "head accessories"] for c in obj.users_collection) or \
                           any(s.material and "hair" in s.material.name.lower() for s in obj.material_slots)
            if not is_head_hair:
                obj.hide_render = True
                continue

        if should_hide_facial_element(obj.name) or any(pat.lower() in name_lower for pat in HIDE_PATTERNS):
            obj.hide_render = True
            continue

        if obj.type == 'CURVE':
            is_head_hair = any("hair" in c.name.lower() for c in obj.users_collection) or \
                           any(s.material and "hair" in s.material.name.lower() for s in obj.material_slots)
            if not is_head_hair:
                obj.hide_render = True
                continue

    bpy.context.view_layer.update()

def final_facial_and_widget_cleanup_pass():
    print("[*] Enforcing hide state for floating 3D facial features & GUI elements before glTF write...", flush=True)
    for obj in list(bpy.data.objects):
        if not obj:
            continue
        if should_hide_facial_element(obj.name) or any(k in obj.name.lower() for k in ["x.l", "x.r", "full_x"]):
            obj.hide_render = True
            obj.hide_viewport = True

    bpy.context.view_layer.update()

def bake_pose_action_to_all_bones(main_rig, target_action_name="Uzi_IDLE"):
    print(f"[*] Visually baking solved IK/FK pose '{target_action_name}' to all bones...", flush=True)
    if not main_rig or not main_rig.data:
        return

    for obj in bpy.data.objects:
        if obj != main_rig and obj.animation_data:
            obj.animation_data_clear()

    target_action = bpy.data.actions.get(target_action_name)
    if not target_action:
        for act in bpy.data.actions:
            if "idle" in act.name.lower() or "uzi" in act.name.lower():
                target_action = act
                break

    if not target_action:
        print(f"[-] Warning: Action '{target_action_name}' not found!", flush=True)
        return

    pose_frame = int(target_action.frame_range[0])
    bpy.context.scene.frame_start = pose_frame
    bpy.context.scene.frame_end = pose_frame
    bpy.context.scene.frame_set(pose_frame)

    main_rig.data.pose_position = 'POSE'
    if not main_rig.animation_data:
        main_rig.animation_data_create()
    main_rig.animation_data.action = target_action

    deselect_all_objects()
    main_rig.select_set(True)
    bpy.context.view_layer.objects.active = main_rig
    bpy.ops.object.mode_set(mode='POSE')

    for pbone in main_rig.pose.bones:
        if hasattr(pbone, "select"):
            pbone.select = True
        elif hasattr(pbone, "bone") and hasattr(pbone.bone, "select"):
            pbone.bone.select = True

    target_action.name = "__tmp_source_idle"

    bpy.ops.nla.bake(
        frame_start=pose_frame,
        frame_end=pose_frame,
        step=1,
        only_selected=True,
        visual_keying=True,
        clear_constraints=False,
        clear_parents=False,
        use_current_action=False,
        clean_curves=True,
        bake_types={'POSE'}
    )

    baked_action = main_rig.animation_data.action
    baked_action.name = "Uzi_IDLE"
    baked_action.use_fake_user = True

    for act in list(bpy.data.actions):
        if act != baked_action:
            try:
                bpy.data.actions.remove(act, do_unlink=True)
            except Exception:
                pass

    for t in list(main_rig.animation_data.nla_tracks):
        main_rig.animation_data.nla_tracks.remove(t)

    track = main_rig.animation_data.nla_tracks.new()
    track.name = "Uzi_IDLE"
    track.mute = False

    strip = track.strips.new("Uzi_IDLE", pose_frame, baked_action)
    strip.action = baked_action
    strip.action_frame_start = pose_frame
    strip.action_frame_end = pose_frame
    strip.frame_start = pose_frame
    strip.frame_end = pose_frame

    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.context.view_layer.update()
    print(f"[+] Baked visual transforms to all {len(main_rig.pose.bones)} bones for 'Uzi_IDLE'.", flush=True)

def run_gltf_export(filepath):
    print("[*] Executing native glTF 2.0 export with baked 'Uzi_IDLE' clip...", flush=True)
    main_rig = bpy.data.objects.get("Rig") or bpy.data.objects.get("Rig.001")

    if main_rig and main_rig.data:
        main_rig.data.pose_position = 'POSE'

    for obj in bpy.data.objects:
        if obj and getattr(obj, "type", None) == 'MESH':
            for m in list(obj.modifiers):
                if m.type == 'SUBSURF':
                    obj.modifiers.remove(m)

    patch_blender_52_gltf_cache_bug()

    bpy.ops.export_scene.gltf(
        filepath=filepath,
        export_format='GLB',
        export_skins=True,
        export_morph=True,
        export_tangents=False,
        export_normals=True,
        export_apply=False,
        export_animations=True,
        export_animation_mode='ACTIONS',
        export_anim_single_armature=True,
        export_force_sampling=False,
        export_bake_animation=False,
        export_def_bones=False,
        export_optimize_animation_size=True,
        use_renderable=True,
        export_cameras=False,
        export_lights=False,
    )
    print(f"[+] GLB export complete. Clean asset ready at: {filepath}", flush=True)

def main():
    for img in bpy.data.images:
        if img.source == 'FILE' and not img.packed_file:
            try:
                img.pack()
            except Exception as e:
                print(f"[~] Notice packing image '{img.name}': {safe_str(e)}", flush=True)

    setup_environment_and_drivers()
    convert_emission_shaders_to_pbr()
    fix_visor_glass_materials()

    main_rig = bpy.data.objects.get("Rig") or bpy.data.objects.get("Rig.001")

    if main_rig and main_rig.data:
        main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()

    # 1. Convert curves before hair pipeline runs
    convert_hair_curves_to_mesh()

    # 2. Setup Bone Hierarchy (Root, Shoulders, Hair Chains)
    setup_root_bone(main_rig)
    setup_shoulder_bones(main_rig)
    setup_and_rename_hair_chains(main_rig)
    preserve_dynamic_hair_skinning(main_rig)
    enable_all_deform_bones(main_rig)

    apply_facial_gui_visibility_and_hide_anchors(main_rig)

    # 3. Attachments & Facial Modifiers (Independently bind necklace components)
    fix_and_bind_necklace_components(main_rig)
    bake_and_attach_teeth(main_rig)

    # 4. Rigid Accessories (Beanie, Visor)
    fix_head_hair_and_accessories_parenting(main_rig)
    fix_and_bake_mouth_shrink(main_rig)

    # 5. Clothing, Particles & Limbs
    bake_clothing_modifiers_with_shapekeys()
    convert_particle_systems_to_real_mesh(main_rig)

    bake_limb_modifiers()
    bake_socks_modifiers()
    attach_socks_to_bones(main_rig)

    # 6. Procedural Materials Bake Pass
    procedural_and_group_node_types = {
        "TEX_NOISE", "TEX_WAVE", "TEX_VORONOI", "TEX_BRICK", "TEX_GRADIENT", "TEX_CHECKER", "GROUP", "BUMP"
    }

    for mat in list(bpy.data.materials):
        if not (mat and mat.use_nodes and mat.node_tree):
            continue

        has_procedural_or_group = any(n.type in procedural_and_group_node_types for n in mat.node_tree.nodes)
        if not has_procedural_or_group:
            continue

        target_objs = [
            obj for obj in bpy.data.objects
            if obj and getattr(obj, "type", None) == "MESH" and not obj.hide_render and any(slot.material == mat for slot in obj.material_slots)
        ]

        if target_objs:
            bake_procedural_material(mat.name, target_objs, resolution=2048)

    # 7. Final Armature Binding & Eye Bake
    skin_limbs_and_boots_safely(main_rig)
    bind_hands_and_fingers_safely(main_rig)
    bake_eyes_onto_visor(main_rig)
    hide_non_character_widgets_and_symbols()
    final_facial_and_widget_cleanup_pass()

    # 8. Visually Bake Pose (IK Arms, Legs, Spine)
    bake_pose_action_to_all_bones(main_rig, target_action_name="Uzi_IDLE")

    # 9. Run glTF Exporter
    run_gltf_export(__GLB_PATH__)

if __name__ == "__main__":
    main()
"""

    json_path = json.dumps(glb_path)
    expr = expr.replace("__GLB_PATH__", json_path)

    try:
        with open(temp_blend_path, "wb") as f:
            f.write(original_bytes)
    except Exception as e:
        print(f"[-] Failed to write temporary file {temp_blend_path}: {e}")
        return False

    env = os.environ.copy()
    env.pop("PYTHONHOME", None)
    env.pop("PYTHONPATH", None)
    env.pop("VIRTUAL_ENV", None)
    env["PYTHONNOUSERSITE"] = "1"

    mac_blender_path = "/Applications/Blender.app/Contents/MacOS/Blender"
    base_cmd = [mac_blender_path] if os.path.exists(mac_blender_path) else ["blender"]

    cmd = base_cmd + ["-y", "-b", temp_blend_path, "--python-expr", expr]

    try:
        process = subprocess.Popen(
            cmd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
        )
        for line in process.stdout:
            print(line, end="", flush=True)
        process.wait()

        file_created = os.path.exists(glb_path) and os.path.getsize(glb_path) > 0
        export_success = process.returncode == 0 or file_created
    except Exception as e:
        print(f"[-] Blender process execution failed: {e}")
        export_success = False
    finally:
        if os.path.exists(temp_blend_path):
            try:
                os.remove(temp_blend_path)
            except Exception as e:
                print(f"[~] Notice removing temp file: {e}")

    return export_success


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if not export_uzi_to_glb(args.input, args.output):
        sys.exit(1)
