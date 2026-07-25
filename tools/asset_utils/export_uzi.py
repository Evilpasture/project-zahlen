# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# tools/asset_utils/export_uzi.py
import subprocess
import os
import sys
import json


def export_uzi_to_glb(blend_path: str, glb_path: str) -> bool:
    """Exports Uzi .blend to .glb natively with Blender 5.x Armature ActionSlot bindings."""
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

def patch_blender_52_gltf_cache_bug():
    # Patches Blender 5.2 LTS io_scene_gltf2 KeyError: None / AttributeError in sampling_cache.py
    try:
        import io_scene_gltf2.blender.exp.animation.sampled.sampling_cache as sc
        from io_scene_gltf2.blender.exp.tree import VExportNode
        import mathutils

        orig_object_caching = sc.object_caching

        def safe_object_caching(*args, **kwargs):
            export_settings = args[-1] if args else kwargs.get("export_settings")
            if export_settings and "vtree" in export_settings:
                vtree = export_settings["vtree"]
                main_rig = bpy.data.objects.get("Rig") or bpy.data.objects.get("Rig.001")
                
                if hasattr(vtree, "nodes") and None not in vtree.nodes and main_rig:
                    class IdentityBone:
                        def __init__(self):
                            self.matrix = mathutils.Matrix.Identity(4)

                    dummy = VExportNode()
                    dummy.keep_tag = False
                    dummy.blender_type = None
                    dummy.blender_bone = IdentityBone()
                    dummy.parent_bone_uuid = None
                    dummy.blender_object = main_rig
                    dummy.skin = None
                    vtree.nodes[None] = dummy

            return orig_object_caching(*args, **kwargs)

        sc.object_caching = safe_object_caching
        print("[+] Patched Blender 5.2 glTF exporter root bone sampling cache.", flush=True)
    except Exception as e:
        print(f"[~] Notice while patching glTF exporter: {e}", flush=True)

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

def convert_emission_shaders_to_pbr():
    print("[*] Converting Emission shader nodes to glTF-compatible PBR materials...", flush=True)

    for mat in bpy.data.materials:
        if not (mat and mat.use_nodes and mat.node_tree):
            continue

        output_node = None
        for node in mat.node_tree.nodes:
            if node.type == 'OUTPUT_MATERIAL' and node.is_active_output:
                output_node = node
                break

        if not output_node:
            continue

        surface_input = output_node.inputs.get("Surface")
        if not (surface_input and surface_input.is_linked):
            continue

        connected_node = surface_input.links[0].from_node

        if connected_node.type == 'EMISSION':
            emission_node = connected_node

            color_val = [1.0, 1.0, 1.0, 1.0]
            if "Color" in emission_node.inputs and not emission_node.inputs["Color"].is_linked:
                c = emission_node.inputs["Color"].default_value
                color_val = [c[0], c[1], c[2], c[3] if len(c) > 3 else 1.0]

            strength_val = 1.0
            if "Strength" in emission_node.inputs and not emission_node.inputs["Strength"].is_linked:
                strength_val = emission_node.inputs["Strength"].default_value

            eff_r = color_val[0] * strength_val
            eff_g = color_val[1] * strength_val
            eff_b = color_val[2] * strength_val

            bsdf_node = None
            for node in mat.node_tree.nodes:
                if node.type == 'BSDF_PRINCIPLED':
                    bsdf_node = node
                    break

            if not bsdf_node:
                bsdf_node = mat.node_tree.nodes.new("ShaderNodeBsdfPrincipled")
                bsdf_node.location = (emission_node.location.x, emission_node.location.y)

            bsdf_node.inputs["Base Color"].default_value = (eff_r, eff_g, eff_b, color_val[3])
            bsdf_node.inputs["Roughness"].default_value = 0.15

            if strength_val > 0.0:
                if "Emission Color" in bsdf_node.inputs:
                    bsdf_node.inputs["Emission Color"].default_value = (color_val[0], color_val[1], color_val[2], 1.0)
                elif "Emission" in bsdf_node.inputs:
                    bsdf_node.inputs["Emission"].default_value = (color_val[0], color_val[1], color_val[2], 1.0)

                if "Emission Strength" in bsdf_node.inputs:
                    bsdf_node.inputs["Emission Strength"].default_value = strength_val
            else:
                if "Emission Color" in bsdf_node.inputs:
                    bsdf_node.inputs["Emission Color"].default_value = (0.0, 0.0, 0.0, 1.0)
                elif "Emission" in bsdf_node.inputs:
                    bsdf_node.inputs["Emission"].default_value = (0.0, 0.0, 0.0, 1.0)

                if "Emission Strength" in bsdf_node.inputs:
                    bsdf_node.inputs["Emission Strength"].default_value = 0.0

            mat.node_tree.links.new(bsdf_node.outputs["BSDF"], surface_input)

    bpy.context.view_layer.update()

def convert_hair_curves_to_mesh():
    print("[*] Converting renderable hair curves to mesh...", flush=True)
    if bpy.ops.object.mode_set.poll():
        bpy.ops.object.mode_set(mode='OBJECT')

    for obj in list(bpy.data.objects):
        if obj.type in {'CURVE', 'SURFACE', 'FONT'} and not obj.hide_render:
            bpy.ops.object.select_all(action='DESELECT')
            obj.select_set(True)
            bpy.context.view_layer.objects.active = obj
            try:
                bpy.ops.object.convert(target='MESH')
                print(f"  [+] Converted curve to MESH: '{obj.name}'", flush=True)
            except Exception as e:
                print(f"  [-] Skipped '{obj.name}': {e}", flush=True)

    bpy.context.view_layer.update()

def align_facial_mesh_parenting():
    print("[*] Aligning facial mesh parenting with Mouth_Shrink screen...", flush=True)
    mouth_shrink = bpy.data.objects.get("Mouth_Shrink")
    target_parent = mouth_shrink.parent if mouth_shrink else None

    FACIAL_MESHES = ["Teeth", "Tongue", "Mouth", "Lip"]

    for obj in list(bpy.data.objects):
        if obj.type == 'MESH' and any(k in obj.name for k in FACIAL_MESHES):
            if target_parent and obj.parent != target_parent:
                w_mat = obj.matrix_world.copy()
                obj.parent = target_parent
                obj.parent_type = 'OBJECT'
                obj.parent_bone = ""
                obj.matrix_world = w_mat
                print(f"  [+] Re-parented '{obj.name}' to '{target_parent.name}' to lock onto Mouth_Shrink.", flush=True)

    bpy.context.view_layer.update()

def bake_facial_shrinkwrap_only():
    print("[*] Baking facial Shrinkwrap modifiers while keeping body skinning intact...", flush=True)

    for arm in [o for o in bpy.data.objects if o.type == 'ARMATURE']:
        if arm.data:
            arm.data.pose_position = 'POSE'

    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()

    FACIAL_SHRINKWRAP_NAMES = ["Teeth", "Tongue", "Mouth", "Lip", "Eyelid", "Eyebrow"]

    for obj in list(bpy.data.objects):
        if not (obj.type == 'MESH' and not obj.hide_render):
            continue

        sw_mods = [m for m in obj.modifiers if m.type == 'SHRINKWRAP']
        if not (sw_mods and any(k in obj.name for k in FACIAL_SHRINKWRAP_NAMES)):
            continue

        print(f"  [*] Processing facial shrinkwrap on: '{obj.name}'...", flush=True)

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
                mesh_eval = bpy.data.meshes.new_from_object(
                    obj_eval, preserve_all_data_layers=True, depsgraph=depsgraph
                )

                tmp_obj = bpy.data.objects.new(f"__tmp_sk_{k_name}", mesh_eval)
                bpy.context.collection.objects.link(tmp_obj)
                eval_objs.append((k_name, tmp_obj))

                if k is not None:
                    k.value = 0.0

            if eval_objs:
                basis_name, base_tmp_obj = eval_objs[0]

                bpy.ops.object.select_all(action='DESELECT')
                base_tmp_obj.select_set(True)
                bpy.context.view_layer.objects.active = base_tmp_obj

                for k_name, tmp_obj in eval_objs:
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

                for k_name, tmp_obj in eval_objs:
                    bpy.data.objects.remove(tmp_obj, do_unlink=True)

                if old_mesh.users == 0:
                    bpy.data.meshes.remove(old_mesh)

                for m in list(obj.modifiers):
                    if m.type == 'SHRINKWRAP':
                        obj.modifiers.remove(m)

                if obj.data.shape_keys:
                    for k_name, val in orig_values.items():
                        if k_name in obj.data.shape_keys.key_blocks:
                            obj.data.shape_keys.key_blocks[k_name].value = val

                print(f"  [+] Baked Shrinkwrap into shape keys for: '{obj.name}'", flush=True)

    bpy.context.view_layer.update()

def is_armature_action(action, main_rig):
    # Dynamically checks if an action targets pose bones or is stashed on the armature.
    if not action:
        return False

    if main_rig and main_rig.animation_data:
        for track in main_rig.animation_data.nla_tracks:
            for strip in track.strips:
                if strip.action == action:
                    return True

    if hasattr(action, "fcurves"):
        for fc in action.fcurves:
            if fc.data_path.startswith("pose.bones"):
                return True

    if hasattr(action, "layers"):
        for layer in action.layers:
            for strip in layer.strips:
                for slot in getattr(action, "slots", []):
                    cb = strip.channelbag(slot) if hasattr(strip, "channelbag") else None
                    if cb and hasattr(cb, "fcurves"):
                        for fc in cb.fcurves:
                            if fc.data_path.startswith("pose.bones"):
                                return True

    return False

def purge_non_armature_animation_data(main_rig):
    # Clears animation data from non-armature objects and purges orphan object actions.
    print("[*] Dynamically purging non-armature animation data...", flush=True)

    for obj in list(bpy.data.objects):
        if obj != main_rig and obj.type != 'ARMATURE':
            if obj.animation_data:
                obj.animation_data_clear()

            if hasattr(obj, "data") and obj.data and hasattr(obj.data, "animation_data") and obj.data.animation_data:
                obj.data.animation_data_clear()

            if hasattr(obj, "data") and obj.data and hasattr(obj.data, "shape_keys") and obj.data.shape_keys:
                if obj.data.shape_keys.animation_data:
                    obj.data.shape_keys.animation_data_clear()

            if hasattr(obj, "material_slots"):
                for slot in obj.material_slots:
                    if slot.material:
                        if slot.material.animation_data:
                            slot.material.animation_data_clear()
                        if slot.material.node_tree and slot.material.node_tree.animation_data:
                            slot.material.node_tree.animation_data_clear()

    for action in list(bpy.data.actions):
        if not is_armature_action(action, main_rig):
            print(f"  [-] Purged non-armature action: '{action.name}'", flush=True)
            try:
                bpy.data.actions.remove(action, do_unlink=True)
            except Exception as e:
                print(f"    [~] Notice removing action '{action.name}': {e}", flush=True)

    bpy.context.view_layer.update()

def deep_fcurve_pruning(main_rig):
    print("[*] Performing deep F-curve pruning to keep only bone keyframe tracks...", flush=True)
    if not main_rig:
        return

    for action in list(bpy.data.actions):
        if hasattr(action, "layers"):
            for layer in action.layers:
                for strip in layer.strips:
                    for slot in action.slots:
                        cb = strip.channelbag(slot) if hasattr(strip, "channelbag") else None
                        if cb and hasattr(cb, "fcurves"):
                            for fc in list(cb.fcurves):
                                if not any(fc.data_path.startswith(prefix) for prefix in ["pose.bones", "location", "rotation", "scale"]):
                                    cb.fcurves.remove(fc)
        elif hasattr(action, "fcurves"):
            for fc in list(action.fcurves):
                if not any(fc.data_path.startswith(prefix) for prefix in ["pose.bones", "location", "rotation", "scale"]):
                    action.fcurves.remove(fc)

    bpy.context.view_layer.update()

def stash_and_slot_character_actions(main_rig):
    print("[*] Stashing character actions into muted NLA tracks with Armature Action Slots...", flush=True)

    if not main_rig or not main_rig.data:
        return

    bpy.ops.object.select_all(action='DESELECT')
    main_rig.select_set(True)
    bpy.context.view_layer.objects.active = main_rig
    if bpy.ops.object.mode_set.poll():
        bpy.ops.object.mode_set(mode='OBJECT')

    if not main_rig.animation_data:
        main_rig.animation_data_create()

    if not main_rig.data.animation_data:
        main_rig.data.animation_data_create()

    for track in list(main_rig.animation_data.nla_tracks):
        try:
            main_rig.animation_data.nla_tracks.remove(track)
        except Exception:
            pass

    character_actions = [a for a in list(bpy.data.actions) if is_armature_action(a, main_rig)]
    print(f"  [+] Stashing {len(character_actions)} armature actions onto main armature:", flush=True)

    for action in character_actions:
        action.use_fake_user = True

        main_rig.animation_data.action = action
        main_rig.data.animation_data.action = action
        bpy.context.view_layer.update()

        slot = None
        if hasattr(action, "slots"):
            for s in action.slots:
                if getattr(s, "id_type", "") == 'ARMATURE' or "AR" in getattr(s, "identifier", ""):
                    slot = s
                    break

            if not slot and hasattr(action.slots, "new"):
                try:
                    slot = action.slots.new(id_type='ARMATURE', name=main_rig.data.name)
                except Exception:
                    try:
                        slot = action.slots.new('ARMATURE', main_rig.data.name)
                    except Exception:
                        pass

            if not slot and len(action.slots) > 0:
                slot = action.slots[0]

        track_name = f"[Stash] {action.name}"
        track = main_rig.animation_data.nla_tracks.new()
        track.name = track_name
        track.mute = True

        start_frame = int(action.frame_range[0])
        end_frame = int(action.frame_range[1])
        if end_frame <= start_frame:
            end_frame = start_frame + 120

        strip = track.strips.new(action.name, start_frame, action)
        strip.action = action

        if hasattr(strip, "action_suitable_slots") and strip.action_suitable_slots:
            if len(strip.action_suitable_slots) > 0:
                try:
                    strip.action_slot = strip.action_suitable_slots[0]
                except Exception:
                    pass

        if slot is not None and not getattr(strip, "action_slot", None):
            if hasattr(strip, "action_slot"):
                try:
                    strip.action_slot = slot
                except Exception:
                    pass

        if hasattr(strip, "channelbag") and getattr(strip, "action_slot", None):
            try:
                strip.channelbag(strip.action_slot, ensure=True)
            except Exception:
                pass

        strip.action_frame_start = start_frame
        strip.action_frame_end = end_frame
        strip.frame_start = start_frame
        strip.frame_end = end_frame

        assigned_slot = getattr(strip, "action_slot", slot)
        slot_repr = getattr(assigned_slot, "identifier", "Active") if assigned_slot else "None"
        print(f"    - Stashed: '{action.name}' -> NLA Track: '{track_name}' (Slot: {slot_repr})", flush=True)

    for track in main_rig.animation_data.nla_tracks:
        track.mute = True

    main_rig.animation_data.action = None
    if character_actions:
        main_rig.animation_data.action = character_actions[0]

    bpy.context.view_layer.update()

def run_gltf_export(filepath):
    print("[*] Executing native glTF 2.0 export in ACTIONS mode...", flush=True)
    main_rig = bpy.data.objects.get("Rig") or bpy.data.objects.get("Rig.001")
    
    if main_rig:
        bpy.ops.object.select_all(action='DESELECT')
        main_rig.select_set(True)
        bpy.context.view_layer.objects.active = main_rig

    for action in list(bpy.data.actions):
        if not is_armature_action(action, main_rig):
            try:
                bpy.data.actions.remove(action, do_unlink=True)
            except Exception:
                pass

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
        export_bake_animation=False,
        use_renderable=True,
        export_cameras=False,
        export_lights=False,
    )
    print("[+] GLB export complete.", flush=True)

def main():
    for img in bpy.data.images:
        if img.source == 'FILE' and not img.packed_file:
            try:
                img.pack()
            except Exception as e:
                print(f"[~] Notice packing image '{img.name}': {e}", flush=True)

    setup_environment_and_drivers()
    convert_emission_shaders_to_pbr()
    convert_hair_curves_to_mesh()
    align_facial_mesh_parenting()
    bake_facial_shrinkwrap_only()

    main_rig = bpy.data.objects.get("Rig") or bpy.data.objects.get("Rig.001")
    purge_non_armature_animation_data(main_rig)
    stash_and_slot_character_actions(main_rig)
    deep_fcurve_pruning(main_rig)

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

    cmd = base_cmd + ["-b", temp_blend_path, "--python-expr", expr]

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
