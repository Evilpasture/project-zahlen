#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject PIMPL escape hatches in first-party C++.

A class that hides its implementation behind a nested ``struct Impl;`` must not
hand that implementation out. An accessor returning the PIMPL -- ``GetImpl()``,
or the same door under another name -- turns "public API, plus the rare friend"
into "private state that happens to be one header away", and every caller that
reaches through one has to be rewritten the moment the implementation moves.

Three mechanical rules cover the door and the aliases people give it:

  * no function whose return type names ``Impl`` by pointer or reference, in
    trailing (``-> const Impl&``) or leading (``Impl* Get()``) form;
  * no conversion operator to the PIMPL (``operator Impl*()``);
  * no identifier that reads as "get the impl" (``GetImpl``, ``get_impl``,
    ``GetPimpl``, ...).

A deleted or defaulted special member is not an accessor, which is why
``auto operator=(const Impl&) -> Impl& = delete;`` is not a violation. Neither is
anything else a PIMPL needs to work: declaring the nested type, defining it out
of line, and holding it by ``std::unique_ptr`` in a private member. Comments and
string literals are stripped before matching, so documenting the policy is fine.

What is left is the intended surface: a class's public methods, and the
deliberate friend -- ``PipelineStatsCapture`` holding the ``RenderContext::Impl*``
only ``RenderContext`` can hand it, or ``Visit()`` reading a ``NativeSurfaceHandle``
it was granted friendship for.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent

# First-party trees. tools/ is python and shell; extern/ and third_party/ are
# vendored, and are not ours to police.
FIRST_PARTY_ROOTS = ("include", "src", "extras", "app", "modules", "samples", "tests")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".ixx", ".cppm"}
SKIP_DIR_NAMES = {".git", "build", ".cache", "__pycache__", "node_modules"}

# Comments and literals, longest alternative first, so a "//" or "/*" inside a
# string literal is not mistaken for the start of a comment.
COMMENT_OR_LITERAL_RE = re.compile(
    r"/\*.*?\*/"  # block comment
    r"|//[^\n]*"  # line comment
    r'|"(?:\\.|[^"\\\n])*"'  # string literal
    r"|'(?:\\.|[^'\\\n])*'",  # character literal
    re.DOTALL,
)

# `auto GetImpl() const -> const Impl&` / `-> Impl*` / `-> Foo::Impl&`.
TRAILING_IMPL_RETURN_RE = re.compile(r"->\s*(?:const\s+)?(?:[A-Za-z_]\w*::)*Impl\s*[&*]")
# `Impl* Get()` / `const Foo::Impl& Target()` at the start of a line. A data
# member (`Impl* _impl = nullptr;`) has no parameter list and does not match.
LEADING_IMPL_RETURN_RE = re.compile(
    r"^\s*(?:\[\[[^\]]*\]\]\s*)*(?:const\s+)?(?:[A-Za-z_]\w*::)*Impl\s*[&*]\s*[A-Za-z_]\w*\s*\(",
    re.MULTILINE,
)
# The same door through a conversion operator: `explicit operator Impl*() const`.
IMPL_CONVERSION_OPERATOR_RE = re.compile(r"\boperator\s+(?:[A-Za-z_]\w*::)*Impl\s*[&*]")
# The accessor by name, whatever verb it borrows.
IMPL_ACCESSOR_NAME_RE = re.compile(
    r"(?i)\b(?:get|grab|peek|fetch|take|obtain|access|borrow|reveal)(?:_?the)?_?impl(?:_?ptr)?\b"
)

DELETE_OR_DEFAULT_RE = re.compile(r"=\s*(?:delete|default)\b")
ASSIGNMENT_OPERATOR_RE = re.compile(r"\boperator\s*=")


def strip_comments_and_literals(text: str) -> str:
    """Blank out comments and literals, keeping the line structure intact."""
    return COMMENT_OR_LITERAL_RE.sub(lambda match: "\n" * match.group().count("\n"), text)


def line_bounds(text: str, index: int) -> tuple[int, int]:
    start = text.rfind("\n", 0, index) + 1
    end = text.find("\n", index)
    return start, len(text) if end == -1 else end


def is_deleted_special_member(text: str, match: re.Match[str]) -> bool:
    """A deleted copy/move assignment is not an accessor.

    `auto operator=(const Impl&) -> Impl& = delete;` names Impl on both sides of
    the arrow because the operators are the PIMPL's own, not because anything
    hands the PIMPL out.
    """
    start, end = line_bounds(text, match.start())
    return bool(DELETE_OR_DEFAULT_RE.search(text[start:end]) or ASSIGNMENT_OPERATOR_RE.search(text[start : match.start()]))


def check_source(path: Path) -> list[str]:
    text = strip_comments_and_literals(path.read_text(encoding="utf-8", errors="ignore"))
    relative = path.relative_to(REPOSITORY_ROOT)
    violations: list[str] = []

    patterns = (
        ("returns the PIMPL", TRAILING_IMPL_RETURN_RE),
        ("returns the PIMPL", LEADING_IMPL_RETURN_RE),
        ("converts to the PIMPL", IMPL_CONVERSION_OPERATOR_RE),
        ("names a PIMPL accessor", IMPL_ACCESSOR_NAME_RE),
    )
    for label, pattern in patterns:
        for match in pattern.finditer(text):
            if pattern is not IMPL_ACCESSOR_NAME_RE and is_deleted_special_member(text, match):
                continue
            line = text.count("\n", 0, match.start()) + 1
            violations.append(f"{relative}:{line} {label}: '{match.group().strip()}'")
    return violations


def main() -> int:
    if not any((REPOSITORY_ROOT / root).is_dir() for root in FIRST_PARTY_ROOTS):
        print(f"ERROR: no first-party tree found under {REPOSITORY_ROOT}", file=sys.stderr)
        return 2

    violations: list[str] = []
    for root in FIRST_PARTY_ROOTS:
        directory = REPOSITORY_ROOT / root
        if not directory.is_dir():
            continue
        for path in sorted(directory.rglob("*")):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            if SKIP_DIR_NAMES.intersection(path.parts):
                continue
            violations.extend(check_source(path))

    if violations:
        print("PIMPL encapsulation violations:", file=sys.stderr)
        for violation in violations:
            print(f"  - {violation}", file=sys.stderr)
        print(
            "A class's implementation is not part of its API. Delete the accessor and add a "
            "method that does the work, or -- if the caller genuinely needs the contents, as "
            "src/render needs a window's native descriptor -- name one friend in the class and "
            "have it read the member directly.",
            file=sys.stderr,
        )
        return 1

    print("PIMPL encapsulation OK (no accessor hands out an Impl).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
