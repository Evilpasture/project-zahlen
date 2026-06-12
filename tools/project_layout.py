# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later


import subprocess
import os

def get_git_tracked_files():
    """Runs git ls-files to get all tracked paths."""
    try:
        output = subprocess.check_output(["git", "ls-files"], text=True)
        return output.splitlines()
    except subprocess.CalledProcessError:
        print("Error: Not a git repository.")
        return []

def print_tree(files):
    """Converts a list of paths into a visual tree."""
    tree = {}
    for path in files:
        parts = path.split(os.sep)
        current = tree
        for part in parts:
            current = current.setdefault(part, {})

    def walk(node, prefix=""):
        items = sorted(node.keys())
        for i, name in enumerate(items):
            is_last = (i == len(items) - 1)
            connector = "└── " if is_last else "├── "
            print(f"{prefix}{connector}{name}")
            
            if node[name]:  # If it's a directory
                extension = "    " if is_last else "│   "
                walk(node[name], prefix + extension)

    print("(project-zahlen)")
    walk(tree)

if __name__ == "__main__":
    tracked_files = get_git_tracked_files()
    if tracked_files:
        print_tree(tracked_files)