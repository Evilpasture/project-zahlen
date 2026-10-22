#!/usr/bin/env python3
"""Keep the descriptor-heap bind out of pass code.

`RenderFrameContext::BindHeapsAndPushFrame` binds both heap segments and pushes
the per-frame device-address block that `<common.slang>`'s
`ParameterBlock<GlobalSceneRegistry> scene` dereferences directly -- there is no
descriptor table behind those reads, only a 64-bit GPU address taken from the
push range. It is state of a *command stream*: it stays in effect until the
command buffer is reset, and it is identical for every pass recorded into that
buffer.

Which is why it is called exactly twice in the frame, once per stream that
begins recording:

    src/render/RenderDestinations.cpp        AcquireTarget -- a window's
                                             graphics stream
    src/render/pipelines/ComputeSimPipeline.cpp  Submit -- the async-compute
                                             stream

Fork and parallel-draw secondaries do not inherit either the heap bindings or
the push range from the primary they are executed in, so `Vk::
ParallelCommandRecorder` re-establishes both for each slot it begins from
`FrameHeapAddresses()`; that half lives in the Vulkan layer and is not written
by hand anywhere.

A pass that calls this function is a pass that works only because of what
happens to run before it on the same buffer. That holds for as long as the graph
order does -- until the graph is reordered, or the pass moves to the front of
the frame, or it is forked onto another queue, and then the shader dereferences
whatever the previous recording left in the push range, or address zero. The
first outcome is `VK_ERROR_DEVICE_LOST` faulting at 0; the second is a frame
that reads the previous frame's uniform buffer and tears.

So the rule is mechanical and checked here rather than remembered: after
comments and string literals are removed, `BindHeapsAndPushFrame` may appear
only in the four files above, exactly once each -- one declaration, one
definition, two roots. Anything else fails the configure with the offending
line and the fix.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = ("src", "include", "app", "tests", "samples", "plugins", "extensions", "gameplay", "modules", "tools")
SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".cppm", ".ixx", ".inl", ".ipp"}

#: file -> how many references are legitimate, and what each one is.
ALLOWED = {
    "src/render/FrameContext.hpp": (1, "the declaration, with the contract it states"),
    "src/render/RenderFrame.cpp": (1, "the definition"),
    "src/render/RenderDestinations.cpp": (1, "the graphics-stream root: AcquireTarget"),
    "src/render/pipelines/ComputeSimPipeline.cpp": (1, "the compute-stream root: Submit"),
}

REFERENCE = re.compile(r"\bBindHeapsAndPushFrame\s*\(")

#: The body of the message a new call site gets. It names the root that already
#: covers the case the author is thinking about, because the usual reason to
#: write this call is that a pass is the first thing on its command buffer --
#: which is a missing root, not a missing bind.
FIX = (
    "heap state is command-stream state: bind it where the stream begins, not from a pass.\n"
    "  A window's graphics stream is bound in RenderContext::Impl::AcquireTarget, the async\n"
    "  compute stream in ComputeSimPipeline::Submit, and fork/parallel-draw secondaries in\n"
    "  Vk::ParallelCommandRecorder::SetHeapState. If a pass needs heap state that is not\n"
    "  there, the stream it records into is missing its root -- add the root, and update\n"
    "  ALLOWED here and the table in DESCRIPTOR_HEAPS.md."
)


def strip_comments_and_strings(text: str) -> str:
    """Blank out comments, string literals and char literals, keeping newlines.

    Line numbers have to survive, so every removed character becomes a space
    (except newlines) rather than disappearing. `R"(...)"`, `L"..."`, `u8"..."`
    and friends all end at the same quote that opened them once the prefix is
    skipped, which is why the prefix is matched as part of the quote.
    """
    out: list[str] = []
    i = 0
    length = len(text)
    while i < length:
        ch = text[i]
        if ch == "/" and i + 1 < length and text[i + 1] == "/":
            while i < length and text[i] != "\n":
                out.append(" ")
                i += 1
        elif ch == "/" and i + 1 < length and text[i + 1] == "*":
            while i < length and not (text[i] == "*" and i + 1 < length and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
        elif ch in "\"'":
            quote = ch
            while i < length:
                if text[i] == "\\" and i + 1 < length:
                    out.append("  ")
                    i += 2
                    continue
                if text[i] == quote:
                    out.append(" ")
                    i += 1
                    break
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def files():
    for root_name in SOURCE_ROOTS:
        root = ROOT / root_name
        if root.exists():
            yield from (p for p in root.rglob("*") if p.is_file() and p.suffix in SUFFIXES)


def main() -> int:
    violations: list[str] = []
    seen: dict[str, int] = {}

    for path in files():
        relative = path.relative_to(ROOT).as_posix()
        text = strip_comments_and_strings(path.read_text(encoding="utf-8", errors="replace"))
        for match in REFERENCE.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            seen[relative] = seen.get(relative, 0) + 1
            if relative not in ALLOWED:
                violations.append(f"{relative}:{line}: BindHeapsAndPushFrame is called outside a command-stream root.\n  {FIX}")

    for relative, (expected, role) in ALLOWED.items():
        found = seen.get(relative, 0)
        if found != expected:
            violations.append(
                f"{relative}: expected {expected} reference to BindHeapsAndPushFrame ({role}), found {found}.\n"
                "  A root that stopped binding is a stream whose passes run with an unpushed address block;\n"
                "  a root that binds twice is a stream that thinks it has two."
            )

    if violations:
        print("heap state root violations:", file=sys.stderr)
        print("\n".join(violations), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
