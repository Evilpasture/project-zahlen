#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject tests that include engine internals.

The tests/ tree may only exercise public behaviour through public headers:

  * include/Zahlen/**
  * extras/** (optional public extras modules)
  * the in-tree test framework
  * third-party and standard-library headers

Including anything under src/ — engine systems, render internals, cooker
headers, and the rest of the private implementation — is forbidden. Do not
add ${PROJECT_SOURCE_DIR}/src to a test include path to work around this.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
SRC = ROOT / "src"
INCLUDE = ROOT / "include"
EXTRAS = ROOT / "extras"
EXTERN = ROOT / "extern"
THIRD_PARTY = ROOT / "third_party"

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".cppm", ".ixx"}
CMAKE_NAMES = {"CMakeLists.txt"}

include_pattern = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)
src_include_dir_pattern = re.compile(
    r"\$\{(?:PROJECT|CMAKE)_SOURCE_DIR\}/src\b"
    r"|\$\{PROJECT_SOURCE_DIR\}[/\\]src\b"
    r"|\$\{CMAKE_SOURCE_DIR\}[/\\]src\b"
)

# Path prefixes that only exist as engine internals. Public counterparts live
# under Zahlen/ (Zahlen/render, Zahlen/physics, Zahlen/ecs, Zahlen/Threading).
INTERNAL_PREFIXES = (
    "src/",
    "engine/",
    "render/",
    "physics/",
    "ecs/",
    "gltf/",
    "audio/",
    "tools/zcook/",
    "threading/",
)

PUBLIC_PREFIXES = (
    "Zahlen/",
    "ALife/",
    "Animation/",
    "VFX/",
)


def normalize(include: str) -> str:
    return include.replace("\\", "/").lstrip("./")


def looks_internal_prefix(include: str) -> bool:
    if include.startswith(PUBLIC_PREFIXES):
        return False
    return include.startswith(INTERNAL_PREFIXES) or any(f"/{prefix}" in f"/{include}" for prefix in INTERNAL_PREFIXES)


def exists_under(root: Path, include: str) -> bool:
    if not root.is_dir():
        return False
    candidate = root / include
    return candidate.is_file()


def resolve_bare_header(include: str) -> list[Path]:
    """Locate a slash-free include name inside known repo trees."""
    name = Path(include).name
    hits: list[Path] = []
    for root in (TESTS, INCLUDE, EXTRAS, EXTERN, THIRD_PARTY, SRC):
        if not root.is_dir():
            continue
        hits.extend(path for path in root.rglob(name) if path.is_file())
    return hits


def include_is_internal(include: str, source: Path) -> bool:
    include = normalize(include)
    if not include:
        return False
    if include == "TestsFramework.hpp" or include.endswith("/TestsFramework.hpp"):
        return False
    if include.startswith(PUBLIC_PREFIXES):
        return False
    if looks_internal_prefix(include):
        return True

    relative = (source.parent / include).resolve()
    try:
        relative.relative_to(SRC.resolve())
        return True
    except ValueError:
        pass

    if exists_under(INCLUDE, include) or exists_under(EXTRAS, include) or exists_under(TESTS, include):
        return False
    if exists_under(EXTERN, include) or exists_under(THIRD_PARTY, include):
        return False
    if exists_under(SRC, include):
        return True

    if "/" not in include:
        hits = resolve_bare_header(include)
        src_hits = [path for path in hits if SRC.resolve() in path.resolve().parents]
        public_hits = [path for path in hits if path not in src_hits]
        if src_hits and not public_hits:
            return True

    return False


def collect_source_violations() -> list[str]:
    violations: list[str] = []
    for path in TESTS.rglob("*"):
        if path.suffix.lower() not in SOURCE_SUFFIXES or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        for match in include_pattern.finditer(text):
            include = match.group(2)
            if include_is_internal(include, path):
                rel = path.relative_to(ROOT)
                violations.append(f"{rel} includes engine internal '{include}'")
    return violations


def collect_cmake_violations() -> list[str]:
    violations: list[str] = []
    for path in TESTS.rglob("*"):
        if path.name not in CMAKE_NAMES and path.suffix.lower() != ".cmake":
            continue
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        if src_include_dir_pattern.search(text):
            violations.append(f"{path.relative_to(ROOT)} adds src/ to a test include path")
    return violations


def main() -> int:
    violations = collect_source_violations() + collect_cmake_violations()
    if violations:
        print("tests/ must only exercise public behaviour:", file=sys.stderr)
        for violation in violations:
            print(f"  - {violation}", file=sys.stderr)
        print(
            "Remove the internal include or rewrite the test against include/Zahlen "
            "(or extras/). Do not add src/ to test include directories.",
            file=sys.stderr,
        )
        return 1
    print("tests/ public-API boundary OK.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
