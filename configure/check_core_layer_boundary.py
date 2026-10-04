#!/usr/bin/env python3
"""Enforce the one-way dependency boundary around the engine core.

The repository's source layers are deliberately distinct:

  * plugins/    asset formats and codecs;
  * extensions/ reusable engine subsystems and network/platform I/O;
  * gameplay/   domain-specific systems and ProjectLight integration.

All three may consume Core. None may become a dependency of src/, include/, or
modules/. This configure-time check catches both imported C++ modules and
headers, including headers included through their public short include roots.
CMake target links are kept explicit in the root composition layer and should
not be added to targets that build Core sources.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE_ROOTS = (ROOT / "src", ROOT / "include", ROOT / "modules")
LAYER_ROOTS = (ROOT / "plugins", ROOT / "extensions", ROOT / "gameplay")
LAYER_INCLUDE_ROOTS = (
    ROOT / "plugins",
    ROOT / "extensions",
    ROOT / "extensions" / "net",
    ROOT / "gameplay",
    ROOT / "gameplay" / "ProjectLight",
)
CORE_INCLUDE_ROOTS = (
    ROOT / "include",
    ROOT / "src",
    ROOT / "modules",
    ROOT / "extern",
    ROOT / "third_party",
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".cppm", ".ixx"}
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".cppm", ".ixx"}

module_pattern = re.compile(r"^\s*export\s+module\s+([^;]+);", re.MULTILINE)
import_pattern = re.compile(r"^\s*(?:export\s+)?import\s+([^;]+);", re.MULTILINE)
include_pattern = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)


def normalize(include: str) -> str:
    return include.replace("\\", "/").lstrip("./")


def layer_module_names() -> set[str]:
    """Every C++ module name declared beneath the optional outer layers."""
    names: set[str] = set()
    for root in LAYER_ROOTS:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.suffix.lower() not in SOURCE_SUFFIXES or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="ignore")
            names.update(match.group(1).strip() for match in module_pattern.finditer(text))
    return names


def layer_headers() -> dict[str, list[Path]]:
    """Header paths indexed by their public include spelling and basename."""
    by_path: dict[str, list[Path]] = {}
    for root in LAYER_INCLUDE_ROOTS:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.suffix.lower() not in HEADER_SUFFIXES or not path.is_file():
                continue
            relative = path.relative_to(root).as_posix()
            by_path.setdefault(relative, []).append(path)
            by_path.setdefault(path.name, []).append(path)
    return by_path


def resolves_in_core(include: str, source: Path) -> bool:
    """True if a Core-visible include root can satisfy this include."""
    relative = normalize(include)
    if not relative:
        return True

    # Quoted includes first search next to the including source.
    if (source.parent / relative).is_file():
        return True

    for root in CORE_INCLUDE_ROOTS:
        if root.is_dir() and (root / relative).is_file():
            return True

    # A slash-free include may resolve by basename on one of the core include
    # roots. Keep this walk to first-party core trees; vendor checkouts can be
    # very large and are not part of the optional-layer public roots.
    if "/" not in relative:
        name = Path(relative).name
        for root in CORE_INCLUDE_ROOTS[:3]:
            if not root.is_dir():
                continue
            if any(hit.is_file() and hit.name == name for hit in root.rglob(name)):
                return True
    return False


def main() -> int:
    modules = layer_module_names()
    headers = layer_headers()
    violations: list[str] = []

    for core_root in CORE_ROOTS:
        if not core_root.is_dir():
            continue
        for path in core_root.rglob("*"):
            if path.suffix.lower() not in SOURCE_SUFFIXES or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="ignore")
            relative_path = path.relative_to(ROOT)

            for match in import_pattern.finditer(text):
                module_name = match.group(1).strip()
                if module_name in modules:
                    violations.append(f"{relative_path} imports optional-layer module {module_name}")

            for match in include_pattern.finditer(text):
                include = normalize(match.group(2))
                candidates = (headers.get(include) or headers.get(Path(include).name)) if include else None
                if not candidates:
                    continue
                if not resolves_in_core(include, path):
                    spelled = candidates[0].relative_to(ROOT).as_posix()
                    violations.append(
                        f"{relative_path} includes {spelled} from an optional layer "
                        f"(written as '{include}')"
                    )

    if violations:
        print("Core-to-optional-layer dependency boundary violated:", file=sys.stderr)
        for violation in sorted(set(violations)):
            print(f"  - {violation}", file=sys.stderr)
        print(
            "Move the implementation into Core or expose a public Core API consumed by the outer layer.",
            file=sys.stderr,
        )
        return 1

    header_count = len([key for key in headers if "/" in key])
    print(f"Core/layer boundary OK ({len(modules)} optional modules, {header_count} headers indexed).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
