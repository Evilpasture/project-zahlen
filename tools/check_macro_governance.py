#!/usr/bin/env python3
"""Govern macro use: allowlist every #define, denylist the ones we retired.

A macro is the one C++ construct that escapes every other rule in this
repository. It has no namespace, no type, no overload set and no scope, it is
invisible to clang-tidy's readability checks, and a `[[nodiscard]]` one used as
a bare statement only shows up as a warning nobody reads. So macros are not
added casually: they are added to tools/macro_allowlist.json, on purpose, with
the name.

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
import os
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
    # Always absolute: every report formats with path.relative_to(ROOT), and a
    # --file passed on the command line is relative to the caller's cwd.
    if paths:
        for p in paths:
            yield Path(p).resolve()
        return
    for root in SOURCE_ROOTS:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if path.suffix.lower() in CXX_SUFFIXES and path.is_file():
                yield path.resolve()


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
                        f"{path.relative_to(ROOT)}:{lineno}: banned macro {name} used"
                    )


# The tree's own headers plus every vendored include root the engine builds
# against. Without these a translation unit stops at its first #include and the
# pass silently checks nothing.
INCLUDE_DIRS = (
    "include", "src", "extras", "tests",
    "extern/JoltPhysics", "extern/yoga", "extern/glfw/include", "extern/imgui",
    "extern/Vulkan-Headers/include", "extern/VulkanMemoryAllocator/include",
    "extern/volk", "extern/cgltf", "extern/stb", "extern/miniaudio",
    "extern/mimalloc/include", "extern/meshoptimizer", "extern/LuaJIT/src",
)


def _load_shared_library(cindex) -> bool:
    """Point the bindings at libclang.so.

    `pip install clang` ships only the ctypes bindings; the shared library comes
    from the separate `libclang` wheel, which drops it in the package's native/
    directory rather than anywhere the dynamic loader looks. Probe for it there
    and in LIBCLANG_PATH instead of failing.
    """
    try:
        cindex.Index.create()
        return True
    except Exception:
        pass

    import pathlib
    candidates: list[pathlib.Path] = []
    env = os.environ.get("LIBCLANG_PATH")
    if env:
        candidates.append(pathlib.Path(env))
    try:
        import clang as _clang
        candidates.append(pathlib.Path(_clang.__file__).parent)
    except Exception:
        pass
    candidates.append(pathlib.Path(sys.prefix) / "lib")

    for base in candidates:
        roots = [base] if base.is_file() else sorted(base.rglob("libclang*.so*")) if base.is_dir() else []
        for cand in roots:
            try:
                cindex.Config.set_library_file(str(cand))
                cindex.Index.create()
                return True
            except Exception:
                continue

    print(
        "  note: libclang bindings installed but no libclang.so found; expansion pass SKIPPED "
        "(definitions and denylist still enforced). pip install libclang, or set LIBCLANG_PATH.",
        file=sys.stderr,
    )
    return False


# A file that is a translation unit in its own right. Anything else (.h, .inl)
# is a fragment: it may depend on inclusion order or a PCH, so failing to parse
# it standalone says nothing about whether the tree is clean.
TU_SUFFIXES = (".cpp", ".cppm", ".c", ".cc", ".cxx")


def _defined_in_repo(cursor) -> bool:
    """True when a macro expansion resolves to a #define inside this repo.

    Returns False for platform and vendored macros, which are not ours to
    allowlist, and True when the definition cannot be resolved -- an unknown
    provenance is reported rather than silently excused.
    """
    definition = cursor.get_definition()
    if definition is None or definition.location.file is None:
        return True
    try:
        origin = Path(definition.location.file.name).resolve()
    except Exception:
        return True
    if not origin.is_relative_to(ROOT):
        return False
    return not is_vendored(origin)


def check_expansions_libclang(paths, allowed, banned, third_party, violations, unparsed) -> int:
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

    if not _load_shared_library(cindex):
        return 0

    index = cindex.Index.create()
    flags = ["-DZHLN_ENGINE_BUILD", "-DJPH_DOUBLE_PRECISION", "-DJPH_SHARED_LIBRARY", "-DGLFW_INCLUDE_NONE"]
    flags += [f"-I{ROOT / d}" for d in INCLUDE_DIRS if (ROOT / d).is_dir()]
    # libclang does not know this project's toolchain. Without the right
    # -isystem/-D it stops at the first libc++ header, and ZHLN_LIBCLANG_FLAGS
    # is how a caller hands it one.
    env_flags = os.environ.get("ZHLN_LIBCLANG_FLAGS", "").split()
    if env_flags:
        flags += env_flags
        print(f"  using {len(env_flags)} flags from ZHLN_LIBCLANG_FLAGS", file=sys.stderr)
    scanned = 0
    for path in iter_sources(paths):
        if is_vendored(path):
            continue
        # A .inl is an included fragment, never a translation unit of its own.
        # It is still covered by the definitions pass, and by this pass when the
        # TU that includes it is parsed.
        if path.suffix == ".inl":
            continue
        # The language flag depends on the file: handing -std=c++26 to a .c
        # file makes libclang refuse the translation unit outright, which is a
        # fatal load error rather than a diagnostic.
        lang_flags = ["-x", "c", "-std=c23"] if path.suffix == ".c" else ["-std=c++26"]
        try:
            tu = index.parse(
                str(path), args=lang_flags + flags,
                options=cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD,
            )
        except Exception as exc:
            if path.suffix in TU_SUFFIXES:
                unparsed.append((path, str(exc)[:120]))
            else:
                print(f"  note: could not parse {path.name} standalone; skipped", file=sys.stderr)
            continue
        # Did the preprocessor actually reach THIS file? Macro records are a
        # preprocessing artefact, so they are complete even when semantic
        # analysis later fails (libclang 18 cannot parse the C++26 this project
        # is written in). What must not happen is counting a file as checked
        # because it yielded nothing: a checker that reports OK having looked
        # at nothing is worse than no checker.
        target = str(path.resolve())
        records = 0
        for cursor in tu.cursor.get_children():
            if not cursor.location.file:
                continue
            if str(Path(cursor.location.file.name).resolve()) != target:
                continue
            records += 1
            name = cursor.spelling
            if cursor.kind == cindex.CursorKind.MACRO_DEFINITION:
                if name not in allowed:
                    violations.append(
                        f"{path.relative_to(ROOT)}:{cursor.location.line}: #define {name} not allowlisted"
                    )
            elif cursor.kind == cindex.CursorKind.MACRO_INSTANTIATION:
                if name in banned:
                    # A ban is enforced wherever the macro comes from: banning
                    # `assert` means banning its use, not just its definition.
                    violations.append(
                        f"{path.relative_to(ROOT)}:{cursor.location.line}: banned macro {name} expanded"
                    )
                elif name not in allowed and not any(name.startswith(p) for p in third_party):
                    # Identifiers containing a double underscore, or _ followed
                    # by a capital, are RESERVED for the implementation. The
                    # compiler defines them itself (__clang__, __linux__,
                    # __has_feature) -- they have no #define of ours to govern.
                    if name.startswith("__") or (name.startswith("_") and name[:2].isupper()):
                        continue
                    # Only macros THIS REPOSITORY defines are ours to govern.
                    # Every file also expands libc/libstdc++ macros (stderr,
                    # SEEK_SET, errno); demanding those be allowlisted would
                    # bury the signal. Ask libclang where the macro came from.
                    if _defined_in_repo(cursor):
                        violations.append(
                            f"{path.relative_to(ROOT)}:{cursor.location.line}: expansion of "
                            f"non-allowlisted macro {name}"
                        )
        if records == 0:
            why = next((d.spelling for d in tu.diagnostics if d.severity >= cindex.Diagnostic.Error), None)
            if why is None:
                # Parsed cleanly but nothing came back for this file. libclang
                # 18 attributes no cursors inside a C++20 module interface unit,
                # so this is a limit of the parser, not evidence about the tree.
                # Say so rather than either failing or silently passing.
                print(f"  note: {path.name} yielded no macro records "
                      f"(module units are not fully supported by libclang); "
                      f"covered by the definitions pass", file=sys.stderr)
                continue
            # A real translation unit that will not preprocess means this pass
            # looked at nothing, and that must not be reported as a pass. A
            # header is only ever a fragment: it may depend on inclusion order
            # or a PCH, so it is noted and left to the definitions pass.
            if path.suffix in TU_SUFFIXES:
                unparsed.append((path, str(why)[:120]))
            else:
                print(f"  note: {path.name} is not a standalone TU; covered by the "
                      f"definitions pass", file=sys.stderr)
            continue
        scanned += 1
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
    unparsed: list[tuple] = []
    scanned = check_definitions(args.file, allowed, violations)
    check_banned(args.file, banned, violations)
    expansion_scanned = 0
    if args.full:
        expansion_scanned = check_expansions_libclang(
            args.file, allowed, banned, third_party, violations, unparsed)

    if violations:
        print("Macro governance violated:", file=sys.stderr)
        for v in sorted(set(violations)):
            print(f"  - {v}", file=sys.stderr)
        print(
            "\nAdd the macro to allowed_macros in tools/macro_allowlist.json, or "
            "delete it. A macro has no namespace, no type and no scope; it is "
            "the one construct every other check in this repository is blind to.",
            file=sys.stderr,
        )
        return 1

    scope = "1 file" if args.file else f"{scanned} sources"
    extra = f", {expansion_scanned} parsed for expansions" if args.full else ""
    print(f"Macro governance OK ({scope}, {len(allowed)} allowlisted, {len(banned)} banned{extra}).")

    if args.full and unparsed:
        print(
            f"  WARNING: {len(unparsed)} of {len(unparsed) + expansion_scanned} files could not be parsed, "
            f"so the expansion pass did NOT check them. This is not a pass -- it is an unchecked tree.\n"
            f"  Supply this project's C++ include paths and predefined macros in ZHLN_LIBCLANG_FLAGS \n"
            f"  (space-separated, as compiler arguments) so libclang can preprocess the real sources.\n"
            f"  First was {unparsed[0][0].name}: {unparsed[0][1]}",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
