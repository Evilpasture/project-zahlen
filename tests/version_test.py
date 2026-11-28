#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check release version metadata.

Usage:
    tests/version_test.py                 # report and check
    tests/version_test.py --check         # exit 1 on any disagreement
    tests/version_test.py --check --tag v0.4.0
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CMAKE = ROOT / "CMakeLists.txt"
PKGBUILD = ROOT / "packaging" / "arch" / "PKGBUILD"
PACKAGING_README = ROOT / "packaging" / "README.md"
ZPKG_REMOTE = ROOT / "utils" / "zpkg" / "src" / "remote.cpp"
ZPKG_CMAKE = ROOT / "utils" / "zpkg" / "CMakeLists.txt"

CALVER_RE = re.compile(r"^\d{4}\.\d{2}\.\d{2}$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")


def project_version() -> str:
    for line in CMAKE.read_text(encoding="utf-8").splitlines():
        match = re.match(
            r"^[ \t]*project\(zahlen[ \t]+VERSION[ \t]+(\d+\.\d+\.\d+)", line
        )
        if match:
            return match.group(1)
    raise SystemExit(f"{CMAKE}: no 'project(zahlen VERSION x.y.z' line")


def zpkg_project_version() -> str:
    matches = re.findall(
        r"^[ \t]*project\(zpkg VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)[ \t]*$",
        ZPKG_CMAKE.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    if len(matches) != 1:
        raise SystemExit(
            f"{ZPKG_CMAKE}: expected exactly one "
            "'project(zpkg VERSION x.y.z LANGUAGES CXX)' line"
        )
    return matches[0]


def pkgbuild_fields() -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in PKGBUILD.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^(_?[a-z_]+)=(.*)$", line.strip())
        if m:
            fields[m.group(1)] = m.group(2).strip().strip("'\"")
    return fields


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--check", action="store_true", help="exit 1 on any disagreement"
    )
    parser.add_argument(
        "--tag", default="", help="release tag that must match the version"
    )
    args = parser.parse_args()

    expected = project_version()
    problems: list[str] = []
    notes: list[str] = [f"CMakeLists.txt PROJECT_VERSION   {expected}"]

    # --- PKGBUILD -----------------------------------------------------------
    fields = pkgbuild_fields()
    pkgver = fields.get("pkgver", "")
    commit = fields.get("_commit", "")
    notes.append(f"packaging/arch/PKGBUILD pkgver    {pkgver or '(missing)'}")
    notes.append(f"packaging/arch/PKGBUILD _commit    {commit or '(missing)'}")
    calver = zpkg_project_version()
    notes.append(f"utils/zpkg/CMakeLists.txt CalVer   {calver}")
    if not CALVER_RE.match(calver):
        problems.append(
            f"utils/zpkg/CMakeLists.txt version is {calver!r}, "
            "expected CalVer YYYY.MM.DD"
        )
    if pkgver != calver:
        problems.append(
            f"packaging/arch/PKGBUILD pkgver={pkgver or '(missing)'}, "
            f"expected zpkg CMake version {calver}"
        )
    if pkgver == expected:
        problems.append(
            "PKGBUILD pkgver still tracks the engine semver; zpkg must use its "
            "own CalVer from utils/zpkg/CMakeLists.txt"
        )
    if not COMMIT_RE.match(commit):
        problems.append(
            "packaging/arch/PKGBUILD _commit must be the full 40-character hash "
            "the tarball is pinned to (archive/<sha>.tar.gz)"
        )
    # A commit cannot contain its own hash, and --amend mints a new one: the
    # pin must reference an EARLIER commit. The metadata edit is always a
    # second commit. Skipped when building from a source tarball (no .git).
    if (ROOT / ".git").exists() and shutil.which("git"):
        pinned = subprocess.run(
            ["bash", str(ROOT / "scripts" / "pin-zpkg-pkgbuild.sh"), "--check"],
            capture_output=True, text=True)
        if pinned.returncode != 0:
            problems.append((pinned.stderr or pinned.stdout).strip())
        else:
            notes.append(pinned.stdout.strip())

    # --- the tag, when asked ------------------------------------------------
    if args.tag:
        notes.append(f"release tag                     {args.tag}")
        if args.tag != f"v{expected}":
            problems.append(f"release tag {args.tag} does not match v{expected}")

    for note in notes:
        print(note)
    for problem in problems:
        print(f"error: {problem}", file=sys.stderr)
    if problems and args.check:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
