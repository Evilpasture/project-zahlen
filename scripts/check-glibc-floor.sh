#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fail when a shipped ELF requires a newer glibc symbol version than the
# baseline the release claims. The floor is not a property of the compiler
# alone: it is the versioned symbols the link actually references, and a single
# call can move it. On Arch this tree picked up GLIBC_2.43 from sqrtf/atan2f in
# libzahlen_engine.so and libJolt.so -- three relocations -- which silently put
# the baseline above Ubuntu 24.04 and every older distro.
#
#   usage: scripts/check-glibc-floor.sh <max, e.g. 2.35> <file|dir|tarball>...
#
# Directories are searched recursively; .tar.{gz,zst} archives are unpacked to a
# temporary directory first, because the artifact is what ships, not the build
# tree. A file with no glibc references (static, or not ELF) is skipped.
set -euo pipefail

max=${1:?usage: scripts/check-glibc-floor.sh <max glibc, e.g. 2.35> <file|dir|tarball>...}
shift
[ "$#" -gt 0 ] || {
    echo "error: no files given" >&2
    exit 64
}
case ${max} in
    2.*) ;;
    *)
        echo "error: expected a glibc version like 2.35, got '${max}'" >&2
        exit 64
        ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "${tmp}"' EXIT

# Collect the candidate files: archives are unpacked, directories are walked.
candidates=()
for target in "$@"; do
    if [ -d "${target}" ]; then
        while IFS= read -r -d '' file; do candidates+=("${file}"); done \
            < <(find "${target}" -type f -print0)
    elif [ -f "${target}" ]; then
        case ${target} in
            *.tar.gz | *.tgz | *.tar.zst | *.tar)
                mkdir -p "${tmp}/unpack"
                tar -xf "${target}" -C "${tmp}/unpack" || {
                    echo "error: cannot unpack ${target}" >&2
                    exit 1
                }
                while IFS= read -r -d '' file; do candidates+=("${file}"); done \
                    < <(find "${tmp}/unpack" -type f -print0)
                rm -rf "${tmp}/unpack"
                ;;
            *) candidates+=("${target}") ;;
        esac
    else
        echo "error: no such file or directory: ${target}" >&2
        exit 1
    fi
done

# The highest version any file reaches, and which files reach it.
highest="GLIBC_0.0"
bad=()
for file in "${candidates[@]}"; do
    # objdump on every non-ELF file in a package would bury the log in noise.
    head -c 4 "${file}" 2>/dev/null | grep -q $'\x7fELF' || continue
    version=$(objdump -T "${file}" 2>/dev/null |
        grep -o 'GLIBC_[0-9][0-9.]*' | sort -Vu | tail -1 || true)
    [ -n "${version}" ] || continue
    if [ "$(printf '%s\n' "${version}" "${highest}" | sort -V | tail -1)" = "${version}" ] &&
        [ "${version}" != "${highest}" ]; then
        highest=${version}
    fi
    if [ "$(printf '%s\n' "${version}" "GLIBC_${max}" | sort -V | tail -1)" != "GLIBC_${max}" ]; then
        bad+=("${file}: ${version}")
    fi
done

printf 'glibc floor: highest reference is %s, baseline is GLIBC_%s\n' "${highest}" "${max}"
if [ "${#bad[@]}" -gt 0 ]; then
    echo "error: these files require a newer glibc than the baseline:" >&2
    printf '  %s\n' "${bad[@]}" >&2
    echo "       Build against an older libc (sysroot) or drop the call that needs it." >&2
    exit 1
fi
printf 'PASS: every file stays within GLIBC_%s\n' "${max}"
