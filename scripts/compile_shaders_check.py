#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile every shader in the declarative table through zshader.

Parses every add_shader_target() block out of cmake/ShaderCompilation.cmake and
runs the SAME compiler driver the build uses, once per stage:

    zshader compile <file> -entry <E> -stage <S> -target spirv \
        -fvk-use-entrypoint-name -matrix-layout-column-major \
        -I resources/shaders -I include [-D...] -o out.spv

There is no slangc, no slangpy wheel and no ctypes binding here: zshader links
libslang directly, so this checks the exact in-process codegen path the manifest
batch uses. Requires a built zshader (cmake --build build/<tag> --target
zshader); --compiler names one explicitly.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parent.parent
CMAKE = REPO / "cmake" / "ShaderCompilation.cmake"
SHADERS = REPO / "resources" / "shaders"
SPV_MAGIC = b"\x03\x02\x23\x07"

STAGE_MAP = {
    "vs_6_0": "vertex", "vs_6_5": "vertex",
    "ps_6_0": "fragment", "ps_6_5": "fragment",
    "cs_6_0": "compute",
    "as_6_5": "amplification",
    "ms_6_5": "mesh",
}


def find_compiler(explicit: str | None) -> list[str]:
    """Returns the argv prefix for one compile: zshader's `compile` mode."""
    if explicit:
        return [explicit, "compile"]
    built = sorted(REPO.glob("build/*/bin/zshader"), key=lambda p: p.stat().st_mtime, reverse=True)
    if not built:
        sys.exit("no build/*/bin/zshader found; build the zshader target first")
    return [str(built[0]), "compile"]


def parse_targets() -> list[tuple[str, str, str, str, list[str]]]:
    """Yields (file, entry, profile, macro, extra_args) tuples."""
    text = CMAKE.read_text()
    targets = []
    for m in re.finditer(r"add_shader_target\((\w+)\s*\n(.*?)\n\)\n", text, re.S):
        body = m.group(2)
        stages_m = re.search(r"STAGES(.*?)EXTRA_ARGS", body, re.S) or \
                   re.search(r"STAGES(.*?)$", body, re.S)
        if not stages_m:
            continue
        stages_blob = stages_m.group(1)
        extra_blob = re.search(r"EXTRA_ARGS(.*?)$", body, re.S)
        extra_args = extra_blob.group(1).split() if extra_blob else []
        for s in re.finditer(r'"([^"]+)"', stages_blob):
            parts = s.group(1).split("|")
            if len(parts) < 4:
                continue
            path, entry, profile, macro = parts[:4]
            path = path.replace("${SHADER_SRC_DIR}", str(SHADERS))
            per_stage = parts[4:]
            targets.append((path, entry, profile, macro, extra_args + per_stage))
    # compile_shaders() is the other half of the table: VSMain/PSMain for every
    # .slang it names. They have no defines and no per-stage flags.
    for m in re.finditer(r"compile_shaders\(\w+\s*\n(.*?)\n\)\n", text, re.S):
        for q in re.finditer(r'"\$\{SHADER_SRC_DIR\}/([^"]+\.slang)"', m.group(1)):
            path = str(SHADERS / q.group(1))
            for entry, profile in (("VSMain", "vs_6_5"), ("PSMain", "ps_6_5")):
                targets.append((path, entry, profile, "", []))
    return targets


def compile_one(compiler: list[str], shader_path: str, entry: str, stage: str,
                extra: list[str], out: pathlib.Path) -> tuple[bool, str]:
    cmd = [*compiler, shader_path,
           "-entry", entry, "-stage", stage, "-target", "spirv",
           "-fvk-use-entrypoint-name", "-matrix-layout-column-major",
           "-I", str(SHADERS), "-I", str(REPO / "include"),
           *extra, "-o", str(out)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        return False, (res.stderr or res.stdout or f"exit {res.returncode}").strip()
    data = out.read_bytes() if out.exists() else b""
    if len(data) < 4 or not data.startswith(SPV_MAGIC):
        return False, "no SPIR-V magic in the output"
    return True, ""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", dest="compiler",
                    help="explicit path to a built zshader")
    ap.add_argument("keep_dir", nargs="?", type=pathlib.Path,
                    help="optional directory to keep the compiled .spv files in")
    args = ap.parse_args()
    compiler = find_compiler(args.compiler)
    keep_dir = args.keep_dir
    if keep_dir:
        keep_dir.mkdir(parents=True, exist_ok=True)

    targets = parse_targets()
    print(f"{len(targets)} stage compilations parsed from ShaderCompilation.cmake\n")
    failures = 0
    with tempfile.TemporaryDirectory() as td:
        for path, entry, profile, macro, extra in targets:
            stage = STAGE_MAP.get(profile, profile)
            out = pathlib.Path(td) / f"{pathlib.Path(path).stem}.{entry}.spv"
            ok, diag = compile_one(compiler, path, entry, stage, extra, out)
            tag = "OK  " if ok else "FAIL"
            label = f"{pathlib.Path(path).name}:{entry}[{','.join(extra) or '-'}]"
            print(f"{tag} {label}")
            if keep_dir and ok:
                suffix = ".".join(a[2:] for a in extra)
                keep = keep_dir / f"{pathlib.Path(path).stem}.{entry}.{suffix}.spv"
                keep.write_bytes(out.read_bytes())
            if not ok:
                failures += 1
                print(diag[:4000])
                print()
    print(f"\n{len(targets) - failures}/{len(targets)} compiled")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
