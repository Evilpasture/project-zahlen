#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Planner for the GCC 16.x compiler matrix.

The workflow is data-driven: .github/gcc16/compilers.json is the only place a
version, a runner, or a pinned artifact is named. `plan` emits the GitHub Actions
matrix as a single JSON line; `verify` refuses to plan a manifest whose pins are
missing, malformed or have drifted from scripts/install-gcc-prebuilt.sh; `cell`
and `artifacts` feed scripts/install-gcc16.sh.

Nothing here downloads anything. Downloads and checksum checks live in
install-gcc16.sh so they also run when a developer executes the installer by
hand, outside Actions.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / ".github" / "gcc16" / "compilers.json"
LEGACY_INSTALLER = ROOT / "scripts" / "install-gcc-prebuilt.sh"
SHA256 = re.compile(r"^[0-9a-f]{64}$")
VERSION = re.compile(r"^16\.\d+\.\d+$")
PLATFORMS = frozenset({"linux", "ucrt64"})
KINDS = frozenset({"conda", "arch-pkg", "winlibs"})
PROBES = frozenset({"reflection", "jolt", "base64", "runtime"})
# Fields that are safe to put in a matrix include entry (flat scalars only).
MATRIX_FIELDS = (
    "id", "gcc", "platform", "runner", "label", "experimental",
    "full_build", "reported_version", "kind",
)


class MatrixError(Exception):
    pass


def load(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise MatrixError(f"cannot read {path}: {exc}") from exc
    if data.get("schema") != 1:
        raise MatrixError(f"{path}: unsupported schema {data.get('schema')!r} (want 1)")
    if not isinstance(data.get("cells"), list) or not data["cells"]:
        raise MatrixError(f"{path}: `cells` must be a non-empty list")
    return data


def legacy_pins() -> list[tuple[str, str, str]]:
    """Read the six `<subdir> <name> <sha256>` lines out of the pinned installer."""
    text = LEGACY_INSTALLER.read_text(encoding="utf-8")
    match = re.search(r"ARTIFACTS=\$\(cat <<'EOF'\n(.*?)\nEOF\n\)", text, re.S)
    if not match:
        raise MatrixError(f"{LEGACY_INSTALLER}: cannot find the ARTIFACTS heredoc")
    rows = []
    for line in match.group(1).splitlines():
        parts = line.split()
        if len(parts) == 3:
            rows.append((parts[0], parts[1], parts[2]))
    return rows


def verify(data: dict) -> None:
    errors: list[str] = []
    seen: set[str] = set()
    for cell in data["cells"]:
        cid = cell.get("id", "<no id>")
        if cid in seen:
            errors.append(f"{cid}: duplicate id")
        seen.add(cid)
        for key in ("gcc", "platform", "runner", "label", "kind",
                    "reported_version", "available", "experimental", "full_build"):
            if key not in cell:
                errors.append(f"{cid}: missing `{key}`")
        if not VERSION.match(str(cell.get("gcc", ""))):
            errors.append(f"{cid}: gcc must look like 16.x.y")
        if cell.get("gcc") != cell.get("reported_version"):
            # A cell may legitimately pin a different compiler than its label,
            # but then it must be unavailable so it never runs silently.
            if cell.get("available"):
                errors.append(f"{cid}: available cell reports {cell.get('reported_version')} "
                              f"but is labelled {cell.get('gcc')}")
        if cell.get("platform") not in PLATFORMS:
            errors.append(f"{cid}: platform must be one of {sorted(PLATFORMS)}")
        if cell.get("kind") not in KINDS:
            errors.append(f"{cid}: kind must be one of {sorted(KINDS)}")
        if cell.get("platform") == "ucrt64" and not str(cell.get("runner", "")).startswith("windows"):
            errors.append(f"{cid}: ucrt64 needs a windows runner")
        if cell.get("platform") == "linux" and str(cell.get("runner", "")).startswith("windows"):
            errors.append(f"{cid}: linux cannot use a windows runner")
        expected = cell.setdefault("expected_probe_failures", [])
        if not isinstance(expected, list) or any(x not in PROBES for x in expected):
            errors.append(f"{cid}: expected_probe_failures must be a list of {sorted(PROBES)}")
        if not cell.get("full_build") and not cell.get("full_build_reason") and cell.get("available"):
            errors.append(f"{cid}: active cell with full_build=false needs full_build_reason")

        arts = cell.get("artifacts", [])
        if cell.get("available") and not arts:
            errors.append(f"{cid}: available cell has no pinned artifacts")
        if not cell.get("available") and arts:
            errors.append(f"{cid}: unavailable cell still has artifacts; flip `available`")
        for art in arts:
            name = art.get("name", "<unnamed>")
            url = str(art.get("url", ""))
            sha = str(art.get("sha256", ""))
            if not url.startswith("https://"):
                errors.append(f"{cid}/{name}: url must be https")
            if not SHA256.match(sha):
                errors.append(f"{cid}/{name}: sha256 must be 64 lowercase hex chars")
            if not isinstance(art.get("bytes"), int) or art["bytes"] <= 0:
                errors.append(f"{cid}/{name}: `bytes` must be a positive integer")
            if url.rsplit("/", 1)[-1] not in (name, name.replace("+", "%2B")):
                errors.append(f"{cid}/{name}: url basename does not match name")

    # A conda cell delegates installation to the existing pinned script. If the
    # two pin sets drift, the manifest would describe a compiler the job does
    # not actually install.
    pins = legacy_pins()
    conda_cells = [c for c in data["cells"] if c.get("kind") == "conda" and c.get("available")]
    for cell in conda_cells:
        got = {(a["name"], a["sha256"]) for a in cell["artifacts"]}
        want = {(name, sha) for _, name, sha in pins}
        if got != want:
            errors.append(f"{cell['id']}: pins differ from {LEGACY_INSTALLER.name} "
                          f"(missing={sorted(want - got)}, extra={sorted(got - want)})")

    if errors:
        raise MatrixError("manifest verification failed:\n  - " + "\n  - ".join(errors))


def plan(data: dict, event: str, mode_input: str, include_unavailable: bool) -> tuple[str, list[dict], str]:
    verify(data)
    full_events = data.get("full_build", {}).get("events", ["schedule"])
    if mode_input == "full" or (mode_input != "gate" and event in full_events):
        mode = "full"
    else:
        mode = "gate"
    cells = []
    for cell in data["cells"]:
        if not cell.get("available") and not include_unavailable:
            continue
        entry = {k: cell.get(k, "") for k in MATRIX_FIELDS}
        # Skipped cells are always experimental so a probe run cannot turn the
        # workflow red while its artifacts are still unpublished.
        entry["experimental"] = bool(cell.get("experimental")) or not cell.get("available")
        cells.append(entry)
    if not cells:
        raise MatrixError("no matrix cells selected")
    summary = (f"{len(cells)} cell(s), mode={mode}, "
               f"active={sum(1 for c in data['cells'] if c.get('available'))}")
    return mode, cells, summary


def emit(lines: list[str]) -> None:
    out = os.environ.get("GITHUB_OUTPUT")
    text = "\n".join(lines) + "\n"
    if out:
        with open(out, "a", encoding="utf-8") as fh:
            fh.write(text)
    else:
        sys.stdout.write(text)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("verify", help="validate pins and schema")

    p_plan = sub.add_parser("plan", help="emit cells=, mode=, summary= for GITHUB_OUTPUT")
    p_plan.add_argument("--event", default=os.environ.get("GITHUB_EVENT_NAME", "push"))
    p_plan.add_argument("--mode", default=os.environ.get("ZHLN_GCC16_MODE", ""))
    p_plan.add_argument("--include-unavailable", action="store_true",
                        default=os.environ.get("ZHLN_GCC16_ALL_CELLS") == "1")

    p_cell = sub.add_parser("cell", help="print a cell's scalar fields as key=value")
    p_cell.add_argument("--id", required=True)

    p_art = sub.add_parser("artifacts", help="print name<TAB>url<TAB>sha256<TAB>bytes lines")
    p_art.add_argument("--id", required=True)

    p_probes = sub.add_parser("probes", help="print the cell's known-failing probes, space separated")
    p_probes.add_argument("--id", required=True)

    p_list = sub.add_parser("list", help="one line per cell, for humans")
    args = ap.parse_args()

    try:
        data = load(args.manifest)
        if args.cmd == "verify":
            verify(data)
            print(f"OK: {len(data['cells'])} cells, "
                  f"{sum(len(c.get('artifacts', [])) for c in data['cells'])} pinned artifacts")
            return 0
        if args.cmd == "plan":
            mode, cells, summary = plan(data, args.event, args.mode, args.include_unavailable)
            emit([f"cells={json.dumps(cells, separators=(',', ':'))}",
                  f"mode={mode}", f"summary={summary}"])
            return 0
        if args.cmd == "list":
            for c in data["cells"]:
                state = "ACTIVE" if c.get("available") else "SKIP  "
                reason = c.get("unavailable_reason", "")
                print(f"{state} {c['id']:<20} {c['label']:<34} "
                      f"{len(c.get('artifacts', []))} pins  {reason[:70]}")
            return 0
        cell = next((c for c in data["cells"] if c.get("id") == args.id), None)
        if cell is None:
            raise MatrixError(f"unknown cell id {args.id!r}")
        if args.cmd == "cell":
            for key, value in cell.items():
                if key == "artifacts":
                    continue
                if isinstance(value, bool):
                    value = "1" if value else "0"
                elif isinstance(value, (str, int)):
                    value = str(value)
                else:
                    continue
                # Values are single-line by construction; refuse anything with a
                # newline so sourcing/reading cannot inject shell lines.
                if "\n" in value:
                    raise MatrixError(f"{args.id}/{key}: newline in scalar value")
                print(f"{key}={value}")
            return 0
        if args.cmd == "artifacts":
            for art in cell.get("artifacts", []):
                print(f"{art['name']}\t{art['url']}\t{art['sha256']}\t{art['bytes']}")
            return 0
        if args.cmd == "probes":
            print(" ".join(cell.get("expected_probe_failures", [])))
            return 0
    except MatrixError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
