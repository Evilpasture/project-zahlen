#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""
check_ffi_layout.py -- prove the Lua-visible struct layouts match C++.

extras/scripting_lua/scripts/core/ffi_cdef.fnl re-states, in C syntax, the
memory layout of core's ECS components. LuaJIT trusts that text absolutely: it
computes offsets from it and writes through them into live C++ objects. Nothing
else in the build compares the two, so a component that gains a field, changes
a type or is re-ordered drifts silently, and the first symptom is a script
corrupting memory somewhere unrelated.

This closes that gap mechanically. It extracts every component struct from the
cdef text, compiles it as real C, compiles the corresponding C++ types, and
compares sizeof and every field offset. Any disagreement is a bug in the cdef.

It is deliberately independent of the reflection layer: it reads the cdef text
and Components.hpp as text, then asks the compilers for the truth. That means
it can audit the hand-written file, and it can audit generated output with no
change.

    python3 extras/scripting_lua/tools/check_ffi_layout.py \
        --c-compiler gcc --cxx-compiler g++

Exit status is 0 only when every compared layout agrees.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
FNL_CDEF = REPO_ROOT / "extras/scripting_lua/scripts/core/ffi_cdef.fnl"
COMPONENTS_HPP = REPO_ROOT / "include/Zahlen/Components.hpp"

# Structs the cdef spells by hand rather than mirroring a Components member.
# They are still worth checking, but they have no C++ counterpart under
# Components:: and are not this tool's subject.
NOT_COMPONENTS = {"String256", "String64", "String128", "vec3", "AAState"}

# Fields the cdef invents to reproduce alignment padding by hand. They cannot
# exist in C++, so they are reported separately from genuinely stale fields.
PADDING_RE = re.compile(r"^_?pad\w*$")


def extract_cdef_text(fnl_path: Path) -> str:
    """Return the raw C text handed to ffi.cdef, as LuaJIT would read it.

    Accepts both shapes this pipeline produces: the hand-written Fennel module,
    which wraps the text in (ffi.cdef "..."), and the generated Lua module,
    which returns it as a [[ long string ]].
    """
    text = fnl_path.read_text()

    generated = re.search(r"return \[\[\n(.*)\n\]\]", text, re.S)
    if generated is not None:
        return generated.group(1)

    lines = text.splitlines()
    try:
        start = next(i for i, l in enumerate(lines) if l.strip().startswith('(ffi.cdef "'))
        end = next(i for i, l in enumerate(lines) if l.strip() == '"))')
    except StopIteration as exc:
        raise SystemExit(
            f"error: {fnl_path} is neither an (ffi.cdef \"...\") module nor a "
            "generated `return [[...]]` module"
        ) from exc
    body = "\n".join(lines[start + 1 : end])
    # Keep every typedef; drop the extern function prototypes, which are not
    # layout and would need the engine's own types to compile.
    return re.sub(
        r"^\s*(?:uint32_t|uint64_t|ZHLN_Engine\*)\s+ZHLN_\w+\([^)]*\);\s*$",
        "",
        body,
        flags=re.M,
    )


def component_names(components_hpp: Path) -> list[str]:
    """Names of the structs nested directly inside `struct Components`."""
    src = components_hpp.read_text()
    match = re.search(r"struct Components \{", src)
    if match is None:
        raise SystemExit(f"error: no `struct Components` in {components_hpp}")
    depth, out = 1, []
    for ch in src[match.end() :]:
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                break
        out.append(ch)
    body = "".join(out)
    return re.findall(r"^\s{4}struct\s+(\w+)\s*\{", body, re.M)


def parse_structs(cdef_text: str, wanted: set[str]) -> dict[str, list[str]]:
    """Map struct name -> field names, for the wanted subset of the cdef."""
    found: dict[str, list[str]] = {}
    for name, body in re.findall(r"typedef struct (\w+) \{(.*?)\} \1;", cdef_text, re.S):
        if name not in wanted:
            continue
        fields: list[str] = []
        for line in body.strip().splitlines():
            line = line.strip().rstrip(";").strip()
            if not line or line.startswith("//"):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            for tok in parts[1:]:
                field = re.sub(r"\[\d+\]$", "", tok.strip().rstrip(","))
                if field and re.fullmatch(r"[A-Za-z_]\w*", field):
                    fields.append(field)
        found[name] = fields
    return found


def build_c_header(cdef_text: str) -> str:
    return (
        "#include <stdint.h>\n#include <stddef.h>\n#include <stdbool.h>\n" + cdef_text
    )


def emit_c_probe(header: str, structs: dict[str, list[str]]) -> str:
    out = [header, "#include <stdio.h>", "int main(void){"]
    for name in structs:
        out.append(f'  printf("S {name} %zu\\n", sizeof({name}));')
        for field in structs[name]:
            out.append(f'  printf("F {name}.{field} %zu\\n", offsetof({name}, {field}));')
    out.append("  return 0;\n}")
    return "\n".join(out)


def compile_and_run(cmd: list[str], source: Path, binary: Path) -> str:
    proc = subprocess.run(cmd + [str(source), "-o", str(binary)], capture_output=True, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"error: compile failed for {source.name}\n{proc.stderr[:4000]}")
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    if run.returncode != 0:
        raise SystemExit(f"error: {binary.name} failed\n{run.stderr[:2000]}")
    return run.stdout


def parse_measurements(text: str) -> dict[str, int]:
    out: dict[str, int] = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        key, _, value = line.rpartition(" ")
        out[key] = int(value)
    return out


def emit_cxx_offsets(probe: dict[str, list[str]]) -> str:
    out = [
        "#include <Zahlen/Components.hpp>",
        "#include <cstdio>",
        "#include <cstddef>",
        "using namespace ZHLN;",
        "int main(){",
    ]
    for name, fields in probe.items():
        out.append(f'  printf("S {name} %zu\\n", sizeof(Components::{name}));')
        for field in fields:
            out.append(
                f'  printf("F {name}.{field} %zu\\n", offsetof(Components::{name}, {field}));'
            )
    out.append("  return 0;\n}")
    return "\n".join(out)




def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--c-compiler", default=os.environ.get("CC", "cc"))
    ap.add_argument("--cxx-compiler", default=os.environ.get("CXX", "c++"))
    ap.add_argument("--cdef", default=str(FNL_CDEF))
    ap.add_argument("--components", default=str(COMPONENTS_HPP))
    ap.add_argument(
        "--cxx-include",
        action="append",
        default=[],
        help="Extra -I for the C++ probe (needs at least include/ and Jolt).",
    )
    ap.add_argument(
        "--cxx-define",
        action="append",
        default=["JPH_DOUBLE_PRECISION"],
        help="Extra -D for the C++ probe. Must match how the engine is built.",
    )
    args = ap.parse_args()

    cdef_text = extract_cdef_text(Path(args.cdef))
    cpp_names = component_names(Path(args.components))
    wanted = set(cpp_names) - NOT_COMPONENTS
    structs = parse_structs(cdef_text, wanted)

    missing = sorted(w for w in wanted if w not in structs)
    print(f"components in Components.hpp : {len(wanted)}")
    print(f"components present in cdef   : {len(structs)}")
    print(f"components missing from cdef : {len(missing)}")

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        c_src = tmp_path / "cdef_probe.c"
        cxx_src = tmp_path / "cxx_probe.cpp"
        c_src.write_text(emit_c_probe(build_c_header(cdef_text), structs))

        c_out = parse_measurements(
            compile_and_run([args.c_compiler, "-I", tmp], c_src, tmp_path / "c_probe")
        )

        # Field offsets need the member to exist in C++. Discover the ones that
        # do not by letting the compiler name them, then report those as drift
        # rather than failing the whole audit on the first stale field.
        stale: set[tuple[str, str]] = set()
        cxx_out: dict[str, int] = {}
        for _attempt in range(8):
            probe = {
                n: [f for f in fs if (n, f) not in stale] for n, fs in structs.items()
            }
            cxx_src.write_text(emit_cxx_offsets(probe))
            proc = subprocess.run(
                [args.cxx_compiler, "-std=c++26", "-w",
                 *[f"-I{i}" for i in args.cxx_include],
                 *[f"-D{d}" for d in args.cxx_define],
                 str(cxx_src), "-o", str(tmp_path / "cxx_probe")],
                capture_output=True, text=True,
            )
            newly = {
                (m.group(2), m.group(1))
                for m in re.finditer(
                    r"no member named '(\w+)' in '(\w+)'", proc.stderr
                )
            }
            if proc.returncode == 0:
                run = subprocess.run([str(tmp_path / "cxx_probe")], capture_output=True, text=True)
                if run.returncode != 0:
                    raise SystemExit(f"error: cxx_probe failed\n{run.stderr[:2000]}")
                cxx_out = parse_measurements(run.stdout)
                break
            if not newly - stale:
                raise SystemExit(f"error: C++ probe would not compile\n{proc.stderr[:4000]}")
            stale |= newly
        else:
            raise SystemExit("error: too many stale fields to resolve")

    size_bad = []
    off_bad = []
    for key, cpp_value in sorted(cxx_out.items()):
        c_value = c_out.get(key)
        if c_value is None or c_value == cpp_value:
            continue
        (size_bad if key.startswith("S ") else off_bad).append((key[2:], c_value, cpp_value))

    padding = sorted(s for s in stale if PADDING_RE.match(s[1]))
    real_stale = sorted(s for s in stale if not PADDING_RE.match(s[1]))

    print()
    print(f"structs with wrong sizeof    : {len(size_bad)} of {len(structs)}")
    for name, cv, pv in size_bad:
        print(f"    {name:<30} cdef={cv:<6} C++={pv}")
    print(f"fields at the wrong offset   : {len(off_bad)}")
    for name, cv, pv in off_bad:
        print(f"    {name:<38} cdef={cv:<6} C++={pv}")
    print(f"cdef fields absent from C++  : {len(real_stale)}")
    for name, field in real_stale:
        print(f"    {name}.{field}")
    print(f"hand-written padding fields  : {len(padding)}")

    ok = not (missing or size_bad or off_bad or real_stale)
    print()
    print("FFI layout OK." if ok else "FFI layout DRIFTED -- ffi_cdef.fnl does not match Components.hpp.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
