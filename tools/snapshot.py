# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

import argparse
import difflib
import os
import subprocess
from datetime import datetime
from pathlib import Path

# Subsystem directory & file mappings based on repo layout
PRESETS = {
    "render": [
        "include/Zahlen/Render",
        "include/Zahlen/Meshlet.hpp",
        "src/render",
        "src/vulkan",
        "resources/shaders",
        "modules/zahlen-render.cppm",
        "cmake/ShaderCompilation.cmake",
        "DESCRIPTOR_HEAPS.md",
        "MESH_SHADERS.md",
    ],
    "physics": [
        "include/Zahlen/physics",
        "src/physics",
        "modules/zahlen-physics.cppm",
    ],
    "ecs": [
        "include/Zahlen/ecs",
        "include/Zahlen/Entity.hpp",
        "include/Zahlen/Components.hpp",
        "src/ecs",
        "modules/zahlen-ecs.cppm",
        "cmake/ECSCompiler.cmake",
    ],
    "audio": [
        "include/Zahlen/Audio.hpp",
        "src/audio",
        "modules/zahlen-audio.cppm",
        "src/engine/system/AudioSystem.hpp",
        "src/engine/system/AudioSystem.cpp",
    ],
    "core": [
        "include/Zahlen/Core",
        "modules/zahlen-core.cppm",
    ],
    "gui": [
        "include/Zahlen/gui",
        "src/gui",
        "src/render/ui",
        "app/UIEditor.cpp",
        "resources/shaders/ui.slang",
        "ui.toml",
        "ui-preview.toml",
    ],
    "threading": [
        "include/Zahlen/Threading",
        "src/threading",
        "modules/zahlen-threading.cppm",
    ],
    "window": [
        "include/Zahlen/Window.hpp",
        "include/Zahlen/WindowInput.hpp",
        "include/Zahlen/PlatformHost.hpp",
        "src/window",
    ],
    "engine": [
        "include/Zahlen/Engine.hpp",
        "include/Zahlen/Kernel.hpp",
        "include/Zahlen/Scene.hpp",
        "include/Zahlen/World.hpp",
        "include/Zahlen/SystemContext.hpp",
        "include/Zahlen/FrameScheduler.hpp",
        "include/Zahlen/CommandLine.hpp",
        "include/Zahlen/Config.hpp",
        "src/engine",
        "modules/zahlen-engine.cppm",
    ],
    "shaders": [
        "resources/shaders",
        "cmake/ShaderCompilation.cmake",
    ],
    "math": [
        "include/Zahlen/Math3D.hpp",
        "include/Zahlen/Geometry2D.hpp",
        "include/Zahlen/Core/Math.hpp",
        "modules/zahlen-math.cppm",
    ],
}


def matches_any_preset(file_path: str, active_presets: list[str]) -> bool:
    """Checks if a file path matches any path pattern in the active presets."""
    normalized_file = Path(file_path).as_posix().lstrip("./")

    for preset_name in active_presets:
        patterns = PRESETS.get(preset_name, [])
        for pattern in patterns:
            norm_pattern = Path(pattern).as_posix().lstrip("./").rstrip("/")
            # Match exact file or directory prefix
            if normalized_file == norm_pattern or normalized_file.startswith(
                norm_pattern + "/"
            ):
                return True
    return False


def get_git_tracked_files(
    target=".",
    ignore_demo=False,
    ignore_tools=False,
    ignore_inlines=False,
    ignore_scripts=False,
    ignore_tests=False,
    ignore_extras=False,
    ignore_samples=False,
    ignore_configure=False,
    active_presets=None,
):
    extensions = {
        ".cpp",
        ".hpp",
        ".mm",
        ".c",
        ".h",
        ".S",
        ".glsl",
        ".slang",
        ".vert",
        ".frag",
        ".metal",
        ".lua",
        ".hlsl",
        ".sh",
        ".py",
        ".inl",
        ".fnl",
        ".cppm",
        ".toml",
    }
    include_filenames = {"CMakeLists.txt"}
    exclude_paths = {"scripts/core/fennel.lua"}

    # Base ignore paths
    ignore_paths = {"third_party", "extern"}
    if ignore_tools:
        ignore_paths.add("tools")
    if ignore_scripts:
        ignore_paths.add("scripts")
    if ignore_tests:
        ignore_paths.add("tests")
    if ignore_extras:
        ignore_paths.add("extras")
    if ignore_samples:
        ignore_paths.add("samples")
    if ignore_configure:
        ignore_paths.add("configure")

    if ignore_inlines:
        extensions.discard(".inl")

    if os.path.isfile(target):
        return [target]

    try:
        output = subprocess.check_output(["git", "ls-files", target], text=True)
        all_files = output.splitlines()

        demo_dirs = set()
        if ignore_demo:
            for f in all_files:
                if os.path.basename(f) == ".DEMO":
                    demo_dirs.add(os.path.dirname(f))

        valid_files = []
        for f in all_files:
            path_obj = Path(f)

            # 1. Skip paths containing globally ignored directories
            if any(part in ignore_paths for part in path_obj.parts):
                continue

            if str(path_obj) in exclude_paths:
                continue

            # 2. Skip .DEMO directories
            if ignore_demo:
                if any(str(parent) in demo_dirs for parent in path_obj.parents):
                    continue
                if ".DEMO" in path_obj.parts:
                    continue

            # 3. Check preset filter
            if active_presets and not matches_any_preset(f, active_presets):
                continue

            # 4. Check extensions
            if path_obj.suffix in extensions or path_obj.name in include_filenames:
                valid_files.append(f)

        return valid_files
    except subprocess.CalledProcessError:
        return []


def generate_snapshot_string(tracked_files, target_dir, active_presets=None):
    """Generates the entire snapshot as a single string."""
    lines = []

    if active_presets:
        scope = f"Presets: {', '.join(active_presets)}"
    elif target_dir in (".", "", "./"):
        scope = "Root"
    else:
        scope = target_dir

    lines.append(f"# Project Snapshot: Zahlen (Scope: {scope})")
    lines.append(
        "> **CONTEXT NOTE:** This is a static markdown snapshot of source code, "
    )
    lines.append(
        "> NOT a live virtual file system or interactive terminal environment. "
    )
    lines.append("> Please treat these files as a read-only codebase reference for ")
    lines.append("> analysis and review within this chat interface.\n")

    lines.append(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f"Total files dumped: {len(tracked_files)}\n")
    lines.append("---\n")

    for file_path in tracked_files:
        filename = os.path.basename(file_path)
        ext = os.path.splitext(file_path)[1]

        if filename == "CMakeLists.txt":
            lang = "cmake"
        elif ext in {".cpp", ".hpp", ".mm", ".inl", ".cppm"}:
            lang = "cpp"
        elif ext == ".S":
            lang = "asm"
        elif ext in {".glsl", ".vert", ".frag"}:
            lang = "glsl"
        elif ext in {".hlsl", ".slang"}:
            lang = "hlsl"
        elif ext == ".log":
            lang = "txt"
        elif ext == ".sh":
            lang = "bash"
        elif ext == ".md":
            lang = "markdown"
        elif ext == ".fnl":
            lang = "fennel"
        elif ext == ".toml":
            lang = "toml"
        else:
            lang = "c"

        lines.append(f"## File: `{file_path}`")
        lines.append(f"```{lang}")
        try:
            with open(file_path, "r", encoding="utf-8") as f:
                lines.append(f.read())
        except Exception as e:
            lines.append(f"// Error reading file: {e}")
        lines.append("```\n")
        lines.append("---\n")

    return "\n".join(lines)


def run_project_manager(
    target=".",
    ignore_demo=False,
    ignore_tools=False,
    ignore_inlines=False,
    ignore_scripts=False,
    ignore_tests=False,
    ignore_extras=False,
    ignore_samples=False,
    ignore_configure=False,
    active_presets=None,
):
    tracked_files = get_git_tracked_files(
        target,
        ignore_demo=ignore_demo,
        ignore_tools=ignore_tools,
        ignore_inlines=ignore_inlines,
        ignore_scripts=ignore_scripts,
        ignore_tests=ignore_tests,
        ignore_extras=ignore_extras,
        ignore_samples=ignore_samples,
        ignore_configure=ignore_configure,
        active_presets=active_presets,
    )
    if not tracked_files:
        scope_msg = (
            f"presets '{','.join(active_presets)}'" if active_presets else f"'{target}'"
        )
        print(f"No matching files found for {scope_msg}.")
        return

    # Determine snapshot / diff filenames
    if active_presets:
        preset_prefix = "_".join(active_presets)
        snapshot_file = f"{preset_prefix}_snapshot.md"
        diff_file = f"{preset_prefix}_diff.md"
    elif os.path.isfile(target):
        name = Path(target).stem
        snapshot_file = f"{name}_snapshot.md"
        diff_file = f"{name}_diff.md"
    elif target in (".", "", "./"):
        snapshot_file = "project_snapshot.md"
        diff_file = "project_diff.md"
    else:
        clean_path = os.path.normpath(target).strip(os.sep)
        prefix = clean_path.replace(os.sep, "_") + "_"
        snapshot_file = f"{prefix}project_snapshot.md"
        diff_file = f"{prefix}project_diff.md"

    # 1. Generate new content
    new_content = generate_snapshot_string(tracked_files, target, active_presets)

    # 2. Try to read old content for comparison
    old_content = ""
    if os.path.exists(snapshot_file):
        with open(snapshot_file, "r", encoding="utf-8") as f:
            old_content = f.read()

    # 3. If there is old content, generate a diff
    if old_content:
        diff = list(
            difflib.unified_diff(
                old_content.splitlines(),
                new_content.splitlines(),
                fromfile="previous_snapshot",
                tofile="current_snapshot",
                lineterm="",
            )
        )

        if diff:
            with open(diff_file, "w", encoding="utf-8") as df:
                df.write(f"# Project Diff: Zahlen (Scope: {snapshot_file})\n")
                df.write(
                    f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n"
                )
                df.write("```diff\n")
                df.write("\n".join(diff))
                df.write("\n```\n")
            print(f"Changes detected. Diff saved to {diff_file}")
        else:
            if os.path.exists(diff_file):
                os.remove(diff_file)
            print(f"No changes detected since last snapshot in {snapshot_file}.")
    else:
        print("Initial snapshot created. No previous version to diff against.")

    # 4. Save the new snapshot
    with open(snapshot_file, "w", encoding="utf-8") as sf:
        sf.write(new_content)

    print(f"Successfully dumped {len(tracked_files)} files to {snapshot_file}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Dump git-tracked context for LLMs.")
    parser.add_argument(
        "target", nargs="?", default=".", help="Target directory to dump."
    )

    # Preset flags
    parser.add_argument(
        "--preset",
        type=str,
        help=f"Comma-separated list of presets to dump: {', '.join(PRESETS.keys())}",
    )
    parser.add_argument(
        "--render-only", action="store_true", help="Dump Render, Vulkan, and Shaders."
    )
    parser.add_argument(
        "--physics-only", action="store_true", help="Dump Physics subsystem."
    )
    parser.add_argument("--ecs-only", action="store_true", help="Dump ECS subsystem.")
    parser.add_argument(
        "--audio-only", action="store_true", help="Dump Audio subsystem."
    )
    parser.add_argument(
        "--core-only", action="store_true", help="Dump Core engine utilities."
    )
    parser.add_argument(
        "--gui-only", "--ui-only", action="store_true", help="Dump GUI and UI renderer."
    )
    parser.add_argument(
        "--threading-only", action="store_true", help="Dump Threading and Task system."
    )
    parser.add_argument(
        "--window-only", action="store_true", help="Dump Window and Platform host."
    )
    parser.add_argument(
        "--engine-only",
        action="store_true",
        help="Dump Engine, Kernel, Scene, and Systems.",
    )
    parser.add_argument(
        "--shaders-only", action="store_true", help="Dump shader files only."
    )
    parser.add_argument(
        "--math-only",
        action="store_true",
        help="Dump Math3D, Geometry, and Math utilities.",
    )

    # Ignore flags
    parser.add_argument(
        "--ignore-demo",
        action="store_true",
        help="Ignore directories containing .DEMO files.",
    )
    parser.add_argument(
        "--ignore-tools", action="store_true", help="Ignore the tools/ directory."
    )
    parser.add_argument(
        "--ignore-inlines",
        action="store_true",
        help="Ignore .inl implementation files.",
    )
    parser.add_argument(
        "--ignore-scripts", action="store_true", help="Ignore the scripts/ directory."
    )
    parser.add_argument(
        "--ignore-tests", action="store_true", help="Ignore the tests/ directory."
    )
    parser.add_argument(
        "--ignore-extras", action="store_true", help="Ignore the extras/ directory."
    )
    parser.add_argument(
        "--ignore-samples", action="store_true", help="Ignore the samples/ directory."
    )
    parser.add_argument(
        "--ignore-configure",
        action="store_true",
        help="Ignore the configure/ directory.",
    )
    parser.add_argument(
        "--ignore-all",
        action="store_true",
        help="Ignore tools, scripts, tests, extras, samples, configure, and .inl files altogether.",
    )

    args = parser.parse_args()

    if args.ignore_all:
        args.ignore_tools = True
        args.ignore_scripts = True
        args.ignore_tests = True
        args.ignore_inlines = True
        args.ignore_extras = True
        args.ignore_samples = True
        args.ignore_configure = True

    # Aggregate active presets
    active_presets = []
    if args.preset:
        for p in args.preset.split(","):
            clean_p = p.strip().lower()
            if clean_p in PRESETS:
                active_presets.append(clean_p)
            else:
                print(f"Warning: Unknown preset '{clean_p}'. Skipping.")

    flag_to_preset = {
        "render_only": "render",
        "physics_only": "physics",
        "ecs_only": "ecs",
        "audio_only": "audio",
        "core_only": "core",
        "gui_only": "gui",
        "threading_only": "threading",
        "window_only": "window",
        "engine_only": "engine",
        "shaders_only": "shaders",
        "math_only": "math",
    }

    for flag, preset_name in flag_to_preset.items():
        if getattr(args, flag, False) and preset_name not in active_presets:
            active_presets.append(preset_name)

    run_project_manager(
        args.target,
        ignore_demo=args.ignore_demo,
        ignore_tools=args.ignore_tools,
        ignore_inlines=args.ignore_inlines,
        ignore_scripts=args.ignore_scripts,
        ignore_tests=args.ignore_tests,
        ignore_extras=args.ignore_extras,
        ignore_samples=args.ignore_samples,
        ignore_configure=args.ignore_configure,
        active_presets=active_presets if active_presets else None,
    )
