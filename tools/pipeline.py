# tools/pipeline.py
import os
import sys
import argparse

# Inject local module path
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from asset_utils.converter import export_blend_to_glb
from asset_utils.cleaner import sanitize_glb
from asset_utils.baker import bake_material_textures

def main():
    parser = argparse.ArgumentParser(description="Unified Asset Processing Pipeline")
    parser.add_argument("--input", required=True, help="Path to raw .blend asset")
    parser.add_argument("--output", required=True, help="Output destination path for .glb")
    args = parser.get_args() if hasattr(parser, 'get_args') else parser.parse_args()

    print(f"[*] Starting pipeline for: {args.input}")

    output_dir = os.path.dirname(args.output)
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)
    
    # Step 1: Export
    if not export_blend_to_glb(args.input, args.output):
        print("[-] Export failed.")
        sys.exit(1)

    # Step 2: Clean and Validate
    if not sanitize_glb(args.output):
        print("[-] Cleaning failed.")
        sys.exit(1)

    # Step 3: Texture Baking
    atlas_dir = os.path.join(os.path.dirname(args.output), "atlases")
    if not bake_material_textures(args.output, atlas_dir):
        print("[-] Texture baking failed.")
        sys.exit(1)

    print(f"[+] Pipeline completed. Cleaned asset ready at: {args.output}\n")

if __name__ == "__main__":
    main()