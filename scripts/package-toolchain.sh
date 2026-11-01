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
#   <out>/packages/  the 16 zahlen-<component>-<id>.tar.gz archives, SHA256SUMS,
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
    Screenshot Netpbm FreeCam Camera CharacterController SkeletalAnimation Animation Cooker)

printf 'toolchain: %s\npackaging %d components into %s\n' "${id}" "${#components[@]}" "${pkgs}"
for component in "${components[@]}"; do
    stage=$(mktemp -d)
    DESTDIR="${stage}" cmake --install "${build}" --prefix /usr --component "Zahlen${component}" >/dev/null
    archive="${pkgs}/zahlen-${component,,}-${id}.tar.gz"
    tar -C "${stage}" -czf "${archive}" usr
    # Every archive must identify its toolchain wherever it ends up on disk.
    tar -tzf "${archive}" | grep -q 'usr/share/zahlen/toolchain.json' || {
        echo "error: ${archive} has no usr/share/zahlen/toolchain.json" >&2
        rm -rf "${stage}"
        exit 1
    }
    rm -rf "${stage}"
done
(cd "${pkgs}" && sha256sum zahlen-*.tar.gz >SHA256SUMS)

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
    openssl genpkey -algorithm ed25519 -out "${keyfile}" 2>/dev/null
    printf 'warning: no ZHLN_RELEASE_KEY_PEM; signing with a throwaway key.\n' >&2
    printf '         Users of this release must re-add the published key.\n' >&2
fi

pubfile="${pkgs}/zahlen-release-key.pub"
openssl pkey -in "${keyfile}" -pubout -outform DER 2>/dev/null | tail -c 32 >"${tools}/release-key.raw"
base64 -w0 "${tools}/release-key.raw" >"${pubfile}"
printf '\n' >>"${pubfile}"
fingerprint=$(sha256sum "${tools}/release-key.raw" | cut -c1-32)
rm -f "${tools}/release-key.raw"
printf '%s\n' "${fingerprint}" >"${tools}/KEY-FINGERPRINT.txt"
printf 'release key fingerprint: %s\n' "${fingerprint}"

sign_file() {
    openssl pkeyutl -sign -inkey "${keyfile}" -rawin -in "$1" -out "$1.sig" 2>/dev/null
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
(cd "${tools}" && sha256sum zpkg-linux-x86_64 "cube-consumer-${id}.tar.gz" >SHA256SUMS.tools)

printf '\nrelease contents:\n'
(cd "${out}" && find . -maxdepth 2 -type f -printf '  %-46p %10s bytes\n' | sort)
printf '\ninstall with:\n  zpkg key add %s/zahlen-release-key.pub\n  zpkg install %s --require-checksums --require-signatures\n' \
    "${pkgs}" "${pkgs}"
