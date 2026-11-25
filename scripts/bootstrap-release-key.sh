#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later
#
# scripts/bootstrap-release-key.sh
#
# Generate the long-lived project signing key ONCE, on a trusted local machine.
# The public half is committed (packaging/keys/); the private half is printed
# as base64 for the ZHLN_RELEASE_KEY_PEM GitHub Actions secret and must never
# be committed, logged in CI, or pasted into an issue.
#
#   scripts/bootstrap-release-key.sh                 # generate; prints the secret
#   scripts/bootstrap-release-key.sh --verify FILE  # check a saved secret matches
#                                                   # the pinned public key
#
# Rotating the key: generate a new pair, put the NEW private key in
# ZHLN_RELEASE_KEY_PEM and the OLD one in ZHLN_RELEASE_KEY_PEM_PREVIOUS, then
# publish one dual-signed release (scripts/package-toolchain.sh writes one
# signature line per key, and zpkg accepts any line signed by a trusted key).
# Drop the previous secret after that release. Never rotate silently.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
keys="${root}/packaging/keys"
mkdir -p "$keys"

fingerprint_of() {
    local pem=$1 out
    out=$(openssl pkey -in "$pem" -pubout -outform DER 2>/dev/null | tail -c 32 | sha256sum | cut -c1-32)
    printf '%s\n' "$out"
}

case "${1:-}" in
--verify)
    secret=${2:?usage: bootstrap-release-key.sh --verify <base64-secret-file>}
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    base64 -d "$secret" >"$tmp/key.pem" 2>/dev/null || {
        echo "error: $secret is not base64-encoded PKCS#8 PEM" >&2
        exit 1
    }
    openssl pkey -in "$tmp/key.pem" -pubout -outform DER 2>/dev/null | tail -c 32 >"$tmp/pub.raw"
    base64 -w0 "$tmp/pub.raw" >"$tmp/pub"
    printf '\n' >>"$tmp/pub"
    if [ "$(tr -d '[:space:]' <"$tmp/pub")" != "$(tr -d '[:space:]' <"$keys/zahlen-release.pub")" ]; then
        echo "error: secret does not match packaging/keys/zahlen-release.pub" >&2
        exit 1
    fi
    if [ "$(fingerprint_of "$tmp/key.pem")" != "$(tr -d '[:space:]' <"$keys/FINGERPRINT")" ]; then
        echo "error: secret does not match packaging/keys/FINGERPRINT" >&2
        exit 1
    fi
    echo "ok: secret matches the pinned project key"
    ;;
"")
    if [ -f "$keys/zahlen-release.pub" ]; then
        echo "error: $keys/zahlen-release.pub already exists." >&2
        echo "       Deliberate rotation only: back it up, remove both files, rerun," >&2
        echo "       then publish one release signed by the old AND new keys." >&2
        exit 1
    fi
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    openssl genpkey -algorithm ed25519 -out "$tmp/key.pem" 2>/dev/null
    openssl pkey -in "$tmp/key.pem" -pubout -outform DER 2>/dev/null | tail -c 32 >"$tmp/pub.raw"
    base64 -w0 "$tmp/pub.raw" >"$keys/zahlen-release.pub"
    printf '\n' >>"$keys/zahlen-release.pub"
    fingerprint_of "$tmp/key.pem" >"$keys/FINGERPRINT"
    base64 -w0 "$tmp/key.pem" >"$tmp/secret.base64"

    echo "wrote $keys/zahlen-release.pub and $keys/FINGERPRINT"
    printf 'fingerprint: %s\n' "$(cat "$keys/FINGERPRINT")"
    echo
    echo 'Set this as the ZHLN_RELEASE_KEY_PEM repository secret (base64 PKCS#8 ed25519 PEM):'
    echo '----- BEGIN SECRET (do not commit, do not paste into CI logs) -----'
    cat "$tmp/secret.base64"
    echo
    echo '----- END SECRET -----'
    echo
    echo 'Then commit packaging/keys/ and verify the secret with:'
    echo '  scripts/bootstrap-release-key.sh --verify <saved-secret-file>'
    echo 'The private key exists only in this output; it is not written to the repository.'
    ;;
*)
    echo "usage: scripts/bootstrap-release-key.sh [--verify <base64-secret-file>]" >&2
    exit 2
    ;;
esac
