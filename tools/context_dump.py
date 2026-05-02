import subprocess
import os

def get_git_tracked_files():
    """Returns a list of git-tracked files with specific extensions."""
    # Added .S for assembly files
    extensions = {'.cpp', '.hpp', '.mm', '.c', '.h', '.S'}
    try:
        # Get all tracked files from git
        output = subprocess.check_output(['git', 'ls-files'], text=True)
        files = output.splitlines()
        # Filter by our 'Zahlen' relevant extensions
        return [f for f in files if os.path.splitext(f)[1] in extensions]
    except subprocess.CalledProcessError:
        print("Error: This directory is not a git repository.")
        return []

def dump_to_markdown(output_file="project_snapshot.md"):
    tracked_files = get_git_tracked_files()
    
    if not tracked_files:
        print("No matching tracked files found.")
        return

    with open(output_file, 'w', encoding='utf-8') as md:
        md.write(f"# Project Snapshot: Zahlen\n\n")
        md.write(f"Total files dumped: {len(tracked_files)}\n\n---\n\n")

        for file_path in tracked_files:
            # Determine markdown language hint
            ext = os.path.splitext(file_path)[1]
            
            # Map extensions to Markdown syntax highlighting labels
            if ext in {'.cpp', '.hpp', '.mm'}:
                lang = "cpp"
            elif ext == '.S':
                lang = "asm"  # Markdown hint for assembly
            else:
                lang = "c"
            
            md.write(f"## File: `{file_path}`\n")
            md.write(f"```{lang}\n")
            try:
                with open(file_path, 'r', encoding='utf-8') as f:
                    md.write(f.read())
            except Exception as e:
                md.write(f"// Error reading file: {e}\n")
            md.write(f"\n```\n\n---\n\n")

    print(f"Successfully dumped {len(tracked_files)} files to {output_file}")

if __name__ == "__main__":
    dump_to_markdown()