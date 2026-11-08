#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep the four meanings of "Handle" out of the public API, and keep "ID" capitalised.

The vocabulary used to call four unrelated concepts "Handle":

  1. inert numeric key         -> *ID    (TextureID, BodyID, VoiceID)
  2. non-owning domain facade  -> a noun  (GraphicsDevice, Simulation, DebugDraw)
  3. scoped begin/end token    -> *Scope  (FrameScope, SessionScope)
  4. owning allocation         -> std::unique_ptr<T> / *Instance

This check makes that taxonomy permanent. It rejects (a) retired *Handle type
names anywhere in the governed tree, (b) any public type declared under
include/Zahlen/ whose name contains "Handle", and (c) any public type whose
name ends in a lower-case "Id" -- the acronym is spelled ID, matching the
pre-existing AssetID / MaterialID and JPH::BodyID. OS-level accessors such as
GetNativeHandle(), Win32/POSIX HANDLE values, and event *Handler types are
different words with different meanings and are deliberately not governed:
they are not inert IDs and not domain facades.

Why a configure-time check and not a style note: a rename like this is only
permanent if adding `enum class NewHandle : uint64_t` back to a public header
fails the build. Words in a comment do not do that.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

RETIRED = (
    "TextureHandle", "BufferHandle", "PipelineHandle", "AudioHandle",
    "BodyHandle", "RagdollHandle", "AnimationHandle", "ConstraintHandle",
    "TerrainHandle", "FileWatchHandle", "FetchHandle", "HeapHandle",
    "StorageImageHandle", "UniformBufferHandle", "StorageBufferHandle",
    "SamplerHandle", "AccelerationStructureHandle",
    "EngineHandle", "KernelHandle", "GraphicsHandle", "PhysicsHandle",
    "FrameHandle", "TargetHandle", "RenderTextureHandle", "ResourceGroupHandle",
)

GOVERNED_ROOTS = (
    "include", "src", "plugins", "extensions", "gameplay", "modules",
    "utils", "scripts", "tests", "samples", "app", "configure", "cmake",
)
GOVERNED_SUFFIXES = frozenset({
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl",
    ".ipp", ".cppm", ".ixx", ".cmake", ".py", ".pl", ".md", ".txt",
})
SKIP_DIRS = frozenset({".git", "build", "target", "__pycache__", ".cache", "node_modules", "extern"})

RETIRED_RE = re.compile(r"\b(" + "|".join(map(re.escape, RETIRED)) + r")\b")

# A public declaration of a type whose name contains "Handle".
PUBLIC_DECL_RE = re.compile(
    r"^\s*(?:template\s*<[^>]*>\s*)?"
    r"(?:enum\s+(?:class|struct)?|struct|class|union|using|typedef)\s+"
    r"([A-Za-z_]\w*Handle\w*)\b",
    re.MULTILINE,
)

# A public type name ending in a lower-case "Id" (ActionID -> ActionID).
PUBLIC_ID_DECL_RE = re.compile(
    r"^\s*(?:template\s*<[^>]*>\s*)?"
    r"(?:enum\s+(?:class|struct)?|struct|class|union|using|typedef)\s+"
    r"([A-Za-z_]\w*Id)\b",
    re.MULTILINE,
)


def governed_files():
    for root in GOVERNED_ROOTS:
        base = ROOT / root
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in GOVERNED_SUFFIXES:
                continue
            if any(part in SKIP_DIRS for part in path.relative_to(ROOT).parts):
                continue
            yield path


def main() -> int:
    violations: list[str] = []
    scanned = 0
    for path in governed_files():
        text = path.read_text(encoding="utf-8", errors="ignore")
        scanned += 1
        rel = path.relative_to(ROOT).as_posix()
        if rel == "configure/check_handle_taxonomy.py":
            continue  # the retired-name list itself
        for m in RETIRED_RE.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            violations.append(f"retired name {m.group(1)} at {rel}:{line}")
        if rel.startswith("include/Zahlen/"):
            for rx, why in ((PUBLIC_DECL_RE, "Handle"), (PUBLIC_ID_DECL_RE, "Id")):
                for m in rx.finditer(text):
                    line = text.count("\n", 0, m.start()) + 1
                    violations.append(f"public type named '{m.group(1)}' ({why} spelling) at {rel}:{line}")

    if violations:
        print("ID / Handle taxonomy violations:", file=sys.stderr)
        for v in violations[:40]:
            print(f"  - {v}", file=sys.stderr)
        print(
            "\nAn inert numeric key is an *ID, a domain facade is a noun, a scoped\n"
            "token is a *Scope, and an owning allocation is a std::unique_ptr<T> or\n"
            "an *Instance. See the taxonomy comment in Render/RenderIDs.hpp.",
            file=sys.stderr,
        )
        return 1
    print(f"ID / Handle taxonomy OK ({scanned} files scanned; IDs, facades, scopes and owners named apart).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
