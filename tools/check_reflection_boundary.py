#!/usr/bin/env python3
"""Keep reflection internals inside the reflection headers and module internals unmarked.

Five invariants, enforced at CMake configure time:

1. No module interface unit declares a namespace named ``detail``, exported or
   not. Module-internal implementation needs no marker namespace: a
   declaration that is not in an export block is internal by definition, so a
   detail namespace in a module unit only exists to be (or become) exported by
   accident. Private helpers live in the module's own namespace instead.

2. Only the reflection headers spell ``std::meta::``, ``^^`` or ``[:`` (the
   splice opener) -- i.e. only they carry raw reflection tokens. Those headers
   are ``include/Zahlen/Core/Reflection.hpp`` (the umbrella, which spells none
   itself) and the modules directly under ``include/Zahlen/Core/Reflection/``:
   Core, Enums, Annotations, Structs, Class, Dynamic, Utilities. Everything
   else consumes their public API. The directory is closed on purpose -- a
   nested subdirectory would have to be added to REFLECTION_DIRS here, which is
   the moment to ask whether the machinery really belongs in another module --
   and each module carries its own degraded stand-ins in the ``#else`` of its own
   feature guard, so a stub can never live outside a reflection header. The
   P3394 annotation syntax ``[[= ...]]`` is source-level metadata, not a raw
   token, and is exempt.

3. ``<ranges>`` comes before ``<meta>``. libc++'s ``<meta>`` -- the P2996
   library's header -- includes ``__ranges/access.h``, ``__ranges/concepts.h``
   and ``__ranges/size.h`` and then writes ``ranges::input_range``,
   ``ranges::data`` and ``ranges::size`` in its own body without including
   ``<ranges>``. A translation unit that reaches ``<meta>`` first fails inside
   ``<meta>`` with "use of undeclared identifier 'ranges'". Nothing in this
   repository can fix that header, so the include order is the invariant: any
   file that includes ``<meta>`` must have included ``<ranges>`` above it. (This
   is why the monolith listed ``<ranges>`` ahead of ``<meta>``, and it is the
   first thing to check if that error ever comes back.)

4. Nothing but the umbrella itself reaches into ``ZHLN::Reflect::detail``.
   The implementation helpers live in the per-module ``TemplatedDetail``
   instead (governed by tools/check_namespace_governance.py); code that needs a
   reflection primitive adds it to the public API rather than to a detail
   namespace.

5. Only ``Reflection/Core.hpp`` tests the feature macros
   (``__cpp_impl_reflection``, ``__has_feature(reflection)``). The result is
   published twice -- as ``ReflectionAvailable`` for code that wants a constant,
   and as ``ZHLN_REFLECTION_AVAILABLE`` for the sibling headers' guards -- so a
   module switching on the capability cannot drift into a second copy of the
   test, and a translation unit that includes one module and not Core cannot
   silently compile the wrong half.

One thing deliberately NOT checked: consumers may extend ``ZHLN::Reflect``
themselves -- Zahlen/Format.hpp specializes ``CustomFormatter`` for Entity and
Jolt's vector types, and JSONSchema.hpp adds its parsing block there. Those are
extensions of a namespace whose customization points are the API, they need no
raw token to exist, and refusing them would be a different (and much larger)
change than keeping the machinery in one place.

Comments and string/character literals are ignored so documentation about the
tokens (this file, and comments in Scripting headers) cannot fail the check.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REFLECTION_HEADER = ROOT / "include" / "Zahlen" / "Core" / "Reflection.hpp"
# The modules the umbrella includes. Direct children only: reflection lives in
# Reflection.hpp plus this directory, and nothing deeper.
REFLECTION_DIRS = (ROOT / "include" / "Zahlen" / "Core" / "Reflection",)
REFLECTION_SUFFIXES = {".hpp", ".inl"}
# The one file allowed to ask the compiler whether P2996 is available.
FEATURE_PROBE = REFLECTION_DIRS[0] / "Core.hpp"

# Module interface units (checked for detail namespace declarations).
MODULE_ROOTS = (ROOT / "modules", ROOT / "extras")
# C++ source trees that must contain no raw reflection tokens.
SOURCE_ROOTS = (ROOT / "modules", ROOT / "extras", ROOT / "src", ROOT / "include", ROOT / "app", ROOT / "tools", ROOT / "tests")
CXX_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".cppm", ".ixx", ".inl", ".ipp"}
MODULE_SUFFIXES = {".cppm", ".ixx"}

namespace_decl = re.compile(r"\bnamespace\s+([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)\b")
raw_token = re.compile(r"std::meta::|\^\^|\[:")
reflect_detail = re.compile(r"\b(?:ZHLN::)?Reflect::detail\b")
feature_probe = re.compile(r"__cpp_impl_reflection|__has_feature\s*\(\s*reflection\s*\)")
meta_include = re.compile(r"^[ \t]*#[ \t]*include[ \t]*<meta>", re.MULTILINE)
ranges_include = re.compile(r"^[ \t]*#[ \t]*include[ \t]*<ranges>", re.MULTILINE)


def is_reflection_header(path: Path) -> bool:
    """True for the umbrella and the modules directly under Reflection/."""
    if path == REFLECTION_HEADER:
        return True
    if path.suffix.lower() not in REFLECTION_SUFFIXES:
        return False
    return any(path.parent == directory for directory in REFLECTION_DIRS)


def strip_comments_and_strings(text: str) -> str:
    """Replace comments and string/char literals with spaces (newlines kept).

    Braces inside string literals ('{}' is all over the Wire format strings)
    must not confuse the namespace scan.
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


def line_of(text: str, index: int) -> int:
    return text.count("\n", 0, index) + 1


def check_module_details(path: Path, violations: list[str]) -> int:
    """Rule 1: no detail namespace may be declared by a module interface unit.

    Non-exported members are already invisible to importers, so a detail
    namespace in a module unit has no legitimate purpose -- it only exists to
    be exported by accident. Module-private helpers belong in the module's own
    namespace.
    """
    text = path.read_text(encoding="utf-8", errors="ignore")
    clean = strip_comments_and_strings(text)
    count = 0
    for ns in namespace_decl.finditer(clean):
        name = ns.group(1)
        if name.split("::")[-1] == "detail":
            violations.append(
                f"{path.relative_to(ROOT)}:{line_of(clean, ns.start())} declares detail namespace '{name}' "
                f"(module internals need no detail namespace; use a non-exported declaration in the module's own namespace)"
            )
            count += 1
    return count


def check_raw_reflection(path: Path, violations: list[str]) -> int:
    """Rules 2-4, for one source file."""
    home = is_reflection_header(path)
    text = path.read_text(encoding="utf-8", errors="ignore")
    clean = strip_comments_and_strings(text)
    count = 0
    if not home:
        for m in raw_token.finditer(clean):
            violations.append(
                f"{path.relative_to(ROOT)}:{line_of(clean, m.start())} uses a raw reflection token "
                f"'{m.group(0)}' outside include/Zahlen/Core/Reflection.hpp and "
                f"include/Zahlen/Core/Reflection/"
            )
            count += 1
    if path != REFLECTION_HEADER:
        for m in reflect_detail.finditer(clean):
            violations.append(
                f"{path.relative_to(ROOT)}:{line_of(clean, m.start())} reaches into ZHLN::Reflect::detail "
                f"(implementation helpers live in the per-module TemplatedDetail, and the public API is "
                f"everything else)"
            )
            count += 1
    for m in meta_include.finditer(clean):
        before = clean[: m.start()]
        if not ranges_include.search(before):
            violations.append(
                f"{path.relative_to(ROOT)}:{line_of(clean, m.start())} includes <meta> without <ranges> "
                f"above it; libc++'s <meta> uses ranges:: names it does not include itself"
            )
            count += 1
    if home and path != FEATURE_PROBE:
        for m in feature_probe.finditer(clean):
            violations.append(
                f"{path.relative_to(ROOT)}:{line_of(clean, m.start())} tests the reflection feature macro "
                f"directly; switch on ZHLN_REFLECTION_AVAILABLE (Reflection/Core.hpp) instead"
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
            "Keep std::meta and reflection tokens in the reflection headers "
            "(include/Zahlen/Core/Reflection.hpp and include/Zahlen/Core/Reflection/), include "
            "<ranges> above <meta>, test the reflection feature macro only in Reflection/Core.hpp, "
            "use the public API elsewhere, and declare no detail namespace in module units.",
            file=sys.stderr,
        )
        return 1

    homes = sorted(
        path.relative_to(ROOT)
        for directory in REFLECTION_DIRS
        for path in [REFLECTION_HEADER, *directory.glob("*")]
        if path.is_file()
    )
    print(
        f"Reflection boundary OK ({module_count} module units, {source_count} C++ sources scanned, "
        f"{len(homes)} reflection headers: {', '.join(str(home) for home in homes)})."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
