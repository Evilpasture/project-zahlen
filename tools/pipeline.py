# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# tools/pipeline.py
import os
import sys
import argparse

# Inject local module path
sys.path.append(os.path.dirname(os.path.abspath(__file__)))
from asset_utils.export_pomni import export_blend_to_glb as export_pomni_to_glb
from asset_utils.export_uzi import export_uzi_to_glb
from asset_utils.cleaner import sanitize_glb
from asset_utils.baker import bake_material_textures


def dispatch_exporter(input_path, output_path, character_override="auto") -> bool:
    """Dispatches to the appropriate character exporter profile."""
    char_type = character_override.lower()
    input_lower = input_path.lower()

    if char_type == "uzi" or (char_type == "auto" and "uzi" in input_lower):
        print("[*] Profile Selected: Uzi (Toxilisk Rig Exporter)")
        return export_uzi_to_glb(input_path, output_path)
    elif char_type == "pomni" or (char_type == "auto" and "pomni" in input_lower):
        print("[*] Profile Selected: Pomni (Rigify Exporter)")
        return export_pomni_to_glb(input_path, output_path)
    else:
        # Fallback default
        print("[*] Profile Auto-Detection: Defaulting to Pomni Exporter")
        return export_pomni_to_glb(input_path, output_path)


def main():
    parser = argparse.ArgumentParser(description="Unified Asset Processing Pipeline")
    parser.add_argument("--input", required=True, help="Path to raw .blend asset")
    parser.add_argument(
        "--output", required=True, help="Output destination path for .glb"
    )
    parser.add_argument(
        "--character",
        choices=["auto", "pomni", "uzi"],
        default="auto",
        help="Character profile to run (default: auto-detect from path)",
    )
    args = parser.get_args() if hasattr(parser, "get_args") else parser.parse_args()

    print(f"[*] Starting pipeline for: {args.input}")

    output_dir = os.path.dirname(args.output)
    if output_dir:
        os.makedirs(output_dir, exist_ok=True)

    # Step 1: Export via dispatched character profile
    if not dispatch_exporter(args.input, args.output, args.character):
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
