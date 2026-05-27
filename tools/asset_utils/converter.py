# tools/asset_utils/converter.py
import subprocess
import os

def export_blend_to_glb(blend_path: str, glb_path: str) -> bool:
    """Runs Blender in background mode to export a .blend file directly to .glb."""
    if not os.path.exists(blend_path):
        print(f"[-] Source blend file not found: {blend_path}")
        return False

    # Inline python expression passed to Blender to configure the export settings.
    # Disabling unreferenced data saves memory before the engine cooks the asset.
    expr = (
        "import bpy; "
        "bpy.ops.export_scene.gltf("
        f"filepath='{glb_path}', "
        "export_format='GLB', "
        "export_skins=True, "
        "export_morph=True, "
        "export_tangents=True, "
        "export_colors=True, "
        "export_normals=True, "
        "export_apply=True, "
        "export_attributes=False"  # Excludes unreferenced custom properties
        ")"
    )

    cmd = ["blender", "-b", blend_path, "--python-expr", expr]
    print(f"[+] Exporting {os.path.basename(blend_path)} using Blender...")
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"[-] Blender export failed:\n{result.stderr}")
        return False
    return True