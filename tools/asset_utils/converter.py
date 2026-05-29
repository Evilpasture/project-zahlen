# tools/asset_utils/converter.py
import subprocess
import os

def export_blend_to_glb(blend_path: str, glb_path: str) -> bool:
    """Runs Blender in background mode to bake complex shaders and export to .glb."""
    blend_path = os.path.abspath(blend_path)
    glb_path = os.path.abspath(glb_path)

    if not os.path.exists(blend_path):
        print(f"[-] Source blend file not found: {blend_path}")
        return False

    # Standard triple-quoted Python script block
    expr = """
import bpy
import os

bpy.ops.file.pack_all()

# Ensure we are in Object Mode before modifying constraints or modifiers
if bpy.context.object and bpy.context.object.mode != 'OBJECT':
    bpy.ops.object.mode_set(mode='OBJECT')

rigs = [o for o in bpy.data.objects if o.type == 'ARMATURE']
rig = next((r for r in rigs if 'pomni' in r.name.lower()), rigs[0] if rigs else None)

if rig:
    menu_bone = next((b for b in rig.pose.bones if 'menu' in b.name.lower()), None)
    if menu_bone:
        for prop in ['Sunglasses', 'Possessed Toggle', 'Outfit']:
            if prop in menu_bone:
                menu_bone[prop] = 0.0

bpy.context.view_layer.update()

# --- 1. CONVERT BONE-PARENTING / CONSTRAINTS & UNPARENT SKINNED MESHES ---
for obj in list(bpy.data.objects):
    if obj.type == 'MESH':
        target_armature = None
        target_bone = None
        if obj.parent and obj.parent_type == 'BONE':
            target_armature = obj.parent
            target_bone = obj.parent_bone
            mw = obj.matrix_world.copy()
            obj.parent = None
            obj.matrix_world = mw
        else:
            for con in list(obj.constraints):
                if con.type in ('CHILD_OF', 'COPY_TRANSFORMS', 'COPY_LOCATION') and con.target and con.target.type == 'ARMATURE':
                    if hasattr(con, 'subtarget') and con.subtarget:
                        target_armature = con.target
                        target_bone = con.subtarget
                        mw = obj.matrix_world.copy()
                        obj.constraints.remove(con)
                        obj.matrix_world = mw
                        break
        if target_armature and target_bone:
            arm_mod = next((m for m in obj.modifiers if m.type == 'ARMATURE'), None)
            if not arm_mod:
                arm_mod = obj.modifiers.new(name='ArmatureSkin', type='ARMATURE')
                arm_mod.object = target_armature
            vg = obj.vertex_groups.get(target_bone) or obj.vertex_groups.new(name=target_bone)
            vg.add([v.index for v in obj.data.vertices], 1.0, 'REPLACE')

        # Clear parent for any Skinned Mesh (avoids NODE_SKINNED_MESH_NON_ROOT warnings)
        if obj.parent:
            has_arm_mod = any(m.type == 'ARMATURE' for m in obj.modifiers)
            if has_arm_mod:
                mw = obj.matrix_world.copy()
                obj.parent = None
                obj.matrix_world = mw

bpy.context.view_layer.update()

# --- 2. APPLY STATIC MODIFIERS (ON MESHES WITHOUT SHAPE KEYS) ---
for obj in list(bpy.data.objects):
    if obj.type == 'MESH' and not obj.data.shape_keys:
        other_mods = [m.name for m in obj.modifiers if m.type != 'ARMATURE']
        if other_mods:
            # Guard against View Layer exclusion errors
            try:
                bpy.context.view_layer.objects.active = obj
                obj.select_set(True)
            except RuntimeError:
                continue # Safely skip if excluded from active View Layer
                
            for m_name in other_mods:
                try:
                    bpy.ops.object.modifier_apply(modifier=m_name)
                except Exception as e:
                    print(f'[-] Modifier apply failed on {obj.name}: {e}')
            obj.select_set(False)

bpy.context.view_layer.update()

# --- 3. AUTO-BAKE COMPLEX SHADERS TO FLAT PBR ---
print("[*] Setting up Cycles for automated flat-color baking...")
bpy.context.scene.render.engine = 'CYCLES'
bpy.context.scene.cycles.device = 'CPU'      # CPU is safest for background headless server environments
bpy.context.scene.cycles.samples = 1         # 1 Sample is extremely fast and perfect for flat unlit color maps

# Target only meshes actually selected for export (visible)
export_meshes = [o for o in bpy.data.objects if o.type == 'MESH' and not o.hide_viewport]
baked_materials = {}

# Materials driven purely by Vertex Colors in our engine shader (Skip baking to avoid UV distortion)
SKIP_BAKE_MATERIALS = ["pomni_mat"]

for obj in export_meshes:
    # Guard against View Layer exclusion errors during baking
    try:
        bpy.ops.object.select_all(action='DESELECT')
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
    except RuntimeError:
        continue # Safely skip if excluded from active View Layer
    
    # Check if the mesh actually has UV coordinates
    has_uvs = len(obj.data.uv_layers) > 0
    
    for mat_slot in obj.material_slots:
        mat = mat_slot.material
        if not mat or not mat.use_nodes:
            continue
            
        # Deduplicate using UV-aware cache key to prevent MESH_PRIMITIVE_TOO_FEW_TEXCOORDS
        cache_key = (mat.name, has_uvs)
        if cache_key in baked_materials:
            mat_slot.material = baked_materials[cache_key]
            continue

        # Create a clean, glTF-compliant material replacement
        suffix = "Baked_Tex" if has_uvs else "Baked_Flat"
        new_mat = bpy.data.materials.new(name=f"{mat.name}_{suffix}")
        
        # Preserves alpha blend methods and backface settings for the exporter
        new_mat.blend_method = mat.blend_method
        new_mat.alpha_threshold = mat.alpha_threshold
        new_mat.use_backface_culling = mat.use_backface_culling

        new_mat.use_nodes = True
        new_nodes = new_mat.node_tree.nodes
        new_links = new_mat.node_tree.links
        new_nodes.clear()
        
        pbr_node = new_nodes.new('ShaderNodeBsdfPrincipled')
        output_node = new_nodes.new('ShaderNodeOutputMaterial')
        new_links.new(pbr_node.outputs['BSDF'], output_node.inputs['Surface'])

        # Path A: Mesh has UVs. Perform fast baking.
        if has_uvs and (mat.name not in SKIP_BAKE_MATERIALS):
            bake_name = f"Bake_{mat.name}"
            img = bpy.data.images.new(bake_name, width=2048, height=2048)
            
            nodes = mat.node_tree.nodes
            links = mat.node_tree.links
            
            # Inject temporary target bake node
            tex_node = nodes.new('ShaderNodeTexImage')
            tex_node.image = img
            nodes.active = tex_node
            
            print(f"[+] Baking material: {mat.name} on {obj.name}...")
            try:
                bpy.ops.object.bake(type='DIFFUSE', pass_filter={'COLOR'}, margin=16)
            except Exception as e:
                print(f"[-] Bake failed on {mat.name}: {e}")
                nodes.remove(tex_node)
                mat_slot.material = new_mat
                baked_materials[cache_key] = new_mat
                continue
                
            # Save image file to your engine's destination build folder
            build_dir = os.path.dirname('__GLB_PATH__')
            img_path = os.path.join(build_dir, f"{bake_name}.png")
            img.filepath_raw = img_path
            img.save()
            
            img_node = new_nodes.new('ShaderNodeTexImage')
            img_node.image = img
            new_links.new(img_node.outputs['Color'], pbr_node.inputs['Base Color'])
            
        # Path B: Mesh has NO UVs or is in the SKIP list. Map direct flat color/vertex color fallback.
        else:
            print(f"[*] Skipping bake for {obj.name} (Using flat diffuse/vertex color fallback)...")
            
            # Use 'ShaderNodeVertexColor' directly — works on all Blender versions including the latest
            color_attr = new_nodes.new('ShaderNodeVertexColor')
            color_attr.layer_name = "Color" # Uses 'layer_name' to select the active Color Attribute
            new_links.new(color_attr.outputs['Color'], pbr_node.inputs['Base Color'])
            
            base_color = mat.diffuse_color # Fallback to viewport color
            principled_old = next((n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
            if principled_old:
                base_color = principled_old.inputs['Base Color'].default_value
                
            pbr_node.inputs['Base Color'].default_value = base_color
            
        mat_slot.material = new_mat
        baked_materials[cache_key] = new_mat

bpy.context.view_layer.update()

# --- 4. EXPORT TO GLB ---
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
    use_visible=True
)
"""

    # Escape backslashes for Windows path safety during evaluation
    safe_glb_path = glb_path.replace('\\', '\\\\')
    expr = expr.replace('__GLB_PATH__', safe_glb_path)

    env = os.environ.copy()
    if "VIRTUAL_ENV" in env:
        venv_path = env["VIRTUAL_ENV"]
        paths = env.get("PATH", "").split(os.pathsep)
        clean_paths = [p for p in paths if not p.startswith(venv_path)]
        env["PATH"] = os.pathsep.join(clean_paths)
        del env["VIRTUAL_ENV"]
    
    env.pop("PYTHONHOME", None)
    env.pop("PYTHONPATH", None)

    cmd = ["blender", "-b", blend_path, "--python-expr", expr]
    print(f"[+] Baking and exporting {os.path.basename(blend_path)} using Blender...")
    
    # Inherit parent stdout/stderr to stream execution details live
    result = subprocess.run(cmd, env=env)
    
    if result.returncode != 0 or not os.path.exists(glb_path):
        print(f"[-] Blender export failed. See tracebacks in terminal logs above.")
        return False

    return True