# tools/asset_utils/converter.py
import subprocess
import os

def export_blend_to_glb(blend_path: str, glb_path: str) -> bool:
    """Runs Blender in background mode to export a .blend file directly to .glb."""
    blend_path = os.path.abspath(blend_path)
    glb_path = os.path.abspath(glb_path)

    if not os.path.exists(blend_path):
        print(f"[-] Source blend file not found: {blend_path}")
        return False

    # Standard triple-quoted Python script block
    expr = """
import bpy
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

# 1. Convert direct Bone Parenting AND Bone Constraints (Child Of, Copy Transforms) to Skeletal Skinning
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

bpy.context.view_layer.update()

# 2. Apply all non-Armature modifiers (Lattice, Shrinkwrap, Mirror) on meshes that do NOT have shape keys
for obj in list(bpy.data.objects):
    if obj.type == 'MESH' and not obj.data.shape_keys:
        other_mods = [m.name for m in obj.modifiers if m.type != 'ARMATURE']
        if other_mods:
            bpy.context.view_layer.objects.active = obj
            obj.select_set(True)
            for m_name in other_mods:
                try:
                    bpy.ops.object.modifier_apply(modifier=m_name)
                except Exception as e:
                    print(f'[-] Modifier apply failed on {obj.name}: {e}')
            obj.select_set(False)

bpy.context.view_layer.update()

# 3. Export Scene to GLB (use_visible excludes viewport-hidden items safely)
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
    print(f"[+] Exporting {os.path.basename(blend_path)} using Blender...")
    
    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    
    if result.returncode != 0 or not os.path.exists(glb_path):
        print(f"[-] Blender export failed. Headless execution logs and Python tracebacks:")
        print(result.stdout)
        if result.stderr:
            print(result.stderr)
        return False

    return True