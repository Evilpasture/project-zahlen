#!/usr/bin/env python3
# configure_assets.py
import os
import sys
import struct


def escape_ninja(path):
    # Ninja uses '$ ' to escape spaces in target and dependency paths
    return path.replace(" ", "$ ")


def write_if_changed(filepath, new_content):
    """Writes to filepath ONLY if the content has changed, preserving mtime otherwise."""
    if os.path.exists(filepath):
        try:
            with open(filepath, "r", encoding="utf-8") as f:
                if f.read() == new_content:
                    return False
        except Exception:
            pass
    with open(filepath, "w", encoding="utf-8") as f:
        f.write(new_content)
    return True


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
                param_count = reader.read_fmt("I")[0]
                for _ in range(param_count):
                    _ = reader.read_string()  # parameter name
                    float_count = reader.read_fmt("B")[0]
                    _ = reader.read_floats(float_count)  # parameter values

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
    # Locate engine tools directory relative to this script
    script_dir = os.path.dirname(os.path.abspath(__file__)).replace("\\", "/")

    escaped_zcook = escape_ninja(zcook_executable)
    escaped_script = escape_ninja(
        os.path.join(script_dir, "tools", "export_metadata.py").replace("\\", "/")
    )
    escaped_wrapper = escape_ninja(
        os.path.join(script_dir, "tools", "run_blender.py").replace("\\", "/")
    )
    escaped_configure = escape_ninja(
        os.path.join(script_dir, "configure_assets.py").replace("\\", "/")
    )

    intermediate_root = os.path.join(source_dir, "resources", "intermediate").replace(
        "\\", "/"
    )

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

    blend_files = sorted(discover_blend_files(source_dir))

    # Pre-calculate and escape all metadata dependencies
    escaped_meta_deps_list = []
    for b in blend_files:
        abs_blend = os.path.abspath(b)
        abs_source = os.path.abspath(source_dir)
        rel_path = os.path.relpath(abs_blend, abs_source)
        level = os.path.splitext(rel_path)[0].replace("\\", "/").replace("/", "_")
        meta_path = os.path.join(intermediate_root, level, "metadata.bin").replace(
            "\\", "/"
        )
        escaped_meta_deps_list.append(escape_ninja(meta_path))

    escaped_meta_deps = " ".join(escaped_meta_deps_list)

    for blend_path in blend_files:
        abs_blend = os.path.abspath(blend_path)
        abs_source = os.path.abspath(source_dir)
        rel_path = os.path.relpath(abs_blend, abs_source)
        level = os.path.splitext(rel_path)[0].replace("\\", "/").replace("/", "_")

        level_dir = os.path.join(intermediate_root, level).replace("\\", "/")
        meta_path = os.path.join(level_dir, "metadata.bin").replace("\\", "/")

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

                samplers = anim.get("samplers", [])
                if not samplers:
                    continue
                bin_name = samplers[0].get("bin_file")
                if not bin_name:
                    continue

                bin_file = os.path.join(level_dir, bin_name).replace("\\", "/")
                output_zanim = f"build_assets/{level}/{anim_id}.zanim"
                virtual_path = f"{anim_id}.zanim"

                ninja_content += f"\nbuild {escape_ninja(output_zanim)}: zanim {escape_ninja(bin_file)} | {escape_ninja(meta_path)} || {escaped_zcook}\n"
                ninja_content += f"  meta = {meta_path}\n"
                ninja_content += f"  id = {anim_id}\n"
                compiled_targets.append(output_zanim)
                manifest_entries.append(f"{virtual_path}={output_zanim}")

            # Map Intermediate Textures
            tex_dir = os.path.join(level_dir, "textures")
            if os.path.exists(tex_dir):
                for tex in sorted(os.listdir(tex_dir)):
                    in_path = os.path.join(tex_dir, tex).replace("\\", "/")
                    out_path = f"build_assets/{level}/lvl_{tex}.ztex"
                    virtual_path = f"textures/{tex}"

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

            ninja_content += f"\nbuild {escape_ninja(output_glb)}: zglb {escape_ninja(meta_path)} | {escaped_deps} || {escaped_zcook}\n"
            glb_targets.append(output_glb)

    # Process raw loose textures and reference GLBs.
    assets_root = os.path.join(source_dir, "resources", "assets")
    if os.path.exists(assets_root):
        raw_textures = []
        raw_models = []
        for root, dirs, files in os.walk(assets_root):
            dirs[:] = [d for d in dirs if d.lower() not in ["build", "cmake", ".git"]]
            for file in files:
                input_path = os.path.join(root, file).replace("\\", "/")
                if file.lower().endswith((".png", ".jpg", ".jpeg", ".tga")):
                    raw_textures.append(input_path)
                elif file.lower().endswith(".glb"):
                    raw_models.append(input_path)

        for input_path in sorted(raw_textures):
            rel_path = os.path.relpath(input_path, assets_root).replace("\\", "/")
            output_ztex = f"build_assets/raw/{rel_path}.ztex"

            ninja_content += f"build {escape_ninja(output_ztex)}: ztex {escape_ninja(input_path)} || {escaped_zcook}\n"
            compiled_targets.append(output_ztex)
            manifest_entries.append(f"{rel_path}={output_ztex}")

        # GLBs are already runtime-ready containers. Pack them byte-for-byte so
        # samples and games can load known-good reference rigs by virtual path.
        for input_path in sorted(raw_models):
            rel_path = os.path.relpath(input_path, assets_root).replace("\\", "/")
            compiled_targets.append(input_path)
            manifest_entries.append(f"{rel_path}={input_path}")

    # Write Manifest
    build_dir = os.path.dirname(output_file)
    manifest_dir = os.path.join(build_dir, "build_assets")
    os.makedirs(manifest_dir, exist_ok=True)
    manifest_target = os.path.join(manifest_dir, "manifest.txt").replace("\\", "/")

    manifest_entries.sort()
    write_if_changed(manifest_target, "\n".join(manifest_entries))

    # Final pack step
    escaped_targets = [escape_ninja(t) for t in sorted(compiled_targets)]
    escaped_manifest = escape_ninja(manifest_target)

    # FIXED: Joined pre-escaped metadata dependencies into ZPAK rule [5.5.0]
    ninja_content += f"\nbuild data/base.pak: zpak {escaped_manifest} | {' '.join(escaped_targets)} {escaped_meta_deps} || {escaped_zcook}\n"

    # Virtual targets
    escaped_glbs = [escape_ninja(g) for g in sorted(glb_targets)]
    ninja_content += f"\nbuild debug_glbs: phony {' '.join(escaped_glbs)}\n"

    # Self-Regeneration details
    escaped_output = escape_ninja(output_file)
    escaped_source = escape_ninja(source_dir)

    blend_deps = " ".join([escape_ninja(b) for b in blend_files])

    ninja_content += "\nrule regenerate_ninja\n"
    ninja_content += f"  command = python3 {escaped_configure} {escaped_output} {escaped_zcook} {escaped_source}\n"
    ninja_content += "  description = Regenerating assets.ninja\n"
    ninja_content += "  generator = 1\n"

    # FIXED: Joined pre-escaped metadata dependencies into self-regeneration rule [5.5.0]
    ninja_content += f"\nbuild {escaped_output}: regenerate_ninja {escaped_configure} | {blend_deps} {escaped_meta_deps} {escaped_script} {escaped_wrapper}\n"

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
