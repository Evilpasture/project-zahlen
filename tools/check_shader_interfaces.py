#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify that every scene pipeline's geometry stage and fragment stage declare
exactly the same varying interface.

A geometry-stage output that no fragment stage reads is legal, but the
validation layer reports each one ("SPIR-V Interface ... no corresponding Input
declared") and it wastes interpolation bandwidth. basic.slang / basic_mesh.slang
therefore compile a per-pass variant of SceneGeometryOutput, and the geometry
and fragment stages of a pipeline must rasterise the same varying struct.

This script compiles all three variants with slangc and reflects the resulting
SPIR-V, so the invariant is checkable without a GPU (unlike the validation
layer, which only notices at pipeline creation).

    python3 tools/check_shader_interfaces.py [--slangc /path/to/slangc]
"""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
SHADERS = REPO / "resources" / "shaders"

# (label, [(file, entry, stage), ...]) -- mirrors cmake/ShaderCompilation.cmake.
# The passes share one module but compile distinct entry points against their
# own varying structs, so there are no defines to replay.
VARIANTS = [
    ("G-buffer", [("basic.slang", "VSMain", "vertex"),
                  ("basic_mesh.slang", "MeshMain", "mesh"),
                  ("basic.slang", "PSMain", "fragment")]),
    ("Shadow",   [("basic.slang", "VSMainShadow", "vertex"),
                  ("basic_mesh.slang", "MeshMainShadow", "mesh"),
                  ("basic.slang", "PSShadow", "fragment")]),
    ("Forward",  [("basic.slang", "VSMainForward", "vertex"),
                  ("basic_mesh.slang", "MeshMainForward", "mesh"),
                  ("basic.slang", "PSForward", "fragment")]),
]

SPV_MAGIC = 0x07230203
OP_NAME, OP_DECORATE, OP_VARIABLE = 5, 71, 59
DECORATION_LOCATION = 30
STORAGE_INPUT, STORAGE_OUTPUT = 1, 3


def find_slangc(explicit: str | None) -> str:
    if explicit:
        return explicit
    for candidate in (shutil.which("slangc"),
                      os.path.join(os.environ.get("VULKAN_SDK", ""), "bin", "slangc"),
                      os.path.join(os.environ.get("SLANG_BIN", ""), "slangc")):
        if candidate and os.path.exists(candidate):
            return candidate
    sys.exit("slangc not found; pass --slangc or set VULKAN_SDK / SLANG_BIN")


def compile_stage(slangc: str, path: str, entry: str, stage: str, out: str) -> None:
    cmd = [slangc, str(SHADERS / path), "-entry", entry, "-stage", stage, "-target", "spirv",
           "-fvk-use-entrypoint-name", "-matrix-layout-column-major", "-I", str(SHADERS),
           "-I", str(REPO / "include")]
    cmd += ["-o", out]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"slangc failed for {path}:{entry}\n{res.stderr}")


def reflect(spv_path: str) -> tuple[dict[int, str], dict[int, str]]:
    """Returns ({location: name} outputs, {location: name} inputs)."""
    data = pathlib.Path(spv_path).read_bytes()
    words = struct.unpack(f"<{len(data) // 4}I", data)
    if words[0] != SPV_MAGIC:
        sys.exit(f"{spv_path}: not SPIR-V")

    i, locations, storage, names = 5, {}, {}, {}
    while i < len(words):
        op, count = words[i] & 0xFFFF, words[i] >> 16
        if count == 0:
            break
        if op == OP_DECORATE and words[i + 2] == DECORATION_LOCATION:
            locations[words[i + 1]] = words[i + 3]
        elif op == OP_VARIABLE:
            storage[words[i + 2]] = words[i + 3]
        elif op == OP_NAME:
            raw = b"".join(struct.pack("<I", w) for w in words[i + 2:i + count])
            names[words[i + 1]] = raw.split(b"\0")[0].decode(errors="replace")
        i += count

    outputs, inputs = {}, {}
    for var, sc in storage.items():
        if var in locations:
            target = outputs if sc == STORAGE_OUTPUT else inputs if sc == STORAGE_INPUT else None
            if target is not None:
                target[locations[var]] = names.get(var, "?")
    return outputs, inputs


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--slangc")
    slangc = find_slangc(ap.parse_args().slangc)

    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for label, stages in VARIANTS:
            spv = {}
            for path, entry, stage in stages:
                out = os.path.join(tmp, f"{label}.{entry}.spv")
                compile_stage(slangc, path, entry, stage, out)
                spv[stage] = out

            vs_out, _ = reflect(spv["vertex"])
            mesh_out, _ = reflect(spv["mesh"])
            _, fs_in = reflect(spv["fragment"])

            problems = []
            for name, produced in (("vertex", vs_out), ("mesh", mesh_out)):
                unused = sorted(set(produced) - set(fs_in))
                missing = sorted(set(fs_in) - set(produced))
                if unused:
                    problems.append(f"{name} writes locations never read by the fragment stage: "
                                    + ", ".join(f"{loc} ({produced[loc]})" for loc in unused))
                if missing:
                    problems.append(f"{name} is MISSING locations the fragment stage reads: {missing}")
            if set(vs_out) != set(mesh_out):
                problems.append(f"vertex and mesh stages disagree: {sorted(set(vs_out) ^ set(mesh_out))}")

            if problems:
                failures += 1
                print(f"[FAIL] {label:9}")
                for p in problems:
                    print(f"        {p}")
            else:
                print(f"[ ok ] {label:9}: {len(fs_in)} varyings, exact match")

    if failures:
        print(f"\n{failures} variant(s) have a mismatched shader interface.")
        return 1
    print("\nAll scene shader interfaces are exact.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
