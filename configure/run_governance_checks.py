#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run all repository configure-time governance checks in parallel with stat-manifest caching.

Invoked once by cmake/GovernanceChecks.cmake so configure does not pay 12
sequential interpreter startups and 12 sequential tree walks on every reconfigure:
  * On POSIX hosts (Linux/macOS), forks lightweight worker processes (avoiding
    11 extra Python startup/import cycles while bypassing the GIL across cores)
    and runs Perl's check_include_provenance.pl concurrently.
  * On Windows (no fork), runs all 12 checks concurrently via subprocesses.
  * Fingerprints every governed source file, CMake file, checker script, and
    JSON allowlist by (relative path, mtime_ns, byte size).
  * Only a 100% clean pass writes the cache file; any violation fails
    immediately with the failing checker's own diagnostic on stderr.
  * Set ZHLN_GOVERNANCE_CACHE=0 (or pass --no-cache) to force a full uncached
    run regardless of cache state.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
import contextlib
import hashlib
import importlib.util
import io
import json
import multiprocessing
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CONFIGURE_DIR = ROOT / "configure"

GOVERNED_ROOTS = (
    "include",
    "src",
    "plugins",
    "extensions",
    "gameplay",
    "modules",
    "tools",
    "tests",
    "samples",
    "app",
)
GOVERNED_SUFFIXES = frozenset({
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
    ".cmake",
})
SKIP_DIRS = frozenset({".git", "build", "target", "__pycache__", ".cache", "node_modules"})

# Ordered to match cmake/GovernanceChecks.cmake's historical execution sequence.
# Ordered heaviest-first when submitted to workers, then reassembled in exact
# historical display order before printing/caching.
HISTORICAL_PYTHON_ORDER: tuple[str, ...] = (
    "check_core_layer_boundary.py",
    "check_layer_dag.py",
    "check_tests_public_api.py",
    "check_subsystem_boundaries.py",
    "check_pimpl_encapsulation.py",
    "check_reflection_boundary.py",
    "check_macro_governance.py",
    "check_include_governance.py",
    "check_namespace_governance.py",
    "check_module_linkage.py",
    "check_error_ordinals.py",
)
# Longest-pole checks scheduled first to minimize parallel tail latency.
SCHEDULE_PYTHON_ORDER: tuple[str, ...] = (
    "check_error_ordinals.py",
    "check_reflection_boundary.py",
    "check_namespace_governance.py",
    "check_include_governance.py",
    "check_tests_public_api.py",
    "check_module_linkage.py",
    "check_layer_dag.py",
    "check_core_layer_boundary.py",
    "check_macro_governance.py",
    "check_subsystem_boundaries.py",
    "check_pimpl_encapsulation.py",
)
PERL_CHECK = "check_include_provenance.pl"

CACHE_SCHEMA_VERSION = 1


def _scan_dir_stats(directory: Path, rel_prefix: str, entries: list[str]) -> None:
    try:
        it = os.scandir(directory)
    except OSError:
        return
    subdirs: list[tuple[str, Path]] = []
    files: list[tuple[str, os.DirEntry[str]]] = []
    with it:
        for entry in it:
            name = entry.name
            if entry.is_dir(follow_symlinks=False):
                if name not in SKIP_DIRS:
                    subdirs.append((f"{rel_prefix}/{name}", Path(entry.path)))
            elif entry.is_file(follow_symlinks=False):
                dot = name.rfind(".")
                ext = name[dot:].lower() if dot != -1 else ""
                if ext in GOVERNED_SUFFIXES or name == "CMakeLists.txt":
                    files.append((f"{rel_prefix}/{name}", entry))

    files.sort(key=lambda item: item[0])
    for rel, entry in files:
        st = entry.stat(follow_symlinks=False)
        entries.append(f"{rel}\0{st.st_mtime_ns}\0{st.st_size}\n")

    subdirs.sort(key=lambda item: item[0])
    for sub_rel, sub_path in subdirs:
        _scan_dir_stats(sub_path, sub_rel, entries)


def compute_tree_digest() -> str:
    hasher = hashlib.sha256()
    hasher.update(f"schema:{CACHE_SCHEMA_VERSION}\n".encode())

    cfg_entries: list[str] = []
    if CONFIGURE_DIR.is_dir():
        with os.scandir(CONFIGURE_DIR) as it:
            for entry in it:
                if not entry.is_file(follow_symlinks=False):
                    continue
                if entry.name.endswith((".py", ".pl", ".json")):
                    st = entry.stat(follow_symlinks=False)
                    cfg_entries.append(f"configure/{entry.name}\0{st.st_mtime_ns}\0{st.st_size}\n")
    cfg_entries.sort()
    hasher.update("".join(cfg_entries).encode())

    tree_entries: list[str] = []
    for root_name in GOVERNED_ROOTS:
        root_path = ROOT / root_name
        if root_path.is_dir():
            _scan_dir_stats(root_path, root_name, tree_entries)
    hasher.update("".join(tree_entries).encode())

    extern_dir = ROOT / "extern"
    if extern_dir.is_dir():
        ext_entries: list[str] = []
        with os.scandir(extern_dir) as it:
            for entry in it:
                git_marker = Path(entry.path) / ".git"
                if git_marker.exists():
                    st = git_marker.stat()
                    ext_entries.append(f"extern/{entry.name}\0{st.st_mtime_ns}\0{st.st_size}\n")
        ext_entries.sort()
        hasher.update("".join(ext_entries).encode())

    return hasher.hexdigest()


def _run_python_check_worker(script_name: str) -> tuple[str, int, str, str]:
    script_path = CONFIGURE_DIR / script_name
    module_name = f"_zhln_gov_{script_path.stem}"
    spec = importlib.util.spec_from_file_location(module_name, script_path)
    if spec is None or spec.loader is None:
        return script_name, 1, "", f"Failed to load {script_path}"
    mod = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = mod

    out_buf = io.StringIO()
    err_buf = io.StringIO()
    saved_argv = sys.argv
    try:
        sys.argv = [str(script_path)]
        with contextlib.redirect_stdout(out_buf), contextlib.redirect_stderr(err_buf):
            spec.loader.exec_module(mod)
            rc = mod.main()
    except SystemExit as exc:
        rc = exc.code if isinstance(exc.code, int) else (0 if exc.code is None else 1)
    finally:
        sys.argv = saved_argv
        sys.modules.pop(module_name, None)

    return script_name, int(rc or 0), out_buf.getvalue().strip(), err_buf.getvalue().strip()


def _run_subprocess_check(script_name: str, interpreter: str) -> tuple[str, int, str, str]:
    proc = subprocess.run(
        [interpreter, str(CONFIGURE_DIR / script_name)],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    return script_name, proc.returncode, proc.stdout.strip(), proc.stderr.strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--perl", default="perl", help="Perl interpreter path")
    parser.add_argument("--cache-file", default="", help="Optional stat-cache JSON path in the build directory")
    parser.add_argument("--no-cache", action="store_true", help="Bypass the stat cache")
    args = parser.parse_args()

    use_cache = (
        bool(args.cache_file)
        and not args.no_cache
        and os.environ.get("ZHLN_GOVERNANCE_CACHE", "1") != "0"
    )
    cache_path = Path(args.cache_file) if args.cache_file else None
    digest = compute_tree_digest() if use_cache else ""

    if use_cache and cache_path is not None and cache_path.is_file():
        try:
            cached = json.loads(cache_path.read_text(encoding="utf-8"))
            if cached.get("digest") == digest and isinstance(cached.get("outputs"), list):
                for line in cached["outputs"]:
                    if line:
                        print(line)
                return 0
        except Exception:
            pass

    os.chdir(ROOT)
    workers = min(len(SCHEDULE_PYTHON_ORDER), max(2, os.cpu_count() or 4))
    results_by_name: dict[str, tuple[int, str, str]] = {}

    perl_proc = subprocess.Popen(
        [args.perl, str(CONFIGURE_DIR / PERL_CHECK)],
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )

    try:
        if "fork" in multiprocessing.get_all_start_methods():
            mp_ctx = multiprocessing.get_context("fork")
            with ProcessPoolExecutor(max_workers=workers, mp_context=mp_ctx) as pool:
                for script_name, rc, out, err in pool.map(_run_python_check_worker, SCHEDULE_PYTHON_ORDER):
                    results_by_name[script_name] = (rc, out, err)
        else:
            with ThreadPoolExecutor(max_workers=workers) as pool:
                futs = [pool.submit(_run_subprocess_check, s, sys.executable) for s in SCHEDULE_PYTHON_ORDER]
                for fut in futs:
                    script_name, rc, out, err = fut.result()
                    results_by_name[script_name] = (rc, out, err)
    except Exception:
        perl_proc.kill()
        perl_proc.communicate()
        raise

    perl_out, perl_err = perl_proc.communicate()
    results_by_name[PERL_CHECK] = (perl_proc.returncode, perl_out.strip(), perl_err.strip())

    outputs: list[str] = []
    for script_name in (*HISTORICAL_PYTHON_ORDER, PERL_CHECK):
        rc, stdout, stderr = results_by_name[script_name]
        if rc != 0:
            err_msg = stderr or stdout or f"{script_name} exited with status {rc}"
            print(err_msg, file=sys.stderr)
            return rc
        if stdout:
            outputs.append(stdout)

    for line in outputs:
        print(line)

    if use_cache and cache_path is not None:
        try:
            cache_path.parent.mkdir(parents=True, exist_ok=True)
            tmp_path = cache_path.with_suffix(".json.tmp")
            tmp_path.write_text(
                json.dumps({"digest": digest, "outputs": outputs}),
                encoding="utf-8",
            )
            tmp_path.replace(cache_path)
        except OSError:
            pass

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
