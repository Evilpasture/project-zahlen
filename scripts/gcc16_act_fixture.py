#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
"""Render a static-matrix copy of gcc16-matrix.yml so `act` can run one cell.

`act` v0.2.97 parses `matrix.include` before it runs any job, so an expression
like `${{ fromJSON(needs.plan.outputs.cells) }}` cannot be evaluated: it errors
with "Invalid JSON: unexpected end of JSON input". Real GitHub Actions expands
that expression fine; this helper is only for local runs.

It takes the planner's real output for ONE cell, inlines it, drops the `plan`
job and the `needs` references, and leaves every step body untouched. The output
is a scratch workflow -- never commit it, and never use it in Actions.

    python3 scripts/gcc16_act_fixture.py --cell 16.1.1-linux -o /tmp/act.yml
    act -W /tmp/act.yml -j gcc16 --pull=false

`--no-apt` replaces the two apt-get steps with a tool-presence check, for a
runner image that already has curl/zstd/unzip but is not Ubuntu.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORKFLOW = ROOT / ".github" / "workflows" / "gcc16-matrix.yml"
PLANNER = ROOT / "scripts" / "gcc16_matrix.py"


def plan_cells(event: str) -> list[dict]:
    out = subprocess.check_output(
        [sys.executable, str(PLANNER), "plan", "--event", event],
        text=True, cwd=ROOT,
    )
    for line in out.splitlines():
        if line.startswith("cells="):
            return json.loads(line[len("cells="):])
    raise RuntimeError("planner produced no cells= line")


def static_include(cell: dict) -> str:
    lines = ["        include:"]
    for i, (key, value) in enumerate(cell.items()):
        prefix = "          - " if i == 0 else "            "
        lines.append(f"{prefix}{key}: {json.dumps(value)}")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cell", required=True, help="cell id, e.g. 16.2.0-linux")
    ap.add_argument("-o", "--output", required=True, type=Path)
    ap.add_argument("--event", default="push")
    ap.add_argument("--no-apt", action="store_true",
                    help="replace apt-get steps with a tool-presence check")
    args = ap.parse_args()

    cell = next((c for c in plan_cells(args.event) if c["id"] == args.cell), None)
    if cell is None:
        print(f"error: {args.cell!r} is not in the {args.event} plan; "
              f"available: {[c['id'] for c in plan_cells(args.event)]}", file=sys.stderr)
        return 1

    lines = WORKFLOW.read_text(encoding="utf-8").splitlines(keepends=True)
    plan_start = next(i for i, l in enumerate(lines) if l.startswith("  plan:"))
    gcc16_start = next(i for i, l in enumerate(lines) if l.startswith("  gcc16:"))
    text = "".join(lines[:plan_start] + lines[gcc16_start:])
    mode = "full" if args.event in ("schedule", "workflow_dispatch") else "gate"

    text = text.replace("${{ matrix.label }} [${{ needs.plan.outputs.mode }}]",
                        f"{cell['label']} [{mode}]")
    text = text.replace("    needs: plan\n", "")
    text = text.replace("    runs-on: ${{ matrix.runner }}", f"    runs-on: {cell['runner']}")
    text = text.replace("    continue-on-error: ${{ matrix.experimental }}",
                        f"    continue-on-error: {'true' if cell['experimental'] else 'false'}")
    old_include = "        include: ${{ fromJSON(needs.plan.outputs.cells) }}"
    if old_include not in text:
        raise RuntimeError("workflow no longer has a dynamic matrix include")
    text = text.replace(old_include, static_include(cell))
    text = text.replace("needs.plan.outputs.mode == 'full'",
                        "true" if mode == "full" else "false")
    text = text.replace("submodules: ${{ needs.plan.outputs.mode == 'full' && matrix.full_build }}",
                        "submodules: false")
    if args.no_apt:
        text = text.replace(
            "          sudo apt-get update -qq\n"
            "          sudo apt-get install -y --no-install-recommends zstd unzip curl ninja-build cmake",
            '          echo "act fixture: base tools are already in the runner image"\n'
            '          command -v python3 curl sha256sum tar zstd unzip',
        )
        text = re.sub(
            r"      - name: Engine dependencies \(Linux, full mode\).*?"
            r"(?=      - name: Report the compiler under test)",
            "", text, flags=re.S,
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")
    print(f"wrote {args.output} (cell={cell['id']}, mode={mode})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
