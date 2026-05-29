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

    # Standard triple-quoted Python script block (Zero baking, pure standard pipelines)
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

# --- 1. PRUNE AUXILIARY FILM HELPERS & SHADOW MESHES ---
# Deletes helper meshes, widgets, corrective cages, and non-standard film shadow rings
print("[*] Pruning auxiliary rigging meshes, shadow meshes, and deformer cages...")
for obj in list(bpy.data.objects):
    if obj.type == 'MESH':
        name_lower = obj.name.lower()
        if any(x in name_lower for x in ["shrink", "helper", "proxy", "cage", "bounds", "corrective", "wgts", "widget", "shadow"]):
            bpy.data.objects.remove(obj, do_unlink=True)

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

# --- 3. AUTO-SIMPLIFY COMPLEX SHADERS TO STANDARD PBR (NO-BAKE PIPELINE) ---
# Programmatically scans her original material node graphs and converts them to 
# clean, standard, glTF-compliant PBR pipelines on the fly.
print("[*] Automatically simplifying complex shader trees to clean PBR...")
export_meshes = [o for o in bpy.data.objects if o.type == 'MESH' and not o.hide_viewport]
baked_materials = {}

for obj in export_meshes:
    has_uvs = len(obj.data.uv_layers) > 0
    
    for mat_slot in obj.material_slots:
        mat = mat_slot.material
        if not mat or not mat.use_nodes or not mat.node_tree:
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
        
        # Scan original nodes
        nodes = mat.node_tree.nodes
        tex_node = next((n for n in nodes if n.type == 'TEX_IMAGE'), None)
        color_node = next((n for n in nodes if n.type in ('VERTEX_COLOR', 'COLOR_ATTRIBUTE')), None)

        # Path A: Material contains a pre-existing Image Texture (face skin, hair, eyes, pupils)
        if tex_node and tex_node.image:
            print(f"[+] Re-linking hand-painted texture {tex_node.image.name} for {mat.name}...")
            img_node = new_nodes.new('ShaderNodeTexImage')
            img_node.image = tex_node.image
            new_links.new(img_node.outputs['Color'], pbr_node.inputs['Base Color'])
            new_links.new(img_node.outputs['Alpha'], pbr_node.inputs['Alpha']) # LINK ALPHA FOR TRANSPARENCY

        # Path B: Material is vertex-color driven (jester suit pomni_mat)
        elif color_node:
            print(f"[+] Re-linking vertex colors for {mat.name}...")
            try:
                color_attr = new_nodes.new('ShaderNodeVertexColor')
            except RuntimeError:
                color_attr = new_nodes.new('ShaderNodeVertexColor')
            color_attr.layer_name = "Color"
            new_links.new(color_attr.outputs['Color'], pbr_node.inputs['Base Color'])
            
        # Path C: Flat color fallback
        else:
            print(f"[*] Assigning flat color for {mat.name}...")
            base_color = mat.diffuse_color
            pbr_old = next((n for n in nodes if n.type == 'BSDF_PRINCIPLED'), None)
            if pbr_old:
                base_color = pbr_old.inputs['Base Color'].default_value
            pbr_node.inputs['Base Color'].default_value = base_color
            
        mat_slot.material = new_mat
        baked_materials[cache_key] = new_mat

bpy.context.view_layer.update()

# --- 4. EXPORT TO GLB ---
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
    print(f"[+] Formatting and exporting {os.path.basename(blend_path)} using Blender...")
    
    # Inherit parent stdout/stderr to stream execution details live
    result = subprocess.run(cmd, env=env)
    
    if result.returncode != 0 or not os.path.exists(glb_path):
        print(f"[-] Blender export failed. See tracebacks in terminal logs above.")
        return False

    return True