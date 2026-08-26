#!/usr/bin/env python3
"""Full shader compile verification via the Slang C API (bundled with slangpy).

Parses every add_shader_target() block out of cmake/ShaderCompilation.cmake and
replays the exact `slangc <file> -entry <E> -stage <S> -target spirv
-fvk-use-entrypoint-name -matrix-layout-column-major -I resources/shaders -I
include [-D...] -o out.spv` command line the build would run, then checks that
SPIR-V actually comes out. Requires only `pip install slangpy` (no Vulkan SDK).
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

import ctypes

REPO = Path(__file__).resolve().parent.parent
CMAKE = REPO / "cmake" / "ShaderCompilation.cmake"
SLANG_LIB = next(
    iter(sorted(Path("/usr/local/lib/python3.11/dist-packages/slangpy").glob("libslang-compiler.so*"))), None
)

# SLANG_SPIRV
SLANG_SPIRV = 8  # verified against SlangCompileTarget: 0=none..? set via CLI anyway

STAGE_MAP = {
    "vs_6_0": "vertex", "vs_6_5": "vertex",
    "ps_6_0": "fragment", "ps_6_5": "fragment",
    "cs_6_0": "compute",
    "as_6_5": "amplification",
    "ms_6_5": "mesh",
}


def parse_targets() -> list[tuple[str, str, str, str, list[str]]]:
    """Yields (file, entry, profile, macro, extra_args) tuples."""
    text = CMAKE.read_text()
    targets = []
    for m in re.finditer(r"add_shader_target\((\w+)\s*\n(.*?)\n\)\n", text, re.S):
        body = m.group(2)
        stages_m = re.search(r"STAGES(.*?)EXTRA_ARGS", body, re.S) or re.search(r"STAGES(.*?)$", body, re.S)
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
            path = path.replace("${SHADER_SRC_DIR}", str(REPO / "resources" / "shaders"))
            per_stage = parts[4:]
            targets.append((path, entry, profile, macro, extra_args + per_stage))
    return targets


def load_slang():
    lib = ctypes.CDLL(str(SLANG_LIB))
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
    rc = lib.slang_createGlobalSession(0, ctypes.byref(session))
    assert rc == 0 and session.value, "slang_createGlobalSession failed"
    return lib, session


def compile_one(lib, gsession, shader_path: Path, entry: str, stage: str, extra: list[str], out: Path) -> tuple[bool, str]:
    req = lib.spCreateCompileRequest(gsession)
    # spProcessCommandLineArguments parses every arg it is given — no argv[0].
    argv = [
        str(shader_path).encode(),
        b"-entry", entry.encode(),
        b"-stage", stage.encode(),
        b"-target", b"spirv",
        b"-fvk-use-entrypoint-name",
        b"-matrix-layout-column-major",
        b"-I", str(REPO / "resources" / "shaders").encode(),
        b"-I", str(REPO / "include").encode(),
        *[(a.encode() if a.startswith("-") else a.encode()) for a in extra],
        b"-o", str(out).encode(),
    ]
    argc = len(argv)
    arr = (ctypes.c_char_p * argc)(*argv)
    r1 = lib.spProcessCommandLineArguments(req, arr, argc)
    if r1 != 0:
        return False, f"arg parse failed: {r1:#x}"
    r2 = lib.spCompile(req)
    diag = (lib.spGetDiagnosticOutput(req) or b"").decode(errors="replace")
    code_ptr = ctypes.c_size_t(0)
    blob = lib.spGetCompileRequestCode(req, ctypes.byref(code_ptr))
    spirv = ctypes.string_at(blob, code_ptr.value) if (r2 == 0 and blob and code_ptr.value > 0) else b""
    lib.spDestroyCompileRequest(req)
    ok = r2 == 0 and len(spirv) > 4 and spirv[:4] == b"\x03\x02\x23\x07"  # SPIR-V magic
    if ok:
        out.write_bytes(spirv)
        return True, ""
    return False, (diag or "no SPIR-V produced").strip()


def main() -> int:
    if not SLANG_LIB:
        print("slangpy not installed; run: pip install --break-system-packages slangpy")
        return 2
    keep_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    if keep_dir:
        keep_dir.mkdir(parents=True, exist_ok=True)
    targets = parse_targets()
    print(f"{len(targets)} stage compilations parsed from ShaderCompilation.cmake\n")
    lib, gsession = load_slang()
    failures = 0
    with tempfile.TemporaryDirectory() as td:
        for path, entry, profile, macro, extra in targets:
            stage = STAGE_MAP.get(profile, profile)
            out = Path(td) / f"{Path(path).stem}.{entry}.spv"
            ok, diag = compile_one(lib, gsession, Path(path), entry, stage, extra, out)
            tag = "OK  " if ok else "FAIL"
            label = f"{Path(path).name}:{entry}[{','.join(extra) or '-'}]"
            print(f"{tag} {label}")
            if keep_dir and ok:
                keep = keep_dir / f"{Path(path).stem}.{entry}.{'.'.join(a[2:] for a in extra)}.spv"
                keep.write_bytes(out.read_bytes())
            if not ok:
                failures += 1
                print((diag or "no SPIR-V produced").strip()[:4000])
                print()
    print(f"\n{len(targets) - failures}/{len(targets)} compiled")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
