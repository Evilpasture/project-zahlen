#!/usr/bin/env python3
"""Only the presenter may name the present layout.

`VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` is not a shading layout. It is a claim about
where an image is handed to the presentation engine, and exactly one part of the
tree is in a position to make it: the code that ends the frame's command buffer,
submits it and calls vkQueuePresentKHR. That code knows both halves of the
question -- whether the destination has a swapchain at all, and what layout the
last pass left the image in -- and neither half is visible from inside a pass.
A pass that names the present layout is therefore guessing, and there are two
ways to guess:

  * on a window's image it moves the image out of the layout the next pass
    asserts. The barrier lands in the frame's own stream, the pass that follows
    records no transition because the frame's bookkeeping still says the image
    was written as a colour attachment, and vkCmdBeginRendering reports the
    mismatch. If that pass was the UI overlay, the frame then presents a
    transition that has already happened, from a layout the image is no longer
    in;

  * on a render texture -- which is what a pass actually has when a caller
    renders to an offscreen target while a window is open -- it moves a
    non-swapchain image into a layout no presentation engine will ever consume,
    which is at best a validation error and at worst a stall.

Both spellings of the mistake reached the tree once, in a single line of the
blit pass, and cost a crash inside the validation layer while it reported the
barrier the presenter then recorded into an already-ended command buffer. This
check is what keeps the rule from being a comment: the token may appear in the
vulkan layer, where the layout traits and the transition helpers live, and in
the presenter itself, and nowhere else. A new presentation path that legitimately
needs it belongs in the allowlist below, where the decision is recorded.

Comments and string literals are stripped first: a file may *talk* about the
present layout -- this very rule is easier to read when the reason is written
next to the code -- without naming it to the compiler.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
LAYOUT_TOKEN = "VK_IMAGE_LAYOUT_PRESENT_SRC_KHR"
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ixx", ".cppm"}

# Project source only. `extern/` and `third_party/` are other people's code --
# Vulkan-Headers names every layout the API has -- and a build directory holds
# copies of both, so neither is walked.
SCANNED_ROOTS = ("src", "include", "app", "extras", "tests", "modules")

# Who may name it, and why. A path prefix and the reason it is excused.
ALLOWED_PREFIXES = (
    # The framework's layout traits and transition helpers: they are how any of
    # this is spelled at all, and they name every layout the API has.
    ("src/vulkan/", "vulkan layer: layout traits and transitions"),
    # The presenter: it ends the command buffer, submits it, presents it.
    ("src/render/RenderAttachments.cpp", "presenter: records the transition, submits, presents"),
)

BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.DOTALL)
LINE_COMMENT_RE = re.compile(r"//[^\n]*")
STRING_LITERAL_RE = re.compile(r'"(?:\\.|[^"\\])*"')


def strip_comments_and_strings(text: str) -> str:
    """Blank out comments and string literals, preserving line structure.

    Line numbers are what a violation is reported with, so every removed span
    is replaced by the same number of newlines it contained rather than by
    nothing.
    """

    def blank(match: re.Match[str]) -> str:
        return "\n" * match.group(0).count("\n")

    without_blocks = BLOCK_COMMENT_RE.sub(blank, text)
    without_lines = LINE_COMMENT_RE.sub("", without_blocks)
    return STRING_LITERAL_RE.sub("", without_lines)


def suspects() -> list[tuple[str, int, str]]:
    """Every code reference to the present layout, as (path, line, text)."""
    found: list[tuple[str, int, str]] = []
    for root in SCANNED_ROOTS:
        directory = REPOSITORY_ROOT / root
        if not directory.is_dir():
            continue
        for path in sorted(directory.rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            relative = path.relative_to(REPOSITORY_ROOT).as_posix()
            if any(relative.startswith(prefix) for prefix, _ in ALLOWED_PREFIXES):
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            if LAYOUT_TOKEN not in text:
                continue
            for number, line in enumerate(strip_comments_and_strings(text).splitlines(), start=1):
                if LAYOUT_TOKEN in line:
                    found.append((relative, number, line.strip()))
    return found


def main() -> int:
    violations = suspects()

    for relative, number, line in violations:
        print(f"  !! {relative}:{number} names {LAYOUT_TOKEN} outside the presenter")
        print(f"     {line}")
    if violations:
        print()
        print("Only the code that submits and presents the frame may transition an image")
        print("into the present layout; no pass knows whether its target is a swapchain")
        print("image or a render texture. Leave the image in the layout the pass's own")
        print("attachment usage declares and let RenderContext::Impl::PresentUsedWindows")
        print("make the transition. A new presentation path belongs in ALLOWED_PREFIXES.")
        return 1

    for prefix, reason in ALLOWED_PREFIXES:
        print(f"  {prefix:<34} {reason}")
    print("PRESENT LAYOUT OWNERSHIP OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
