#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""A `slangc` work-alike built on the Slang compiler inside the PyPI `slangpy` wheel.

Slang's official binaries are distributed as GitHub release assets, which not
every machine can reach. The PyPI `slangpy` package carries the same compiler
as `libslang-compiler.so`, and Slang's C API exposes
`spProcessCommandLineArguments()` -- the entry point the real `slangc`
front-end is built on. This script feeds it the `slangc` command line and
writes the resulting code blob to the requested output path, so the shader
build in cmake/ShaderCompilation.cmake runs unmodified.

Supported subset (what the build uses): one input file with one entry point,
`-entry`, `-stage`, `-target`, `-I`, extra compiler flags, `-o`.
"""

import ctypes
import glob
import importlib.util
import os
import sys

SPIRV_MAGIC = 0x07230203


def find_slang_library() -> str:
    """Locate libslang-compiler.so, preferring the copy shipped by slangpy."""
    override = os.environ.get("ZHLN_SLANG_LIB")
    if override:
        return override

    spec = importlib.util.find_spec("slangpy")
    if spec is not None and spec.submodule_search_locations:
        package_dir = list(spec.submodule_search_locations)[0]
        candidates = sorted(glob.glob(os.path.join(package_dir, "libslang-compiler.so*")))
        if candidates:
            return candidates[-1]

    for pattern in ("/usr/local/lib/libslang-compiler.so*", "/usr/lib/libslang-compiler.so*"):
        candidates = sorted(glob.glob(pattern))
        if candidates:
            return candidates[-1]

    # Fall back to the loader's search path.
    return "libslang-compiler.so"


def bind(library_path: str) -> ctypes.CDLL:
    slang = ctypes.CDLL(library_path)
    slang.spCreateSession.restype = ctypes.c_void_p
    slang.spCreateSession.argtypes = [ctypes.c_char_p]
    slang.spDestroySession.argtypes = [ctypes.c_void_p]
    slang.spCreateCompileRequest.restype = ctypes.c_void_p
    slang.spCreateCompileRequest.argtypes = [ctypes.c_void_p]
    slang.spDestroyCompileRequest.argtypes = [ctypes.c_void_p]
    slang.spProcessCommandLineArguments.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p), ctypes.c_int]
    slang.spCompile.restype = ctypes.c_int
    slang.spCompile.argtypes = [ctypes.c_void_p]
    slang.spGetDiagnosticOutput.restype = ctypes.c_char_p
    slang.spGetDiagnosticOutput.argtypes = [ctypes.c_void_p]
    slang.spGetEntryPointCode.restype = ctypes.c_void_p
    slang.spGetEntryPointCode.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_size_t)]
    slang.spGetCompileRequestCode.restype = ctypes.c_void_p
    slang.spGetCompileRequestCode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)]
    return slang


def compile(slang: ctypes.CDLL, compiler_args: list[str]) -> tuple[int, bytes, str]:
    """Run one compilation. Returns (exit code, code blob, diagnostics)."""
    session = slang.spCreateSession(b"")
    if not session:
        return 1, b"", "failed to create Slang session"

    request = slang.spCreateCompileRequest(session)
    if not request:
        slang.spDestroySession(session)
        return 1, b"", "failed to create Slang compile request"

    try:
        encoded = [arg.encode() for arg in compiler_args]
        argv_type = ctypes.c_char_p * len(encoded)
        status = slang.spProcessCommandLineArguments(request, argv_type(*encoded), len(encoded))
        if status != 0:
            return 1, b"", _diagnostics(slang, request) or f"argument processing failed ({status})"

        status = slang.spCompile(request)
        diagnostics = _diagnostics(slang, request)
        if status != 0:
            return 1, b"", diagnostics or f"compilation failed ({status})"

        size = ctypes.c_size_t(0)
        code = slang.spGetEntryPointCode(request, 0, ctypes.byref(size))
        if not code:
            # Whole-program output (no separate entry points compiled).
            code = slang.spGetCompileRequestCode(request, ctypes.byref(size))
        if not code or size.value == 0:
            return 1, b"", diagnostics or "compilation produced no code"

        return 0, ctypes.string_at(code, size.value), diagnostics
    finally:
        slang.spDestroyCompileRequest(request)
        slang.spDestroySession(session)


def _diagnostics(slang: ctypes.CDLL, request: ctypes.c_void_p) -> str:
    raw = slang.spGetDiagnosticOutput(request)
    return raw.decode(errors="replace").strip() if raw else ""


def main(argv: list[str]) -> int:
    # Everything except -o is handed to the compiler verbatim; -o is satisfied
    # from the code blob it hands back.
    compiler_args: list[str] = []
    output_path: str | None = None

    index = 0
    while index < len(argv):
        if argv[index] == "-o" and index + 1 < len(argv):
            output_path = argv[index + 1]
            index += 2
            continue
        compiler_args.append(argv[index])
        index += 1

    if not compiler_args or output_path is None:
        print("usage: slangc_slangpy.py <input> [options] -o <output>", file=sys.stderr)
        return 2

    status, code, diagnostics = compile(bind(find_slang_library()), compiler_args)

    if diagnostics:
        print(diagnostics, file=sys.stderr if status else sys.stdout)
    if status != 0:
        return status

    directory = os.path.dirname(os.path.abspath(output_path))
    os.makedirs(directory, exist_ok=True)
    with open(output_path, "wb") as handle:
        handle.write(code)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
