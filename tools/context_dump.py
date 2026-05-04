import subprocess
import os
import difflib
from datetime import datetime
import argparse

def get_git_tracked_files(target_dir="."):
    """Returns a list of git-tracked files with specific extensions within a target directory."""
    extensions = {'.cpp', '.hpp', '.mm', '.c', '.h', '.S', '.glsl', '.vert', '.frag', '.metal'}
    try:
        # git ls-files natively filters by path if provided
        output = subprocess.check_output(['git', 'ls-files', target_dir], text=True)
        files = output.splitlines()
        return [f for f in files if os.path.splitext(f)[1] in extensions]
    except subprocess.CalledProcessError:
        print("Error: This directory is not a git repository.")
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
        ext = os.path.splitext(file_path)[1]
        
        # Mapping highlighting
        if ext in {'.cpp', '.hpp', '.mm'}: lang = "cpp"
        elif ext == '.S': lang = "asm"
        elif ext in {'.glsl', '.vert', '.frag'}: lang = "glsl"
        else: lang = "c"
        
        lines.append(f"## File: `{file_path}`")
        lines.append(f"```{lang}")
        try:
            with open(file_path, 'r', encoding='utf-8') as f:
                lines.append(f.read())
        except Exception as e:
            lines.append(f"// Error reading file: {e}")
        lines.append(f"```\n")
        lines.append("---\n")
    
    return "\n".join(lines)

def run_project_manager(target_dir="."):
    tracked_files = get_git_tracked_files(target_dir)
    if not tracked_files:
        print(f"No matching tracked files found in '{target_dir}'.")
        return

    # Determine filenames based on the target directory
    if target_dir in (".", "", "./"):
        snapshot_file = "project_snapshot.md"
        diff_file = "project_diff.md"
    else:
        # Normalize the path (e.g., 'src/render/' -> 'src/render') and replace slashes
        clean_path = os.path.normpath(target_dir).strip(os.sep)
        prefix = clean_path.replace(os.sep, '_') + "_"
        snapshot_file = f"{prefix}project_snapshot.md"
        diff_file = f"{prefix}project_diff.md"

    # 1. Generate new content
    new_content = generate_snapshot_string(tracked_files, target_dir)
    
    # 2. Try to read old content for comparison
    old_content = ""
    if os.path.exists(snapshot_file):
        with open(snapshot_file, 'r', encoding='utf-8') as f:
            old_content = f.read()

    # 3. If there is old content, generate a diff
    if old_content:
        # Generate unified diff
        diff = list(difflib.unified_diff(
            old_content.splitlines(), 
            new_content.splitlines(), 
            fromfile='previous_snapshot', 
            tofile='current_snapshot',
            lineterm=''
        ))

        if diff:
            with open(diff_file, 'w', encoding='utf-8') as df:
                df.write(f"# Project Diff: Zahlen (Scope: {target_dir})\n")
                df.write(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n")
                df.write("```diff\n")
                df.write("\n".join(diff))
                df.write("\n```\n")
            print(f"Changes detected. Diff saved to {diff_file}")
        else:
            if os.path.exists(diff_file):
                os.remove(diff_file)
            print(f"No changes detected in '{target_dir}' since last snapshot.")
    else:
        print("Initial snapshot created. No previous version to diff against.")

    # 4. Save the new snapshot
    with open(snapshot_file, 'w', encoding='utf-8') as sf:
        sf.write(new_content)

    print(f"Successfully dumped {len(tracked_files)} files to {snapshot_file}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Dump git-tracked context for LLMs.")
    parser.add_argument(
        "target", 
        nargs="?", 
        default=".", 
        help="Target directory to dump (e.g., src/render). Defaults to the root directory."
    )
    
    args = parser.parse_args()
    run_project_manager(args.target)