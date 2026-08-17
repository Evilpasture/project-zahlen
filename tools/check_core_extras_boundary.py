#!/usr/bin/env python3
"""Reject dependencies from Zahlen core source trees into extras/.

Extras are allowed to consume public core APIs. The reverse dependency would
make optional feature modules mandatory, so this check runs during CMake
configuration and fails before compilation.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CORE_ROOTS = (ROOT / "src", ROOT / "include", ROOT / "modules")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".cppm", ".ixx"}

module_pattern = re.compile(r"^\s*export\s+module\s+([^;]+);", re.MULTILINE)
import_pattern = re.compile(r"^\s*(?:export\s+)?import\s+([^;]+);", re.MULTILINE)
include_extras_pattern = re.compile(r"^\s*#\s*include\s*[<\"][^>\"]*extras[/\\]", re.MULTILINE | re.IGNORECASE)

extra_modules: set[str] = set()
for path in (ROOT / "extras").rglob("*"):
    if path.suffix.lower() not in SOURCE_SUFFIXES or not path.is_file():
        continue
    text = path.read_text(encoding="utf-8", errors="ignore")
    extra_modules.update(match.group(1).strip() for match in module_pattern.finditer(text))

violations: list[str] = []
for core_root in CORE_ROOTS:
    for path in core_root.rglob("*"):
        if path.suffix.lower() not in SOURCE_SUFFIXES or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="ignore")
        if include_extras_pattern.search(text):
            violations.append(f"{path.relative_to(ROOT)} includes a path under extras/")
        for match in import_pattern.finditer(text):
            module_name = match.group(1).strip()
            if module_name in extra_modules:
                violations.append(f"{path.relative_to(ROOT)} imports extras module {module_name}")

if violations:
    print("Core-to-extras dependency boundary violated:", file=sys.stderr)
    for violation in violations:
        print(f"  - {violation}", file=sys.stderr)
    print("Move the implementation into core or expose a public core API consumed by extras.", file=sys.stderr)
    raise SystemExit(1)

print(f"Core/extras boundary OK ({len(extra_modules)} optional modules indexed).")
