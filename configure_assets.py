#!/usr/bin/env python3
# configure_assets.py
import os
import sys
import struct


def escape_ninja(path):
    # Ninja uses '$ ' to escape spaces in target and dependency paths
    return path.replace(" ", "$ ")


class BinaryMetadataReader:
    """Helper to read the fast binary metadata stream directly in the build configure step."""

    def __init__(self, filepath):
        self.f = open(filepath, "rb")
        magic = self.f.read(4)
        if magic != b"ZMET":
            raise ValueError("Invalid magic header")
        self.version = struct.unpack("<I", self.f.read(4))[0]

    def close(self):
        self.f.close()

    def read_string(self):
        length_bytes = self.f.read(4)
        if not length_bytes or len(length_bytes) < 4:
            return ""
        length = struct.unpack("<I", length_bytes)[0]
        if length == 0:
            return ""
        return self.f.read(length).decode("utf-8")

    def read_fmt(self, fmt):
        size = struct.calcsize("<" + fmt)
        data = self.f.read(size)
        if len(data) < size:
            raise EOFError(f"Expected {size} bytes, got {len(data)}")
        return struct.unpack("<" + fmt, data)

    def read_floats(self, count):
        if count == 0:
            return []
        return list(self.read_fmt(f"{count}f"))


def parse_metadata_bin(filepath):
    """Unpacks metadata.bin directly to extract meshes and animations for Ninja rule generation."""
    reader = BinaryMetadataReader(filepath)
    try:
        manifest = {
            "materials": [],
            "meshes": [],
            "nodes": [],
            "lights": [],
            "skins": [],
            "animations": [],
        }

        # 1. Level Info
        scene_name = reader.read_string()
        manifest["scene_info"] = {"name": scene_name}

        # 2. Materials
        mat_count = reader.read_fmt("I")[0]
        for _ in range(mat_count):
            _ = reader.read_string()  # id
            _ = reader.read_floats(4)  # base_color
            _ = reader.read_fmt("f")[0]  # metallic
            _ = reader.read_fmt("f")[0]  # roughness
            _ = reader.read_floats(3)  # emissive_factor
            _ = reader.read_fmt("f")[0]  # emissive_strength
            _ = reader.read_fmt("B")[0]  # double_sided
            _ = reader.read_string()  # albedo map
            _ = reader.read_string()  # normal map
            _ = reader.read_string()  # metallic_roughness map
            _ = reader.read_string()  # emissive map
            has_procedural = reader.read_fmt("B")[0]
            if has_procedural:
                _ = reader.read_string()  # type
                _ = reader.read_fmt("ff")  # scale, randomness

        # 3. Meshes
        mesh_count = reader.read_fmt("I")[0]
        for _ in range(mesh_count):
            mesh_id = reader.read_string()
            _ = reader.read_string()  # layout
            bin_file = reader.read_string()
            byte_offset, byte_length = reader.read_fmt("II")

            # Primitives
            prim_count = reader.read_fmt("I")[0]
            for _ in range(prim_count):
                _ = reader.read_string()  # material_id
                _, _ = reader.read_fmt("II")  # vertex_offset, vertex_count

            # Morph targets
            morph_count = reader.read_fmt("I")[0]
            for _ in range(morph_count):
                _ = reader.read_string()  # target_name
                _ = reader.read_string()  # target_bin
                _, _ = reader.read_fmt("II")  # t_offset, t_length

            manifest["meshes"].append(
                {
                    "id": mesh_id,
                    "buffers": {
                        "bin_file": bin_file,
                        "vertex_buffer": {
                            "byte_offset": byte_offset,
                            "byte_length": byte_length,
                        },
                    },
                }
            )

        # 4. Nodes
        node_count = reader.read_fmt("I")[0]
        for _ in range(node_count):
            _ = reader.read_string()  # id
            _ = reader.read_string()  # parent_id
            _ = reader.read_fmt("B")[0]  # visible
            _ = reader.read_floats(16)  # local transform
            _ = reader.read_floats(16)  # world transform
            _ = reader.read_string()  # mesh_id
            _ = reader.read_string()  # skin_id
            _ = reader.read_string()  # light_id

        # 5. Lights
        light_count = reader.read_fmt("I")[0]
        for _ in range(light_count):
            _ = reader.read_string()  # id
            _ = reader.read_string()  # type
            _ = reader.read_floats(3)  # color
            _ = reader.read_fmt("f")[0]  # intensity

        # 6. Skins
        skin_count = reader.read_fmt("I")[0]
        for _ in range(skin_count):
            _ = reader.read_string()  # id
            _ = reader.read_string()  # name

            joint_count = reader.read_fmt("I")[0]
            for _ in range(joint_count):
                _ = reader.read_string()

            parent_count = reader.read_fmt("I")[0]
            for _ in range(parent_count):
                _ = reader.read_string()

            ibm_count = reader.read_fmt("I")[0]
            _ = reader.read_floats(ibm_count)

            rest_count = reader.read_fmt("I")[0]
            _ = reader.read_floats(rest_count)

        # 7. Animations
        anim_count = reader.read_fmt("I")[0]
        for _ in range(anim_count):
            anim_id = reader.read_string()
            _ = reader.read_string()  # name
            _, _ = reader.read_fmt("fB")  # duration, loop

            chan_count = reader.read_fmt("I")[0]
            for _ in range(chan_count):
                _ = reader.read_string()  # target_node_id
                _ = reader.read_string()  # target_path
                _ = reader.read_fmt("I")  # sampler_id

            samplers = []
            samp_count = reader.read_fmt("I")[0]
            for _ in range(samp_count):
                _ = reader.read_string()  # interpolation
                _, _, _, _ = reader.read_fmt("IIII")  # input/output offsets/lengths
                bin_file = reader.read_string()
                samplers.append({"bin_file": bin_file})

            manifest["animations"].append({"id": anim_id, "samplers": samplers})

        return manifest
    finally:
        reader.close()


def discover_blend_files(search_path):
    """Recursively scans directories to discover valid Blend files."""
    blend_files = []
    for root, dirs, files in os.walk(search_path):
        # Prevent os.walk from scanning build, system, and external library directories
        dirs[:] = [
            d
            for d in dirs
            if d.lower()
            not in [
                "build",
                "cmake",
                ".git",
                ".github",
                "bin",
                "extern",
                "third_party",
                "build_assets",
            ]
        ]

        norm_root = root.replace("\\", "/").lower()
        if any(
            p in norm_root
            for p in [
                "resources/intermediate",
                "exported_assets",
            ]
        ):
            continue
        for file in files:
            if file.endswith(".blend") and not file.startswith("."):
                if "void" in file.lower():
                    continue
                blend_files.append(os.path.join(root, file).replace("\\", "/"))
    return blend_files


def generate_ninja(output_file, zcook_executable, source_dir):
    escaped_zcook = escape_ninja(zcook_executable)
    escaped_script = escape_ninja(
        os.path.join(source_dir, "tools", "export_metadata.py").replace("\\", "/")
    )

    # 1. ADDED: Resolve and escape path to the run_blender.py environment wrapper
    escaped_wrapper = escape_ninja(
        os.path.join(source_dir, "tools", "run_blender.py").replace("\\", "/")
    )

    intermediate_root = os.path.join(source_dir, "resources", "intermediate").replace(
        "\\", "/"
    )

    # 2. CHANGED: Updated the blender_extract command to execute through run_blender.py
    ninja_content = f"""# Automatically generated by configure_assets.py
ninja_required_version = 1.3
builddir = build_assets

rule blender_extract
  command = python3 "{escaped_wrapper}" blender -b $in -P "{escaped_script}" -- $in "{intermediate_root}"
  description = BLENDER $in

rule zmesh
  command = "{escaped_zcook}" mesh --meta "$meta" --id "$id" -i $in -o $out
  description = ZMESH $id

rule zanim
  command = "{escaped_zcook}" anim --meta "$meta" --id "$id" -o $out
  description = ZANIM $id

rule ztex
  command = "{escaped_zcook}" tex -i $in -o $out
  description = ZTEX $in

rule zglb
  command = "{escaped_zcook}" glb --meta $in -o $out
  description = ZGLB $in

rule zpak
  command = "{escaped_zcook}" pak -o $out -i $in
  description = ZPAK $out
  pool = console
"""

    compiled_targets = []
    glb_targets = []
    manifest_entries = []

    # Find master level blend sources and sort them for binary determinism
    blend_files = sorted(discover_blend_files(source_dir))

    # Pre-calculate namespaced metadata dependencies using absolute paths
    # This ensures both the build graph and generator rules share identical namespaced targets
    meta_deps_list = []
    for b in blend_files:
        abs_blend = os.path.abspath(b)
        abs_source = os.path.abspath(source_dir)
        rel_path = os.path.relpath(abs_blend, abs_source)
        level = os.path.splitext(rel_path)[0].replace("\\", "/").replace("/", "_")
        meta_deps_list.append(
            os.path.join(intermediate_root, level, "metadata.bin").replace("\\", "/")
        )

    meta_deps = " ".join([escape_ninja(m) for m in meta_deps_list])

    # 3. Output blender extraction targets and map intermediate outputs
    for blend_path in blend_files:
        abs_blend = os.path.abspath(blend_path)
        abs_source = os.path.abspath(source_dir)
        rel_path = os.path.relpath(abs_blend, abs_source)
        level = os.path.splitext(rel_path)[0].replace("\\", "/").replace("/", "_")

        level_dir = os.path.join(intermediate_root, level).replace("\\", "/")
        meta_path = os.path.join(level_dir, "metadata.bin").replace("\\", "/")

        # 4. CHANGED: Added escaped_wrapper as an implicit dependency to trigger extraction if run_blender.py is edited
        ninja_content += f"\nbuild {escape_ninja(meta_path)}: blender_extract {escape_ninja(blend_path)} | {escaped_script} {escaped_wrapper}\n"

        if os.path.exists(meta_path):
            try:
                manifest = parse_metadata_bin(meta_path)
            except Exception as e:
                print(f"[Warning] Failed to parse {meta_path}: {e}")
                manifest = {}

            # Map individual Meshes
            for mesh in manifest.get("meshes", []):
                mesh_id = mesh.get("id")
                if not mesh_id:
                    continue

                bin_name = mesh.get("buffers", {}).get("bin_file")
                if not bin_name:
                    continue

                bin_file = os.path.join(level_dir, bin_name).replace("\\", "/")
                output_zmesh = f"build_assets/{level}/{mesh_id}.zmesh"
                virtual_path = f"{mesh_id}.zmesh"

                # Changed zcook dependency to order-only (||) so recompiling engine doesn't rebuild assets
                ninja_content += f"\nbuild {escape_ninja(output_zmesh)}: zmesh {escape_ninja(bin_file)} | {escape_ninja(meta_path)} || {escaped_zcook}\n"
                ninja_content += f"  meta = {meta_path}\n"
                ninja_content += f"  id = {mesh_id}\n"
                compiled_targets.append(output_zmesh)
                manifest_entries.append(f"{virtual_path}={output_zmesh}")

            # Map individual Animations
            for anim in manifest.get("animations", []):
                anim_id = anim.get("id")
                if not anim_id:
                    continue

                # Locate target animation keyframe .bin dependency via the first sampler's binary file
                samplers = anim.get("samplers", [])
                if not samplers:
                    continue
                bin_name = samplers[0].get("bin_file")
                if not bin_name:
                    continue

                bin_file = os.path.join(level_dir, bin_name).replace("\\", "/")
                output_zanim = f"build_assets/{level}/{anim_id}.zanim"
                virtual_path = f"{anim_id}.zanim"

                # Changed zcook dependency to order-only (||) so recompiling engine doesn't rebuild assets
                ninja_content += f"\nbuild {escape_ninja(output_zanim)}: zanim {escape_ninja(bin_file)} | {escape_ninja(meta_path)} || {escaped_zcook}\n"
                ninja_content += f"  meta = {meta_path}\n"
                ninja_content += f"  id = {anim_id}\n"
                compiled_targets.append(output_zanim)
                manifest_entries.append(f"{virtual_path}={output_zanim}")

            # Map Intermediate Textures
            tex_dir = os.path.join(level_dir, "textures")
            if os.path.exists(tex_dir):
                # Sort directory listing to keep target generation order stable
                for tex in sorted(os.listdir(tex_dir)):
                    in_path = os.path.join(tex_dir, tex).replace("\\", "/")
                    out_path = f"build_assets/{level}/lvl_{tex}.ztex"
                    virtual_path = f"textures/{tex}"

                    # Changed zcook dependency to order-only (||) so recompiling engine doesn't rebuild assets
                    ninja_content += f"build {escape_ninja(out_path)}: ztex {escape_ninja(in_path)} | {escape_ninja(meta_path)} || {escaped_zcook}\n"
                    compiled_targets.append(out_path)
                    manifest_entries.append(f"{virtual_path}={out_path}")

            # Emit Standard GLB Target
            output_glb = f"build_assets/debug_glb/{level}.glb"
            bin_dependencies = []
            for mesh in manifest.get("meshes", []):
                bin_name = mesh.get("buffers", {}).get("bin_file")
                if bin_name:
                    bin_dependencies.append(
                        os.path.join(level_dir, bin_name).replace("\\", "/")
                    )

            bin_dependencies = list(set(bin_dependencies))
            escaped_deps = " ".join([escape_ninja(d) for d in sorted(bin_dependencies)])

            # Changed zcook dependency to order-only (||) so recompiling engine doesn't rebuild assets
            ninja_content += f"\nbuild {escape_ninja(output_glb)}: zglb {escape_ninja(meta_path)} | {escaped_deps} || {escaped_zcook}\n"
            glb_targets.append(output_glb)

    # 2. Process Raw Loose Textures
    assets_root = os.path.join(source_dir, "resources", "assets")
    if os.path.exists(assets_root):
        raw_textures = []
        for root, dirs, files in os.walk(assets_root):
            dirs[:] = [d for d in dirs if d.lower() not in ["build", "cmake", ".git"]]
            for file in files:
                if file.lower().endswith((".png", ".jpg", ".jpeg", ".tga")):
                    raw_textures.append(os.path.join(root, file).replace("\\", "/"))

        # Sort paths to keep the rule block output order consistent
        for input_path in sorted(raw_textures):
            rel_path = os.path.relpath(input_path, assets_root).replace("\\", "/")
            output_ztex = f"build_assets/raw/{rel_path}.ztex"

            # Changed zcook dependency to order-only (||) so recompiling engine doesn't rebuild assets
            ninja_content += f"build {escape_ninja(output_ztex)}: ztex {escape_ninja(input_path)} || {escaped_zcook}\n"
            compiled_targets.append(output_ztex)
            manifest_entries.append(f"{rel_path}={output_ztex}")

    # 3. Write Manifest
    build_dir = os.path.dirname(output_file)
    manifest_dir = os.path.join(build_dir, "build_assets")
    os.makedirs(manifest_dir, exist_ok=True)
    manifest_target = os.path.join(manifest_dir, "manifest.txt").replace("\\", "/")

    # Ensure manifest entries are sorted alphabetically
    manifest_entries.sort()
    with open(manifest_target, "w") as f:
        f.write("\n".join(manifest_entries))

    # 4. Final pack step: data/base.pak depends on the manifest, all compiled targets, and all metadata.bin files
    escaped_targets = [escape_ninja(t) for t in sorted(compiled_targets)]
    escaped_manifest = escape_ninja(manifest_target)
    escaped_meta_deps = [escape_ninja(m) for m in sorted(meta_deps_list)]

    # Added zcook as an order-only dependency (||)
    ninja_content += f"\nbuild data/base.pak: zpak {escaped_manifest} | {' '.join(escaped_targets)} {' '.join(escaped_meta_deps)} || {escaped_zcook}\n"

    # 5. Virtual targets
    escaped_glbs = [escape_ninja(g) for g in sorted(glb_targets)]
    ninja_content += f"\nbuild debug_glbs: phony {' '.join(escaped_glbs)}\n"

    # 6. Self-Regeneration details
    escaped_configure = escape_ninja(
        os.path.join(source_dir, "configure_assets.py").replace("\\", "/")
    )
    escaped_output = escape_ninja(output_file)
    escaped_source = escape_ninja(source_dir)

    blend_deps = " ".join([escape_ninja(b) for b in blend_files])

    ninja_content += "\nrule regenerate_ninja\n"
    ninja_content += f"  command = python3 {escaped_configure} {escaped_output} {escaped_zcook} {escaped_source}\n"
    ninja_content += "  description = Regenerating assets.ninja\n"
    ninja_content += "  generator = 1\n"

    # 5. CHANGED: Added escaped_wrapper to regeneration dependencies
    ninja_content += f"\nbuild {escaped_output}: regenerate_ninja {escaped_configure} | {blend_deps} {meta_deps} {escaped_script} {escaped_wrapper}\n"

    ninja_content += "\ndefault data/base.pak\n"

    with open(output_file, "w") as f:
        f.write(ninja_content)


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print(
            "Usage: configure_assets.py <output_ninja_file> <zcook_executable> <source_dir>"
        )
        sys.exit(1)

    generate_ninja(sys.argv[1], sys.argv[2], sys.argv[3])
