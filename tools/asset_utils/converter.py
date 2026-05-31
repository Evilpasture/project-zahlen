# tools/asset_utils/converter.py
import subprocess
import os


def export_blend_to_glb(blend_path: str, glb_path: str) -> bool:
    """Runs Blender in background mode to prepare and export a pristine .glb."""
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

# ---------------------------------------------------------------------------
# CORE ASSET VISIBILITY REGISTER
# ---------------------------------------------------------------------------
CORE_CHARACTER_MESHES = {
    "pomni_eyes", "pomni_pupils", "pomni_eyelidsL", "pomni_eyelidsR", 
    "pomni_eyelidsTOP", "pomni_head.002", "pomni_body", "pomni_arms", 
    "pomni_neck", "pomni_collar", "pomni_eyebrows", "pomni_eyelashes", 
    "pomni_hairFront", "pomni_hairSide", "pomni_hat", "pomni_hatBorder", 
    "pomni_hip", "pomni_teethbot", "pomni_teethtop", "pomni_tongue",
    "pomni_mouth", "pomni_gloveballs", "pomni_balls", "pomni_ballsbody",
    "pomni_ballshat"
}

# ---------------------------------------------------------------------------
# 0. LOW-LEVEL PACK ASSETS (Bypasses bpy.ops.file.pack_all)
# ---------------------------------------------------------------------------
print("[*] Packing image assets using low-level bpy.data loops...")
for img in bpy.data.images:
    if img.source == 'FILE' and not img.packed_file:
        try:
            img.pack()
        except Exception as e:
            print(f"  [-] Failed to pack image '{img.name}': {e}")

# ---------------------------------------------------------------------------
# 1. SETUP SKELETON REFERENCE
# ---------------------------------------------------------------------------
rigs = [o for o in bpy.data.objects if o.type == 'ARMATURE']

# Prioritize the main game rig based on name keywords and bone count
def get_rig_priority(obj):
    name_lower = obj.name.lower()
    score = 0
    if 'pomni' in name_lower:
        score += 100
    if 'rig' in name_lower and 'eye' not in name_lower:
        score += 50
    score += len(obj.data.bones)
    return score

rigs.sort(key=get_rig_priority, reverse=True)
rig = rigs[0] if rigs else None

if rig:
    print(f"[*] Main rig selected: {rig.name}")
    
    # Reset pose bone control properties
    for bone in rig.pose.bones:
        if 'menu' in bone.name.lower():
            for prop in ['Sunglasses', 'Possessed Toggle', 'Outfit']:
                if prop in bone:
                    bone[prop] = 0.0

    # Purge existing NLA tracks to prevent duplicates
    print("[*] Purging all existing NLA tracks from Armature to prevent duplicates...")
    if rig.animation_data:
        for track in list(rig.animation_data.nla_tracks):
            rig.animation_data.nla_tracks.remove(track)

# ---------------------------------------------------------------------------
# 1.1 PURGE COMPETITIVE ARMATURES (Rigify Metarigs/Helpers)
#     Deleting competing armatures ensures that the glTF exporter maps 
#     actions exclusively to POMNI_rig.
# ---------------------------------------------------------------------------
print("[*] Purging competing metarigs and template armatures...")
for obj in list(bpy.data.objects):
    if obj.type == 'ARMATURE' and obj != rig:
        print(f"  [-] Removing non-essential armature: {obj.name}")
        bpy.data.objects.remove(obj, do_unlink=True)

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 1.2 AUTOMATICALLY STASH ALL BONE ACTIONS TO SKELETON NLA
#     This links the actions to the skeleton for the exporter. 
#     We force mute them so they do not blend or corrupt keyframe values.
# ---------------------------------------------------------------------------
print("[*] Stashing bone actions into NLA tracks...")
if rig:
    if not rig.animation_data:
        rig.animation_data_create()
        
    for action in list(bpy.data.actions):
        is_bone_anim = False
        
        # Legacy Blender (4.3 and older)
        if hasattr(action, "fcurves"):
            is_bone_anim = any(fc.data_path.startswith("pose.bones") for fc in action.fcurves)
            
        # Modern Blender 5.x+ (Slotted Actions)
        elif hasattr(action, "layers"):
            try:
                for layer in action.layers:
                    for strip in layer.strips:
                        for cb in strip.channelbags:
                            if cb and hasattr(cb, "fcurves"):
                                if any(fc.data_path.startswith("pose.bones") or "location" in fc.data_path or "rotation" in fc.data_path for fc in cb.fcurves):
                                    is_bone_anim = True
                                    break
                        if is_bone_anim:
                            break
                    if is_bone_anim:
                        break
            except Exception:
                is_bone_anim = True
        else:
            is_bone_anim = True

        if is_bone_anim:
            print(f"  [+] Stashing bone action to NLA: {action.name}")
            track = rig.animation_data.nla_tracks.new()
            track.name = f"[Stash] {action.name}"
            # Mute the track to prevent NLA-stack conflicts during baking/export!
            track.mute = True
            start_frame = int(action.frame_range[0])
            track.strips.new(action.name, start_frame, action)

    # Double check that all NLA tracks are muted
    for track in rig.animation_data.nla_tracks:
        track.mute = True

    rig.animation_data.action = None

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 1.3 SECURE CORE CHARACTER MESH PARENT HIERARCHY
#     Parents core meshes directly to the skeleton while preserving world 
#     matrices, preventing eye, lid, and head drift.
# ---------------------------------------------------------------------------
print("[*] Securing core character mesh parent hierarchy...")
if rig:
    for name in CORE_CHARACTER_MESHES:
        obj = bpy.data.objects.get(name)
        if obj and obj.type == 'MESH':
            # Save current absolute transform, parent to rig, restore transform
            world_matrix = obj.matrix_world.copy()
            obj.parent = rig
            obj.matrix_parent_inverse = rig.matrix_world.inverted()
            obj.matrix_world = world_matrix

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 2. BAKE OBJECT CONSTRAINTS INTO TRANSFORMS
# ---------------------------------------------------------------------------
print("[*] Baking object constraint transforms using low-level database operations...")
depsgraph = bpy.context.evaluated_depsgraph_get()

for obj in bpy.data.objects:
    if obj.type not in ('MESH', 'EMPTY') or not obj.constraints:
        continue
    try:
        evaluated = obj.evaluated_get(depsgraph)
        obj.matrix_world = evaluated.matrix_world.copy()
        obj.constraints.clear()
        print(f"[+] Baked constraints on: {obj.name}")
    except Exception as e:
        print(f"[-] Could not bake constraints on {obj.name}: {e}")

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 3. IDENTIFY SHRINKWRAP TARGETS AND PRUNE AUXILIARY HELPERS
# ---------------------------------------------------------------------------
PRUNE_KEYWORDS = [
    "shrink", "helper", "proxy", "cage", "bounds", "corrective",
    "wgts", "widget", "shadow",
]

shrinkwrap_targets = set()
for obj in bpy.data.objects:
    if obj.type != 'MESH':
        continue
    for mod in obj.modifiers:
        if mod.type == 'SHRINKWRAP' and mod.target is not None:
            shrinkwrap_targets.add(mod.target.name)

helpers_to_clean = set()
for name in shrinkwrap_targets:
    if any(kw in name.lower() for kw in PRUNE_KEYWORDS):
        helpers_to_clean.add(name)

print(f"[*] Shrinkwrap targets: {shrinkwrap_targets}")
print(f"[*] Shrinkwrap targets identified as helper cages: {helpers_to_clean or 'none'}")

print("[*] Pruning unreferenced rigging helpers and shadow meshes...")
for obj in list(bpy.data.objects):
    if obj.type == 'MESH':
        if obj.name in shrinkwrap_targets or obj.name in CORE_CHARACTER_MESHES:
            continue
        name_lower = obj.name.lower()
        if any(kw in name_lower for kw in PRUNE_KEYWORDS):
            print(f"[~] Removing helper mesh: {obj.name}")
            bpy.data.objects.remove(obj, do_unlink=True)

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 3.6 CLAMP COLOR ATTRIBUTE CHANNELS TO ELIMINATE VALIDATION ERRORS
# ---------------------------------------------------------------------------
print("[*] Clamping vertex color attributes to [0.0, 1.0] range...")
for obj in bpy.data.objects:
    if obj.type == 'MESH' and obj.data:
        for attr in obj.data.color_attributes:
            try:
                for d in attr.data:
                    d.color[0] = max(0.0, min(1.0, d.color[0]))
                    d.color[1] = max(0.0, min(1.0, d.color[1]))
                    d.color[2] = max(0.0, min(1.0, d.color[2]))
                    d.color[3] = max(0.0, min(1.0, d.color[3]))
            except Exception:
                pass

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 4. UNIFIED DEFORMATION BAKING (Dependency-Cycle-Proofed)
# ---------------------------------------------------------------------------
print("[*] Performing Unified Deformation Baking...")
DEFORMERS = {
    'SHRINKWRAP', 'LATTICE', 'CORRECTIVE_SMOOTH', 'SURFACE_DEFORM', 
    'SIMPLE_DEFORM', 'MESH_DEFORM', 'CAST', 'WAVE', 'DISPLACE', 
    'SMOOTH', 'LAPLACIANSMOOTH', 'WARP'
}

muted_bone_constraints = []
muted_object_constraints = []

for o in bpy.data.objects:
    if o.type == 'ARMATURE':
        for bone in o.pose.bones:
            for c in bone.constraints:
                if not c.mute:
                    c.mute = True
                    muted_bone_constraints.append(c)
    for c in o.constraints:
        if not c.mute:
            c.mute = True
            muted_object_constraints.append(c)

print(f"  [+] Muted {len(muted_bone_constraints)} bone constraints and {len(muted_object_constraints)} object constraints to resolve cycles.")

disabled_armatures = []
for o in bpy.data.objects:
    for m in o.modifiers:
        if m.type == 'ARMATURE' and m.show_viewport:
            m.show_viewport = False
            disabled_armatures.append(m)

for obj in list(bpy.data.objects):
    if obj.type != 'MESH':
        continue
        
    active_deformers = [m for m in obj.modifiers if m.type in DEFORMERS and m.show_viewport]
    if not active_deformers:
        continue
        
    print(f"[~] Baking deformers mathematically for mesh: {obj.name}")
    
    if obj.data and obj.data.users > 1:
        obj.data = obj.data.copy()
        
    has_shape_keys = bool(obj.data.shape_keys)
    
    old_key_values = {}
    if has_shape_keys:
        for kb in obj.data.shape_keys.key_blocks:
            old_key_values[kb.name] = kb.value
            if kb.name != 'Basis':
                kb.value = 0.0
                
    disabled_mods = []
    for mod in obj.modifiers:
        if mod not in active_deformers:
            if mod.show_viewport:
                mod.show_viewport = False
                disabled_mods.append(mod)
        else:
            mod.show_viewport = True
                
    bpy.context.view_layer.update()
    temp_depsgraph = bpy.context.evaluated_depsgraph_get()
    
    try:
        evaluated_obj = obj.evaluated_get(temp_depsgraph)
        evaluated_mesh = evaluated_obj.data
        
        if len(evaluated_mesh.vertices) == len(obj.data.vertices):
            new_basis_cos = [v.co.copy() for v in evaluated_mesh.vertices]
            
            if has_shape_keys:
                basis_key = obj.data.shape_keys.key_blocks.get('Basis')
                if basis_key:
                    old_basis_cos = [v.co.copy() for v in basis_key.data]
                    for i, co in enumerate(new_basis_cos):
                        basis_key.data[i].co = co
                        
                    for kb in obj.data.shape_keys.key_blocks:
                        if kb == basis_key:
                            continue
                        rel_key = kb.relative_key if kb.relative_key else basis_key
                        if rel_key == basis_key:
                            for i, key_vert in enumerate(kb.data):
                                offset = key_vert.co - old_basis_cos[i]
                                key_vert.co = new_basis_cos[i] + offset
                    print(f"  [+] Overwrote morph 'Basis' coordinates.")
            else:
                for i, co in enumerate(new_basis_cos):
                    obj.data.vertices[i].co = co
                print(f"  [+] Overwrote base vertex coordinates.")
                
            for mod in list(obj.modifiers):
                if mod in active_deformers:
                    obj.modifiers.remove(mod)
        else:
            print(f"  [-] Vertex count mismatch on '{obj.name}'. Deformers skipped.")
    except Exception as e:
        print(f"  [-] Failed to bake coordinates on '{obj.name}': {e}")
    finally:
        for mod in disabled_mods:
            mod.show_viewport = True
        if has_shape_keys:
            for kb_name, val in old_key_values.items():
                kb = obj.data.shape_keys.key_blocks.get(kb_name)
                if kb: kb.value = val

for c in muted_bone_constraints:
    c.mute = False
for c in muted_object_constraints:
    c.mute = False

for m in disabled_armatures:
    m.show_viewport = True

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 5. APPLY STATIC MODIFIERS (Double-Deformation-Proofed)
#    Temporarily disables active deforming modifiers (Armatures/Shrinkwraps)
#    during static modifier baking to prevent pose coordinates from being 
#    compounded and baked as base mesh vertices.
# ---------------------------------------------------------------------------
SKIP_MOD_TYPES = {'ARMATURE', 'SHRINKWRAP'}

print("[*] Baking static modifiers via low-level data-block swaps...")
depsgraph = bpy.context.evaluated_depsgraph_get()

for obj in list(bpy.data.objects):
    if obj.type != 'MESH' or obj.data.shape_keys:
        continue
    candidate_mods = [m for m in obj.modifiers if m.type not in SKIP_MOD_TYPES]
    if not candidate_mods:
        continue
    
    try:
        # Disable deforming modifiers prior to evaluation
        disabled_mods = []
        for m in obj.modifiers:
            if m.type in SKIP_MOD_TYPES and m.show_viewport:
                m.show_viewport = False
                disabled_mods.append(m)
                
        bpy.context.view_layer.update()
        obj_eval = obj.evaluated_get(depsgraph)
        mesh_eval = obj_eval.data.copy()
        
        # Replace base mesh reference
        old_mesh = obj.data
        obj.data = mesh_eval
        
        if old_mesh.users == 0:
            bpy.data.meshes.remove(old_mesh)
            
        # Restore deforming modifier visibility
        for m in disabled_mods:
            m.show_viewport = True
            
        # Remove only the static baked modifiers
        for m in list(obj.modifiers):
            if m.type not in SKIP_MOD_TYPES:
                obj.modifiers.remove(m)
                print(f"  [+] Successfully baked static modifiers on '{obj.name}'")
    except Exception as e:
        print(f"  [-] Failed to bake modifiers on '{obj.name}': {e}")

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 6. CONVERT COMPLEX SHADER TREES TO GLTF-COMPLIANT PBR
# ---------------------------------------------------------------------------
print("[*] Converting shader trees to glTF-compliant PBR...")
export_meshes = [o for o in bpy.data.objects if o.type == 'MESH' and not o.hide_viewport]
converted_cache = {}

for obj in export_meshes:
    has_uvs = len(obj.data.uv_layers) > 0

    for mat_slot in obj.material_slots:
        mat = mat_slot.material
        if not mat or not mat.use_nodes or not mat.node_tree:
            continue

        cache_key = (mat.name, has_uvs)
        if cache_key in converted_cache:
            mat_slot.material = converted_cache[cache_key]
            continue

        suffix   = "PBR_Tex" if has_uvs else "PBR_Flat"
        new_mat  = bpy.data.materials.new(name=f"{mat.name}_{suffix}")

        src_blend = mat.blend_method
        new_mat.blend_method       = src_blend
        new_mat.alpha_threshold    = mat.alpha_threshold
        new_mat.use_backface_culling = mat.use_backface_culling

        new_mat.use_nodes = True
        nn    = new_mat.node_tree.nodes
        nl    = new_mat.node_tree.links
        nn.clear()

        pbr_node    = nn.new('ShaderNodeBsdfPrincipled')
        output_node = nn.new('ShaderNodeOutputMaterial')
        nl.new(pbr_node.outputs['BSDF'], output_node.inputs['Surface'])

        src_nodes   = mat.node_tree.nodes
        tex_node    = next((n for n in src_nodes if n.type == 'TEX_IMAGE'),   None)
        color_node  = next((n for n in src_nodes if n.type in ('VERTEX_COLOR', 'COLOR_ATTRIBUTE')), None)
        src_pbr     = next((n for n in src_nodes if n.type == 'BSDF_PRINCIPLED'), None)

        is_transparent = src_blend in ('BLEND', 'CLIP', 'HASHED')

        if tex_node and tex_node.image:
            print(f"[+] Path A  | texture  | {mat.name} -> {tex_node.image.name}")
            img_node       = nn.new('ShaderNodeTexImage')
            img_node.image = tex_node.image
            nl.new(img_node.outputs['Color'], pbr_node.inputs['Base Color'])
            if is_transparent:
                nl.new(img_node.outputs['Alpha'], pbr_node.inputs['Alpha'])
            else:
                pbr_node.inputs['Alpha'].default_value = 1.0
                new_mat.blend_method = 'OPAQUE'

        elif color_node:
            print(f"[+] Path B  | vtx color| {mat.name}")
            col_attr = nn.new('ShaderNodeVertexColor')
            col_attr.layer_name = color_node.layer_name if hasattr(color_node, 'layer_name') else "Color"
            nl.new(col_attr.outputs['Color'], pbr_node.inputs['Base Color'])
            new_mat.blend_method = 'OPAQUE'
            pbr_node.inputs['Alpha'].default_value = 1.0

        else:
            print(f"[+] Path C  | flat col | {mat.name}")
            if src_pbr:
                pbr_node.inputs['Base Color'].default_value = src_pbr.inputs['Base Color'].default_value
            else:
                pbr_node.inputs['Base Color'].default_value = mat.diffuse_color
            new_mat.blend_method = 'OPAQUE'
            pbr_node.inputs['Alpha'].default_value = 1.0

        mat_slot.material            = new_mat
        converted_cache[cache_key]   = new_mat

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 6.5 CLEANUP AND VISIBILITY PASS BEFORE EXPORT
# ---------------------------------------------------------------------------
print("[*] Cleaning up remaining shrinkwrap targets and helper meshes before export...")

for target_name in list(helpers_to_clean):
    target_obj = bpy.data.objects.get(target_name)
    if target_obj:
        print(f"[~] Removing shrinkwrap helper cage: {target_name}")
        bpy.data.objects.remove(target_obj, do_unlink=True)

for obj in list(bpy.data.objects):
    if obj.type == 'MESH':
        if obj.name not in CORE_CHARACTER_MESHES:
            print(f"[~] Removing non-core mesh (optional outfit/helper): {obj.name}")
            try:
                bpy.data.objects.remove(obj, do_unlink=True)
            except Exception as e:
                print(f"  [-] Failed to remove {obj.name}: {e}")

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 6.6 AUTOMATIC SKELETAL PARENTING RECOVERY
#     To prevent "Armature must be the parent of skinned mesh" errors and 
#     ensure skeletal skin animation link bindings render correctly, core 
#     skinned meshes must be parented directly to the Armature rig.
# ---------------------------------------------------------------------------
print("[*] Performing Skeletal Parenting Recovery...")
if rig:
    for obj in bpy.data.objects:
        if obj.type == 'MESH':
            has_armature_mod = any(
                mod.type == 'ARMATURE' and mod.object == rig 
                for mod in obj.modifiers
            )
            if has_armature_mod:
                if obj.parent != rig:
                    print(f"  [+] Parenting skinned mesh directly to Armature: {obj.name}")
                    world_matrix = obj.matrix_world.copy()
                    obj.parent = rig
                    obj.matrix_parent_inverse = rig.matrix_world.inverted()
                    obj.matrix_world = world_matrix

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 6.8 DEEP F-CURVE PRUNING (Context-Aware Morph Target Filtering)
#     Scans and removes orphaned animation curves targeting omitted skeleton 
#     bones or shape keys. This resolves all validator target-node errors.
#     Supports legacy (4.x) and modern (5.x+) layered slotted action systems.
# ---------------------------------------------------------------------------
print("[*] Performing deep F-curve pruning to clean up export...")
if rig:
    valid_bones = {b.name for b in rig.pose.bones}
    
    valid_shape_keys = set()
    for name in CORE_CHARACTER_MESHES:
        obj = bpy.data.objects.get(name)
        if obj and obj.type == 'MESH' and obj.data.shape_keys:
            for kb in obj.data.shape_keys.key_blocks:
                valid_shape_keys.add(kb.name)
                
    for action in list(bpy.data.actions):
        # Scenario A: Modern Blender 5.x+ (Slotted/Layered Action system)
        if hasattr(action, "layers"):
            for layer in action.layers:
                for strip in layer.strips:
                    # Clean up based on slots
                    for slot in action.slots:
                        cb = strip.channelbag(slot)
                        if cb and hasattr(cb, "fcurves"):
                            target_id = slot.target if hasattr(slot, "target") else (slot.id if hasattr(slot, "id") else None)
                            
                            # Check if this slot targets shape keys of a deleted mesh
                            is_valid_slot = True
                            if target_id and isinstance(target_id, bpy.types.Key):
                                is_valid_slot = any(
                                    o.type == 'MESH' and o.data.shape_keys == target_id and o.name in CORE_CHARACTER_MESHES
                                    for o in bpy.data.objects
                                )
                                if not is_valid_slot:
                                    cb.fcurves.clear()
                                    continue
                                    
                            # Check if this slot targets an Object
                            elif target_id and isinstance(target_id, bpy.types.Object):
                                # If the target object is not a core mesh we are exporting, clear it
                                if target_id.name not in CORE_CHARACTER_MESHES:
                                    is_valid_slot = False
                                    cb.fcurves.clear()
                                    continue
                                    
                                # Check individual curves for valid shape keys on this specific object
                                for fc in list(cb.fcurves):
                                    if "key_blocks" in fc.data_path:
                                        has_valid_key = False
                                        if target_id.data and target_id.data.shape_keys:
                                            parts = fc.data_path.split('"')
                                            if len(parts) > 1:
                                                key_name = parts[1]
                                                if key_name in target_id.data.shape_keys.key_blocks:
                                                    has_valid_key = True
                                        if not has_valid_key:
                                            cb.fcurves.remove(fc)
                                            
                            if is_valid_slot:
                                # Clean individual curves in valid slot
                                for fc in list(cb.fcurves):
                                    if fc.data_path.startswith("pose.bones"):
                                        parts = fc.data_path.split('"')
                                        if len(parts) > 1:
                                            bone_name = parts[1]
                                            if bone_name not in valid_bones:
                                                cb.fcurves.remove(fc)
                                    elif "key_blocks" in fc.data_path:
                                        parts = fc.data_path.split('"')
                                        if len(parts) > 1:
                                            shape_key_name = parts[1]
                                            if shape_key_name not in valid_shape_keys:
                                                cb.fcurves.remove(fc)
                                                
        # Scenario B: Legacy Blender (4.x and older) fallback
        elif hasattr(action, "fcurves"):
            for fc in list(action.fcurves):
                if fc.data_path.startswith("pose.bones"):
                    parts = fc.data_path.split('"')
                    if len(parts) > 1:
                        bone_name = parts[1]
                        if bone_name not in valid_bones:
                            action.fcurves.remove(fc)
                elif "key_blocks" in fc.data_path:
                    parts = fc.data_path.split('"')
                    if len(parts) > 1:
                        shape_key_name = parts[1]
                        if shape_key_name not in valid_shape_keys:
                            action.fcurves.remove(fc)

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 7. EXPORT TO GLB
# ---------------------------------------------------------------------------
print("[*] Exporting to clean GLB using Actions mode...")
bpy.ops.export_scene.gltf(
    filepath='__GLB_PATH__',
    export_format='GLB',
    export_skins=True,
    export_morph=True,
    export_tangents=True,
    export_normals=True,
    export_apply=False,
    export_animations=True,
    export_animation_mode='ACTIONS',  # Pulls the stashed animations from the rig
    export_def_bones=False,
    export_attributes=True,
    use_visible=False,
    export_cameras=False,
    export_lights=False,
)
print("[+] GLB export complete.")
"""

    safe_glb_path = glb_path.replace("\\", "\\\\")
    expr = expr.replace("__GLB_PATH__", safe_glb_path)

    # Write the cached original bytes into the temporary file in the same folder
    try:
        with open(temp_blend_path, "wb") as f:
            f.write(original_bytes)
    except Exception as e:
        print(f"[-] Failed to write temporary file {temp_blend_path}: {e}")
        return False

    env = os.environ.copy()
    if "VIRTUAL_ENV" in env:
        venv_path = env["VIRTUAL_ENV"]
        clean_path = [
            p
            for p in env.get("PATH", "").split(os.pathsep)
            if not p.startswith(venv_path)
        ]
        env["PATH"] = os.pathsep.join(clean_path)
        del env["VIRTUAL_ENV"]
    env.pop("PYTHONHOME", None)
    env.pop("PYTHONPATH", None)

    cmd = ["blender", "-b", temp_blend_path, "--python-expr", expr]
    print(
        f"[+] Exporting {os.path.basename(blend_path)} via temporary clone (streaming output)..."
    )

    try:
        result = subprocess.run(cmd, env=env)
        export_success = result.returncode == 0 and os.path.exists(glb_path)
    except Exception as e:
        print(f"[-] Blender process execution failed: {e}")
        export_success = False
    finally:
        if os.path.exists(temp_blend_path):
            try:
                os.remove(temp_blend_path)
            except Exception as e:
                print(
                    f"[-] Warning: Failed to clean up temporary file {temp_blend_path}: {e}"
                )

        with open(blend_path, "rb") as f:
            current_bytes = f.read()

        assert original_bytes == current_bytes, (
            "CRITICAL EXPORT FAILURE: A bit-level modification was detected on the source "
            f"input file: '{blend_path}'! The program has halted with an assertion error."
        )

    return export_success
