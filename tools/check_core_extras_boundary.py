#!/usr/bin/env python3
"""Reject dependencies from Zahlen core source trees into extras/.

Extras are allowed to consume public core APIs. The reverse dependency would
make optional feature modules mandatory, so this check runs during CMake
configuration and fails before compilation.

Two ways a core file can reach into extras, both rejected here:

  1. `import ZHLN.Foo;` where ZHLN.Foo is a module declared under extras/.
  2. `#include` of anything that resolves to a file under extras/ -- whether it
     spells the directory (`#include <extras/json/JSON.hpp>`) or not
     (`#include <json/JSON.hpp>`, which works because extras/ is itself an
     include root for consumers of zahlen_extras). Form (2) is the one that
     matters in practice: the JSON and TOML layers live in extras/json and
     extras/toml and are included by their short paths.

Linking is the third way, and it is not visible in the sources: a core target
that added `target_link_libraries(... zahlen_extras)` would compile fine and
still break the rule. Keep zahlen_extras out of every target defined in
CMakeLists.txt, src/, include/ and modules/.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE_ROOTS = (ROOT / "src", ROOT / "include", ROOT / "modules")
EXTRAS = ROOT / "extras"
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".cppm", ".ixx"}
HEADER_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".cppm", ".ixx"}

# Trees an `#include` may legitimately resolve through. Anything that resolves
# only under extras/ is a boundary violation; anything that resolves nowhere in
# the repo is a third-party or standard header and none of this check's business.
SEARCH_ROOTS = (ROOT / "include", ROOT / "src", EXTRAS, ROOT / "extern", ROOT / "third_party", ROOT / "tests")

module_pattern = re.compile(r"^\s*export\s+module\s+([^;]+);", re.MULTILINE)
import_pattern = re.compile(r"^\s*(?:export\s+)?import\s+([^;]+);", re.MULTILINE)
include_pattern = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)


def normalize(include: str) -> str:
    return include.replace("\\", "/").lstrip("./")


def extras_module_names() -> set[str]:
    """Every module name declared by a source file under extras/."""
    names: set[str] = set()
    if not EXTRAS.is_dir():
        return names
    for path in EXTRAS.rglob("*"):
        if path.suffix.lower() not in SOURCE_SUFFIXES or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        names.update(match.group(1).strip() for match in module_pattern.finditer(text))
    return names


def extras_headers() -> dict[str, list[Path]]:
    """Header paths under extras/, keyed both by repo-relative and bare name."""
    by_path: dict[str, list[Path]] = {}
    if not EXTRAS.is_dir():
        return by_path
    for path in EXTRAS.rglob("*"):
        if path.suffix.lower() not in HEADER_SUFFIXES or not path.is_file():
            continue
        relative = path.relative_to(EXTRAS).as_posix()
        by_path.setdefault(relative, []).append(path)
        by_path.setdefault(path.name, []).append(path)
    return by_path


def resolves_outside_extras(include: str, source: Path) -> bool:
    """True if any include root other than extras/ can satisfy this include."""
    relative = normalize(include)
    if not relative:
        return True

    # Quoted includes also search the directory of the including file.
    if (source.parent / relative).is_file():
        return True

    for root in SEARCH_ROOTS:
        if root == EXTRAS or not root.is_dir():
            continue
        if (root / relative).is_file():
            return True

    # A slash-free include ("Foo.hpp") also resolves next to any directory on
    # the include path, so look for the name anywhere in the non-extras trees.
    if "/" not in relative:
        name = Path(relative).name
        for root in SEARCH_ROOTS:
            if root == EXTRAS or not root.is_dir():
                continue
            if any(hit.is_file() for hit in root.rglob(name)):
                return True
    return False


def main() -> int:
    modules = extras_module_names()
    headers = extras_headers()

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
                    violations.append(f"{relative_path} imports extras module {module_name}")

            for match in include_pattern.finditer(text):
                include = normalize(match.group(2))
                candidates = headers.get(include) or headers.get(Path(include).name) if include else None
                if not candidates:
                    continue
                if not resolves_outside_extras(include, path):
                    spelled = candidates[0].relative_to(ROOT).as_posix()
                    violations.append(f"{relative_path} includes {spelled} from extras/ (written as '{include}')")

    if violations:
        print("Core-to-extras dependency boundary violated:", file=sys.stderr)
        for violation in sorted(set(violations)):
            print(f"  - {violation}", file=sys.stderr)
        print("Move the implementation into core or expose a public core API consumed by extras.", file=sys.stderr)
        return 1

    print(f"Core/extras boundary OK ({len(modules)} optional modules, {len([k for k in headers if '/' in k])} extras headers indexed).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
