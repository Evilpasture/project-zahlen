#!/usr/bin/env python3
"""Keep reflection internals inside Reflection.hpp and modules' details unexported.

Three invariants, enforced at CMake configure time:

1. No module interface unit exports a namespace named ``detail`` -- neither
   ``export namespace ZHLN::Wire::detail`` nor a ``namespace detail`` declared
   inside an ``export namespace ...`` / ``export { ... }`` block. A namespace
   nested inside an exported namespace is exported too, so both forms leak the
   implementation namespace to importers; an exported detail namespace must
   never exist again.

2. No C++ source outside ``include/Zahlen/Core/Reflection.hpp`` spells
   ``std::meta::``, ``^^`` or ``[:`` (the splice opener) -- i.e. no raw
   reflection tokens. Reflection.hpp is the single home of the machinery;
   everything else consumes its public API. The P3394 annotation syntax
   ``[[= ...]]`` is source-level metadata, not a raw token, and is exempt.

3. No source outside ``include/Zahlen/Core/Reflection.hpp`` reaches into
   ``ZHLN::Reflect::detail``. That namespace is the implementation boundary
   the first two rules draw: code that needs a reflection primitive adds it
   to the public API in Reflection.hpp instead.

Comments and string/character literals are ignored so documentation about the
tokens (this file, and comments in Scripting headers) cannot fail the check.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REFLECTION_HEADER = ROOT / "include" / "Zahlen" / "Core" / "Reflection.hpp"

# Module interface units that may declare detail namespaces.
MODULE_ROOTS = (ROOT / "modules", ROOT / "extras")
# C++ source trees that must contain no raw reflection tokens.
SOURCE_ROOTS = (ROOT / "modules", ROOT / "extras", ROOT / "src", ROOT / "include", ROOT / "app", ROOT / "tests")
CXX_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".cppm", ".ixx", ".inl", ".ipp"}
MODULE_SUFFIXES = {".cppm", ".ixx"}

export_block = re.compile(r"\bexport\s+(?:(?:namespace\s+[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\s*)?\{")
namespace_decl = re.compile(r"\bnamespace\s+([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\b")
raw_token = re.compile(r"std::meta::|\^\^|\[:")
reflect_detail = re.compile(r"\b(?:ZHLN::)?Reflect::detail\b")


def strip_comments_and_strings(text: str) -> str:
    """Replace comments and string/char literals with spaces (newlines kept).

    Braces inside string literals ('{}' is all over the Wire format strings)
    must not confuse the namespace-block walker.
    """
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n:
            if text[i + 1] == "/":
                while i < n and text[i] != "\n":
                    out[i] = " "
                    i += 1
                continue
            if text[i + 1] == "*":
                out[i] = out[i + 1] = " "
                i += 2
                while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                    if text[i] != "\n":
                        out[i] = " "
                    i += 1
                if i + 1 < n:
                    out[i] = out[i + 1] = " "
                    i += 2
                continue
        if c in "\"'":
            quote, out[i] = c, " "
            i += 1
            while i < n:
                if text[i] == "\\":
                    out[i] = " "
                    i += 1
                    if i < n:
                        out[i] = " "
                        i += 1
                    continue
                if text[i] == quote:
                    out[i] = " "
                    i += 1
                    break
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            continue
        i += 1
    return "".join(out)


def matching_brace(text: str, open_index: int) -> int:
    depth = 0
    for i in range(open_index, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return i
    return len(text)


def line_of(text: str, index: int) -> int:
    return text.count("\n", 0, index) + 1


def check_module_details(path: Path, violations: list[str]) -> int:
    """Rule 1: no detail namespace may be exported by a module interface unit."""
    text = path.read_text(encoding="utf-8", errors="ignore")
    clean = strip_comments_and_strings(text)
    count = 0
    for block in export_block.finditer(clean):
        close = matching_brace(clean, block.end() - 1)
        for ns in namespace_decl.finditer(clean, block.start(), close + 1):
            name = ns.group(1)
            if name.split("::")[-1] == "detail":
                violations.append(
                    f"{path.relative_to(ROOT)}:{line_of(clean, ns.start())} exports detail namespace '{name}' "
                    f"(detail namespaces must stay module-private)"
                )
                count += 1
    return count


def check_raw_reflection(path: Path, violations: list[str]) -> int:
    """Rules 2 and 3: no raw reflection tokens / Reflect::detail outside Reflection.hpp."""
    if path == REFLECTION_HEADER:
        return 0
    text = path.read_text(encoding="utf-8", errors="ignore")
    clean = strip_comments_and_strings(text)
    count = 0
    for m in raw_token.finditer(clean):
        violations.append(
            f"{path.relative_to(ROOT)}:{line_of(clean, m.start())} uses a raw reflection token "
            f"'{m.group(0)}' outside include/Zahlen/Core/Reflection.hpp"
        )
        count += 1
    for m in reflect_detail.finditer(clean):
        violations.append(
            f"{path.relative_to(ROOT)}:{line_of(clean, m.start())} reaches into ZHLN::Reflect::detail "
            f"outside include/Zahlen/Core/Reflection.hpp"
        )
        count += 1
    return count


def main() -> int:
    violations: list[str] = []

    module_count = 0
    for root in MODULE_ROOTS:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.suffix.lower() in MODULE_SUFFIXES and path.is_file():
                module_count += 1
                check_module_details(path, violations)

    source_count = 0
    for root in SOURCE_ROOTS:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.suffix.lower() not in CXX_SUFFIXES or not path.is_file():
                continue
            source_count += 1
            check_raw_reflection(path, violations)

    if violations:
        print("Reflection boundary violated:", file=sys.stderr)
        for violation in sorted(set(violations)):
            print(f"  - {violation}", file=sys.stderr)
        print(
            "Keep std::meta and reflection tokens in include/Zahlen/Core/Reflection.hpp, "
            "use its public API elsewhere, and never export a detail namespace.",
            file=sys.stderr,
        )
        return 1

    print(f"Reflection boundary OK ({module_count} module units, {source_count} C++ sources scanned).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
