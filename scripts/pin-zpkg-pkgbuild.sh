#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/pin-zpkg-pkgbuild.sh
#
# Pin packaging/arch/PKGBUILD to a commit that ALREADY EXISTS.
#
# A commit cannot contain its own SHA-1, and `git commit --amend` produces a
# different hash, so `_commit` (and the checksum of its tarball) can never be
# edited in the same commit they point at. The recipe is always TWO commits:
#
#   A  the code change (utils/zpkg/VERSION, packaging/keys/, sync, ...) pushed
#      to the remote, with `_commit` still holding the PREVIOUS value;
#   B  `scripts/pin-zpkg-pkgbuild.sh <A>` and commit ONLY the PKGBUILD.
#
# makepkg builds with B's PKGBUILD but extracts A's tarball, which contains all
# the code; B is metadata only. Never amend A after pinning it -- that changes
# both the hash and the checksum. Nothing in the tree requires _commit == HEAD;
# the only requirement is that the pinned commit contains the zpkg tree.
#
# Usage:
#   scripts/pin-zpkg-pkgbuild.sh [commit-or-ref]  # default HEAD; writes pin
#   scripts/pin-zpkg-pkgbuild.sh --check          # CI: refuse a self-pin
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
pkgbuild="$root/packaging/arch/PKGBUILD"
upstream="https://github.com/Evilpasture/project-zahlen"

have_git=0
if command -v git >/dev/null 2>&1 && [ -e "$root/.git" ]; then
    have_git=1
fi

if [ "${1:-}" = "--check" ]; then
    if [ "$have_git" -eq 0 ]; then
        echo "ok: no git checkout here; pin check skipped"
        exit 0
    fi
    commit=$(sed -n 's/^_commit=//p' "$pkgbuild" | head -1)
    head=$(git -C "$root" rev-parse HEAD)
    if [ "$commit" = "$head" ]; then
        echo "error: PKGBUILD _commit equals HEAD ($head)." >&2
        echo "       A commit cannot contain its own hash. Commit and push the code," >&2
        echo "       then run: scripts/pin-zpkg-pkgbuild.sh $head" >&2
        echo "       and commit the PKGBUILD change as a SECOND commit." >&2
        exit 1
    fi
    # A full clone can also prove the pinned tree carries the zpkg files. A
    # shallow CI checkout (fetch-depth 1) cannot see that older object; the
    # self-pin test above is still valid there, and the checksum recorded by
    # `pin-zpkg-pkgbuild.sh <commit>` is the authority for the tarball.
    if ! git -C "$root" cat-file -e "${commit}^{commit}" 2>/dev/null; then
        echo "ok: PKGBUILD pins $commit (HEAD $head); pinned object absent from this shallow clone"
        exit 0
    fi
    for required in utils/zpkg/VERSION packaging/keys/zahlen-release.pub; do
        if ! git -C "$root" cat-file -e "${commit}:${required}" 2>/dev/null; then
            echo "error: pinned commit $commit has no $required" >&2
            echo "       pin a commit that contains the zpkg CalVer and the pinned key" >&2
            exit 1
        fi
    done
    echo "ok: PKGBUILD pins $commit (HEAD $head), which contains the zpkg tree"
    exit 0
fi

if [ "$have_git" -eq 0 ]; then
    echo "error: need a git checkout to resolve the commit to pin" >&2
    exit 1
fi

target=${1:-HEAD}
if ! commit=$(git -C "$root" rev-parse --verify "${target}^{commit}" 2>/dev/null); then
    echo "error: cannot resolve '$target' to a commit" >&2
    exit 1
fi
for required in utils/zpkg/VERSION packaging/keys/zahlen-release.pub; do
    if ! git -C "$root" cat-file -e "${commit}:${required}" 2>/dev/null; then
        echo "error: $commit has no $required; commit (and push) the code first" >&2
        exit 1
    fi
done
if [ "$(grep -c '^sha256sums=' "$pkgbuild")" -ne 1 ]; then
    echo "error: expected exactly one single-line sha256sums= in $pkgbuild" >&2
    exit 1
fi

head=$(git -C "$root" rev-parse HEAD)
if [ "$commit" = "$head" ]; then
    echo "note: pinning to the current HEAD. The PKGBUILD edit MUST be a second" >&2
    echo "      commit; do not 'git commit --amend' it." >&2
fi

tarball=$(mktemp)
trap 'rm -f "$tarball"' EXIT
if ! curl -fsSL "${upstream}/archive/${commit}.tar.gz" -o "$tarball"; then
    echo "error: could not download ${upstream}/archive/${commit}.tar.gz" >&2
    echo "       push ${commit} first; GitHub can only archive commits it has." >&2
    exit 1
fi
sum=$(sha256sum "$tarball" | cut -d' ' -f1)
sed -i -E "s/^_commit=.*/_commit=${commit}/" "$pkgbuild"
sed -i -E "s/^sha256sums=\(.*/sha256sums=('${sum}')/" "$pkgbuild"

echo "pinned _commit=${commit}"
echo "sha256sums=('${sum}')"
echo
echo "Next: commit ONLY this PKGBUILD change. Do not amend ${commit:0:12}."
echo "      makepkg extracts ${commit:0:12}'s tarball (which has the code);"
echo "      this commit carries metadata only."
