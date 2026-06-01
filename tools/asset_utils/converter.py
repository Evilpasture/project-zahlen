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
    "pomni_gloveballs", "pomni_balls", "pomni_ballsbody", "pomni_ballshat"
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
# 0.5 FORCE-ENABLE AND UN-HIDE ALL COLLECTIONS AND OBJECTS
#     Blender's dependency graph (depsgraph) only evaluates modifiers and 
#     deformation targets (like Lattices and Shrinkwrap cages) if they, 
#     their parent collections, and their objects are fully un-excluded and 
#     visible in the viewport. We un-exclude and un-hide everything at the start.
# ---------------------------------------------------------------------------
print("[*] Force-enabling and un-hiding all collections and objects for accurate baking...")

# 1. Un-exclude and un-hide all scene collections
for col in bpy.data.collections:
    col.hide_viewport = False
    col.hide_render = False

# 2. Un-exclude and un-hide all view-layer collections
layer_collections = [bpy.context.view_layer.layer_collection]
while layer_collections:
    l_c = layer_collections.pop(0)
    layer_collections.extend(l_c.children)
    l_c.exclude = False
    l_c.hide_viewport = False

# 3. Force all scene objects to be completely unhidden in the viewport
for obj in bpy.data.objects:
    obj.hide_viewport = False
    obj.hide_render = False
    obj.hide_set(False)

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 1. SETUP SKELETON REFERENCE
# ---------------------------------------------------------------------------
rigs = [o for o in bpy.data.objects if o.type == 'ARMATURE']

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
# ---------------------------------------------------------------------------
print("[*] Purging competing metarigs and template armatures...")
for obj in list(bpy.data.objects):
    if obj.type == 'ARMATURE' and obj != rig:
        print(f"  [-] Removing non-essential armature: {obj.name}")
        bpy.data.objects.remove(obj, do_unlink=True)

# Explicitly remove pomni_mouth from the Blender scene as requested
mouth_obj = bpy.data.objects.get("pomni_mouth")
if mouth_obj:
    print("  [-] Removing pomni_mouth mesh object completely from final rig...")
    bpy.data.objects.remove(mouth_obj, do_unlink=True)

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 1.2 AUTOMATICALLY STASH ALL BONE ACTIONS TO SKELETON NLA
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
                                if any(fc.data_path.startswith("pose.bones") or "location" in fc.data_path or "rotation" in fc.data_path for cb_fc in cb.fcurves):
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
# 3.2 PRESERVE HEAD HOLLOWS (Disable head-flattening shrinkwraps)
# ---------------------------------------------------------------------------
print("[*] Preserving head eye sockets and mouth cavities...")
head_obj = bpy.data.objects.get("pomni_head.002")
if head_obj:
    for m in list(head_obj.modifiers):
        if m.name == 'Main Shrinkwrap':
            print(f"  [-] Deleting head-flattening modifier '{m.name}' from {head_obj.name}")
            head_obj.modifiers.remove(m)

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 3.5 UNIVERSAL MODIFIER PRUNING ON SHAPE-KEYED MESHES
# ---------------------------------------------------------------------------
print("[*] Performing universal modifier pruning on non-baked shape-keyed meshes...")
BAKE_SHAPE_KEYED_MESHES = {
    "pomni_eyelidsL", "pomni_eyelidsR", "pomni_eyelidsTOP", 
    "pomni_eyes", "pomni_pupils"
}

for obj in list(bpy.data.objects):
    if obj.type == 'MESH' and obj.data.shape_keys:
        if obj.name in BAKE_SHAPE_KEYED_MESHES:
            # Prune Corrective Smooth on baked meshes to prevent evaluation blocks
            for m in list(obj.modifiers):
                if m.type == 'CORRECTIVE_SMOOTH':
                    print(f"  [-] Pruning Corrective Smooth modifier {m.name} from baked mesh: {obj.name}")
                    obj.modifiers.remove(m)
            continue
            
        for m in list(obj.modifiers):
            if m.type in {'SOLIDIFY', 'SUBSURF', 'DECIMATE', 'BOOLEAN', 'EDGE_SPLIT', 'MIRROR', 'CORRECTIVE_SMOOTH'}:
                print(f"  [-] Pruning generative modifier {m.type} from shape-keyed mesh: {obj.name}")
                obj.modifiers.remove(m)

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
# 3.7 PRUNE UNUSED SHAPE KEYS FROM EYELIDS
#     To avoid exporting empty/useless shape keys (morph targets) with active
#     drivers that do nothing on specific mesh segments, we prune them here.
#     To prevent dependency graph evaluation crashes (BKE_animsys_eval_driver),
#     associated drivers are explicitly removed prior to shape key deletion.
# ---------------------------------------------------------------------------
print("[*] Pruning unused shape keys and drivers from specific eyelid meshes...")
EYELID_SHAPE_KEY_PRESERVATION = {
    "pomni_eyelidsR": {"Basis", "botR"},
    "pomni_eyelidsL": {"Basis", "botL"},
    "pomni_eyelidsTOP": {"Basis", "topL", "topR"}
}

for mesh_name, keys_to_keep in EYELID_SHAPE_KEY_PRESERVATION.items():
    obj = bpy.data.objects.get(mesh_name)
    if obj and obj.type == 'MESH':
        if obj.data.shape_keys:
            if obj.data.users > 1:
                print(f"  [~] Making mesh data single-user for {mesh_name}...")
                obj.data = obj.data.copy()
            
            print(f"  [~] Processing shape keys for {mesh_name}...")
            
            # 1. Cleanly delete drivers targeting the shape keys we are removing
            if obj.data.shape_keys.animation_data:
                drivers = obj.data.shape_keys.animation_data.drivers
                for d in list(drivers):
                    path = d.data_path
                    key_name = None
                    if "key_blocks[" in path:
                        start_idx = path.find("key_blocks[") + 11
                        if start_idx < len(path):
                            quote_char = path[start_idx]
                            if quote_char in ('"', "'"):
                                end_idx = path.find(quote_char, start_idx + 1)
                                if end_idx != -1:
                                    key_name = path[start_idx+1:end_idx]
                    
                    if key_name and key_name not in keys_to_keep:
                        print(f"    [-] Removing driver for unused shape key: {key_name} (path: {path})")
                        drivers.remove(d)
            
            # Flush dependency updates after removing drivers
            obj.data.shape_keys.update_tag()
            bpy.context.view_layer.update()
            
            # 2. Safely remove the actual shape key block now that drivers are detached
            kb_list = list(obj.data.shape_keys.key_blocks)
            for kb in kb_list:
                if kb.name not in keys_to_keep:
                    print(f"    [-] Removing unused shape key: {kb.name}")
                    obj.shape_key_remove(kb)

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 4. UNIFIED DEFORMATION BAKING (Supports Shape Key Modifier Baking)
# ---------------------------------------------------------------------------
print("[*] Performing Unified Deformation Baking...")

DEFORMERS = {
    'SHRINKWRAP', 'LATTICE', 'CORRECTIVE_SMOOTH', 'SURFACE_DEFORM', 
    'SIMPLE_DEFORM', 'MESH_DEFORM', 'CAST', 'WAVE', 'DISPLACE', 
    'SMOOTH', 'LAPLACIANSMOOTH', 'WARP', 'BOOLEAN'
}

orig_simplify = bpy.context.scene.render.use_simplify
bpy.context.scene.render.use_simplify = False
if rig:
    rig.data.pose_position = 'REST'
bpy.context.view_layer.update()

def bake_modifiers_on_shape_keyed_mesh(obj, modifiers_to_bake):
    if not obj.data.shape_keys:
        return False
        
    key_blocks = obj.data.shape_keys.key_blocks
    orig_values = {kb.name: kb.value for kb in key_blocks}
    
    # 1. Temporarily mute shape key drivers so they do not override our manual
    #    baking evaluation coordinates of 1.0 back to 0.0 on view_layer updates.
    muted_drivers = []
    if obj.data.shape_keys and obj.data.shape_keys.animation_data:
        for d in obj.data.shape_keys.animation_data.drivers:
            if not d.mute:
                d.mute = True
                muted_drivers.append(d)
                
    for kb in key_blocks:
        kb.value = 0.0
        
    disabled_mods = []
    for m in obj.modifiers:
        if m not in modifiers_to_bake and m.show_viewport:
            m.show_viewport = False
            disabled_mods.append(m)
        elif m in modifiers_to_bake:
            m.show_viewport = True
            
    bpy.context.view_layer.update()
    depsgraph = bpy.context.evaluated_depsgraph_get()
    
    # Extract the new base mesh as a clean, database-linked block
    obj_eval = obj.evaluated_get(depsgraph)
    new_base_mesh = bpy.data.meshes.new_from_object(
        obj_eval, preserve_all_data_layers=True, depsgraph=depsgraph
    )
    
    # Evaluate each shape key with modifiers applied
    new_shape_meshes = {}
    for kb in key_blocks:
        if kb.name == 'Basis':
            continue
        kb.value = 1.0
        bpy.context.view_layer.update()
        
        # Query a fresh depsgraph so that shape key values of 1.0 are evaluated
        shape_depsgraph = bpy.context.evaluated_depsgraph_get()
        obj_eval_shape = obj.evaluated_get(shape_depsgraph)
        shape_mesh = bpy.data.meshes.new_from_object(
            obj_eval_shape, preserve_all_data_layers=True, depsgraph=shape_depsgraph
        )
            
        new_shape_meshes[kb.name] = shape_mesh
        kb.value = 0.0
        
    # Unmute the original drivers
    for d in muted_drivers:
        d.mute = False
        
    for m in disabled_mods:
        m.show_viewport = True
        
    old_mesh = obj.data
    
    # 2. Copy materials to prevent mesh-level data assignments from being wiped
    for mat in old_mesh.materials:
        new_base_mesh.materials.append(mat)
        
    obj.data = new_base_mesh
    
    # Clear any procedurally copied shape keys on the new database-linked base mesh
    if obj.data.shape_keys:
        for kb in list(obj.data.shape_keys.key_blocks):
            obj.shape_key_remove(kb)
            
    obj.shape_key_add(name="Basis")
    
    # Reassign the deformed morph key coordinates
    for name, shape_mesh in new_shape_meshes.items():
        new_key = obj.shape_key_add(name=name)
        for i, v in enumerate(shape_mesh.vertices):
            new_key.data[i].co = v.co.copy()
        bpy.data.meshes.remove(shape_mesh)
        
    # Copy animation data & drivers to preserve blink animations!
    if old_mesh.shape_keys and old_mesh.shape_keys.animation_data:
        obj.data.shape_keys.animation_data_create()
        if old_mesh.shape_keys.animation_data.action:
            obj.data.shape_keys.animation_data.action = old_mesh.shape_keys.animation_data.action
        for d in old_mesh.shape_keys.animation_data.drivers:
            try:
                new_d = obj.data.shape_keys.animation_data.drivers.new(data_path=d.data_path)
                
                # Force driver type to SCRIPTED to allow our range correction expressions to evaluate.
                # AVERAGE type ignores expression strings entirely and causes negative clamped bounds.
                new_d.driver.type = 'SCRIPTED'
                
                # Math Correction (Corrects Inversion):
                # - top keys (Top Eyelids): Location Y ranges [0.1 (open) -> 0.0 (closed)]. Expression: var * 10.0
                # - bot keys (Bottom Eyelids): Location Y ranges [-0.1 (open) -> 0.0 (closed)]. Expression: var * -10.0
                path = d.data_path
                if "topL" in path or "topR" in path:
                    new_d.driver.expression = "var * 10.0"
                elif "botL" in path or "botR" in path:
                    new_d.driver.expression = "var * -10.0"
                else:
                    new_d.driver.expression = d.driver.expression
                
                new_d.mute = False  # Ensure the newly created driver is NOT muted
                for var in d.driver.variables:
                    new_var = new_d.driver.variables.new()
                    new_var.name = var.name
                    new_var.type = var.type
                    for i, t in enumerate(var.targets):
                        if i < len(new_var.targets):
                            new_t = new_var.targets[i]
                        else:
                            new_t = new_var.targets.new()
                        new_t.id = t.id
                        new_t.data_path = t.data_path
                        if hasattr(t, "bone_target"):
                            new_t.bone_target = t.bone_target
                        if hasattr(t, "transform_type"):
                            new_t.transform_type = t.transform_type
                        if hasattr(t, "transform_space"):
                            new_t.transform_space = t.transform_space
                
                # Force-compile the programmatic driver so that the depsgraph links it properly
                expr = new_d.driver.expression
                new_d.driver.expression = expr + " "
                new_d.driver.expression = expr
                
            except Exception as e:
                print(f"  [-] Failed to copy shape key driver: {e}")
        
    for kb_name, val in orig_values.items():
        kb = obj.data.shape_keys.key_blocks.get(kb_name)
        if kb: kb.value = val
            
    if old_mesh.users == 0:
        bpy.data.meshes.remove(old_mesh)
        
    for m in list(obj.modifiers):
        if m in modifiers_to_bake:
            obj.modifiers.remove(m)
            
    # Force RNA array flush to prevent GLTF export expected size mismatch errors
    obj.data.validate(verbose=False)
    obj.data.update()
    
    # Tag and update the key blocks to ensure compiled drivers evaluate on export
    if obj.data.shape_keys:
        obj.data.shape_keys.update_tag()
    
    print(f"  [+] Baked modifiers on shape-keyed mesh: {obj.name}")
    return True

# ---------------------------------------------------------------------------
# 4.1 Unified Deformation Baking (Targeted on Eyelids)
# ---------------------------------------------------------------------------
print("[*] Baking deforming and deterministic modifiers on eyelids...")
for obj in list(bpy.data.objects):
    if obj.type == 'MESH' and obj.data.shape_keys and obj.name in BAKE_SHAPE_KEYED_MESHES:
        mods_to_bake = [m for m in obj.modifiers if m.type != 'ARMATURE']
        if mods_to_bake:
            print(f"  [+] Baking modifiers on shape-keyed mesh: {obj.name}")
            bake_modifiers_on_shape_keyed_mesh(obj, mods_to_bake)

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 4.2 Bake standard meshes (without shape keys)
# ---------------------------------------------------------------------------
SKIP_MOD_TYPES = {'ARMATURE'}
depsgraph = bpy.context.evaluated_depsgraph_get()

for obj in list(bpy.data.objects):
    if obj.type != 'MESH':
        continue
    # Skip shape-keyed meshes processed in Step 4.1
    if obj.data.shape_keys:
        continue
        
    candidate_mods = [m for m in obj.modifiers if m.type not in SKIP_MOD_TYPES]
    if not candidate_mods:
        continue
    
    try:
        disabled_mods = []
        for m in obj.modifiers:
            if m.type in SKIP_MOD_TYPES and m.show_viewport:
                m.show_viewport = False
                disabled_mods.append(m)
            elif m.type not in SKIP_MOD_TYPES:
                m.show_viewport = True
                
        bpy.context.view_layer.update()
        
        # Get evaluated mesh using robust database-linked new_from_object
        obj_eval = obj.evaluated_get(depsgraph)
        mesh_eval = bpy.data.meshes.new_from_object(
            obj_eval, preserve_all_data_layers=True, depsgraph=depsgraph
        )
        
        old_mesh = obj.data
        
        # Copy materials to prevent standard meshes from losing data assignments
        for mat in old_mesh.materials:
            mesh_eval.materials.append(mat)
            
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
                
        obj.data.validate(verbose=False)
        obj.data.update()
    except Exception as e:
        print(f"  [-] Failed to bake modifiers on '{obj.name}': {e}")

bpy.context.view_layer.update()

# Restore Blender's original "Simplify" settings and rig pose position
bpy.context.scene.render.use_simplify = orig_simplify
if rig:
    rig.data.pose_position = 'POSE'
bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 6. CONVERT COMPLEX SHADER TREES TO GLTF-COMPLIANT PBR
# ---------------------------------------------------------------------------
print("[*] Converting shader trees to glTF-compliant PBR...")
export_meshes = [o for o in bpy.data.objects if o.type == 'MESH']
converted_cache = {}

for obj in export_meshes:
    has_uvs = len(obj.data.uv_layers) > 0

    for mat_slot in obj.material_slots:
        mat = mat_slot.material
        if not mat or not mat.node_tree:
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

        # Preserve transparent blend method settings to keep pupil outline anti-aliasing smooth.
        # For solid meshes with no transparency requirements, fallback to OPAQUE to prevent depth sorting bugs.
        if src_blend in {'BLEND', 'HASHED', 'CLIP'}:
            new_mat.blend_method = src_blend
        else:
            new_mat.blend_method = 'OPAQUE'

        if hasattr(new_mat, "use_nodes"):
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

        pbr_node.inputs['Alpha'].default_value = 1.0

        if tex_node and tex_node.image:
            print(f"[+] Path A  | texture  | {mat.name} -> {tex_node.image.name}")
            img_node       = nn.new('ShaderNodeTexImage')
            img_node.image = tex_node.image
            nl.new(img_node.outputs['Color'], pbr_node.inputs['Base Color'])

        elif color_node:
            print(f"[+] Path B  | vtx color| {mat.name}")
            col_attr = nn.new('ShaderNodeVertexColor')
            col_attr.layer_name = color_node.layer_name if hasattr(color_node, 'layer_name') else "Color"
            nl.new(col_attr.outputs['Color'], pbr_node.inputs['Base Color'])

        else:
            print(f"[+] Path C  | flat col | {mat.name}")
            if src_pbr:
                pbr_node.inputs['Base Color'].default_value = src_pbr.inputs['Base Color'].default_value
            else:
                pbr_node.inputs['Base Color'].default_value = mat.diffuse_color

        mat_slot.material            = new_mat
        converted_cache[cache_key]   = new_mat

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 6.5 CLEANUP AND VISIBILITY PASS BEFORE EXPORT
# ---------------------------------------------------------------------------
print("[*] Cleaning up remaining helper meshes before export...")

for obj in list(bpy.data.objects):
    if obj.type == 'MESH':
        if obj.name in shrinkwrap_targets:
            continue
        if obj.name not in CORE_CHARACTER_MESHES:
            print(f"[~] Removing non-core mesh (optional outfit/helper): {obj.name}")
            try:
                bpy.data.objects.remove(obj, do_unlink=True)
            except Exception as e:
                print(f"  [-] Failed to remove {obj.name}: {e}")

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 6.5.5 SKELETAL EXCLUSION FOR HELPER TARGETS BEFORE EXPORT
#       We unlink non-core helper cages from all scene collections and clear
#       their parenting hierarchy (preserving their world coordinates) so they 
#       are completely omitted from the scene graph. This prevents parent-child
#       traversal leakage during glTF export.
# ---------------------------------------------------------------------------
print("[*] Unlinking and unparenting non-core shrinkwrap helper meshes...")

# Unlink and unparent shrinkwrap targets that are not core meshes
for name in shrinkwrap_targets:
    if name not in CORE_CHARACTER_MESHES:
        target_obj = bpy.data.objects.get(name)
        if target_obj:
            print(f"  [-] Unlinking and unparenting helper target: {target_obj.name}")
            # Cache and restore world transform so they don't shift in space
            world_matrix = target_obj.matrix_world.copy()
            target_obj.parent = None
            target_obj.matrix_world = world_matrix
            
            # Unlink from all scene collections
            for col in list(target_obj.users_collection):
                col.objects.unlink(target_obj)

# Force-enable and un-hide all scene collections recursively
for col in bpy.data.collections:
    col.hide_viewport = False
    col.hide_render = False

# Unlink and unhide layer collections recursively
layer_collections = [bpy.context.view_layer.layer_collection]
while layer_collections:
    l_c = layer_collections.pop(0)
    layer_collections.extend(l_c.children)
    l_c.exclude = False
    l_c.hide_viewport = False

# Ensure all remaining core character meshes are linked to the scene collection and visible
for name in CORE_CHARACTER_MESHES:
    obj = bpy.data.objects.get(name)
    if obj:
        obj.hide_viewport = False
        obj.hide_render = False
        obj.hide_set(False)  # Force eye icon open
        if obj.name not in bpy.context.scene.collection.objects:
            try:
                bpy.context.scene.collection.objects.link(obj)
            except Exception:
                pass

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 6.6 AUTOMATIC SKELETAL PARENTING RECOVERY
#     To prevent "Armature must be the parent of skinned mesh" errors, core
#     skinned meshes must be parented directly to the Armature rig.
#     We skip non-core helper cages so they stay unparented and hidden.
# ---------------------------------------------------------------------------
print("[*] Performing Skeletal Parenting Recovery...")
if rig:
    for obj in bpy.data.objects:
        # Only perform recovery for core character meshes
        if obj.type == 'MESH' and obj.name in CORE_CHARACTER_MESHES:
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
                    for slot in action.slots:
                        cb = strip.channelbag(slot)
                        if cb and hasattr(cb, "fcurves"):
                            target_id = slot.target if hasattr(slot, "target") else (slot.id if hasattr(slot, "id") else None)
                            
                            is_valid_slot = True
                            if target_id and isinstance(target_id, bpy.types.Key):
                                is_valid_slot = any(
                                    o.type == 'MESH' and o.data.shape_keys == target_id and o.name in CORE_CHARACTER_MESHES
                                    for o in bpy.data.objects
                                )
                                if not is_valid_slot:
                                    cb.fcurves.clear()
                                    continue
                                    
                            elif target_id and isinstance(target_id, bpy.types.Object):
                                if target_id.name not in CORE_CHARACTER_MESHES:
                                    is_valid_slot = False
                                    cb.fcurves.clear()
                                    continue
                                    
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
    export_animation_mode='ACTIONS',
    export_def_bones=False,
    export_attributes=True,
    use_visible=False,  # Bypass background-mode viewport evaluation bugs!
    export_cameras=False,
    export_lights=False,
)
print("[+] GLB export complete.")
"""

    safe_glb_path = glb_path.replace("\\", "\\\\")
    expr = expr.replace("__GLB_PATH__", safe_glb_path)

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
    print(f"[+] Exporting {os.path.basename(blend_path)} via temporary clone...")

    try:
        result = subprocess.run(
            cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )
        output_text = result.stdout.decode("utf-8", errors="replace")

        lines = output_text.splitlines()
        counts = {
            "Boolean non-manifold inputs warnings": 0,
            "Bone constraint animation baking warnings": 0,
            "Multiple rotation mode warnings": 0,
            "Dependency cycle warnings": 0,
            "Active Vertex Color not exported warnings": 0,
        }

        filtered_lines = []
        in_dep_cycle = False

        for line in lines:
            line_strip = line.strip()
            if any(
                line_strip.startswith(prefix) for prefix in ("[*]", "[+]", "[~]", "[-]")
            ):
                filtered_lines.append(line)
                continue

            if "non-manifold inputs" in line:
                counts["Boolean non-manifold inputs warnings"] += 1
            elif "Baking animation" in line and "unsupported constraints" in line:
                counts["Bone constraint animation baking warnings"] += 1
            elif "Multiple rotation mode detected" in line:
                counts["Multiple rotation mode warnings"] += 1
            elif "Vertex Color" in line and "not used in the node tree" in line:
                counts["Active Vertex Color not exported warnings"] += 1
            elif "Dependency cycle detected" in line:
                counts["Dependency cycle warnings"] += 1
                in_dep_cycle = True
            elif in_dep_cycle and line.startswith("                            |"):
                pass
            else:
                if in_dep_cycle and not line.startswith(
                    "                            |"
                ):
                    in_dep_cycle = False

                if any(
                    x in line
                    for x in (
                        "gltf",
                        "export",
                        "Finished",
                        "Starting",
                        "Error",
                        "Traceback",
                        "RuntimeError",
                        "Exception",
                        "Location:",
                    )
                ):
                    filtered_lines.append(line)

        print("\n--- BLENDER PROCESS OUTPUT SUMMARY ---")
        for f_line in filtered_lines:
            print(f_line)

        print("\n--- REPETITIVE WARNINGS SUPPRESSED ---")
        for warn_name, count in counts.items():
            if count > 0:
                print(f"  - {warn_name}: {count} occurrences")
        print("---------------------------------------\n")

        # Verify if glTF file was actually generated during this specific run
        # by checking its existence and return code
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