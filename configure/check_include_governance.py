#!/usr/bin/env python3
"""Enforce platform-normalization and application-facing Jolt include policy."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ("src", "include", "app", "tests", "samples", "plugins", "extensions", "gameplay", "modules", "tools")
SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".cppm", ".ixx", ".inl", ".ipp"}
APPLICATION_ROOTS = {"app", "samples", "plugins", "extensions", "gameplay"}
PLATFORM = re.compile(r'^\s*#\s*include\s*[<"]Zahlen/Core/Platform\.hpp[>"]')
DIRECT_JOLT = re.compile(r'^\s*#\s*include\s*[<"]Jolt/Jolt\.h[>"]')
CONDITIONAL = re.compile(r'^\s*#\s*(if|ifdef|ifndef)\b')
ENDIF = re.compile(r'^\s*#\s*endif\b')


def files():
    for root_name in SOURCE_ROOTS:
        root = ROOT / root_name
        if root.exists():
            yield from (p for p in root.rglob("*") if p.is_file() and p.suffix in SUFFIXES)


def main() -> int:
    failures: list[str] = []
    for path in files():
        depth = 0
        relative = path.relative_to(ROOT)
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            if ENDIF.match(line):
                depth = max(0, depth - 1)
            if PLATFORM.match(line) and depth:
                failures.append(f"{relative}:{number}: Platform.hpp must be included unconditionally; it owns its platform guards")
            if relative.parts[0] in APPLICATION_ROOTS and DIRECT_JOLT.match(line):
                failures.append(f"{relative}:{number}: application code must include <Zahlen/Physics/Jolt.hpp>, not <Jolt/Jolt.h>")
            if CONDITIONAL.match(line):
                depth += 1
    if failures:
        print("include governance violations:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
