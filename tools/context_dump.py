# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


import subprocess
import os
import difflib
from pathlib import Path
from datetime import datetime
import argparse


def get_git_tracked_files(
    target=".", ignore_demo=False, ignore_tools=False, ignore_inlines=False
):
    extensions = {
        ".cpp",
        ".hpp",
        ".mm",
        ".c",
        ".h",
        ".S",
        ".glsl",
        ".vert",
        ".frag",
        ".metal",
        ".lua",
        ".hlsl",
        ".sh",
        ".py",
        ".inl",
    }
    include_filenames = {"CMakeLists.txt"}

    # Base ignore paths
    ignore_paths = {"third_party", "extern"}
    if ignore_tools:
        ignore_paths.add("tools")

    # If the flag is set, remove .inl from the allowed extensions
    if ignore_inlines:
        extensions.discard(".inl")

    if os.path.isfile(target):
        return [target]

    try:
        # Get all files
        output = subprocess.check_output(["git", "ls-files", target], text=True)
        all_files = output.splitlines()

        # If --ignore-demo is enabled, find all directories containing a file named ".DEMO"
        demo_dirs = set()
        if ignore_demo:
            # Look for any file named exactly ".DEMO" in the repo
            for f in all_files:
                if os.path.basename(f) == ".DEMO":
                    demo_dirs.add(os.path.dirname(f))

        valid_files = []
        for f in all_files:
            path_obj = Path(f)

            # 1. Skip paths containing globally ignored directories
            if any(part in ignore_paths for part in path_obj.parts):
                continue

            # 2. Skip if the file or any of its parent directories are in a .DEMO folder/path
            if ignore_demo:
                # Check if any parent of this file is one of the directories we identified
                if any(str(parent) in demo_dirs for parent in path_obj.parents):
                    continue
                # Also check if the file is IN a .DEMO directory
                if ".DEMO" in path_obj.parts:
                    continue

            # 3. Check extensions
            # Check: Is it an allowed extension OR is it in our include_filenames?
            if path_obj.suffix in extensions or path_obj.name in include_filenames:
                valid_files.append(f)

        return valid_files
    except subprocess.CalledProcessError:
        return []


def generate_snapshot_string(tracked_files, target_dir):
    """Generates the entire snapshot as a single string."""
    lines = []

    # Add a little context to the header so the LLM knows what scope it's looking at
    scope = "Root" if target_dir == "." else target_dir
    lines.append(f"# Project Snapshot: Zahlen (Scope: {scope})")
    lines.append(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f"Total files dumped: {len(tracked_files)}\n")
    lines.append("---\n")

    for file_path in tracked_files:
        filename = os.path.basename(file_path)
        ext = os.path.splitext(file_path)[1]

        # Mapping highlighting
        if filename == "CMakeLists.txt":
            lang = "cmake"
        elif ext in {".cpp", ".hpp", ".mm"}:
            lang = "cpp"
        elif ext == ".S":
            lang = "asm"
        elif ext in {".glsl", ".vert", ".frag"}:
            lang = "glsl"
        elif ext == ".hlsl":
            lang = "hlsl"
        elif ext == ".log":
            lang = "txt"
        elif ext == ".sh":
            lang = "bash"
        elif ext == ".md":
            lang = "markdown"
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
    target=".", ignore_demo=False, ignore_tools=False, ignore_inlines=False
):
    tracked_files = get_git_tracked_files(
        target,
        ignore_demo=ignore_demo,
        ignore_tools=ignore_tools,
        ignore_inlines=ignore_inlines,
    )
    if not tracked_files:
        print(f"No matching files found at '{target}'.")
        return

    # Handle naming for single files
    if os.path.isfile(target):
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
    new_content = generate_snapshot_string(tracked_files, target)

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
                df.write(f"# Project Diff: Zahlen (Scope: {target})\n")
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
            print(f"No changes detected in '{target}' since last snapshot.")
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
    parser.add_argument(
        "--ignore-demo",
        action="store_true",
        help="Ignore directories containing .DEMO files.",
    )
    # NEW: Add the --ignore-tools flag
    parser.add_argument(
        "--ignore-tools", action="store_true", help="Ignore the tools/ directory."
    )

    parser.add_argument(
        "--ignore-inlines",
        action="store_true",
        help="Ignore .inl implementation files.",
    )

    args = parser.parse_args()
    # Pass the new argument into your main function
    run_project_manager(
        args.target,
        ignore_demo=args.ignore_demo,
        ignore_tools=args.ignore_tools,
        ignore_inlines=args.ignore_inlines,
    )
