#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject casts that turn an error into its ordinal.

ZHLN::ErrorCode keeps its two words private and has no conversion to an integral
type, so `static_cast<int>(code)` -- and every other spelling of it -- is
ill-formed by construction. tests/core/TestError.cpp pins that with
static_asserts, and the compiler is the guarantee. Two shapes survive it, which
is what this check watches:

  1. The enumerator unpacked out of the code: `static_cast<uint32_t>(err.As<E>())`
     compiles, and it is exactly the shape that had every font warning printing
     "2" where the annotation says "BMFont descriptor lacks info/common/page/char
     data". One site needs the number on purpose -- the Lua boundary, which hands
     the script host an ordinal -- and it is named, with its reason, in
     configure/error_ordinal_allowlist.json. An allowlist entry that stops
     matching fails this check, so the exemption cannot outlive what it exempted.

  2. The words themselves, published again: `res.error().value`, `err.category`.
     The seal is what makes them private, and this is the one edit that would
     reopen the question silently.

`bit_cast` and `reinterpret_cast` are in the trigger list because they are the
other two ways to read a trivially copyable type's bytes. The floor under all of
this is real and deliberate: an eight-byte trivially copyable pair is what the
FFI and the wire format read, so the bytes cannot be hidden -- only the typed
paths to them can be, which is every path but the ones this check names.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ALLOWLIST = ROOT / "configure" / "error_ordinal_allowlist.json"

# The trees a first-party error can be spelled in. configure/ is deliberately
# absent: the checkers talk about these shapes in their own docstrings.
SCAN_ROOTS = ("src", "include", "plugins", "extensions", "gameplay", "modules", "app", "samples", "tools", "tests")
SOURCE_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".cpp", ".cc", ".cxx", ".cppm", ".ixx"}
SKIP_DIR_NAMES = {".git", "build", "target", "__pycache__", ".cache", "node_modules"}

CAST_KEYWORDS = ("static_cast", "bit_cast", "reinterpret_cast")

# What makes a cast argument an error expression: the call shapes, the names an
# error travels under, and a word read through any of them.
ERROR_CALL = re.compile(r"\.(?:error\(\)|As\s*<)")

# Only an integral target is a way out: `static_cast<ScriptError>(error)` turns a
# code into the enum that names it, which is the intended direction, and
# `static_cast<SomeHandle>(...)` is somebody else's type entirely.
INTEGRAL_TARGET = re.compile(
    r"^(?:(?:const|volatile)\s+)*"
    r"(?:"
    r"(?:std::)?(?:u?int(?:8|16|32|64)_t|size_t|ptrdiff_t|intptr_t|uintptr_t|streamsize)|"
    r"(?:std::)?underlying_type_t\s*<[^>]*>|"
    r"bool|char|signed\s+char|unsigned\s+char|"
    r"short(?:\s+int)?|unsigned\s+short(?:\s+int)?|"
    r"int|unsigned(?:\s+int)?|"
    r"long(?:\s+int)?|long\s+long(?:\s+int)?|"
    r"unsigned\s+long(?:\s+long)?(?:\s+int)?"
    r")$"
)
ERROR_NAME = re.compile(r"(?<![\w.])(?:err|error|errCode|errorCode)(?![\w])")
ERROR_WORD_READ = re.compile(r"(?<![\w.])(?:err|error|errCode|errorCode|code)\s*\.\s*(?:value|category)\b")
UNPACKED_WORD_READ = re.compile(r"\.(?:error\(\)|As\s*<[^>]*>\s*\(\s*\))\s*\.\s*(?:value|category)\b")


_COMMENT_OR_LITERAL_RE = re.compile(
    r"//[^\n]*"
    r"|/\*[\s\S]*?\*/"
    r'|R"([^()\\\s]{0,16})\([\s\S]*?\)\1"'
    r'|"(?:\\[\s\S]|[^"\\\n])*"'
    r"|'(?:\\[\s\S]|[^'\\\n])*'"
)
_PREFILTER_RE = re.compile(r"\b(?:static_cast|bit_cast|reinterpret_cast)\b|\.\s*(?:value|category)\b")


def _blank_match(match: re.Match[str]) -> str:
    span = match.group(0)
    if "\n" not in span:
        return " " * len(span)
    return "\n".join(" " * len(part) for part in span.split("\n"))


def blank_out_comments_and_strings(text: str) -> str:
    """Return @p text with comments and string/char literals replaced by spaces.

    Offsets and line numbers are preserved so a violation still points at the
    line a reader sees. A cast mentioned in prose -- or in one of this file's own
    docstrings -- is not a cast.
    """
    return _COMMENT_OR_LITERAL_RE.sub(_blank_match, text)


def cast_sites(code: str):
    """Yield (offset, cast text, argument text) for every explicit integral cast.

    The argument is taken by balancing parentheses rather than with a regex, so
    `static_cast<int>(res.error().As<CodecError>())` is seen whole.
    """
    for match in re.finditer(r"\b(" + "|".join(CAST_KEYWORDS) + r")\s*<", code):
        i = match.end()  # just past '<'
        depth = 1
        while i < len(code) and depth:
            if code[i] == "<":
                depth += 1
            elif code[i] == ">":
                depth -= 1
            i += 1
        if depth:
            continue
        target = re.sub(r"\s+", " ", code[match.end() : i - 1].strip())
        if not INTEGRAL_TARGET.match(target):
            continue
        while i < len(code) and code[i].isspace():
            i += 1
        if i >= len(code) or code[i] != "(":
            continue
        start = i + 1
        depth = 1
        i += 1
        while i < len(code) and depth:
            if code[i] == "(":
                depth += 1
            elif code[i] == ")":
                depth -= 1
            i += 1
        if depth:
            continue
        yield match.start(), code[match.start() : i], code[start : i - 1]


def violations_in(path: Path):
    """Yield (line number, source line) for every ordinal escape in @p path."""
    raw = path.read_text(errors="replace")
    if not _PREFILTER_RE.search(raw):
        return
    code = blank_out_comments_and_strings(raw)
    lines = raw.splitlines()

    def line_of(offset: int) -> str:
        number = code.count("\n", 0, offset)
        return lines[number].strip() if number < len(lines) else ""

    for offset, _cast, argument in cast_sites(code):
        if ERROR_CALL.search(argument) or ERROR_NAME.search(argument) or ERROR_WORD_READ.search(argument):
            yield code.count("\n", 0, offset) + 1, line_of(offset)

    for match in UNPACKED_WORD_READ.finditer(code):
        yield code.count("\n", 0, match.start()) + 1, line_of(match.start())

    for match in ERROR_WORD_READ.finditer(code):
        yield code.count("\n", 0, match.start()) + 1, line_of(match.start())


def load_allowlist():
    entries = json.loads(ALLOWLIST.read_text())["allow"] if ALLOWLIST.exists() else []
    return entries


def _scan_files() -> list[Path]:
    found: list[Path] = []
    for root_name in SCAN_ROOTS:
        root = ROOT / root_name
        if not root.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
            dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIR_NAMES)
            base = Path(dirpath)
            for name in sorted(filenames):
                candidate = base / name
                if candidate.suffix in SOURCE_SUFFIXES and candidate.is_file():
                    found.append(candidate)
    found.sort()
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--file", action="append", default=[], help="Check one file (relative to the caller's cwd). Repeatable.")
    args = parser.parse_args()

    if args.file:
        files = [Path(f).resolve() for f in args.file]
    else:
        files = _scan_files()

    allow = load_allowlist()
    used = set()
    violations: list[str] = []
    for path in files:
        try:
            relative = path.relative_to(ROOT).as_posix()
        except ValueError:
            relative = path.as_posix()
        for line_number, line in sorted(set(violations_in(path))):
            sanctioned = [entry for entry in allow if entry["file"] == relative and entry["match"] in line]
            if sanctioned:
                used.update(entry["match"] for entry in sanctioned)
                continue
            violations.append(f"{relative}:{line_number}  {line}")

    # A targeted run sees one file, so it cannot judge whether an entry is still
    # needed; only the full walk fails on an exemption that outlived its site.
    if not args.file:
        for index, entry in enumerate(allow):
            if entry["match"] not in used:
                violations.append(
                    f"configure/error_ordinal_allowlist.json  entry {index} no longer matches anything in {entry['file']}: "
                    f"{entry['match']}"
                )

    if violations:
        print("Error ordinal escapes:", file=sys.stderr)
        for violation in violations:
            print(f"  - {violation}", file=sys.stderr)
        print(
            "\nAn error is a diagnostic, not a number. Its two words are private and carry no conversion\n"
            "to an integral type, so a logging site formats the code (Log(\"... {}\", res.error()) prints the\n"
            "annotated message) instead of reducing it to an ordinal that means nothing without its\n"
            "category. Where code genuinely has to cross as a number -- a scripting ABI, a wire format --\n"
            "spell it `err.As<E>()`, category-checked, and name the site in\n"
            "configure/error_ordinal_allowlist.json with its reason.",
            file=sys.stderr,
        )
        return 1

    sanctioned = "1 sanctioned unpack" if len(allow) == 1 else f"{len(allow)} sanctioned unpacks"
    print(f"Error ordinals OK ({len(files)} files scanned, {sanctioned}).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
