#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reject internal state that reaches the public API.

Two doors are checked, and both of them are the same mistake: something an
engine type owns privately, handed out because it was easier than adding a
method that does the work.

1. PIMPL accessors. A class that hides its implementation behind a nested
   ``struct Impl;`` must not hand that implementation out -- ``GetImpl()``, or
   the same door under another name:

     * no function whose return type names ``Impl`` by pointer or reference, in
       trailing (``-> const Impl&``) or leading (``Impl* Get()``) form;
     * no conversion operator to the PIMPL (``operator Impl*()``);
     * no identifier that reads as "get the impl" (``GetImpl``, ``get_impl``,
       ``GetPimpl``, ...).

   A deleted or defaulted special member is not an accessor, which is why
   ``auto operator=(const Impl&) -> Impl& = delete;`` is not a violation. Neither
   is anything else a PIMPL needs to work: declaring the nested type, defining it
   out of line, and holding it by ``std::unique_ptr`` in a private member.
   Comments and string literals are stripped before matching.

2. The presentation seam. ``PresentationTarget`` (src/window/PresentationTarget.hpp)
   is an engine internal: the renderer's low-level verbs take one, and the kernel
   -- which owns the session and every window in it -- is what resolves a frame's
   destination and hands callers an attachment. So:

     * no ``GetPresentationTarget`` anywhere in first-party code;
     * the type is named only in the headers allowed to name it, and where it is
       named in Window.hpp / PlatformHost.hpp it sits in a private section (the
       forward declaration and the friends that read it), never in a public one;
     * each producer grants friendship to exactly one class, and the engine's
       reach stops at the façade: ``Window`` friends ``PlatformHost`` (in its own
       subsystem, for the windowed case of the session's target) and
       ``PlatformHost`` friends ``Kernel`` (the composition root that resolves
       frames). A friend *function* declared in a public header would be
       reachable by ADL from anything that includes it, seam type uncompleted, so
       the doors are private members behind one class friendship each.

What is left is the intended surface: a class's public methods, and the
deliberate friends -- ``PipelineStatsCapture`` holding the ``RenderContext::Impl*``
only ``RenderContext`` can hand it, ``Kernel`` asking ``PlatformHost`` which
target a frame is drawn into, ``Visit()`` reading a ``NativeSurfaceHandle``, and
``PlatformHost`` asking the window it owns for the windowed case of its target.
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

PUBLIC_INCLUDE_ROOT = "include"

# Comments and literals, longest alternative first, so a "//" or "/*" inside a
# string literal is not mistaken for the start of a comment.
COMMENT_OR_LITERAL_RE = re.compile(
    r"/\*.*?\*/"  # block comment
    r"|//[^\n]*"  # line comment
    r'|"(?:\\.|[^"\\\n])*"'  # string literal
    r"|'(?:\\.|[^'\\\n])*'",  # character literal
    re.DOTALL,
)

# --- Rule 1: PIMPL accessors ---

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

# --- Rule 2: the presentation seam ---

PRESENTATION_ACCESSOR_RE = re.compile(r"\bGetPresentationTarget\b")
PRESENTATION_TYPE_RE = re.compile(r"\bPresentationTarget\b")
# The public headers that may name the seam type at all: the low-level renderer
# that takes one, and the two producers that hand it to the kernel privately.
PRESENTATION_TYPE_ALLOWED_IN = {
    "include/Zahlen/Render/RenderContext.hpp",
    "include/Zahlen/Window.hpp",
    "include/Zahlen/PlatformHost.hpp",
}
# Headers where the type may only appear where it is not public: the forward
# declaration, and the private friends/members that read a producer's target.
PRESENTATION_PRIVATE_ONLY_IN = {
    "include/Zahlen/Window.hpp",
    "include/Zahlen/PlatformHost.hpp",
}
FORWARD_DECLARATION_RE = re.compile(r"^class\s+PresentationTarget\s*;$")
ACCESS_SPECIFIER_RE = re.compile(r"^(public|private|protected)\s*:")
# The machinery both producers may let in, and nothing else. Window lends its
# target to the host that presents a windowed session, which is the same
# subsystem; PlatformHost lends the session's targets to the kernel, which is the
# one engine class that orchestrates frames. Engine code in a window is how the
# layering got reversed before, and a friend *function* would be an ADL-reachable
# door, so both are rejected outright.
FRIEND_CLASS_RE = re.compile(r"\bfriend\s+(?:class|struct)\s+([A-Za-z_]\w*)")
CLASS_FRIENDS_ALLOWED_IN = {
    "include/Zahlen/Window.hpp": frozenset({"PlatformHost"}),
    "include/Zahlen/PlatformHost.hpp": frozenset({"Kernel"}),
}


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


def impl_accessor_violations(relative: str, text: str) -> list[str]:
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


def presentation_violations(relative: str, text: str) -> list[str]:
    violations: list[str] = []

    allowed_class_friends = CLASS_FRIENDS_ALLOWED_IN.get(relative)
    if allowed_class_friends is not None:
        for match in FRIEND_CLASS_RE.finditer(text):
            if match.group(1) in allowed_class_friends:
                continue
            line = text.count("\n", 0, match.start()) + 1
            violations.append(
                f"{relative}:{line} grants class friendship to '{match.group(1)}'. Window may friend only PlatformHost, and "
                "PlatformHost only Kernel: the engine resolves a frame's target through the host, never through an engine "
                "class reachable from a window."
            )

    for match in PRESENTATION_ACCESSOR_RE.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        violations.append(
            f"{relative}:{line} hands out the presentation seam: '{match.group()}' "
            "(a caller asks the kernel for an attachment; see Kernel::AcquireTarget)"
        )

    if not relative.startswith(f"{PUBLIC_INCLUDE_ROOT}/"):
        return violations
    if relative not in PRESENTATION_TYPE_ALLOWED_IN:
        for match in PRESENTATION_TYPE_RE.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            violations.append(
                f"{relative}:{line} names the presentation seam, an engine internal. "
                "RenderContext's low-level verbs may take one; the engine's own API hands out render attachments."
            )
        return violations
    if relative not in PRESENTATION_PRIVATE_ONLY_IN:
        return violations

    # These two producers may name the type, but only where it is not public:
    # members are private until a specifier says otherwise, and the forward
    # declaration sits outside the class.
    access = "private"
    for line_number, line in enumerate(text.splitlines(), start=1):
        specifier = ACCESS_SPECIFIER_RE.match(line.strip())
        if specifier:
            access = specifier.group(1)
            continue
        if not PRESENTATION_TYPE_RE.search(line) or FORWARD_DECLARATION_RE.match(line.strip()):
            continue
        if access != "private":
            violations.append(
                f"{relative}:{line_number} puts the presentation seam in the {access} part of the class: '{line.strip()}'. "
                "Only the kernel and the owning host may ask a window or a host for its target, from a private member."
            )
    return violations


def check_source(path: Path) -> list[str]:
    text = strip_comments_and_literals(path.read_text(encoding="utf-8", errors="ignore"))
    relative = path.relative_to(REPOSITORY_ROOT).as_posix()
    return impl_accessor_violations(relative, text) + presentation_violations(relative, text)


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
        print("Encapsulation violations:", file=sys.stderr)
        for violation in violations:
            print(f"  - {violation}", file=sys.stderr)
        print(
            "A class's implementation and the engine's internal service types are not part of its API. "
            "Delete the accessor and add a method that does the work, or -- if the caller genuinely needs the "
            "contents, as src/render needs a window's native descriptor -- name one friend in the class and have "
            "it read the member directly.",
            file=sys.stderr,
        )
        return 1

    print("Encapsulation OK (no PIMPL accessor; the presentation seam stays internal).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
