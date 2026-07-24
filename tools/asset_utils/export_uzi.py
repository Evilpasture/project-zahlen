# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# tools/asset_utils/export_uzi.py
import subprocess
import os
import sys
import json


def export_uzi_to_glb(blend_path: str, glb_path: str) -> bool:
    """Exports Uzi .blend to .glb natively with clean character actions and no mesh-level animation trash."""
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
    # Enables Python script execution for rig drivers and un-hides elements for evaluation.
    print("[*] Enabling Python script auto-execution for rig drivers...", flush=True)
    bpy.context.preferences.filepaths.use_scripts_auto_execute = True

    # Execute embedded rig scripts (Toxilisk_DroneUI.py) if present
    for txt in bpy.data.texts:
        if txt.name.endswith(".py") or any(k in txt.name.lower() for k in ["rig", "driver", "ui"]):
            try:
                exec(txt.as_string(), {"__name__": "__main__"})
                print(f"  [+] Executed embedded rig script: '{txt.name}'", flush=True)
            except Exception as e:
                print(f"  [~] Notice running embedded script '{txt.name}': {e}", flush=True)

    # Un-exclude view layer collections so depsgraph can calculate target meshes
    layer_collections = [bpy.context.view_layer.layer_collection]
    while layer_collections:
        l_c = layer_collections.pop(0)
        layer_collections.extend(l_c.children)
        l_c.exclude = False
        l_c.hide_viewport = False

    bpy.context.view_layer.update()

def convert_emission_shaders_to_pbr():
    # Evaluates Emission shader nodes (Color * Strength) and converts them to glTF-compatible Principled BSDF.
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
    # Converts renderable hair curves/NURBS to mesh geometry for glTF export.
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
    # Re-parents Teeth and Tongue directly to Head_Parent so control bone offsets (CTR-Bot_Teeth)
    # do not push the baked shrinkwrapped teeth away from Mouth_Shrink during animation playback.
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
    # Bakes Shrinkwrap modifiers ONLY for facial meshes (Teeth, Tongue, Mouth, Eyelids) in POSE mode.
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

def setup_rig_actions_and_purge_mesh_tracks(main_rig):
    # Clears object-level animation data from mesh/curve objects so glTF exporter does NOT
    # generate individual animation tracks for meshes (Cylinder.007, Circle.076, etc.).
    # Binds all 6 character actions to main_rig with Blender 5.2 Action Slots.
    print("[*] Setting up character actions and purging mesh-level animation tracks...", flush=True)

    if not main_rig:
        return

    # 1. Clear animation_data on all non-armature objects
    for obj in list(bpy.data.objects):
        if obj != main_rig and obj.type != 'ARMATURE':
            if obj.animation_data:
                obj.animation_data_clear()
                print(f"  [-] Cleared mesh animation data from: '{obj.name}'", flush=True)

    # 2. Select main_rig and make it active
    bpy.ops.object.select_all(action='DESELECT')
    main_rig.select_set(True)
    bpy.context.view_layer.objects.active = main_rig
    if bpy.ops.object.mode_set.poll():
        bpy.ops.object.mode_set(mode='OBJECT')

    if not main_rig.animation_data:
        main_rig.animation_data_create()

    # Clear old NLA tracks
    for track in list(main_rig.animation_data.nla_tracks):
        try:
            main_rig.animation_data.nla_tracks.remove(track)
        except Exception:
            pass

    # 3. Filter character actions
    character_actions = [a for a in list(bpy.data.actions) if a.name.startswith("Uzi_") or "Rig" in a.name]
    if not character_actions:
        character_actions = list(bpy.data.actions)

    # Delete any non-character actions from database
    for a in list(bpy.data.actions):
        if a not in character_actions:
            print(f"  [-] Purging non-character action from database: '{a.name}'", flush=True)
            try:
                bpy.data.actions.remove(a, do_unlink=True)
            except Exception:
                pass

    print(f"  [+] Configured {len(character_actions)} character actions for export:", flush=True)

    # 4. Bind Action Slots and stash into muted NLA tracks for Blender 5.2
    for action in character_actions:
        action.use_fake_user = True

        main_rig.animation_data.action = action
        bpy.context.view_layer.update()

        slot = None
        if hasattr(action, "slots"):
            if len(action.slots) == 0 and hasattr(action.slots, "new"):
                try:
                    slot = action.slots.new(id_type='OBJECT', name=main_rig.name)
                except Exception:
                    pass
            if not slot and len(action.slots) > 0:
                slot = action.slots[0]

        if not slot and hasattr(main_rig.animation_data, "action_slot"):
            slot = main_rig.animation_data.action_slot

        track_name = action.name
        track = main_rig.animation_data.nla_tracks.new()
        track.name = track_name
        track.mute = True  # Muted to prevent T-pose locks

        start_frame = int(action.frame_range[0])
        end_frame = int(action.frame_range[1])
        if end_frame <= start_frame:
            end_frame = start_frame + 120

        strip = track.strips.new(action.name, start_frame, action)
        strip.action = action

        if slot is not None:
            if hasattr(strip, "action_slot"):
                try:
                    strip.action_slot = slot
                except Exception:
                    pass
            elif hasattr(strip, "slot"):
                try:
                    strip.slot = slot
                except Exception:
                    pass

        if not getattr(strip, "action_slot", None) and hasattr(strip, "action_suitable_slots") and strip.action_suitable_slots:
            if len(strip.action_suitable_slots) > 0:
                try:
                    strip.action_slot = strip.action_suitable_slots[0]
                except Exception:
                    pass

        strip.action_frame_start = start_frame
        strip.action_frame_end = end_frame
        strip.frame_start = start_frame
        strip.frame_end = end_frame

        print(f"    - '{action.name}' (Frames {start_frame}-{end_frame})", flush=True)

    for track in main_rig.animation_data.nla_tracks:
        track.mute = True

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

    # Apply safe root bone sampling patch for Blender 5.2
    patch_blender_52_gltf_cache_bug()

    bpy.ops.export_scene.gltf(
        filepath=filepath,
        export_format='GLB',
        export_skins=True,
        export_morph=True,
        export_tangents=False,
        export_normals=True,
        export_apply=False,                 # Preserves skeleton skin weights & bone parenting
        export_animations=True,
        export_animation_mode='ACTIONS',    # Exports character armature actions cleanly
        export_bake_animation=True,
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
            except Exception:
                pass

    # 1. Setup environment and drivers
    setup_environment_and_drivers()

    # 2. Convert Emission nodes (White * 0.0 = Black) to PBR materials
    convert_emission_shaders_to_pbr()

    # 3. Convert hair curves to mesh
    convert_hair_curves_to_mesh()

    # 4. Align facial mesh parenting with Mouth_Shrink screen
    align_facial_mesh_parenting()

    # 5. Bake facial shrinkwrap only in POSE mode
    bake_facial_shrinkwrap_only()

    # 6. Configure character actions on main_rig and clear mesh-level animation data
    main_rig = bpy.data.objects.get("Rig") or bpy.data.objects.get("Rig.001")
    setup_rig_actions_and_purge_mesh_tracks(main_rig)

    # 7. Native glTF Export in ACTIONS mode
    run_gltf_export(__GLB_PATH__)

if __name__ == "__main__":
    main()
"""

    # Format output GLB path using json.dumps to avoid string escape syntax errors
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
            except Exception:
                pass

    return export_success


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if not export_uzi_to_glb(args.input, args.output):
        sys.exit(1)
