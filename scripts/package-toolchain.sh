#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Package a built Release tree into zpkg-installable artifacts, plus the small
# extras a release needs (the manager binary, the consumer example, the release
# key). This is the publishing half of scripts/test-toolchain-install.sh: that
# script packages and then *installs and runs* the result as an unprivileged
# user, which is a test; this one only produces the release, and is what the
# release workflow publishes.
#
#   usage: scripts/package-toolchain.sh <out-dir> [build-dir]
#
# Layout it writes:
#   <out>/packages/  the 17 zahlen-<component>-<id>.tar.gz archives, SHA256SUMS,
#                    SHA256SUMS.ed25519, index.json(+.ed25519), the public key,
#                    toolchain.json  <- this directory is what `zpkg install`
#                    is pointed at, and it must contain nothing but packages:
#                    zpkg's indexer claims every zahlen-*.tar.gz it sees.
#   <out>/tools/     zpkg binary, cube-consumer-<id>.tar.gz, SHA256SUMS.tools.
#                    The consumer bundle is deliberately NOT named zahlen-*, so
#                    a user who flattens the release into one directory can
#                    still run `zpkg install .` without the bundle being parsed
#                    as a package.
#
# Signing key: set ZHLN_RELEASE_KEY_PEM to a base64-encoded PKCS#8 ed25519
# private key (GitHub secret) to sign with the project key. Without it an
# ephemeral key is generated so forks can still produce a consistent release;
# the fingerprint is printed either way, and users who care pin it.
#
# No output is discarded here. The CI log is the only place a failure explains
# itself, and nobody reads it until the run is already red -- by which point a
# swallowed error is unrecoverable. The only redirects allowed are existence
# probes (command -v, dpkg -s, grep -q), which have no output worth keeping.
# If something is too noisy to print, make it quieter; do not hide it.
set -euo pipefail

out=${1:?usage: scripts/package-toolchain.sh <out-dir> [build-dir]}
build=${2:-build}

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "${root}"

metadata="${build}/toolchain/toolchain.json"
[ -f "${metadata}" ] || {
    echo "error: ${metadata} is missing; configure and build the tree first" >&2
    exit 1
}
# Read the id back from CMake instead of reconstructing it: it is derived from
# the ABI hash, and a shell copy of that derivation is exactly how artifact
# names drift from their contents.
id=$(sed -n 's/.*"id":"\([^"]*\)".*/\1/p' "${metadata}")
test -n "${id}" || {
    echo "error: ${metadata} has no id" >&2
    exit 1
}

pkgs="${out}/packages"
tools="${out}/tools"
rm -rf "${pkgs}" "${tools}"
mkdir -p "${pkgs}" "${tools}"

# One package per install component. The list is the set of components that
# carry an installed export; a component that does not exist is a hard error,
# because a silently missing package is a release that cannot install.
components=(Engine CommandLine Signal HostRunner Application UISchema Serialization GLTF
    Screenshot Netpbm FreeCam Camera CharacterController SkeletalAnimation Animation Cooker Fonts)

# ---------------------------------------------------------------------------
# Debug information does not ship in the packages.
#
# The release profile compiles with -g, and DWARF is most of an archive:
# libzahlen_engine.so measures 195 MB unstripped and 13 MB with the debug
# sections removed, so the engine package alone was 93.5 MB of the toolchain's
# 117.7 MB -- 79.5% of a download spent on debug info for a build whose sources
# the consumer does not have. The sections are not thrown away: they are
# collected into tools/symbols-<id>.tar.gz, and every shipped object carries a
# .gnu_debuglink to its entry, so a stack trace can still be symbolized with
#     gdb -ex 'set debug-file-directory <prefix>/usr/lib/debug'
#
# ZHLN_SHIP_DEBUG=1 keeps the old layout (debug inside the package) for anyone
# who prefers one archive to two.
# ---------------------------------------------------------------------------
debugstage=$(mktemp -d)
symbols="${tools}/symbols-${id}.tar.gz"

is_elf() {
    [ "$(head -c 4 "$1" | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]
}

strip_debug() { # $1 = staged install tree
    local stage=$1 file relative
    while IFS= read -r -d '' file; do
        is_elf "${file}" || continue
        relative=${file#"${stage}"}
        mkdir -p "$(dirname "${debugstage}/usr/lib/debug${relative}")"
        # Idempotent: a second pass over an already-stripped tree must not be a
        # hard failure, and objcopy refuses a debuglink that is already there.
        objcopy --remove-section=.gnu_debuglink "${file}" 2>/dev/null || true
        objcopy --only-keep-debug "${file}" "${debugstage}/usr/lib/debug${relative}"
        strip --strip-debug "${file}"
        objcopy --add-gnu-debuglink="${debugstage}/usr/lib/debug${relative}" "${file}"
    done < <(find "${stage}" -type f -print0)
}

printf 'toolchain: %s\npackaging %d components into %s\n' "${id}" "${#components[@]}" "${pkgs}"
for component in "${components[@]}"; do
    stage=$(mktemp -d)
    DESTDIR="${stage}" cmake --install "${build}" --prefix /usr --component "Zahlen${component}"
    if [ "${ZHLN_SHIP_DEBUG:-0}" != "1" ]; then
        strip_debug "${stage}"
    fi
    archive="${pkgs}/zahlen-${component,,}-${id}.tar.gz"
    tar -C "${stage}" -czf "${archive}" usr
    # Every archive must identify its toolchain wherever it ends up on disk.
    # grep reads every line on purpose: with `set -o pipefail`, `grep -q` exits as
    # soon as it matches, tar takes SIGPIPE and the pipeline reports 141, so a
    # perfectly good archive is rejected as missing its toolchain.json (measured
    # on zahlen-cooker: pipeline status 141 with -q, 0 without).
    tar -tzf "${archive}" | grep -F 'usr/share/zahlen/toolchain.json' >/dev/null || {
        echo "error: ${archive} has no usr/share/zahlen/toolchain.json" >&2
        rm -rf "${stage}"
        exit 1
    }
    printf '  %-48s %8s\n' "$(basename "${archive}")" "$(du -h "${archive}" | cut -f1)"
    rm -rf "${stage}"
done
(cd "${pkgs}" && sha256sum zahlen-*.tar.gz >SHA256SUMS)

# The symbol bundle is a debugging aid, not a package: it is not indexed, it has
# no manifest, and it lives in tools/ so `zpkg install <release-dir>` never sees
# it. It is only produced when the packages were stripped.
if [ -d "${debugstage}/usr/lib/debug" ]; then
    tar -C "${debugstage}" -czf "${symbols}" usr
    printf 'debug symbols: %s (%s)\n' "$(basename "${symbols}")" "$(du -h "${symbols}" | cut -f1)"
fi
rm -rf "${debugstage}"

# ---------------------------------------------------------------------------
# Release key. zpkg's signature files are "<fingerprint> <base64 signature>",
# and the fingerprint is the first 32 hex characters of SHA-256 over the raw
# 32-byte public key, so it can be produced here without a store or a zpkg run.
# ---------------------------------------------------------------------------
keyfile=$(mktemp)
trap 'rm -f "${keyfile}"' EXIT
if [ -n "${ZHLN_RELEASE_KEY_PEM:-}" ]; then
    printf '%s' "${ZHLN_RELEASE_KEY_PEM}" | base64 -d >"${keyfile}"
    printf 'signing with the project key from ZHLN_RELEASE_KEY_PEM\n'
else
    openssl genpkey -algorithm ed25519 -out "${keyfile}"
    printf 'warning: no ZHLN_RELEASE_KEY_PEM; signing with a throwaway key.\n' >&2
    printf '         Users of this release must re-add the published key.\n' >&2
fi

pubfile="${pkgs}/zahlen-release-key.pub"
openssl pkey -in "${keyfile}" -pubout -outform DER | tail -c 32 >"${tools}/release-key.raw"
base64 -w0 "${tools}/release-key.raw" >"${pubfile}"
printf '\n' >>"${pubfile}"
fingerprint=$(sha256sum "${tools}/release-key.raw" | cut -c1-32)
rm -f "${tools}/release-key.raw"
printf '%s\n' "${fingerprint}" >"${tools}/KEY-FINGERPRINT.txt"
printf 'release key fingerprint: %s\n' "${fingerprint}"

sign_file() {
    openssl pkeyutl -sign -inkey "${keyfile}" -rawin -in "$1" -out "$1.sig"
    {
        printf '%s ' "${fingerprint}"
        base64 -w0 "$1.sig"
        printf '\n'
    } >"$1.ed25519"
    rm -f "$1.sig"
}

sign_file "${pkgs}/SHA256SUMS"
# The index names every package with its size and digest; it is what a remote
# install resolves against, so it is signed like the sums are.
zpkg_bin=${ZPKG_BIN:-build-zpkg/zpkg}
"${zpkg_bin}" index "${pkgs}" --out "${pkgs}/index.json"
sign_file "${pkgs}/index.json"
cp "${metadata}" "${pkgs}/toolchain.json"

# ---------------------------------------------------------------------------
# Extras: the manager itself and the consumer project the release is tested
# with, so a user does not need this checkout to use the toolchain.
# ---------------------------------------------------------------------------
[ -x "${zpkg_bin}" ] || {
    echo "error: ${zpkg_bin} is missing; build utils/zpkg first (or set ZPKG_BIN)" >&2
    exit 1
}
install -m755 "${zpkg_bin}" "${tools}/zpkg-linux-x86_64"
tar -C tests/installed -czf "${tools}/cube-consumer-${id}.tar.gz" .
# Named differently from the packages' SHA256SUMS on purpose: a release is a
# flat directory once downloaded, and two files with that name would collide.
(cd "${tools}" && sha256sum zpkg-linux-x86_64 "cube-consumer-${id}.tar.gz" $( [ -f "${symbols}" ] && basename "${symbols}" ) >SHA256SUMS.tools)

printf '\nrelease contents:\n'
(cd "${out}" && find . -maxdepth 2 -type f -printf '  %-46p %10s bytes\n' | sort)
printf '\ninstall with:\n  zpkg key add %s/zahlen-release-key.pub\n  zpkg install %s --require-checksums --require-signatures\n' \
    "${pkgs}" "${pkgs}"
