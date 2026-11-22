#!/usr/bin/env python3
# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail when a version number is written in two places.

`PROJECT_VERSION` in the top-level CMakeLists.txt is the only hand-written
version in the tree. Everything else reads it:

  * include/Zahlen/Version.hpp   generated from it by configure_file()
  * utils/zpkg                   reads the project() line, so zpkg reports it
  * .github/workflows/release.yml takes the release tag from it
                                 (scripts/release-tag.sh)

Three numbers are still literals, because a packager or a publisher reads them
without running CMake: packaging/arch/PKGBUILD's pkgver (which its _commit must
carry as the full hash), the example toolchain id in packaging/README.md, and
zpkg's invalid-tag error message. Those are checked here, so a bump that forgets
one fails at configure time instead of publishing two versions of one release.

Usage:
    tests/version_test.py                 # report and check
    tests/version_test.py --check         # exit 1 on any disagreement
    tests/version_test.py --check --tag v0.4.0
                                          # also require the tag to match
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CMAKE = ROOT / "CMakeLists.txt"
PKGBUILD = ROOT / "packaging" / "arch" / "PKGBUILD"
PACKAGING_README = ROOT / "packaging" / "README.md"
ZPKG_REMOTE = ROOT / "utils" / "zpkg" / "src" / "remote.cpp"
ZPKG_CMAKE = ROOT / "utils" / "zpkg" / "CMakeLists.txt"

VERSION_RE = re.compile(r"\b\d+\.\d+\.\d+\b")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")


def project_version() -> str:
    for line in CMAKE.read_text(encoding="utf-8").splitlines():
        match = re.match(
            r"^[ \t]*project\(zahlen[ \t]+VERSION[ \t]+(\d+\.\d+\.\d+)", line
        )
        if match:
            return match.group(1)
    raise SystemExit(f"{CMAKE}: no 'project(zahlen VERSION x.y.z' line")


def pkgbuild_fields() -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in PKGBUILD.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^(_?[a-z_]+)=(.*)$", line.strip())
        if m:
            fields[m.group(1)] = m.group(2).strip().strip("'\"")
    return fields


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true", help="exit 1 on any disagreement")
    parser.add_argument("--tag", default="", help="release tag that must match the version")
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
    if pkgver != expected:
        problems.append(f"packaging/arch/PKGBUILD pkgver={pkgver or '(missing)'}, expected pkgver={expected}")
    if not COMMIT_RE.match(commit):
        problems.append(
            "packaging/arch/PKGBUILD _commit must be the full 40-character hash "
            "the tarball is pinned to (archive/<sha>.tar.gz)"
        )

    # --- docs and messages that carry a version as an example ---------------
    readme = PACKAGING_README.read_text(encoding="utf-8")
    example = f"{expected}-7c6e062-gcc-16.2.1-x86_64-release"
    if example not in readme:
        found = sorted(set(VERSION_RE.findall(readme)))
        problems.append(
            f"packaging/README.md toolchain-id example is {found}, expected {expected!r} "
            f"(id '{example}')"
        )

    remote = ZPKG_REMOTE.read_text(encoding="utf-8")
    if f"such as v{expected}" not in remote:
        problems.append(
            f'utils/zpkg/src/remote.cpp should name the current release in its '
            f'invalid-tag message: "use a release name such as v{expected}"'
        )

    # --- nothing re-introduced a literal of its own -------------------------
    zpkg_cmake = ZPKG_CMAKE.read_text(encoding="utf-8")
    if re.search(r"project\(zpkg[^)]*VERSION[ \t]+\d", zpkg_cmake):
        problems.append(
            "utils/zpkg/CMakeLists.txt names a version in project(); it must take "
            "the engine's PROJECT_VERSION"
        )

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
