#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/release-tag.sh -- the release tag, derived from PROJECT_VERSION.
#
# CMakeLists.txt is the only place the Zahlen version is written by hand. A
# release tag is that version with a `v` in front, and nothing else: v0.4.0 is
# engine 0.4.0, the toolchain id starts `0.4.0-`, `zpkg --version` says 0.4.0 and
# the installed package configs claim 0.4.0. There is no separate tag thought to
# be had by hand at publish time.
#
# Usage:
#   scripts/release-tag.sh                # print v<PROJECT_VERSION>
#   scripts/release-tag.sh --print-version  # print <PROJECT_VERSION>
#   scripts/release-tag.sh --git-describe   # what git would call HEAD, by
#                                         # comparison (see .github/workflows/
#                                         # release.yml); never used to publish
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cmake_file="${root}/CMakeLists.txt"

if [ ! -f "${cmake_file}" ]; then
    echo "error: ${cmake_file} not found; run this from a Zahlen checkout." >&2
    exit 1
fi

# First `project(zahlen VERSION x.y.z` line. A tarball of the tree has the same
# line, so this works without git -- which matters, because the release job that
# needs it runs before any submodule is initialised.
project_line=$(grep -m1 -E '^[[:space:]]*project\(zahlen[[:space:]]+VERSION[[:space:]]+[0-9]+\.[0-9]+\.[0-9]+' "${cmake_file}" || true)
if [ -z "${project_line}" ]; then
    echo "error: no 'project(zahlen VERSION x.y.z' line in ${cmake_file}." >&2
    echo "       PROJECT_VERSION is the single source of the version; put it back." >&2
    exit 1
fi

version=$(printf '%s\n' "${project_line}" | sed -E 's/.*VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/')

case "${1:-}" in
    "")            printf 'v%s\n' "${version}" ;;
    --print-version) printf '%s\n' "${version}" ;;
    --git-describe)
        # Not a source of truth; here so CI can report the disagreement when a
        # push is tagged differently from the tree it points at.
        git -C "${root}" describe --tags --abbrev=0 2>/dev/null || true
        ;;
    *)
        echo "usage: $(basename "$0") [--print-version|--git-describe]" >&2
        exit 2
        ;;
esac
