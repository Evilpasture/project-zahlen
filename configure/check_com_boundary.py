#!/usr/bin/env python3
"""Enforce the COM boundary: COM is an ABI seam, not a C++ utility library.

`include/Zahlen/COM/` exists so objects cross DSO boundaries through vtables
and fixed-layout payloads. Everything on the *engine side* of that seam is
plain C++: ZHLN::Logger, ZHLN::ErrorRegistry, ZHLN::AudioContext,
ZHLN::RenderContext, ... This check keeps that true, because the boundary is
easy to invert by accident: pass a COM::ILogger* "just" to log a line from an
importer, and Chain.hpp, <meta> reflection and refcount semantics leak into
code that should never have heard of COM.

Rules:

  1. `include/Zahlen/**` may not include a COM header, except the two COM
     homes themselves (`include/Zahlen/COM/`, `include/Zahlen/Core/COM/`).
     Layer 0 (`Log.hpp`, `ErrorCode.hpp`, ...) must stay free of them.

  2. `src/**` may not include a COM header, except `src/engine/com/`, which
     is the one place that is *allowed* to know both worlds (it holds the
     thin adapters over the engine-native classes).

  3. Pure-data plugins (`plugins/toml`, `plugins/json`, `plugins/glTF`) may
     not include, reference or name a COM type at all: they are format
     libraries. When they have something to report they return rich
     diagnostics (`ParseFailure`, `Document::warnings`) and the caller logs.

The sanctioned exceptions are listed below with the reason they exist. An
entry is a path prefix plus a comment; adding one is a deliberate act, which
is the point.

Comment lines are stripped before matching, so prose that mentions COM (the
logging docs do) is not a violation.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

SOURCE_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".cxx",
    ".h",
    ".hh",
    ".hpp",
    ".hxx",
    ".inl",
    ".ipp",
    ".cppm",
    ".ixx",
}

COM_INCLUDE_PATTERN = re.compile(
    r'^\s*#\s*include\s*[<"](Zahlen/(?:Core/)?COM/[^>"]+)[>"]',
    re.MULTILINE,
)
COM_MODULE_PATTERN = re.compile(r"^\s*(?:export\s+)?import\s+Zahlen\.(?:Core\.)?COM[.;]", re.MULTILINE)
COM_NAME_PATTERN = re.compile(r"\b(?:ZHLN::)?COM::[A-Z]\w*")

# ---- Rule 1: the public include tree, minus the COM homes ------------------
PUBLIC_ROOT = ROOT / "include" / "Zahlen"
PUBLIC_ALLOWED_DIRS = (
    ROOT / "include" / "Zahlen" / "COM",
    ROOT / "include" / "Zahlen" / "Core" / "COM",
)

# ---- Rule 2: the engine and its libraries, minus the COM adapter directory --
SRC_ROOT = ROOT / "src"
SRC_ALLOWED_DIRS = (ROOT / "src" / "engine" / "com",)

# Path prefixes (relative to ROOT) that may cross the seam anyway, each with
# the reason it has to. Keep this list short and review it on every change.
SRC_ALLOWLIST: dict[str, str] = {
    "src/engine/Engine.cpp": (
        "returns COM::FramePipelineError ordinals, which are defined at the COM boundary because "
        "they cross IFramePipeline; moving the enum would change its anchored error category hash"
    ),
}

# Rule 1's deliberate exceptions in the public include tree. The physics wire
# vocabulary needs the boundary's own fixed-layout value types (COM::Span for
# borrowed geometry, COM::Option for optional pivots): both are standard-layout
# and trivially copyable, which is exactly what ChainPayload and LayoutHash
# demand of a chain node, and there is no non-COM header that spells one.
PUBLIC_ALLOWLIST: dict[str, str] = {
    "include/Zahlen/Physics/PhysicsShapes.hpp": (
        "shape wire structs borrow geometry through COM::Span; the span IS the fixed-layout "
        "spelling a chain node carries, and no engine-side header may redefine it"
    ),
    "include/Zahlen/Physics/PhysicsValues.hpp": (
        "constraint wire structs carry optional pivots/limits as COM::Option; same bargain "
        "as PhysicsShapes.hpp, and the struct must stay chain-safe"
    ),
}

# ---- Rule 3: pure-data format plugins have no COM at all --------------------
# plugins/BSP and plugins/glTF are *extension* plugins: they implement or
# consume COM capabilities (IAssetLibrary, IGraphicsDevice) at the plugin
# boundary, which is legitimate. The format libraries (toml, json) parse and
# serialize data and must never name a COM type.
PURE_DATA_ROOTS = (
    ROOT / "plugins" / "toml",
    ROOT / "plugins" / "json",
)

# ---- Rule 4: no COM logger threaded through subsystem APIs ------------------
# The inversion this check exists for: an importer or binder grows a
# `COM::ILogger* logger = nullptr` parameter "just to log a line", which drags
# the whole COM surface into pure C++ code and makes the caller responsible for
# a logger it may not have. Subsystem headers log ambient
# (ZHLN::Log<LogTag::...>) or return a rich diagnostic for the caller to log.
LOGGER_PARAM_ROOTS = (
    ROOT / "plugins",
    ROOT / "extensions",
    ROOT / "gameplay",
    ROOT / "utils",
)
LOGGER_PARAM_PATTERN = re.compile(r"\bILogger\s*\*")


def strip_comments(text: str) -> str:
    """Remove // and /* */ comments so prose about COM is not a violation."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def iter_sources(root: Path):
    if not root.is_dir():
        return
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        if any(part in {".git", "build", "__pycache__"} for part in path.parts):
            continue
        yield path


def is_under(path: Path, prefix: Path) -> bool:
    try:
        path.relative_to(prefix)
        return True
    except ValueError:
        return False


def relative(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def check_public_tree(violations: list[str]) -> None:
    for path in iter_sources(PUBLIC_ROOT):
        if any(is_under(path, allowed) for allowed in PUBLIC_ALLOWED_DIRS):
            continue
        rel = relative(path)
        if rel in PUBLIC_ALLOWLIST:
            continue
        body = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for match in COM_INCLUDE_PATTERN.finditer(body):
            line = body[: match.start()].count("\n") + 1
            violations.append(
                f"{relative(path)}:{line}: public header includes <{match.group(1)}>; "
                "only include/Zahlen/COM/ and include/Zahlen/Core/COM/ may (rule 1)"
            )


def check_src_tree(violations: list[str]) -> None:
    for path in iter_sources(SRC_ROOT):
        if any(is_under(path, allowed) for allowed in SRC_ALLOWED_DIRS):
            continue
        rel = relative(path)
        if rel in SRC_ALLOWLIST:
            continue
        body = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
        for match in COM_INCLUDE_PATTERN.finditer(body):
            line = body[: match.start()].count("\n") + 1
            violations.append(
                f"{rel}:{line}: includes <{match.group(1)}>; src/ is COM-free outside src/engine/com/ (rule 2)"
            )
        for match in COM_MODULE_PATTERN.finditer(body):
            line = body[: match.start()].count("\n") + 1
            violations.append(f"{rel}:{line}: imports a COM module; src/ is COM-free outside src/engine/com/ (rule 2)")


def check_pure_data_plugins(violations: list[str]) -> None:
    for root in PURE_DATA_ROOTS:
        for path in iter_sources(root):
            body = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            rel = relative(path)
            for match in COM_INCLUDE_PATTERN.finditer(body):
                line = body[: match.start()].count("\n") + 1
                violations.append(
                    f"{rel}:{line}: format plugin includes <{match.group(1)}>; return diagnostics instead (rule 3)"
                )
            for match in COM_NAME_PATTERN.finditer(body):
                line = body[: match.start()].count("\n") + 1
                violations.append(
                    f"{rel}:{line}: format plugin names '{match.group(0)}'; format libraries stay COM-free (rule 3)"
                )


def check_logger_parameters(violations: list[str]) -> None:
    for root in LOGGER_PARAM_ROOTS:
        for path in iter_sources(root):
            if path.suffix.lower() not in {".h", ".hh", ".hpp", ".hxx", ".cppm", ".ixx"}:
                continue
            body = strip_comments(path.read_text(encoding="utf-8", errors="replace"))
            rel = relative(path)
            for match in LOGGER_PARAM_PATTERN.finditer(body):
                line = body[: match.start()].count("\n") + 1
                violations.append(
                    f"{rel}:{line}: subsystem header takes a COM logger; "
                    "log ambient (ZHLN::Log<LogTag::...>) or return a diagnostic for the caller (rule 4)"
                )


def main() -> int:
    violations: list[str] = []
    check_public_tree(violations)
    check_src_tree(violations)
    check_pure_data_plugins(violations)
    check_logger_parameters(violations)

    if violations:
        for violation in violations:
            print(violation, file=sys.stderr)
        print(
            f"COM boundary check failed: {len(violations)} violation(s). "
            "Log ambient (ZHLN::Log<LogTag::...>) or return a rich diagnostic and let the caller log; "
            "COM objects are adapted at src/engine/com/ (see configure/check_com_boundary.py).",
            file=sys.stderr,
        )
        return 1

    print("COM boundary OK (public headers, src/ outside src/engine/com/, pure-data plugins, logger parameters)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
