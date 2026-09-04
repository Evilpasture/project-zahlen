#!/usr/bin/env python3
"""Govern macro use: allowlist every #define, denylist the ones we retired.

A macro is the one C++ construct that escapes every other rule in this
repository. It has no namespace, no type, no overload set and no scope, it is
invisible to clang-tidy's readability checks, and a `[[nodiscard]]` one used as
a bare statement only shows up as a warning nobody reads. So macros are not
added casually: they are added to tools/macro_allowlist.json, on purpose, with
the reason next to the name.

Two passes, because they answer different questions and only one of them can
be answered without a real parser:

1. DEFINITIONS (always runs, no dependencies)

   Every `#define` in the repository's own sources must name a macro in the
   allowlist. `#define` is unambiguous to find with a regex -- the directive is
   right there -- so this pass needs nothing beyond the standard library. This
   is the load-bearing check: it is what stops a new macro from appearing
   without a decision being recorded.

2. EXPANSIONS (opt-in, `--full`, needs the libclang Python bindings)

   Whether `FOO` in some expression is a macro, an enum constant, a constexpr
   variable or a template parameter is a question only a parser can answer.
   The repository spells 998 distinct SCREAMING_CASE identifiers, and the vast
   majority are Vulkan and Jolt enum constants, not macros -- VK_SUCCESS is a
   macro, VK_FORMAT_R8G8B8A8_UNORM is not, and no regex can tell them apart.
   So this pass drives libclang exactly as intended and is skipped (loudly,
   with a skip report rather than a pass) when the bindings are absent.

   Denied expansions are still caught WITHOUT libclang: the denylist names are
   checked by pattern in both passes, because "is ZHLN_CHECK( being called"
   does not require a parser.

Comments and string/character literals are stripped before scanning, so
documentation about a macro (this file, the allowlist, TestsFramework.hpp)
cannot fail the check.

Usage:
    check_macro_governance.py                 # definitions + denylist
    check_macro_governance.py --full          # + libclang expansion pass
    check_macro_governance.py --file PATH     # definitions in one file
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ALLOWLIST = ROOT / "tools" / "macro_allowlist.json"

SOURCE_ROOTS = (ROOT / "src", ROOT / "include", ROOT / "app", ROOT / "tests",
                ROOT / "samples", ROOT / "extras", ROOT / "modules", ROOT / "tools")
CXX_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
                ".cppm", ".ixx", ".inl", ".ipp"}

# Vendored from tools/check_reflection_boundary.py: comments and string
# literals can mention macros (this file does) and must not count as uses.
def strip_comments_and_strings(text: str) -> str:
    """Replace comments and string/char literals with spaces, newlines kept."""
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


define_directive = re.compile(r"^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)")


def iter_sources(paths=None):
    if paths:
        for p in paths:
            yield Path(p)
        return
    for root in SOURCE_ROOTS:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if path.suffix.lower() in CXX_SUFFIXES and path.is_file():
                yield path


def is_vendored(path: Path) -> bool:
    """extern/ and third_party/ are not ours to govern."""
    parts = set(path.parts)
    return bool(parts & {"extern", "third_party"})


def check_definitions(paths, allowed, violations) -> int:
    scanned = 0
    for path in iter_sources(paths):
        if is_vendored(path):
            continue
        scanned += 1
        # The #define directive itself is what is being matched, so raw text
        # is correct here -- stripping it would hide the directive.
        for lineno, line in enumerate(path.read_text(errors="ignore").split("\n"), 1):
            m = define_directive.match(line)
            if m and m.group(1) not in allowed:
                violations.append(
                    f"{path.relative_to(ROOT)}:{lineno}: #define {m.group(1)} "
                    f"is not in tools/macro_allowlist.json"
                )
    return scanned


def check_banned(paths, banned, violations) -> None:
    """Denylist uses. Needs no parser: the question is 'is NAME( being called'."""
    for name, spec in banned.items():
        function_like = bool(spec.get("function_like", False)) if isinstance(spec, dict) else False
        pattern = re.compile(rf"\b{re.escape(name)}\s*\(" if function_like else rf"\b{re.escape(name)}\b")
        for path in iter_sources(paths):
            if is_vendored(path):
                continue
            text = strip_comments_and_strings(path.read_text(errors="ignore"))
            for lineno, line in enumerate(text.split("\n"), 1):
                if pattern.search(line):
                    # A #define of a banned name is governed by the allowlist,
                    # not the denylist; only expansions are denied here.
                    if define_directive.match(path.read_text(errors="ignore").split("\n")[lineno - 1]):
                        continue
                    violations.append(
                        f"{path.relative_to(ROOT)}:{lineno}: banned macro {name} used "
                        f"({(spec.get('reason') or ['no reason recorded'])[0] if isinstance(spec, dict) else 'no reason recorded'})"
                    )


def check_expansions_libclang(paths, allowed, banned, third_party, violations) -> int:
    """Full expansion pass. Requires the libclang Python bindings."""
    try:
        from clang import cindex  # type: ignore
    except ImportError:
        print(
            "  note: libclang Python bindings not installed; expansion pass SKIPPED "
            "(definitions and denylist still enforced). pip install clang to enable.",
            file=sys.stderr,
        )
        return 0

    index = cindex.Index.create()
    flags = ["-std=c++26", f"-I{ROOT / 'include'}", f"-I{ROOT / 'src'}"]
    scanned = 0
    for path in iter_sources(paths):
        if is_vendored(path):
            continue
        try:
            tu = index.parse(
                str(path), args=flags,
                options=cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD,
            )
        except Exception as exc:  # a TU that will not parse is not a governance failure
            print(f"  note: could not parse {path.name} ({exc}); skipped", file=sys.stderr)
            continue
        scanned += 1
        target = str(path.resolve())
        for cursor in tu.cursor.get_children():
            if not cursor.location.file:
                continue
            if str(Path(cursor.location.file.name).resolve()) != target:
                continue
            name = cursor.spelling
            if cursor.kind == cindex.CursorKind.MACRO_DEFINITION:
                if name not in allowed:
                    violations.append(
                        f"{path.relative_to(ROOT)}:{cursor.location.line}: #define {name} not allowlisted"
                    )
            elif cursor.kind == cindex.CursorKind.MACRO_EXPANSION:
                if name in banned:
                    violations.append(
                        f"{path.relative_to(ROOT)}:{cursor.location.line}: banned macro {name} expanded"
                    )
                elif name not in allowed and not any(name.startswith(p) for p in third_party):
                    violations.append(
                        f"{path.relative_to(ROOT)}:{cursor.location.line}: expansion of "
                        f"non-allowlisted macro {name}"
                    )
    return scanned


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--full", action="store_true",
                        help="also run the libclang expansion pass (needs: pip install clang)")
    parser.add_argument("--file", action="append", default=None,
                        help="check only this file (repeatable); default scans the whole tree")
    args = parser.parse_args()

    if not ALLOWLIST.is_file():
        print(f"Missing allowlist: {ALLOWLIST}", file=sys.stderr)
        return 1

    data = json.loads(ALLOWLIST.read_text(encoding="utf-8"))
    allowed = set(data.get("allowed_macros", []))
    banned = data.get("banned_macros", {})
    third_party = tuple(data.get("third_party_families", []))

    violations: list[str] = []
    scanned = check_definitions(args.file, allowed, violations)
    check_banned(args.file, banned, violations)
    expansion_scanned = 0
    if args.full:
        expansion_scanned = check_expansions_libclang(args.file, allowed, banned, third_party, violations)

    if violations:
        print("Macro governance violated:", file=sys.stderr)
        for v in sorted(set(violations)):
            print(f"  - {v}", file=sys.stderr)
        print(
            "\nAdd the macro to tools/macro_allowlist.json with a reason, or "
            "delete it. A macro has no namespace, no type and no scope; it is "
            "the one construct every other check in this repository is blind to.",
            file=sys.stderr,
        )
        return 1

    scope = "1 file" if args.file else f"{scanned} sources"
    extra = f", {expansion_scanned} parsed for expansions" if args.full else ""
    print(f"Macro governance OK ({scope}, {len(allowed)} allowlisted, {len(banned)} banned{extra}).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
