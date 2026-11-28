#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/check-release-consistency.sh
#
# Refuse a release whose artifacts, notes, pinned key, keys used for signing,
# zpkg CalVer and PKGBUILD disagree with each other. Run it twice: once on the
# freshly built dist/ (before publishing) and once on a download of the
# published release (after publishing), because a re-upload can replace every
# asset while leaving the release body -- including its fingerprint line --
# untouched. That is exactly how v0.2.0, v0.3.0, v0.1.0-baseline, v0.5.0 and
# v0.6.0 ended up advertising the wrong fingerprint.
#
# Usage:
#   scripts/check-release-consistency.sh \
#       --assets dist/packages --tools dist/tools \
#       --notes dist/release-notes.md --asset-list dist/asset-names.txt \
#       --versions --date 2026.11.25 --zpkg dist/tools/zpkg-linux-x86_64
#
# --assets and --tools may be the same flat directory (what `gh release
# download` produces). Every check runs and all failures are reported together.
set -uo pipefail

usage() {
    cat <<'EOF'
usage: check-release-consistency.sh --assets <dir> [--tools <dir>] [--notes <file>]
           [--asset-list <file>] [--versions] [--date YYYY.MM.DD]
           [--zpkg <binary>] [--repo <root>]
EOF
}

assets=""
tools=""
notes=""
asset_list=""
zpkg_bin=""
expected_date=""
repo=""
check_versions=0

while [ $# -gt 0 ]; do
    case "$1" in
        --assets)     assets=${2:?--assets needs a directory}; shift 2 ;;
        --tools)      tools=${2:?--tools needs a directory}; shift 2 ;;
        --notes)      notes=${2:?--notes needs a file}; shift 2 ;;
        --asset-list) asset_list=${2:?--asset-list needs a file}; shift 2 ;;
        --zpkg)       zpkg_bin=${2:?--zpkg needs a binary}; shift 2 ;;
        --date)       expected_date=${2:?--date needs YYYY.MM.DD}; shift 2 ;;
        --repo)       repo=${2:?--repo needs a directory}; shift 2 ;;
        --versions)   check_versions=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *) printf 'error: unknown option %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done

if [ -z "$assets" ]; then
    printf 'error: --assets is required\n' >&2
    usage >&2
    exit 2
fi
tools=${tools:-$assets}
repo=${repo:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}

problems=()
fail() { problems+=("$1"); }
pass() { printf 'ok   %s\n' "$1"; }
have() { [ -f "$1" ]; }

# -- the key is the anchor: everything else is derived from its bytes ---------
pub="$assets/zahlen-release-key.pub"
raw=$(mktemp)
trap 'rm -f "$raw"' EXIT
computed=""
if ! have "$pub" || ! base64 -d "$pub" >"$raw" 2>/dev/null; then
    fail "cannot read the published public key at $pub"
elif [ "$(stat -c %s "$raw")" -ne 32 ]; then
    fail "zahlen-release-key.pub is not a raw 32-byte ed25519 public key"
else
    computed=$(sha256sum "$raw" | cut -c1-32)
    pass "published key fingerprint $computed"
fi

if [ -n "$computed" ]; then
    # KEY-FINGERPRINT.txt ships next to the tools, not the packages.
    fpmeta="$tools/KEY-FINGERPRINT.txt"
    if ! have "$fpmeta"; then
        fail "missing $fpmeta"
    elif [ "$(tr -d '[:space:]' <"$fpmeta")" != "$computed" ]; then
        fail "KEY-FINGERPRINT.txt says $(tr -d '[:space:]' <"$fpmeta"), the published key says $computed"
    else
        pass "KEY-FINGERPRINT.txt matches the published key"
    fi

    # The checked-in key is the out-of-band trust anchor. A release signed by
    # anything else is a rotation nobody announced.
    pinned="$repo/packaging/keys/zahlen-release.pub"
    pinned_fp="$repo/packaging/keys/FINGERPRINT"
    if ! have "$pinned" || ! have "$pinned_fp"; then
        fail "missing packaging/keys/zahlen-release.pub or FINGERPRINT (run scripts/bootstrap-release-key.sh)"
    else
        if [ "$(tr -d '[:space:]' <"$pinned")" != "$(tr -d '[:space:]' <"$pub")" ]; then
            fail "published key differs from the pinned project key in packaging/keys/"
        else
            pass "published key equals the pinned project key"
        fi
        if [ "$(tr -d '[:space:]' <"$pinned_fp")" != "$computed" ]; then
            fail "packaging/keys/FINGERPRINT says $(tr -d '[:space:]' <"$pinned_fp"), expected $computed"
        else
            pass "pinned fingerprint matches the published key"
        fi
    fi

    # Signature files are one line per signer. The pinned key must be one of
    # them; extra lines are allowed only for a deliberate, dual-signed rotation.
    for signed in SHA256SUMS index.json; do
        sigfile="$assets/$signed.ed25519"
        if ! have "$sigfile"; then
            fail "missing $sigfile"
            continue
        fi
        signers=$(awk '{print $1}' "$sigfile")
        if ! grep -Fxq "$computed" <<<"$signers"; then
            fail "$signed.ed25519 has no signature by the published key ($computed): $(tr '\n' ' ' <<<"$signers")"
        else
            pass "$signed.ed25519 is signed by the published key"
        fi
        while read -r fp b64; do
            [ -z "$fp" ] && continue
            if [ "$(printf '%s' "$b64" | base64 -d 2>/dev/null | wc -c)" -ne 64 ]; then
                fail "$signed.ed25519: signer $fp does not carry a 64-byte signature"
            fi
        done <"$sigfile"
    done
fi

# -- checksums must cover exactly the archives that ship ---------------------
if have "$assets/SHA256SUMS"; then
    if ! ( cd "$assets" && sha256sum -c SHA256SUMS --quiet >/dev/null 2>&1 ); then
        fail "SHA256SUMS does not verify against the archives in $assets"
    else
        pass "SHA256SUMS verifies every listed archive"
    fi
    listed=$(awk '{print $2}' "$assets/SHA256SUMS" | sort)
    present=$(cd "$assets" && printf '%s\n' zahlen-*.tar.gz 2>/dev/null | sort)
    if [ "$listed" != "$present" ]; then
        fail "SHA256SUMS and the zahlen-*.tar.gz files in $assets are not the same set"
    else
        pass "SHA256SUMS covers exactly the published packages"
    fi
else
    fail "missing $assets/SHA256SUMS"
fi

if have "$tools/SHA256SUMS.tools"; then
    if ! ( cd "$tools" && sha256sum -c SHA256SUMS.tools --quiet >/dev/null 2>&1 ); then
        fail "SHA256SUMS.tools does not verify against the tools in $tools"
    else
        pass "SHA256SUMS.tools verifies every listed tool"
    fi
    if ! grep -Eq '(^|[[:space:]])zpkg-linux-x86_64$' "$tools/SHA256SUMS.tools"; then
        fail "SHA256SUMS.tools does not list zpkg-linux-x86_64"
    fi
else
    fail "missing $tools/SHA256SUMS.tools"
fi

# -- the example surfaces are gone; nothing may reintroduce them -------------
if compgen -G "$assets/cube-consumer-*.tar.gz" >/dev/null ||
   compgen -G "$tools/cube-consumer-*.tar.gz" >/dev/null ||
   compgen -G "$assets/zahlen-cube-*.png" >/dev/null ||
   compgen -G "$tools/zahlen-cube-*.png" >/dev/null; then
    fail "a consumer example or rendered frame is still published; examples are documentation, not release assets"
else
    pass "no example project or rendered frame is published"
fi

# -- index and toolchain descriptor must describe one toolchain -------------
if have "$assets/index.json" && have "$assets/toolchain.json"; then
    if python3 - "$assets/index.json" "$assets/toolchain.json" <<'PY'
import json, sys
index = json.load(open(sys.argv[1], encoding="utf-8"))
descriptor = json.load(open(sys.argv[2], encoding="utf-8"))
ids = [entry["id"] for entry in index["toolchains"]]
assert descriptor["id"] in ids, (
    f"toolchain.json names {descriptor['id']}, index.json publishes {ids}")
PY
    then
        pass "index.json and toolchain.json name the same toolchain"
    else
        fail "index.json and toolchain.json disagree about the toolchain id"
    fi
else
    fail "missing index.json or toolchain.json in $assets"
fi

# -- release notes must not advertise a stale key or a deleted example -------
if [ -n "$notes" ]; then
    if ! have "$notes"; then
        fail "notes file $notes does not exist"
    else
        if [ -n "$computed" ] && ! grep -Fq "* release key fingerprint: \`$computed\`" "$notes"; then
            fail "release notes do not advertise the published fingerprint $computed (stale body after a re-upload?)"
        else
            pass "release notes advertise the published fingerprint"
        fi
        if grep -Fq 'cube-consumer' "$notes" || grep -Fq '## Build against it' "$notes"; then
            fail "release notes still carry a consumer example; link docs at the tag instead"
        else
            pass "release notes carry no duplicated example snippets"
        fi
        if ! grep -Fq 'zpkg sync' "$notes"; then
            fail "release notes do not mention 'zpkg sync' for upgrades"
        fi
    fi
fi

# -- the published asset list must be one generation ------------------------
if [ -n "$asset_list" ]; then
    if ! have "$asset_list"; then
        fail "asset list $asset_list does not exist"
    else
        for required in index.json index.json.ed25519 SHA256SUMS SHA256SUMS.ed25519 \
                        zahlen-release-key.pub KEY-FINGERPRINT.txt SHA256SUMS.tools \
                        zpkg-linux-x86_64 toolchain.json; do
            grep -Fxq "$required" "$asset_list" || fail "published release is missing $required"
        done
        if grep -E '^(cube-consumer-|zahlen-cube-)' "$asset_list" >/dev/null; then
            fail "published release still carries example/render assets from an earlier generation"
        else
            pass "published asset list is one generation"
        fi
        # A re-upload from a different commit leaves two sets of packages.
        count=$(grep -c '^zahlen-engine-.*\.tar\.gz$' "$asset_list")
        [ "$count" -eq 1 ] || fail "published release carries $count zahlen-engine archives; expected exactly one"
    fi
fi

if [ "$check_versions" -eq 1 ]; then
    calver=""
    zpkg_cmake="$repo/utils/zpkg/CMakeLists.txt"
    pkgbuild="$repo/packaging/arch/PKGBUILD"
    if ! have "$zpkg_cmake"; then
        fail "missing $zpkg_cmake"
    else
        project_pattern='^[[:space:]]*project\(zpkg VERSION [0-9]+\.[0-9]+\.[0-9]+ LANGUAGES CXX\)[[:space:]]*$'
        project_count=$(grep -Ec "$project_pattern" "$zpkg_cmake" || true)
        if [ "$project_count" -ne 1 ]; then
            fail "expected exactly one 'project(zpkg VERSION ... LANGUAGES CXX)' in $zpkg_cmake"
            calver=""
        else
            calver=$(sed -nE 's/^[[:space:]]*project\(zpkg VERSION ([0-9]+\.[0-9]+\.[0-9]+) LANGUAGES CXX\)[[:space:]]*$/\1/p' "$zpkg_cmake")
        fi
        if [[ ! $calver =~ ^[0-9]{4}\.[0-9]{2}\.[0-9]{2}$ ]]; then
            fail "zpkg CMake version is '$calver', expected CalVer YYYY.MM.DD"
        else
            pass "zpkg CMake version is CalVer $calver"
        fi
        pkgver=$(sed -n 's/^pkgver=//p' "$pkgbuild" | head -1 | tr -d "'\"")
        if [ "$pkgver" != "$calver" ]; then
            fail "PKGBUILD pkgver=$pkgver but zpkg CMake version=$calver"
        else
            pass "PKGBUILD pkgver matches zpkg's CalVer"
        fi
        if [ -n "$expected_date" ] && [ "$calver" != "$expected_date" ]; then
            fail "zpkg CalVer $calver is not the release date $expected_date"
        else
            [ -n "$expected_date" ] && pass "zpkg CalVer matches the release date"
        fi
        # A self-referential _commit is impossible to satisfy: a commit cannot
        # contain its own hash and --amend just mints a new one.
        pin="$repo/scripts/pin-zpkg-pkgbuild.sh"
        if [ -x "$pin" ]; then
            if out=$("$pin" --check 2>&1); then
                pass "${out#ok: }"
            else
                fail "PKGBUILD pin: $out"
            fi
        fi
        if [ -n "$zpkg_bin" ]; then
            if ! report=$("$zpkg_bin" --version 2>/dev/null); then
                fail "cannot run $zpkg_bin --version"
            elif [[ $report != "zpkg $calver "* ]]; then
                fail "the shipped zpkg reports '$report', expected 'zpkg $calver ...'"
            else
                pass "shipped zpkg reports $report"
            fi
            # Exercise the real verification path with only the pinned key.
            # The key is baked into zpkg, so a fresh store verifies with no
            # `key add`; an explicit `key add` of the same key still works.
            store=$(mktemp -d)
            if ZHLN_ROOT="$store" "$zpkg_bin" verify "$assets" \
                    --require-checksums --require-signatures >/dev/null 2>&1; then
                pass "built-in pinned key verifies every published archive through zpkg itself"
            else
                fail "zpkg verify refused $assets with only the built-in pinned key trusted"
            fi
            if ZHLN_ROOT="$store" "$zpkg_bin" key add \
                    "$repo/packaging/keys/zahlen-release.pub" --name "release check" >/dev/null 2>&1 &&
               ZHLN_ROOT="$store" "$zpkg_bin" verify "$assets" \
                    --require-checksums --require-signatures >/dev/null 2>&1; then
                pass "pinned key verifies every published archive through zpkg itself"
            else
                fail "zpkg verify refused $assets with only the pinned key trusted"
            fi
            rm -rf "$store"
        fi
    fi
fi

if [ "${#problems[@]}" -ne 0 ]; then
    for problem in "${problems[@]}"; do
        printf 'STALE: %s\n' "$problem" >&2
    done
    printf '\n%d consistency check(s) failed\n' "${#problems[@]}" >&2
    exit 1
fi
printf '\nrelease is internally consistent\n'
