#!/usr/bin/env python3
"""Drop-in `slangc` replacement for machines without a Vulkan SDK.

Drives the Slang compiler bundled with slangpy through its C API and emits real
SPIR-V, so repo tooling that shells out to slangc (e.g.
tools/check_shader_interfaces.py) keeps working:

    pip install --break-system-packages slangpy
    python3 tools/slangc_shim.py <file> -entry <E> -stage <S> -target spirv \
        -fvk-use-entrypoint-name -matrix-layout-column-major -I <dir> ... \
        [-DFOO ...] -o <out.spv>
"""

import ctypes
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SLANG_LIB_GLOB = "/usr/local/lib/python3.11/dist-packages/slangpy/libslang-compiler.so*"


def find_lib() -> str:
    import glob

    hits = sorted(glob.glob(SLANG_LIB_GLOB))
    if not hits:
        sys.exit("slangpy not installed; run: pip install --break-system-packages slangpy")
    return hits[0]


def main() -> int:
    args = sys.argv[1:]
    out_path = None
    entry = None
    stage = None
    defines: list[str] = []
    source = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == "-o":
            out_path = args[i + 1]
            i += 2
        elif a == "-entry":
            entry = args[i + 1]
            i += 2
        elif a == "-stage":
            stage = args[i + 1]
            i += 2
        elif a.startswith("-D"):
            defines.append(a)
            i += 1
        elif a.startswith("-"):
            i += 2 if i + 1 < len(args) and not args[i + 1].startswith("-") else 1
        else:
            source = a
            i += 1

    if source is None or entry is None or out_path is None:
        sys.stderr.write(__doc__)
        return 2

    lib = ctypes.CDLL(find_lib())
    lib.slang_createGlobalSession.restype = ctypes.c_int
    lib.slang_createGlobalSession.argtypes = [ctypes.c_longlong, ctypes.POINTER(ctypes.c_void_p)]
    lib.spCreateCompileRequest.restype = ctypes.c_void_p
    lib.spCreateCompileRequest.argtypes = [ctypes.c_void_p]
    lib.spProcessCommandLineArguments.restype = ctypes.c_int
    lib.spProcessCommandLineArguments.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p), ctypes.c_int]
    lib.spCompile.restype = ctypes.c_int
    lib.spCompile.argtypes = [ctypes.c_void_p]
    lib.spGetDiagnosticOutput.restype = ctypes.c_char_p
    lib.spGetDiagnosticOutput.argtypes = [ctypes.c_void_p]
    lib.spGetCompileRequestCode.restype = ctypes.c_void_p
    lib.spGetCompileRequestCode.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)]
    lib.spDestroyCompileRequest.argtypes = [ctypes.c_void_p]

    session = ctypes.c_void_p()
    if lib.slang_createGlobalSession(0, ctypes.byref(session)) != 0 or not session.value:
        sys.stderr.write("slangc-shim: slang_createGlobalSession failed\n")
        return 2
    req = lib.spCreateCompileRequest(session)
    argv = [
        source.encode(),
        b"-entry", entry.encode(),
        b"-stage", (stage or "fragment").encode(),
        b"-target", b"spirv",
        b"-fvk-use-entrypoint-name",
        b"-matrix-layout-column-major",
        *[d.encode() for d in defines],
        b"-o", out_path.encode(),
    ]
    arr = (ctypes.c_char_p * len(argv))(*argv)
    if lib.spProcessCommandLineArguments(req, arr, len(argv)) != 0:
        sys.stderr.write(f"slangc-shim: argument error for {source}\n")
        return 2
    rc = lib.spCompile(req)
    diag = (lib.spGetDiagnosticOutput(req) or b"").decode(errors="replace")
    size = ctypes.c_size_t(0)
    blob = lib.spGetCompileRequestCode(req, ctypes.byref(size)) if rc == 0 else None
    spirv = ctypes.string_at(blob, size.value) if blob and size.value else b""
    lib.spDestroyCompileRequest(req)
    if rc != 0 or len(spirv) < 8 or spirv[:4] != b"\x03\x02\x23\x07":
        if diag:
            sys.stderr.write(diag)
        elif rc == 0:
            sys.stderr.write(f"slangc-shim: {source}:{entry} produced no SPIR-V\n")
        return 1
    Path(out_path).write_bytes(spirv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
