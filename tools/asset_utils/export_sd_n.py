# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# tools/asset_utils/export_sd_n.py
import json
import os
import subprocess
import sys


def export_sd_n_to_glb(blend_path: str, glb_path: str) -> bool:
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
import math
import os

FACIAL_HIDE_PATTERNS = [
    "eyelid", "eyebrow", "tongue", "blush", "expression", "mark",
    "anc-", "ctr-", "wgt-", "aux_mouth", "aux_screen", "aux_eye", "aux_face", "icosphere.001",
    "mouthless", "display_frame", "marks_frame", "solver", "warning", "full_x",
    "x.l", "x.r", "tail_hooks", "nurbspath.002"
]

HAND_FINGER_PATTERNS = [
    "hand", "finger", "thumb", "index", "middle", "pinky", 
    "roundcube", "plane.037", "plane.128"
]

def safe_str(val) -> str:
    try:
        if isinstance(val, bytes):
            return val.decode("utf-8", errors="replace")
        return str(val).encode("utf-8", errors="replace").decode("utf-8")
    except Exception:
        return "Unknown Error"

def is_fur_object(obj):
    if not obj:
        return False
    name_l = obj.name.lower()
    col_names = " ".join([c.name.lower() for c in obj.users_collection])
    parent_names = []
    curr = obj.parent
    while curr:
        parent_names.append(curr.name.lower())
        curr = curr.parent
    p_chain = " ".join(parent_names)
    mat_names = " ".join([s.material.name.lower() for s in obj.material_slots if s.material])
    return "fur" in col_names or "fur" in name_l or "fur" in p_chain or "fur" in mat_names or "clothes_fur" in col_names

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

            name_lower = obj.name.lower()
            if any(k in name_lower for k in ["tail", "nurbspath.002", "nurbspath.026", "curve_sock", "wing"]):
                continue
            if any("tail" in col.name.lower() for col in obj.users_collection):
                continue

            is_hair_or_fur = any(k in col.name.lower() for k in ["hair", "fur", "clothes_fur"] for col in obj.users_collection) or \
                             any(slot.material and any(k in slot.material.name.lower() for k in ["hair", "fur"]) for slot in obj.material_slots) or \
                             (hasattr(obj.data, "bevel_depth") and obj.data.bevel_depth > 0)

            if is_hair_or_fur:
                deselect_all_objects()
                obj.select_set(True)
                bpy.context.view_layer.objects.active = obj
                try:
                    bpy.ops.object.convert(target='MESH')
                    obj.name = f"Hair_Mesh_{obj.name}"
                    obj.hide_render = False
                    obj.hide_viewport = False
                    print(f"  [+] Converted strand curve to MESH: '{obj.name}'", flush=True)
                except Exception as e:
                    print(f"  [-] Skipped '{obj.name}': {e}", flush=True)

    bpy.context.view_layer.update()

def merge_and_setup_hair_rig(main_rig):
    print("[*] Merging Hair_Rig into main rig and renaming hair chains to DEF-Hair_SXX_YY...", flush=True)
    if not main_rig or not main_rig.data:
        return

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()

    head_bone_name = "DEF-Head" if "DEF-Head" in main_rig.data.bones else "Head"

    hair_rigs = [
        o for o in bpy.data.objects 
        if o.type == 'ARMATURE' and o != main_rig and any(k in o.name.lower() for k in ["hair", "armature.051", "hair_rig"])
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
            print(f"  [~] Notice transform_apply on '{h_rig.name}': {e}", flush=True)

        deselect_all_objects()
        h_rig.select_set(True)
        main_rig.select_set(True)
        bpy.context.view_layer.objects.active = main_rig
        try:
            bpy.ops.object.join()
            print(f"  [+] Merged '{h_rig.name}' into main rig.", flush=True)
        except Exception as e:
            print(f"  [~] Notice joining hair rig: {safe_str(e)}", flush=True)

    deselect_all_objects()
    main_rig.select_set(True)
    bpy.context.view_layer.objects.active = main_rig
    bpy.ops.object.mode_set(mode='EDIT')
    ebs = main_rig.data.edit_bones

    head_eb = ebs.get(head_bone_name) or ebs.get("DEF-Head") or ebs.get("Head")

    hair_root_bones = []
    for b in list(ebs):
        b_name = b.name
        if b_name.startswith("Bone") or b_name.startswith("Hair") or "hair" in b_name.lower():
            if b != head_eb:
                if b.parent is None or b.parent == head_eb or not (b.parent.name.startswith("Bone") or "hair" in b.parent.name.lower()):
                    hair_root_bones.append(b)

    print(f"  [+] Found {len(hair_root_bones)} hair strand root bones.", flush=True)

    strand_idx = 1
    for root_b in hair_root_bones:
        if head_eb:
            root_b.parent = head_eb
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

def bind_hair_meshes_to_rig(main_rig):
    print("[*] Binding and skinning all hair strand meshes to main rig...", flush=True)
    if not main_rig or not main_rig.data:
        return

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()

    head_bone_name = "DEF-Head" if "DEF-Head" in main_rig.data.bones else "Head"
    main_bone_names = {b.name for b in main_rig.data.bones}

    for obj in list(bpy.data.objects):
        if not (obj and getattr(obj, "type", None) == 'MESH' and not obj.hide_render):
            continue

        if is_fur_object(obj):
            continue

        name_lower = obj.name.lower()
        col_names = " ".join([c.name.lower() for c in obj.users_collection])

        is_hair = "hair" in col_names or "hair" in name_lower or (name_lower.startswith("nurbspath") and "tail" not in col_names and "tail" not in name_lower and "nurbspath.002" not in name_lower and "nurbspath.026" not in name_lower) or \
                  any(s.material and "hair" in s.material.name.lower() for s in obj.material_slots)

        if not is_hair:
            continue

        if is_limb_object(obj) or any(k in name_lower for k in ["tail", "wing", "nurbspath.002", "nurbspath.026"]):
            continue

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

        existing_vgs = [vg.name for vg in obj.vertex_groups if vg.name in main_bone_names]

        if not existing_vgs:
            obj.vertex_groups.clear()
            vg = obj.vertex_groups.new(name=head_bone_name)
            if obj.data and hasattr(obj.data, "vertices") and len(obj.data.vertices) > 0:
                vg.add(list(range(len(obj.data.vertices))), 1.0, 'REPLACE')
            print(f"  [+] Bound unweighted hair mesh '{obj.name}' directly to '{head_bone_name}'.", flush=True)
        else:
            if head_bone_name in main_rig.data.bones:
                main_rig.data.bones[head_bone_name].use_deform = True
            for vg_name in existing_vgs:
                if vg_name in main_rig.data.bones:
                    main_rig.data.bones[vg_name].use_deform = True
            print(f"  [+] Preserved dynamic skinning for hair mesh '{obj.name}' with bones: {existing_vgs}", flush=True)

        obj.hide_render = False
        obj.hide_viewport = False

    bpy.context.view_layer.update()

def point_to_segment_projection(p, h, t):
    v = t - h
    w = p - h
    c2 = v.dot(v)
    if c2 <= 1e-8:
        return 0.0, (p - h).length, h
    c1 = w.dot(v)
    t_param = max(0.0, min(1.0, c1 / c2))
    proj = h + t_param * v
    dist = (p - proj).length
    return t_param, dist, proj

def get_ordered_tail_bones(main_rig):
    if not (main_rig and main_rig.data):
        return []

    ctr_tail_bones = []
    for i in range(1, 20):
        name_candidates = [f"CTR-Tail_{i}", f"CTR-Tail.{i:03d}", f"CTR-tail_{i}"]
        found = next((main_rig.data.bones[n] for n in name_candidates if n in main_rig.data.bones), None)
        if found:
            ctr_tail_bones.append(found)
        else:
            break

    if len(ctr_tail_bones) >= 3:
        return ctr_tail_bones

    def_tail_bones = [
        b for b in main_rig.data.bones
        if (b.name.lower().startswith("def-tail") or "tail" in b.name.lower()) and
           not any(p in b.name for p in ["CTR-", "MCH-", "FK-", "IK-", "WGT-", "AUX_"])
    ]
    if def_tail_bones:
        root_tail = next((b for b in def_tail_bones if b.parent is None or b.parent not in def_tail_bones), def_tail_bones[0])
        ordered = [root_tail]
        curr = root_tail
        while curr:
            children = [c for c in curr.children if c in def_tail_bones]
            if children:
                curr = children[0]
                ordered.append(curr)
            else:
                break
        return ordered

    return [b for b in main_rig.data.bones if "tail" in b.name.lower() and not any(p in b.name for p in ["WGT-", "MCH-"])]

def convert_and_rig_tail(main_rig):
    print("[*] Converting Tail cable to 3D mesh with 13-point CTR-Tail skinning...", flush=True)
    if not (main_rig and main_rig.data):
        return

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()

    def find_bone(candidates, fallback):
        for c in candidates:
            if c in main_rig.data.bones:
                return c
        for b in main_rig.data.bones:
            for c in candidates:
                if b.name.lower() == c.lower():
                    return b.name
        return fallback

    tail_bones = get_ordered_tail_bones(main_rig)
    if not tail_bones:
        print("[-] Warning: No tail bones found on armature!", flush=True)
        return

    for b in tail_bones:
        b.use_deform = True

    # 1. Convert syringe accessories curves to real MESH
    for obj in list(bpy.data.objects):
        if obj and getattr(obj, "type", None) in {'CURVE', 'SURFACE'} and not obj.name.startswith("__"):
            name_l = obj.name.lower()
            p_name_l = obj.parent.name.lower() if obj.parent else ""
            if "nurbspath.026" in name_l or "tip" in p_name_l or ("syringe" in name_l and "tail" in name_l):
                print(f"  [*] Converting tail syringe tube curve '{obj.name}' to MESH...", flush=True)
                deselect_all_objects()
                obj.select_set(True)
                bpy.context.view_layer.objects.active = obj
                try:
                    bpy.ops.object.convert(target='MESH')
                    obj.hide_render = False
                    obj.hide_viewport = False
                except Exception as e:
                    print(f"  [-] Error converting '{obj.name}': {e}", flush=True)

    # 2. Build continuous joint spine in world coordinates for main tail cable
    joints = [main_rig.matrix_world @ tail_bones[0].head_local]
    for b in tail_bones:
        joints.append(main_rig.matrix_world @ b.tail_local)

    seg_lengths = []
    for i in range(len(joints) - 1):
        seg_lengths.append((joints[i+1] - joints[i]).length)
    
    total_len = sum(seg_lengths) if sum(seg_lengths) > 1e-6 else 1.0
    cum_dist = [0.0]
    for l in seg_lengths:
        cum_dist.append(cum_dist[-1] + l)

    bone_centers = []
    for i in range(len(tail_bones)):
        c_param = (cum_dist[i] + cum_dist[i+1]) / (2.0 * total_len)
        bone_centers.append((tail_bones[i].name, c_param))

    hips_name = find_bone(["DEF-Hips", "Hips", "DEF-Pelvis"], None)
    if hips_name and hips_name in main_rig.data.bones:
        main_rig.data.bones[hips_name].use_deform = True

    tail_curves = [
        o for o in list(bpy.data.objects)
        if o and getattr(o, "type", None) in {'CURVE', 'SURFACE'} and 
           "tail" in o.name.lower() and "nurbspath.002" not in o.name.lower() and "nurbspath.026" not in o.name.lower() and
           not o.name.startswith("__")
    ]

    for t_obj in tail_curves:
        t_orig_name = t_obj.name
        print(f"  [*] Baking Hook modifiers into smooth curved mesh for '{t_orig_name}'...", flush=True)

        t_obj.hide_render = False
        t_obj.hide_viewport = False

        deselect_all_objects()
        t_obj.select_set(True)
        bpy.context.view_layer.objects.active = t_obj

        try:
            bpy.ops.object.duplicate()
            tail_mesh_obj = bpy.context.view_layer.objects.active
            tail_mesh_obj.name = t_orig_name

            for m in tail_mesh_obj.modifiers:
                m.show_viewport = True
                m.show_render = True

            bpy.ops.object.convert(target='MESH')
            print(f"  [+] Converted curved mesh '{tail_mesh_obj.name}': {len(tail_mesh_obj.data.vertices)} vertices.", flush=True)

            t_obj.name = f"__orig_curve_{t_orig_name}"
            t_obj.hide_render = True
            t_obj.hide_viewport = True

            if not tail_mesh_obj.material_slots and t_obj.material_slots:
                for slot in t_obj.material_slots:
                    if slot.material:
                        tail_mesh_obj.data.materials.append(slot.material)

            if tail_mesh_obj.data and hasattr(tail_mesh_obj.data, "polygons"):
                for poly in tail_mesh_obj.data.polygons:
                    poly.use_smooth = True

            deselect_all_objects()
            tail_mesh_obj.select_set(True)
            bpy.context.view_layer.objects.active = tail_mesh_obj
            try:
                bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
            except Exception as e:
                print(f"  [~] Notice applying transform on tail mesh: {e}", flush=True)

            tail_mesh_obj.vertex_groups.clear()
            vgroups = {b.name: tail_mesh_obj.vertex_groups.new(name=b.name) for b in tail_bones}
            if hips_name:
                vgroups[hips_name] = tail_mesh_obj.vertex_groups.new(name=hips_name)

            sigma = 1.0 / (len(tail_bones) * 1.5)

            for v in tail_mesh_obj.data.vertices:
                p_world = v.co
                
                best_seg = 0
                best_t = 0.0
                best_dist = 1e9
                for i in range(len(joints) - 1):
                    t_p, d, _ = point_to_segment_projection(p_world, joints[i], joints[i+1])
                    if d < best_dist:
                        best_dist = d
                        best_seg = i
                        best_t = t_p

                u = (cum_dist[best_seg] + best_t * seg_lengths[best_seg]) / total_len
                u = max(0.0, min(1.0, u))

                raw_w = []
                for _, c_param in bone_centers:
                    gw = math.exp(-((u - c_param) ** 2) / (2.0 * (sigma ** 2)))
                    raw_w.append(gw)

                hips_w = 0.0
                if hips_name and u < 0.15:
                    hips_w = math.exp(-((u - 0.0) ** 2) / (2.0 * ((sigma * 0.8) ** 2))) * 0.6

                tot_w = sum(raw_w) + hips_w
                if tot_w < 1e-6:
                    tot_w = 1.0

                for i, (b_name, _) in enumerate(bone_centers):
                    nw = raw_w[i] / tot_w
                    if nw > 0.01:
                        vgroups[b_name].add([v.index], nw, 'REPLACE')

                if hips_name and hips_w > 0.01:
                    vgroups[hips_name].add([v.index], hips_w / tot_w, 'REPLACE')

            tail_mesh_obj.parent = main_rig
            tail_mesh_obj.parent_type = 'OBJECT'
            tail_mesh_obj.matrix_world = main_rig.matrix_world

            arm_mod = tail_mesh_obj.modifiers.new(name="Armature", type='ARMATURE')
            arm_mod.object = main_rig

            tail_mesh_obj.hide_render = False
            tail_mesh_obj.hide_viewport = False
            print(f"  [+] Successfully rigged smooth continuous tail mesh '{tail_mesh_obj.name}'.", flush=True)

        except Exception as e:
            print(f"  [-] Error converting and rigging tail: {e}", flush=True)

    bpy.context.view_layer.update()

def enable_all_deform_bones(main_rig):
    print("[*] Enabling 'use_deform = True' on all deform, hair, CTR-Tail, and limb bones...", flush=True)
    if not (main_rig and main_rig.data):
        return

    for bone in main_rig.data.bones:
        b_name_lower = bone.name.lower()
        if b_name_lower.startswith("def-") or \
           b_name_lower.startswith("bone") or \
           b_name_lower.startswith("ctr-tail") or \
           "hair" in b_name_lower or \
           "tail" in b_name_lower or \
           b_name_lower in [
               "root", "thigh.l", "thigh.r", "head", "foot.l", "foot.r", 
               "forearm.l", "forearm.r", "hand.l", "hand.r", "shin.l", "shin.r", "hips"
           ]:
            if any(k in b_name_lower for k in ["wgt", "direction", "twist"]):
                bone.use_deform = False
            else:
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
            print("[~] Notice: GPU not detected by Cycles API, falling back to CPU.", flush=True)
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
    name_l = obj.name.lower()
    if any(k in name_l for k in ["symbol", "dd_symbol", "wd_symbol", "logo"]):
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
    print("[*] Converting coat collar/cuff particle systems to real meshes...", flush=True)
    deselect_all_objects()

    if main_rig and main_rig.data:
        main_rig.data.pose_position = "REST"
    bpy.context.view_layer.update()

    ALLOWED_PARTICLE_OBJECTS = ["Cylinder.001", "Jacket", "Clothes", "Coat", "Collar", "Fur"]

    for obj in list(bpy.data.objects):
        if not (obj and getattr(obj, "type", None) == "MESH" and not obj.hide_render):
            continue

        psys_mods = [m for m in obj.modifiers if m.type == "PARTICLE_SYSTEM"]
        if not psys_mods:
            continue

        is_allowed = any(target.lower() in obj.name.lower() for target in ALLOWED_PARTICLE_OBJECTS)
        if not is_allowed:
            print(f"  [-] Stripped particle system from '{obj.name}' to prevent mesh bloat.", flush=True)
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

                print(f"  [+] Seamlessly merged fur trim into coat mesh: '{obj.name}'", flush=True)

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
    print("[*] Configuring outer visor glass materials for glTF transparency...", flush=True)
    for mat in bpy.data.materials:
        if mat and any(k.lower() in mat.name.lower() for k in ["screen_ext", "visorext", "glass_ext", "drone_screen_ext"]):
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

    if obj.parent != main_rig or obj.parent_type != 'OBJECT':
        world_mat = obj.matrix_world.copy()
        obj.parent = main_rig
        obj.parent_type = 'OBJECT'
        obj.matrix_world = world_mat

    arm_mod = next((m for m in obj.modifiers if m.type == 'ARMATURE'), None)
    if not arm_mod:
        arm_mod = obj.modifiers.new(name="Armature", type='ARMATURE')
    arm_mod.object = main_rig

    target_vgroup = bone_name if (main_rig.data and bone_name in main_rig.data.bones) else "Head"
    obj.vertex_groups.clear()
    vgroup = obj.vertex_groups.new(name=target_vgroup)

    if obj.data and hasattr(obj.data, "vertices") and len(obj.data.vertices) > 0:
        all_vert_indices = [v.index for v in obj.data.vertices]
        vgroup.add(all_vert_indices, 1.0, 'REPLACE')

    obj.hide_render = False
    obj.hide_viewport = False

def resolve_accessory_bone(obj, bones):
    head_bone = bones["head"]
    chest_bone = bones["chest"]
    thigh_r_bone = bones["thigh_r"]
    thigh_l_bone = bones["thigh_l"]
    forearm_l_bone = bones["forearm_l"]
    forearm_r_bone = bones["forearm_r"]
    hand_l_bone = bones["hand_l"]
    hand_r_bone = bones["hand_r"]
    foot_r_bone = bones["foot_r"]
    foot_l_bone = bones["foot_l"]
    tail_tip_bone = bones["tail_tip"]
    arm_l_bone = bones["arm_l"]
    hips_bone = bones["hips"]

    name_lower = obj.name.lower()
    parent_names = []
    curr = obj.parent
    while curr:
        parent_names.append(curr.name.lower())
        curr = curr.parent
    p_chain = " ".join(parent_names)

    col_names = " ".join([c.name.lower() for c in obj.users_collection])

    if "hair" in col_names or "hair" in name_lower or (name_lower.startswith("nurbspath") and "tail" not in col_names and "tail" not in name_lower and "nurbspath.002" not in name_lower and "nurbspath.026" not in name_lower and not is_fur_object(obj)):
        return None
    if name_lower in ["tail", "nurbspath.002"] or name_lower.startswith("__orig_curve"):
        return None

    if "hand.l" in col_names or "hand.r" in col_names or any(k in name_lower for k in ["plane.037", "plane.128", "circle.074", "circle.076", "circle.077", "circle.044", "circle.045"]):
        return None

    # 1. Coat Buttons & Fasteners (Circle.002 on N_coat/Jacket)
    if "circle.002" in name_lower or "button" in name_lower or (any(k in p_chain for k in ["n_coat", "coat", "jacket"]) and any(k in name_lower for k in ["circle", "button", "clasp"])):
        if not is_fur_object(obj):
            return chest_bone

    # 2. Tail Syringe Bulb, Needle & Nanite Fluid Tube (Tip, NurbsPath.026, Syringe, Nanite, Bulb)
    if any(k == name_lower or k in name_lower for k in ["tip", "tail_bulb", "syringe", "nanite", "acid", "nurbspath.026"]) or \
       any(k in p_chain for k in ["tip", "tail_tip", "tail"]):
        if not any(k in name_lower for k in ["belt", "waist", "hips"]):
            return tail_tip_bone

    # 3. Left Forearm Internals & Sockets (Sphere.014, Cylinder.005, AUX_Door.L, Circle.082, Circle.083, Circle.084, Cylinder.063)
    if "sphere.014" in name_lower or "cylinder.005" in p_chain or "aux_door.l" in p_chain or "cylinder.063" in name_lower or \
       any(k in name_lower for k in ["aux_door.l", "circle.082", "circle.083", "circle.084", "blade_parent.l", "smg_parent.l"]):
        return forearm_l_bone

    # 4. Right Forearm Internals & Sockets (Sphere.015, AUX_Door.R, etc.)
    if "sphere.015" in name_lower or "aux_door.r" in p_chain or any(k in name_lower for k in ["aux_door.r", "blade_parent.r", "smg_parent.r", "plane.045"]):
        return forearm_r_bone

    # 5. Left Boot / Foot / Sole Details (Cylinder.042, Cylinder.043, Circle.059, and all sole children of Cylinder.042)
    if "cylinder.042" in p_chain or "cylinder.042" in name_lower or "cylinder.043" in name_lower or \
       "boot.l" in p_chain or "foot.l" in p_chain or "foot.l" in col_names or "boot.l" in col_names or "circle.059" in name_lower:
        return foot_l_bone

    # 6. Right Boot / Foot / Sole Details (Cylinder.039, Cylinder.041, right boot sole details)
    if "cylinder.039" in p_chain or "cylinder.039" in name_lower or "cylinder.041" in name_lower or \
       "boot.r" in p_chain or "foot.r" in p_chain or "foot.r" in col_names or "boot.r" in col_names:
        return foot_r_bone

    # 7. Head Accessories (Headband, Sockets like Cylinder.030 / Cylinder.038, Bulbs like Sphere.037, Hat Badge, Hat, Beanie)
    if any(k in p_chain for k in ["headband", "head_parent", "head_accessories", "badge", "empty_badge", "headtop", "hat", "cap", "beanie"]) or \
       any(k in col_names for k in ["head accessories", "headband", "head", "hat"]) or \
       any(k in name_lower for k in ["headband", "badge", "cylinder.030", "cylinder.038", "sphere.037", "cylinder.017"]):
        if not any(k in p_chain or k in name_lower for k in [
            "cylinder.042", "cylinder.040", "cylinder.037", "cylinder.005", "cylinder.039", "cylinder.041", 
            "cylinder.043", "cylinder.063", "sphere.014", "sphere.015", "claw", "weapon", "thigh", "boot", "foot", "sole"
        ]):
            return head_bone

    # 8. Thigh Hazard Bands
    if "cylinder.040" in name_lower or "roundcube.030" in p_chain or "thigh_band.r" in name_lower:
        return thigh_r_bone
    if "cylinder.037" in name_lower or "roundcube.046" in p_chain or "thigh_band.l" in name_lower:
        return thigh_l_bone

    # 9. Pilot Armband
    if "armband" in name_lower:
        return arm_l_bone

    # 10. Waist / Belt
    if "waist" in name_lower or "belt_n" in name_lower:
        return hips_bone

    return None

def fix_and_bind_sd_n_accessories(main_rig):
    print("[*] Binding SD-N accessories (Coat Button, Tail Syringe, Arm Internals, Foot Soles, Headband Lights)...", flush=True)
    if not main_rig or not main_rig.data:
        return

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()

    def find_bone(candidates, fallback):
        for c in candidates:
            if c in main_rig.data.bones:
                return c
        for b in main_rig.data.bones:
            for c in candidates:
                if b.name.lower() == c.lower():
                    return b.name
        return fallback

    tail_bones = get_ordered_tail_bones(main_rig)
    last_tail_bone = tail_bones[-1].name if tail_bones else "DEF-Tail_Tip"

    bones = {
        "head": find_bone(["DEF-Head", "Head", "head"], "DEF-Head"),
        "chest": find_bone(["DEF-Chest", "Chest", "chest", "DEF-Spine_02", "Spine_02"], "DEF-Chest"),
        "thigh_r": find_bone(["DEF-Thigh.R", "DEF-thigh.R", "DEF-UpLeg.R", "DEF-upleg.R", "Thigh.R", "thigh.R", "DEF-Leg.R"], "DEF-Thigh.R"),
        "thigh_l": find_bone(["DEF-Thigh.L", "DEF-thigh.L", "DEF-UpLeg.L", "DEF-upleg.L", "Thigh.L", "thigh.L", "DEF-Leg.L"], "DEF-Thigh.L"),
        "forearm_l": find_bone(["DEF-Forearm.L", "DEF-forearm.L", "DEF-ForeArm.L", "DEF-Hand.L", "Forearm.L", "forearm.L"], "DEF-Forearm.L"),
        "forearm_r": find_bone(["DEF-Forearm.R", "DEF-forearm.R", "DEF-ForeArm.R", "DEF-Hand.R", "Forearm.R", "forearm.R"], "DEF-Forearm.R"),
        "hand_l": find_bone(["DEF-Hand.L", "DEF-hand.L", "DEF-Wrist.L", "Hand.L", "DEF-Forearm.L"], "DEF-Hand.L"),
        "hand_r": find_bone(["DEF-Hand.R", "DEF-hand.R", "DEF-Wrist.R", "Hand.R", "DEF-Forearm.R"], "DEF-Hand.R"),
        "foot_r": find_bone(["DEF-Foot.R", "DEF-foot.R", "DEF-Shin.R", "DEF-shin.R", "Foot.R"], "DEF-Foot.R"),
        "foot_l": find_bone(["DEF-Foot.L", "DEF-foot.L", "DEF-Shin.L", "DEF-shin.L", "Foot.L"], "DEF-Foot.L"),
        "tail_tip": find_bone([last_tail_bone, "CTR-Tail_13", "DEF-Tail_Tip", "DEF-Tail_4", "DEF-Tail.004", "DEF-Tail_5", "Tail_Tip"], last_tail_bone),
        "arm_l": find_bone(["DEF-Upper_arm.L", "DEF-upper_arm.L", "DEF-Forearm.L", "DEF-forearm.L", "Upper_Arm.L"], "DEF-Upper_arm.L"),
        "hips": find_bone(["DEF-Hips", "Hips", "hips", "DEF-Pelvis", "Pelvis"], "DEF-Hips"),
    }

    accessories = []

    for obj in list(bpy.data.objects):
        if not (obj and getattr(obj, "type", None) == 'MESH'):
            continue
        if should_hide_facial_element(obj.name):
            continue

        target_bone = resolve_accessory_bone(obj, bones)
        if target_bone:
            accessories.append((obj, target_bone))

    for obj, target_bone in accessories:
        obj.hide_render = False
        obj.hide_viewport = False

        world_mat = obj.matrix_world.copy()

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

        for c in list(obj.constraints):
            obj.constraints.remove(c)

        obj.parent = main_rig
        obj.parent_type = 'OBJECT'
        obj.matrix_world = world_mat
        bpy.context.view_layer.update()

        arm_mod = next((m for m in obj.modifiers if m.type == 'ARMATURE'), None)
        if not arm_mod:
            arm_mod = obj.modifiers.new(name="Armature", type='ARMATURE')
        arm_mod.object = main_rig

        if target_bone in main_rig.data.bones:
            main_rig.data.bones[target_bone].use_deform = True

        obj.vertex_groups.clear()
        vg = obj.vertex_groups.new(name=target_bone)
        if obj.data and hasattr(obj.data, "vertices") and len(obj.data.vertices) > 0:
            all_indices = list(range(len(obj.data.vertices)))
            vg.add(all_indices, 1.0, 'REPLACE')

        print(f"  [+] Bound accessory '{obj.name}' cleanly to bone '{target_bone}'.", flush=True)

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()

def disable_and_scale_down_weapons(main_rig):
    print("[*] Scaling down and disabling inactive weapon meshes (Claws, Blades, SMG)...", flush=True)
    WEAPON_PATTERNS = ["claw", "blade", "smg", "gun", "sword"]

    for obj in list(bpy.data.objects):
        if not (obj and getattr(obj, "type", None) == 'MESH'):
            continue

        if any(k in obj.name.lower() for k in ["sphere.014", "sphere.015"]):
            continue

        name_lower = obj.name.lower()
        parent_names = []
        curr = obj.parent
        while curr:
            parent_names.append(curr.name.lower())
            curr = curr.parent
        p_chain = " ".join(parent_names)
        col_names = " ".join([c.name.lower() for c in obj.users_collection])

        is_weapon = any(w in name_lower or w in p_chain or w in col_names for w in WEAPON_PATTERNS)
        is_in_weapons_col = any("weapon" in c.name.lower() for c in obj.users_collection)

        if (is_weapon or is_in_weapons_col) and not any(k in name_lower for k in [
            "cylinder.005", "aux_door", "circle.082", "circle.083", "circle.084", "sphere.014", "sphere.015", "tip", "tail", "nurbspath.002", "nurbspath.026", "plane.037", "plane.128"
        ]):
            print(f"  [-] Disabling/scaling down weapon mesh: '{obj.name}'", flush=True)
            if obj.data and hasattr(obj.data, "vertices"):
                for v in obj.data.vertices:
                    v.co *= 0.0
            obj.scale = (0.0, 0.0, 0.0)
            obj.hide_render = True
            obj.hide_viewport = True

    bpy.context.view_layer.update()

def bind_hands_and_fingers_safely(main_rig):
    print("[*] Binding and skinning all hand palms, fingers, and knuckles with applied rest matrices...", flush=True)
    if not (main_rig and main_rig.data):
        return

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()

    def find_bone(candidates, fallback):
        for c in candidates:
            if c in main_rig.data.bones:
                return c
        for b in main_rig.data.bones:
            for c in candidates:
                if b.name.lower() == c.lower():
                    return b.name
        return fallback

    hand_l_bone = find_bone(["DEF-Hand.L", "DEF-hand.L", "DEF-Wrist.L", "Hand.L", "DEF-Forearm.L"], "DEF-Hand.L")
    hand_r_bone = find_bone(["DEF-Hand.R", "DEF-hand.R", "DEF-Wrist.R", "Hand.R", "DEF-Forearm.R"], "DEF-Hand.R")

    if hand_l_bone in main_rig.data.bones:
        main_rig.data.bones[hand_l_bone].use_deform = True
    if hand_r_bone in main_rig.data.bones:
        main_rig.data.bones[hand_r_bone].use_deform = True

    hand_objects = []

    for obj in list(bpy.data.objects):
        if not (obj and getattr(obj, "type", None) == 'MESH' and not obj.hide_render):
            continue

        name_lower = obj.name.lower()
        col_names = " ".join([c.name.lower() for c in obj.users_collection])
        parent_name = obj.parent.name.lower() if obj.parent else ""

        is_in_hand_col = "hand.l" in col_names or "hand.r" in col_names or "hands" in col_names
        is_hand_mesh = any(k in name_lower for k in ["plane.037", "plane.128"]) or is_hand_or_finger(obj)
        is_hand_parent = any(k in parent_name for k in ["hand.l", "hand.r", "palm.l", "palm.r"])

        if is_in_hand_col or is_hand_mesh or is_hand_parent:
            if any(k in name_lower for k in ["claw", "blade", "smg", "cylinder.040", "cylinder.037", "tail", "nurbspath"]):
                continue

            target_bone = None
            if obj.parent == main_rig and obj.parent_type == 'BONE' and obj.parent_bone:
                if obj.parent_bone in main_rig.data.bones:
                    target_bone = obj.parent_bone

            if not target_bone:
                if "hand.r" in col_names or ".r" in name_lower or ".r" in parent_name or name_lower == "plane.037":
                    target_bone = hand_r_bone
                else:
                    target_bone = hand_l_bone

            hand_objects.append((obj, target_bone))

    print(f"  [*] Processing {len(hand_objects)} hand and finger meshes...", flush=True)

    for obj, target_bone in hand_objects:
        obj.hide_render = False
        obj.hide_viewport = False

        world_mat = obj.matrix_world.copy()

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

        for c in list(obj.constraints):
            obj.constraints.remove(c)

        obj.parent = None
        obj.matrix_world = world_mat
        bpy.context.view_layer.update()

        deselect_all_objects()
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        try:
            bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        except Exception as e:
            print(f"  [~] Notice applying transform on '{obj.name}': {e}", flush=True)

        obj.parent = main_rig
        obj.parent_type = 'OBJECT'
        obj.matrix_world = main_rig.matrix_world

        arm_mod = next((m for m in obj.modifiers if m.type == 'ARMATURE'), None)
        if not arm_mod:
            arm_mod = obj.modifiers.new(name="Armature", type='ARMATURE')
        arm_mod.object = main_rig

        if target_bone in main_rig.data.bones:
            main_rig.data.bones[target_bone].use_deform = True

        obj.vertex_groups.clear()
        vg = obj.vertex_groups.new(name=target_bone)
        if obj.data and hasattr(obj.data, "vertices") and len(obj.data.vertices) > 0:
            all_indices = list(range(len(obj.data.vertices)))
            vg.add(all_indices, 1.0, 'REPLACE')

        obj.hide_render = False
        obj.hide_viewport = False

    main_rig.data.pose_position = 'REST'
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
    facial_rig = bpy.data.objects.get("Facial_Rig") or bpy.data.objects.get("Eye_rig")
    if facial_rig:
        facial_rig.hide_render = True

    for obj in list(bpy.data.objects):
        if not obj:
            continue
        if should_hide_facial_element(obj.name):
            obj.hide_render = True

    bpy.context.view_layer.update()

def bake_clothing_modifiers_with_shapekeys():
    print("[*] Baking Subdivision, Mirror, Shrinkwrap, Solidify, and Displace modifiers into clothing, chestplate, and visor...", flush=True)
    for arm in [o for o in bpy.data.objects if o and o.type == 'ARMATURE']:
        if arm.data:
            arm.data.pose_position = 'REST'
    bpy.context.view_layer.update()

    TARGET_MOD_TYPES = {'MIRROR', 'SUBSURF', 'SOLIDIFY', 'SHRINKWRAP', 'DISPLACE', 'CORRECTIVE_SMOOTH', 'BEVEL', 'MASK'}

    CLOTHING_PATTERNS = [
        "cylinder.001", "cylinder.002", "jacket", "coat", "clothes", "shirt", 
        "dd_symbol", "wd_symbol", "symbol", "circle.060", "chest.001",
        "plane.130", "plane.197", "plane.055", "chestplate", "shield",
        "visorext", "visorint", "visor", "screen", "n_coat", "waist", "pelvis"
    ]

    for obj in list(bpy.data.objects):
        if not (obj and getattr(obj, "type", None) == 'MESH' and not obj.hide_render):
            continue

        name_lower = obj.name.lower()
        if "armband" in name_lower or (name_lower.startswith("cylinder.") and name_lower not in ["cylinder.001", "cylinder.002"]):
            continue

        has_mirror = any(m.type == 'MIRROR' for m in obj.modifiers)
        is_clothing_match = any(k in name_lower for k in CLOTHING_PATTERNS)

        if not (is_clothing_match or has_mirror):
            continue

        target_mods = [m for m in obj.modifiers if m.type in TARGET_MOD_TYPES]
        if not target_mods:
            continue

        optimize_subsurf_modifiers(obj, max_level=1)

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
                        if m.type in TARGET_MOD_TYPES:
                            obj.modifiers.remove(m)

                    if obj.data.shape_keys:
                        for k_name, val in orig_values.items():
                            if k_name in obj.data.shape_keys.key_blocks:
                                obj.data.shape_keys.key_blocks[k_name].value = val
                    print(f"  [+] Baked modifiers (including Mirror/Subsurf) into shape keys for: '{obj.name}'", flush=True)

            else:
                depsgraph = bpy.context.evaluated_depsgraph_get()
                obj_eval = obj.evaluated_get(depsgraph)
                new_mesh = bpy.data.meshes.new_from_object(obj_eval, preserve_all_data_layers=True, depsgraph=depsgraph)
                old_mesh = obj.data
                obj.data = new_mesh
                if old_mesh and old_mesh.users == 0:
                    bpy.data.meshes.remove(old_mesh)
                for m in list(obj.modifiers):
                    if m.type in TARGET_MOD_TYPES:
                        obj.modifiers.remove(m)
                print(f"  [+] Baked modifiers (including Mirror/Subsurf) for: '{obj.name}'", flush=True)

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

def skin_limbs_and_boots_safely(main_rig):
    print("[*] Preserving limb skinning and boot hierarchies safely...", flush=True)
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

    EXCLUDED_SOLE_PATTERNS = [
        "cylinder.022", "cylinder.024", "cylinder.025", "cylinder.026", 
        "cylinder.030", "cylinder.035", "cylinder.037", "cylinder.038", 
        "cylinder.039", "cylinder.040", "cylinder.041", "cylinder.043", 
        "cylinder.063", "sphere.014", "sphere.015", "circle.", "plane.", "tip", "tail", "nurbspath.002", "roundcube."
    ]

    for obj in bpy.data.objects:
        if obj and getattr(obj, "type", None) == 'MESH' and not obj.hide_render:
            name_lower = obj.name.lower()
            if any(p in name_lower for p in EXCLUDED_SOLE_PATTERNS):
                continue
            if is_limb_object(obj) or any(k in name_lower for k in [
                "cylinder.004", "cylinder.005", "cylinder.006", "cylinder.019", "cylinder.020",
                "cylinder.021", "cylinder.029", "cylinder.042", "cylinder.044", "cylinder.046"
            ]):
                if obj not in limb_objects:
                    limb_objects.append(obj)

    for obj in limb_objects:
        has_armature = any(m.type == 'ARMATURE' for m in obj.modifiers)
        if has_armature and obj.vertex_groups:
            obj.hide_render = False
            obj.hide_viewport = False
            continue

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

    main_rig.data.pose_position = 'REST'
    bpy.context.view_layer.update()

def fix_head_hair_and_accessories_parenting(main_rig):
    print("[*] Re-parenting active head, beanie, and accessories directly to 'DEF-Head'...", flush=True)
    if not main_rig or not main_rig.data:
        return
    target_bone = "DEF-Head" if "DEF-Head" in main_rig.data.bones else "Head"
    RIGID_HEAD_NAMES = [
        "HeadTop", "VisorExt", "VisorInt", "AUX_SCREEN", "Sphere.037",
        "Cylinder.017", "Cylinder.030", "Cylinder.038", "Headband", "Beanie", "Hat", "Cap", "Pilot"
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
        "Mouthless", "Hide_Mouth", "ANC-Teeth", "NurbsPath.002"
    ]

    for obj in list(bpy.data.objects):
        if not obj:
            continue

        if is_fur_object(obj):
            obj.hide_render = False
            obj.hide_viewport = False
            continue

        name_lower = obj.name.lower()
        if name_lower == "nurbspath.002":
            obj.hide_render = True
            obj.hide_viewport = True
            continue

        if "nurbspath" in name_lower:
            is_head_hair = any(c.name.lower() in ["hair", "head", "head accessories"] for c in obj.users_collection) or \
                           any(s.material and "hair" in s.material.name.lower() for s in obj.material_slots) or \
                           "hair" in name_lower
            is_tail = "tail" in name_lower or "nurbspath.026" in name_lower or any("tail" in c.name.lower() for c in obj.users_collection)
            if not (is_head_hair or is_tail):
                obj.hide_render = True
                continue

        if should_hide_facial_element(obj.name) or any(pat.lower() in name_lower for pat in HIDE_PATTERNS):
            obj.hide_render = True
            continue

        if obj.type == 'CURVE':
            is_head_hair = any("hair" in c.name.lower() for c in obj.users_collection) or \
                           any(s.material and "hair" in s.material.name.lower() for s in obj.material_slots) or \
                           "hair" in name_lower
            is_tail = "tail" in name_lower or "nurbspath.026" in name_lower or any("tail" in c.name.lower() for c in obj.users_collection)
            if not (is_head_hair or is_tail):
                obj.hide_render = True
                continue

    bpy.context.view_layer.update()

def final_facial_and_widget_cleanup_pass():
    print("[*] Enforcing hide state for floating 3D facial features & GUI elements before glTF write...", flush=True)
    for obj in list(bpy.data.objects):
        if not obj:
            continue

        if is_fur_object(obj):
            obj.hide_render = False
            obj.hide_viewport = False
            continue

        if obj.name.lower() == "nurbspath.002":
            obj.hide_render = True
            obj.hide_viewport = True
            continue
        if should_hide_facial_element(obj.name) or any(k in obj.name.lower() for k in ["x.l", "x.r", "full_x"]):
            obj.hide_render = True
            obj.hide_viewport = True

    bpy.context.view_layer.update()

def get_action_frame_range(action):
    try:
        if hasattr(action, "frame_range"):
            fr = action.frame_range
            return int(round(fr[0])), int(round(fr[1]))
    except Exception:
        pass

    try:
        if hasattr(action, "layers"):
            frames = []
            for layer in action.layers:
                for strip in getattr(layer, "strips", []):
                    for cb in getattr(strip, "channelbags", []):
                        for fc in getattr(cb, "fcurves", []):
                            for kp in fc.keyframe_points:
                                frames.append(kp.co[0])
            if frames:
                return int(round(min(frames))), int(round(max(frames)))
    except Exception:
        pass

    try:
        if hasattr(action, "fcurves") and action.fcurves:
            frames = [kp.co[0] for fc in action.fcurves for kp in fc.keyframe_points]
            if frames:
                return int(round(min(frames))), int(round(max(frames)))
    except Exception:
        pass

    return 1, 24

def bind_action_and_slot(rig, action):
    if not (rig and rig.animation_data and action):
        return

    rig.animation_data.action = action

    try:
        if hasattr(rig.animation_data, "action_suitable_slots") and len(rig.animation_data.action_suitable_slots) > 0:
            rig.animation_data.action_slot = rig.animation_data.action_suitable_slots[0]
        elif hasattr(action, "slots") and len(action.slots) > 0:
            rig.animation_data.action_slot = action.slots[0]
    except Exception as e:
        print(f"  [~] Notice binding slot for '{action.name}': {e}", flush=True)

def bake_all_character_animations(main_rig, target_action_names=None):
    print("[*] Starting multi-animation visual baking pass...", flush=True)
    if not main_rig or not main_rig.data:
        return

    if hasattr(main_rig.data, "collections"):
        for bcoll in main_rig.data.collections:
            bcoll.is_visible = True
    if hasattr(main_rig.data, "layers"):
        main_rig.data.layers = [True] * len(main_rig.data.layers)
    for b in main_rig.data.bones:
        b.hide = False
    for pb in main_rig.pose.bones:
        pb.bone.hide = False

    deselect_all_objects()
    main_rig.select_set(True)
    bpy.context.view_layer.objects.active = main_rig
    bpy.ops.object.mode_set(mode='POSE')
    main_rig.data.pose_position = 'POSE'

    if not main_rig.animation_data:
        main_rig.animation_data_create()

    for t in list(main_rig.animation_data.nla_tracks):
        main_rig.animation_data.nla_tracks.remove(t)
    main_rig.animation_data.use_nla = False

    for obj in bpy.data.objects:
        if obj != main_rig and obj.animation_data:
            obj.animation_data_clear()

    raw_actions = []
    if target_action_names:
        for name in target_action_names:
            act = bpy.data.actions.get(name)
            if act and act not in raw_actions:
                raw_actions.append(act)
    else:
        for act in list(bpy.data.actions):
            name_lower = act.name.lower()
            if any(name_lower.startswith(p) for p in ["sd_n_", "sd-n_", "n_", "f sd_n_", "f sd-n_", "f n_"]):
                raw_actions.append(act)
            elif any(k in name_lower for k in ["idle", "walk", "run", "fly", "jump", "attack", "salute"]):
                if not act.name.startswith("__"):
                    raw_actions.append(act)

    if not raw_actions:
        print("[-] Warning: No matching actions found to bake!", flush=True)
        return

    print(f"[*] Found {len(raw_actions)} action(s) to bake: {[a.name for a in raw_actions]}", flush=True)

    baked_actions = []

    for src_action in raw_actions:
        orig_name = src_action.name.replace("F ", "").strip()
        start_frame, end_frame = get_action_frame_range(src_action)

        print(f"  [*] Baking '{orig_name}' (Frames {start_frame} -> {end_frame})...", flush=True)

        src_action.name = f"__raw_{orig_name}"
        bind_action_and_slot(main_rig, src_action)
        main_rig.animation_data.use_nla = False

        bpy.context.scene.frame_start = start_frame
        bpy.context.scene.frame_end = end_frame
        
        bpy.context.scene.frame_set(start_frame + 1 if start_frame == 0 else start_frame - 1)
        bpy.context.scene.frame_set(start_frame)
        bpy.context.view_layer.update()

        bpy.ops.nla.bake(
            frame_start=start_frame,
            frame_end=end_frame,
            step=1,
            only_selected=False,
            visual_keying=True,
            clear_constraints=False,
            clear_parents=False,
            use_current_action=False,
            clean_curves=False,
            bake_types={'POSE'}
        )

        baked_act = main_rig.animation_data.action
        baked_act.name = orig_name
        baked_act.use_fake_user = True
        baked_actions.append(baked_act)

        for t in list(main_rig.animation_data.nla_tracks):
            main_rig.animation_data.nla_tracks.remove(t)
        main_rig.animation_data.use_nla = False

    for act in list(bpy.data.actions):
        if act not in baked_actions:
            try:
                bpy.data.actions.remove(act, do_unlink=True)
            except Exception:
                pass

    print("[*] Clearing pose constraints on armature to lock baked keyframes...", flush=True)
    for pbone in main_rig.pose.bones:
        for c in list(pbone.constraints):
            pbone.constraints.remove(c)

    for c in list(main_rig.constraints):
        main_rig.constraints.remove(c)

    if baked_actions:
        bind_action_and_slot(main_rig, baked_actions[0])

    bpy.ops.object.mode_set(mode='OBJECT')
    bpy.context.view_layer.update()
    print(f"[+] Successfully baked {len(baked_actions)} pure keyframe animation clip(s).", flush=True)

def run_gltf_export(filepath):
    print("[*] Executing native glTF 2.0 export with baked animation clips...", flush=True)
    main_rig = bpy.data.objects.get("Rig") or bpy.data.objects.get("Rig.001")

    if main_rig and main_rig.data:
        main_rig.data.pose_position = 'POSE'

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

    # 1. Convert hair and fur curves to mesh (NurbsPath.002 & NurbsPath.026 are explicitly skipped)
    convert_hair_curves_to_mesh()

    # 2. Setup Bone Hierarchy (Root, Shoulders, Hair Chains systematically renamed to DEF-Hair_SXX_YY)
    setup_root_bone(main_rig)
    setup_shoulder_bones(main_rig)
    merge_and_setup_hair_rig(main_rig)
    bind_hair_meshes_to_rig(main_rig)

    # 3. Convert & Rig Tail (Cable with 13-point CTR-Tail Skinning, NurbsPath.026 converted to mesh)
    convert_and_rig_tail(main_rig)

    # 4. Enable Deform Flags on all Bone Chains
    enable_all_deform_bones(main_rig)

    apply_facial_gui_visibility_and_hide_anchors(main_rig)

    # 5. Attachments & SD-N Accessories (Coat Button, Tail Syringe, Arm Internals, Foot Soles, Headband Lights)
    fix_and_bind_sd_n_accessories(main_rig)
    bake_and_attach_teeth(main_rig)

    # 6. Rigid Accessories (Visor, Beanie/Hat, Headband Lights)
    fix_head_hair_and_accessories_parenting(main_rig)
    fix_and_bake_mouth_shrink(main_rig)

    # 7. Clothing, Particles & Limbs (Bakes Mirror, Solidify, Subsurf on Plane.130, DD_Symbol.001, Circle.060, Plane.055, etc.)
    bake_clothing_modifiers_with_shapekeys()
    convert_particle_systems_to_real_mesh(main_rig)
    bake_limb_modifiers()

    # 8. Procedural Materials Bake Pass
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

    # 9. Final Armature Binding, Hands/Fingers Skinning, Weapon Scaling & Eye Bake
    skin_limbs_and_boots_safely(main_rig)
    bind_hands_and_fingers_safely(main_rig)
    bake_eyes_onto_visor(main_rig)
    disable_and_scale_down_weapons(main_rig)
    hide_non_character_widgets_and_symbols()
    final_facial_and_widget_cleanup_pass()

    # 10. Visually Bake Pose Animations
    bake_all_character_animations(main_rig)

    # 11. Run glTF Exporter
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


export_n_to_glb = export_sd_n_to_glb

if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if not export_sd_n_to_glb(args.input, args.output):
        sys.exit(1)
