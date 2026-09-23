#!/usr/bin/env python3
"""Reject direct private-header dependencies between src/ subsystems.

The supported cross-subsystem C++ surface lives in include/Zahlen/. A source
file in src/<subsystem>/ may include headers in its own subtree, but it may not
include a header that resolves to src/<different-subsystem>/. This keeps an
implementation header from becoming an accidental public API merely because a
target happened to export the repository's src/ directory.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOT = REPOSITORY_ROOT / "src"
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ixx", ".cppm"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^">]+)[">]', re.MULTILINE)


def source_subsystem(path: Path) -> str | None:
    """Return the immediate src/ child containing ``path``, if any."""
    try:
        relative = path.resolve().relative_to(SOURCE_ROOT.resolve())
    except ValueError:
        return None
    return relative.parts[0] if relative.parts else None


def resolved_source_include(source: Path, delimiter: str, include: str) -> Path | None:
    """Resolve only include spellings that name a real repository src header.

    Angle includes may name third-party paths such as <vulkan/vulkan_core.h>;
    those are not source-subsystem includes unless the same spelling resolves
    to an actual file below this repository's src/ tree. Quoted relative
    includes are resolved from the including file first, exactly as the
    compiler does.
    """
    candidates: list[Path] = []
    if delimiter == '"' and include.startswith("."):
        candidates.append(source.parent / include)
    if not include.startswith("."):
        candidates.append(SOURCE_ROOT / include)

    for candidate in candidates:
        if candidate.is_file() and source_subsystem(candidate) is not None:
            return candidate.resolve()
    return None


def main() -> int:
    if not SOURCE_ROOT.is_dir():
        print(f"ERROR: expected source tree at {SOURCE_ROOT}", file=sys.stderr)
        return 2

    violations: list[tuple[Path, str, str, str]] = []
    for source in sorted(path for path in SOURCE_ROOT.rglob("*") if path.is_file() and path.suffix in SOURCE_SUFFIXES):
        owner = source_subsystem(source)
        if owner is None:
            continue
        try:
            text = source.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            print(f"ERROR: cannot decode source file {source.relative_to(REPOSITORY_ROOT)}", file=sys.stderr)
            return 2

        for delimiter, include in INCLUDE_RE.findall(text):
            resolved = resolved_source_include(source, delimiter, include)
            if resolved is None:
                continue
            dependency = source_subsystem(resolved)
            if dependency is not None and dependency != owner:
                violations.append((source, include, owner, dependency))

    if violations:
        print("Subsystem private-header boundary violations:", file=sys.stderr)
        for source, include, owner, dependency in violations:
            print(
                f"  {source.relative_to(REPOSITORY_ROOT)}: #include <{include}> crosses {owner} -> {dependency} "
                f"(private header: {SOURCE_ROOT.joinpath(include).relative_to(REPOSITORY_ROOT)})",
                file=sys.stderr,
            )
        print("Use a public header under include/Zahlen/ or move the needed operation behind the owning subsystem's façade.", file=sys.stderr)
        return 1

    subsystem_count = sum(1 for path in SOURCE_ROOT.iterdir() if path.is_dir())
    source_count = sum(1 for path in SOURCE_ROOT.rglob("*") if path.is_file() and path.suffix in SOURCE_SUFFIXES)
    print(f"Subsystem include boundary OK ({subsystem_count} subsystems, {source_count} source files scanned).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
