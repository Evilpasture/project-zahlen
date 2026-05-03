import subprocess
import os
import difflib
from datetime import datetime

def get_git_tracked_files():
    """Returns a list of git-tracked files with specific extensions."""
    extensions = {'.cpp', '.hpp', '.mm', '.c', '.h', '.S', '.glsl', '.vert', '.frag', '.metal'}
    try:
        output = subprocess.check_output(['git', 'ls-files'], text=True)
        files = output.splitlines()
        return [f for f in files if os.path.splitext(f)[1] in extensions]
    except subprocess.CalledProcessError:
        print("Error: This directory is not a git repository.")
        return []

def generate_snapshot_string(tracked_files):
    """Generates the entire snapshot as a single string."""
    lines = []
    lines.append(f"# Project Snapshot: Zahlen")
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

def run_project_manager(snapshot_file="project_snapshot.md", diff_file="project_diff.md"):
    tracked_files = get_git_tracked_files()
    if not tracked_files:
        print("No matching tracked files found.")
        return

    # 1. Generate new content
    new_content = generate_snapshot_string(tracked_files)
    
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
                df.write(f"# Project Diff: Zahlen\n")
                df.write(f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n\n")
                df.write("```diff\n")
                df.write("\n".join(diff))
                df.write("\n```\n")
            print(f"Changes detected. Diff saved to {diff_file}")
        else:
            if os.path.exists(diff_file):
                os.remove(diff_file)
            print("No changes detected since last snapshot.")
    else:
        print("Initial snapshot created. No previous version to diff against.")

    # 4. Save the new snapshot
    with open(snapshot_file, 'w', encoding='utf-8') as sf:
        sf.write(new_content)

    print(f"Successfully dumped {len(tracked_files)} files to {snapshot_file}")

if __name__ == "__main__":
    run_project_manager()