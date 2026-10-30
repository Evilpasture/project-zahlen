#!/usr/bin/env python3
"""Reject unauthorized cross-subsystem dependencies across src/ subsystems.

The supported cross-subsystem C++ surface lives in include/Zahlen/ (plus
explicit internal seams such as src/vulkan for src/render and the header-only
src/presentation seam shared by src/window and src/render).

In addition to private-header resolution (including bare includes resolved via
private target_include_directories), this check enforces that src/render/ and
src/vulkan/ remain completely decoupled at both the header and CMake target
levels from zahlen_window and zahlen_filesystem.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOT = REPOSITORY_ROOT / "src"
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ixx", ".cppm", ".inl", ".ipp"}
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"}
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^">]+)[">]', re.MULTILINE)
COMMENT_OR_STRING_RE = re.compile(
    r"//[^\n]*"
    r"|/\*[\s\S]*?\*/"
    r'|"(?:\\[\s\S]|[^"\\])*"'
    r"|'(?:\\[\s\S]|[^'\\])*'"
)
VULKAN_BUILD_MODE_TOKEN_RE = re.compile(r"\b(?:isDev|isDebug|ZHLN_DEV_MODE|ZHLN_DEBUG)\b")

# Explicit internal src/<owner> -> src/<dependency> seam allowlist.
ALLOWED_INTERNAL_SUBSYSTEM_DEPS: dict[str, frozenset[str]] = {
    "render": frozenset({"vulkan", "presentation"}),
    "window": frozenset({"presentation"}),
}
ALLOWED_INTERNAL_HEADER_EXCEPTIONS: frozenset[tuple[str, str, str]] = frozenset(
    {
        ("window", "engine", "tty/TTYBackend.hpp"),
    }
)

# Public include/Zahlen/* headers that render/vulkan must never consume, keeping
# libzahlen_render.a free of zahlen_window and zahlen_filesystem symbol edges.
FORBIDDEN_RENDER_PUBLIC_INCLUDES: tuple[str, ...] = (
    "Zahlen/FileSystem/",
    # Current spellings.
    "Zahlen/Window/",
    # Legacy spellings kept alive by the forwarding shims.
    "Zahlen/Window.hpp",
    "Zahlen/WindowInput.hpp",
    "Zahlen/PlatformHost.hpp",
)

FORBIDDEN_RENDER_CMAKE_TOKENS_RE = re.compile(
    r"\b(?:zahlen_window|zahlen_filesystem)\b|src/(?:window|filesystem)\b"
)


def _blank_match(match: re.Match[str]) -> str:
    span = match.group(0)
    if "\n" not in span:
        return " " * len(span)
    return "\n".join(" " * len(part) for part in span.split("\n"))


def strip_comments_and_strings(text: str) -> str:
    return COMMENT_OR_STRING_RE.sub(_blank_match, text)


def strip_cmake_comments(text: str) -> str:
    return re.sub(r"#[^\n]*", "", text)


def source_subsystem(path: Path) -> str | None:
    """Return the immediate src/ child containing ``path``, if any."""
    try:
        relative = path.resolve().relative_to(SOURCE_ROOT.resolve())
    except ValueError:
        return None
    return relative.parts[0] if relative.parts else None


def build_subsystem_header_index() -> dict[str, list[tuple[str, Path]]]:
    """Map subsystem-relative header path -> [(subsystem, resolved_path), ...]."""
    index: dict[str, list[tuple[str, Path]]] = {}
    for sub_dir in sorted(path for path in SOURCE_ROOT.iterdir() if path.is_dir()):
        sub = sub_dir.name
        for header in sorted(sub_dir.rglob("*")):
            if header.is_file() and header.suffix in HEADER_SUFFIXES:
                rel = header.relative_to(sub_dir).as_posix()
                index.setdefault(rel, []).append((sub, header.resolve()))
    return index


def resolved_source_includes(
    source: Path,
    owner: str,
    delimiter: str,
    include: str,
    header_index: dict[str, list[tuple[str, Path]]],
) -> list[tuple[str, Path]]:
    """Resolve includes that land in another src/<subsystem>/ tree."""
    normalized = include.replace("\\", "/")

    # 1. Relative to the including file (quoted includes or explicit ./ ../).
    if delimiter == '"' or normalized.startswith("."):
        local = (source.parent / normalized)
        if local.is_file():
            resolved = local.resolve()
            dep = source_subsystem(resolved)
            return [(dep, resolved)] if dep is not None else []

    # 2. Explicit src/-rooted path (e.g. <window/PresentationTarget.hpp>).
    if not normalized.startswith("."):
        rooted = SOURCE_ROOT / normalized
        if rooted.is_file():
            resolved = rooted.resolve()
            dep = source_subsystem(resolved)
            return [(dep, resolved)] if dep is not None else []

    # 3. Bare subsystem-relative include satisfied via private -I src/<dep>.
    # If the owning subsystem itself provides that relative header, it resolves locally.
    matches = header_index.get(normalized, [])
    if not matches or any(sub == owner for sub, _ in matches):
        return []
    return matches


def is_allowed_internal_dep(owner: str, dependency: str, target_header: Path) -> bool:
    if dependency == owner:
        return True
    if dependency in ALLOWED_INTERNAL_SUBSYSTEM_DEPS.get(owner, frozenset()):
        return True
    dep_rel = target_header.relative_to((SOURCE_ROOT / dependency).resolve()).as_posix()
    return (owner, dependency, dep_rel) in ALLOWED_INTERNAL_HEADER_EXCEPTIONS


def main() -> int:
    if not SOURCE_ROOT.is_dir():
        print(f"ERROR: expected source tree at {SOURCE_ROOT}", file=sys.stderr)
        return 2

    header_index = build_subsystem_header_index()
    violations: list[tuple[Path, str, str, str, Path]] = []
    public_edge_violations: list[tuple[Path, str, str]] = []
    cmake_violations: list[tuple[Path, int, str]] = []
    policy_violations: list[tuple[Path, int, str]] = []

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
            norm_inc = include.replace("\\", "/")
            if owner in {"render", "vulkan"} and any(
                norm_inc == forbidden or norm_inc.startswith(forbidden)
                for forbidden in FORBIDDEN_RENDER_PUBLIC_INCLUDES
            ):
                public_edge_violations.append((source, include, owner))

            for dependency, target_header in resolved_source_includes(source, owner, delimiter, include, header_index):
                if not is_allowed_internal_dep(owner, dependency, target_header):
                    violations.append((source, include, owner, dependency, target_header))

        if owner == "vulkan":
            clean = strip_comments_and_strings(text)
            for match in VULKAN_BUILD_MODE_TOKEN_RE.finditer(clean):
                line = clean.count("\n", 0, match.start()) + 1
                policy_violations.append((source, line, match.group(0)))

    for sub in ("render", "vulkan"):
        for cmake_file in sorted((SOURCE_ROOT / sub).rglob("CMakeLists.txt")):
            clean_cmake = strip_cmake_comments(cmake_file.read_text(encoding="utf-8"))
            for match in FORBIDDEN_RENDER_CMAKE_TOKENS_RE.finditer(clean_cmake):
                line = clean_cmake.count("\n", 0, match.start()) + 1
                cmake_violations.append((cmake_file, line, match.group(0)))

    if violations or public_edge_violations or cmake_violations or policy_violations:
        if violations:
            print("Subsystem private-header boundary violations:", file=sys.stderr)
            for source, include, owner, dependency, target_header in violations:
                print(
                    f"  {source.relative_to(REPOSITORY_ROOT)}: #include <{include}> crosses {owner} -> {dependency} "
                    f"(private header: {target_header.relative_to(REPOSITORY_ROOT)})",
                    file=sys.stderr,
                )
            print("Use a public header under include/Zahlen/ or an explicit header-only seam target.", file=sys.stderr)
        if public_edge_violations:
            print("Render/RHI subsystem decoupling violations (window/filesystem headers forbidden):", file=sys.stderr)
            for source, include, owner in public_edge_violations:
                print(
                    f"  {source.relative_to(REPOSITORY_ROOT)}: #include <{include}> couples src/{owner}/ to window/filesystem",
                    file=sys.stderr,
                )
        if cmake_violations:
            print("Render/RHI CMake link/include boundary violations:", file=sys.stderr)
            for cmake_file, line, token in cmake_violations:
                print(
                    f"  {cmake_file.relative_to(REPOSITORY_ROOT)}:{line}: forbidden dependency token `{token}`",
                    file=sys.stderr,
                )
        if policy_violations:
            print("RHI build-mode policy leak violations in src/vulkan/:", file=sys.stderr)
            for source, line, token in policy_violations:
                print(
                    f"  {source.relative_to(REPOSITORY_ROOT)}:{line}: hardcoded `{token}` in src/vulkan/; "
                    "pass an explicit runtime config/policy struct from the caller instead.",
                    file=sys.stderr,
                )
        return 1

    subsystem_count = sum(1 for path in SOURCE_ROOT.iterdir() if path.is_dir())
    source_count = sum(1 for path in SOURCE_ROOT.rglob("*") if path.is_file() and path.suffix in SOURCE_SUFFIXES)
    print(f"Subsystem include boundary OK ({subsystem_count} subsystems, {source_count} source files scanned).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
