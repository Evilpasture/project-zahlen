#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import sys
import subprocess
import shutil

def find_blender():
    """Locates the Blender system executable dynamically."""
    if "BLENDER_PATH" in os.environ:
        return os.environ["BLENDER_PATH"]

    if sys.platform == "darwin":
        mac_path = "/Applications/Blender.app/Contents/MacOS/Blender"
        if os.path.exists(mac_path):
            return mac_path

    shutil_path = shutil.which("blender")
    if shutil_path:
        return shutil_path

    if sys.platform == "win32":
        paths = [
            r"C:\Program Files\Blender Foundation\Blender 5.1\blender.exe",
            r"C:\Program Files\Blender Foundation\Blender 4.2\blender.exe",
            r"C:\Program Files\Blender Foundation\Blender 4.1\blender.exe",
            r"C:\Program Files\Blender Foundation\Blender 4.0\blender.exe",
        ]
        for p in paths:
            if os.path.exists(p):
                return p

    raise FileNotFoundError("Could not automatically locate the 'blender' executable.")

def main():
    if len(sys.argv) < 2:
        print("Usage: run_blender.py [blender_path] [blender_args...]")
        sys.exit(1)

    first_arg = sys.argv[1]
    
    # Resolve the absolute canonical path (resolving any macOS symlinks)
    if first_arg.startswith("-") or (os.path.exists(first_arg) and first_arg.endswith(".blend")):
        try:
            blender_bin = os.path.realpath(find_blender())
            blender_args = sys.argv[1:]
        except FileNotFoundError as e:
            print(f"[Error] {e}")
            sys.exit(1)
    else:
        resolved = shutil.which(first_arg)
        if resolved:
            blender_bin = os.path.realpath(resolved)
        else:
            try:
                blender_bin = os.path.realpath(find_blender())
            except FileNotFoundError:
                blender_bin = first_arg
        blender_args = sys.argv[2:]

    # Clean the environment to prevent virtual environment leakage
    venv_path = os.environ.get("VIRTUAL_ENV", "")
    env = os.environ.copy()

    # Strip environment overrides that disrupt Blender's bundled interpreter
    env.pop("PYTHONHOME", None)
    env.pop("PYTHONPATH", None)
    env.pop("VIRTUAL_ENV", None)
    env["PYTHONNOUSERSITE"] = "1"

    # Remove the active virtualenv's bin path from the PATH variable
    if "PATH" in env and venv_path:
        clean_path = [
            p for p in env["PATH"].split(os.pathsep) if not p.startswith(venv_path)
        ]
        env["PATH"] = os.pathsep.join(clean_path)

    cmd = [blender_bin] + blender_args

    # Spawn Blender and pipe back the return status code
    result = subprocess.run(cmd, env=env)
    sys.exit(result.returncode)

if __name__ == "__main__":
    main()