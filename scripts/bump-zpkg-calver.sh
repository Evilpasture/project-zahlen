#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/bump-zpkg-calver.sh
#
# zpkg uses CalVer (YYYY.MM.DD), independent of the engine's semver. The date
# is the *release* date, not the version of the engine packages: a manager
# update and an engine toolchain update are different events and must not share
# a number. utils/zpkg/VERSION is the single source of truth; the PKGBUILD's
# static pkgver is a copy that this script keeps in sync, and makepkg's
# pkgver() re-reads VERSION from the source tarball.
#
#   scripts/bump-zpkg-calver.sh                    # set both to today (UTC)
#   scripts/bump-zpkg-calver.sh --date 2026.12.01 # set both explicitly
#   scripts/bump-zpkg-calver.sh --check --date 2026.11.25
#                                                 # CI: fail if stale
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
version_file="$root/utils/zpkg/VERSION"
pkgbuild="$root/packaging/arch/PKGBUILD"
requested=""
check=0

while [ $# -gt 0 ]; do
    case "$1" in
        --date)  requested=${2:?--date needs YYYY.MM.DD}; shift 2 ;;
        --check) check=1; shift ;;
        -h|--help)
            echo "usage: scripts/bump-zpkg-calver.sh [--date YYYY.MM.DD] [--check]"
            exit 0 ;;
        *) echo "error: unknown option $1" >&2; exit 2 ;;
    esac
done

target=${requested:-$(date -u +%Y.%m.%d)}
if [[ ! $target =~ ^[0-9]{4}\.[0-9]{2}\.[0-9]{2}$ ]]; then
    echo "error: '$target' is not CalVer YYYY.MM.DD" >&2
    exit 2
fi

current=$(tr -d '[:space:]' <"$version_file")
pkgver=$(sed -n 's/^pkgver=//p' "$pkgbuild" | head -1 | tr -d "'\"")

if [ "$check" -eq 1 ]; then
    if [ "$current" != "$target" ]; then
        echo "error: zpkg CalVer is $current, release date is $target." >&2
        echo "       run: scripts/bump-zpkg-calver.sh --date $target" >&2
        exit 1
    fi
    if [ "$pkgver" != "$current" ]; then
        echo "error: PKGBUILD pkgver=$pkgver but utils/zpkg/VERSION=$current" >&2
        exit 1
    fi
    echo "ok: zpkg CalVer $current matches the release date and the PKGBUILD"
    exit 0
fi

printf '%s\n' "$target" >"$version_file"
sed -i "s/^pkgver=.*/pkgver=${target}/" "$pkgbuild"
echo "zpkg CalVer: $current -> $target (utils/zpkg/VERSION, packaging/arch/PKGBUILD)"
