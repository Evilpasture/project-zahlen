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

    expr = """
import bpy
import mathutils

# ---------------------------------------------------------------------------
# HELPERS
# ---------------------------------------------------------------------------

def ensure_object_mode():
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')

def safe_set_active(obj):
    \"\"\"Set active object, returns False if the object is excluded from the view layer.\"\"\"
    try:
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        return True
    except RuntimeError:
        return False

# ---------------------------------------------------------------------------
# 0. PACK ASSETS & INITIAL SETUP
# ---------------------------------------------------------------------------
bpy.ops.file.pack_all()
ensure_object_mode()

# ---------------------------------------------------------------------------
# 1. RESET DRIVER/PROPERTY TOGGLES
# ---------------------------------------------------------------------------
rigs = [o for o in bpy.data.objects if o.type == 'ARMATURE']
rig  = next((r for r in rigs if 'pomni' in r.name.lower()), rigs[0] if rigs else None)

if rig:
    menu_bone = next((b for b in rig.pose.bones if 'menu' in b.name.lower()), None)
    if menu_bone:
        for prop in ['Sunglasses', 'Possessed Toggle', 'Outfit']:
            if prop in menu_bone:
                menu_bone[prop] = 0.0

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 2. BAKE OBJECT CONSTRAINTS INTO TRANSFORMS
# ---------------------------------------------------------------------------
print("[*] Baking object constraint transforms...")
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
        if obj.name in shrinkwrap_targets:
            print(f"[~] Temporarily keeping shrinkwrap target: {obj.name}")
            continue
        name_lower = obj.name.lower()
        if any(kw in name_lower for kw in PRUNE_KEYWORDS):
            print(f"[~] Removing helper mesh: {obj.name}")
            bpy.data.objects.remove(obj, do_unlink=True)

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 3.5 APPLY TRANSFORMS TO SKELETON (PREVENTS GLOBAL SCALE DRIFT)
#     Applying transforms on the Armature rig resets its scale to 1.0,
#     preventing global scaling drift in game engines.
#     We explicitly do NOT apply transforms to skinned meshes themselves,
#     as doing so breaks their skin weight binding matrices.
# ---------------------------------------------------------------------------
print("[*] Applying transforms to skeleton...")
if rig:
    # Safely apply scale/rotation to the Armature rig
    if safe_set_active(rig):
        try:
            bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
            print(f"  [+] Applied scale/rotation on Armature: {rig.name}")
        except Exception as e:
            print(f"  [-] Failed to apply transforms on Armature: {e}")

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
#    Temporarily mutes active bone/object constraints to decouple cyclic loops,
#    allowing the depsgraph to resolve the shrinkwrap shape correctly.
# ---------------------------------------------------------------------------
print("[*] Performing Unified Deformation Baking...")
DEFORMERS = {
    'SHRINKWRAP', 'LATTICE', 'CORRECTIVE_SMOOTH', 'SURFACE_DEFORM', 
    'SIMPLE_DEFORM', 'MESH_DEFORM', 'CAST', 'WAVE', 'DISPLACE', 
    'SMOOTH', 'LAPLACIANSMOOTH', 'WARP'
}

# 1. Temporarily mute all bone and object constraints to break dependency cycles
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

# 2. Temporarily disable ARMATURE modifiers on ALL objects in the scene
#    to guarantee a clean, uncontaminated rest-pose evaluation of Lattices
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
    
    # Ensure mesh data is single-user
    if obj.data and obj.data.users > 1:
        obj.data = obj.data.copy()
        
    has_shape_keys = bool(obj.data.shape_keys)
    
    # Store and clear active morphs
    old_key_values = {}
    if has_shape_keys:
        for kb in obj.data.shape_keys.key_blocks:
            old_key_values[kb.name] = kb.value
            if kb.name != 'Basis':
                kb.value = 0.0
                
    # Disable non-deforming modifiers (Subsurf, Solidify, etc.) to ensure 1:1 vertex count
    # Keep ALL deformers active so they can be baked simultaneously in their correct order.
    disabled_mods = []
    for mod in obj.modifiers:
        if mod not in active_deformers:
            if mod.show_viewport:
                mod.show_viewport = False
                disabled_mods.append(mod)
        else:
            mod.show_viewport = True
                
    # Update depsgraph to obtain clean shrinkwrapped rest-state coordinates without animation/constraint cycle noise
    bpy.context.view_layer.update()
    temp_depsgraph = bpy.context.evaluated_depsgraph_get()
    
    try:
        evaluated_obj = obj.evaluated_get(temp_depsgraph)
        evaluated_mesh = evaluated_obj.data
        
        if len(evaluated_mesh.vertices) == len(obj.data.vertices):
            new_basis_cos = [v.co.copy() for v in evaluated_mesh.vertices]
            
            # Shape key copy logic
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
            # Standard copy logic (e.g. for the Head)
            else:
                for i, co in enumerate(new_basis_cos):
                    obj.data.vertices[i].co = co
                print(f"  [+] Overwrote base vertex coordinates.")
                
            # Remove baked deformers
            for mod in active_deformers:
                obj.modifiers.remove(mod)
        else:
            print(f"  [-] Vertex count mismatch on '{obj.name}'. Deformers skipped.")
    except Exception as e:
        print(f"  [-] Failed to bake coordinates on '{obj.name}': {e}")
    finally:
        # Restore non-deforming modifiers (Subsurf, Data Transfer)
        for mod in disabled_mods:
            mod.show_viewport = True
        # Restore original shape key morph values
        if has_shape_keys:
            for kb_name, val in old_key_values.items():
                kb = obj.data.shape_keys.key_blocks.get(kb_name)
                if kb: kb.value = val

# Unmute all bone and object constraints
for c in muted_bone_constraints:
    c.mute = False
for c in muted_object_constraints:
    c.mute = False

# Re-enable Armature modifiers
for m in disabled_armatures:
    m.show_viewport = True

bpy.context.view_layer.update()

# ---------------------------------------------------------------------------
# 5. APPLY STATIC MODIFIERS (MESHES WITHOUT SHAPE KEYS ONLY)
# ---------------------------------------------------------------------------
SKIP_MOD_TYPES = {'ARMATURE', 'SHRINKWRAP'}  # Shrinkwrap already handled above

print("[*] Applying static modifiers on non-shape-keyed meshes...")
for obj in list(bpy.data.objects):
    if obj.type != 'MESH' or obj.data.shape_keys:
        continue
    candidate_mods = [m.name for m in obj.modifiers if m.type not in SKIP_MOD_TYPES]
    if not candidate_mods:
        continue
    if not safe_set_active(obj):
        continue
    for m_name in candidate_mods:
        try:
            bpy.ops.object.modifier_apply(modifier=m_name)
        except Exception as e:
            print(f"[-] Modifier apply failed on {obj.name}/{m_name}: {e}")
    obj.select_set(False)

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
#     1. Delete helper cages completely so they don't leak.
#     2. Force-enable visibility for core character meshes (eyes, pupils, lids)
#        so they are exported, while keeping optional outfits (swimsuit, caps) 
#        hidden so glTF 'use_visible=True' filters them out.
# ---------------------------------------------------------------------------
print("[*] Cleaning up remaining shrinkwrap targets and helper meshes before export...")

# Remove targets identified as helper/corrective cages (e.g. pomni_headshrink)
for target_name in list(helpers_to_clean):
    target_obj = bpy.data.objects.get(target_name)
    if target_obj:
        print(f"[~] Removing shrinkwrap helper cage: {target_name}")
        bpy.data.objects.remove(target_obj, do_unlink=True)

# Force-enable visibility for core character meshes to prevent silent omission
CORE_CHARACTER_MESHES = {
    "pomni_eyes", "pomni_pupils", "pomni_eyelidsL", "pomni_eyelidsR", 
    "pomni_eyelidsTOP", "pomni_head.002", "pomni_body", "pomni_arms", 
    "pomni_neck", "pomni_collar", "pomni_eyebrows", "pomni_eyelashes", 
    "pomni_hairFront", "pomni_hairSide", "pomni_hat", "pomni_hatBorder", 
    "pomni_hip", "pomni_teethbot", "pomni_teethtop", "pomni_tongue",
    "pomni_mouth", "pomni_gloveballs", "pomni_balls", "pomni_ballsbody",
    "pomni_ballshat"
}

# Delete any mesh that is NOT in CORE_CHARACTER_MESHES
# This completely purges all alternative/optional costumes ( swimsuits, caps, etc.)
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
# 7. EXPORT TO GLB
# ---------------------------------------------------------------------------
print("[*] Exporting to clean GLB...")
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
    use_visible=False,
    export_cameras=False,
    export_lights=False,
)
print("[+] GLB export complete.")
"""

    safe_glb_path = glb_path.replace("\\", "\\\\")
    expr = expr.replace("__GLB_PATH__", safe_glb_path)

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

    cmd = ["blender", "-b", blend_path, "--python-expr", expr]
    print(
        f"[+] Exporting {os.path.basename(blend_path)} via Blender (streaming output)..."
    )

    result = subprocess.run(cmd, env=env)

    if result.returncode != 0 or not os.path.exists(glb_path):
        print("[-] Blender export failed.")
        return False

    return True
