#!/usr/bin/env bash
# Install a *released* GCC 16.x for an Ubuntu 22.04 (glibc 2.35) host.
#
# Why this exists, in one paragraph. The tree needs -freflection, which GCC 15
# does not have and GCC 16 does. The only GCC 16 that any apt source offers for
# jammy is `16-20260315` from the ubuntu-toolchain-r/test PPA -- a trunk
# snapshot, not a release. That snapshot fails to compile Jolt, which declares
# `static const StaticArray<Vec3, 1026> sUnitSphere;` inside Vec3 and so asks
# for an instantiation of a class template whose member is still incomplete;
# released GCC (14.2.0 and 16.2.0 alike) defers it, the March trunk does not.
# No distro carries a released 16.x built against glibc 2.35, because every
# distro that has one has moved to a newer glibc.
#
# So the compiler is downloaded, pinned by sha256, and pointed at the host's own
# glibc rather than at the sysroot it ships with: that keeps the build native
# (system headers, system crt files, system libc) and keeps the 2.35 floor a
# property of the host rather than of a bundled sysroot.
#
# Usage: install-gcc-prebuilt.sh [version] [prefix]     (default 16.2.0, /opt/gcc16)
set -Eeuo pipefail
trap 'printf "error: exit %s at line %s\n  command: %s\n" "$?" "${LINENO}" "${BASH_COMMAND}" >&2' ERR

GCC_VERSION=${1:-16.2.0}
PREFIX=${2:-/opt/gcc16}
GCC_MIN=${GCC_MIN:-16.1.1}
BASE_URL=${GCC_BASE_URL:-https://conda.anaconda.org/conda-forge}

log() { printf '  %s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }

# Pinned artifacts. The package set is (C compiler, C++ compiler + libstdc++,
# libstdc++ runtime, C++ headers, crt objects). Binutils, glibc headers and the
# linker are the host's. Checksums are of the exact files this script was tested
# with -- a rebuild of the same version elsewhere is a different artifact, so a
# mismatch is a real signal, not noise.
if [ "${GCC_VERSION}" != "16.2.0" ]; then
    {
        echo "error: no pinned artifact set for gcc ${GCC_VERSION}." >&2
        echo "       This script ships checksums for 16.2.0 only; adding another" >&2
        echo "       version means adding its five artifacts and their sha256 here." >&2
    } >&2
    exit 64
fi
ARTIFACTS=$(cat <<'EOF'
linux-64 gcc_impl_linux-64-16.2.0-hfe51025_5.conda 7780003a2cfd8872289904a71237744c698fec19020ae60886b97de86c583c01
linux-64 gxx_impl_linux-64-16.2.0-h0d273dc_5.conda 0f6b73e3a0854d5f3d8e3635cae7d57750767615f912f50a80581f1b7a0fac79
linux-64 libstdcxx-16.2.0-h934c35e_5.conda 38cadde30fad8f7c101f9eec79f60bf03fc99310c7195d23f8b30832516d666f
noarch libstdcxx-devel_linux-64-16.2.0-h86e191b_105.conda db4009a129f73e5f4e301a06b443bb5d160dbd7a89685ca54db1f50cd47f6963
noarch libgcc-devel_linux-64-16.2.0-he3ce08f_105.conda 8c50468146b12ae9f2c7b457aa54fef603f69f7c46d607d6c892c16d1c8d2f70
EOF
)

step "gcc ${GCC_VERSION} (released, downloaded; jammy's PPA only has a trunk snapshot)"

missing=()
for tool in curl tar unzip sha256sum; do
    command -v "${tool}" >/dev/null 2>&1 || missing+=("${tool}")
done
if [ "${#missing[@]}" -gt 0 ]; then
    echo "error: missing tools: ${missing[*]}" >&2
    exit 1
fi

# .conda files are a zip of zstd-compressed tarballs. The zstd CLI is the fast
# path; a python module is the fallback so this works on a host without it.
zstd_cli=0
command -v zstd >/dev/null 2>&1 && zstd_cli=1
py_zstd=0
python3 -c 'import zstandard' >/dev/null 2>&1 && py_zstd=1
if [ "${zstd_cli}" -eq 0 ] && [ "${py_zstd}" -eq 0 ]; then
    echo "error: need either the zstd CLI or python3 -m pip install zstandard" >&2
    exit 1
fi

# The download plus the unpacked tree is a few hundred MB. /tmp is a tmpfs on
# some hosts (GitHub runners included, when the disk is small), so keep the work
# next to the install prefix instead -- same filesystem, and the space is
# accounted for where the compiler itself will land.
mkdir -p "$(dirname "${PREFIX}")"
work=$(mktemp -d "$(dirname "${PREFIX}")/.gcc-prebuilt.XXXXXX")
trap 'rm -rf "${work}"' EXIT

extract_conda() {
    local pkg=$1 dest=$2
    unzip -q -o "${pkg}" -d "${work}/unpacked"
    local member
    for member in "${work}/unpacked"/pkg-*.tar.zst; do
        [ -e "${member}" ] || continue
        if [ "${zstd_cli}" -eq 1 ]; then
            zstd -dc --quiet "${member}" | tar -x -C "${dest}" --no-same-owner
        else
            python3 - "${member}" "${dest}" <<'PY'
import io, sys, tarfile, zstandard
src, dest = sys.argv[1], sys.argv[2]
with open(src, 'rb') as fh:
    reader = zstandard.ZstdDecompressor().stream_reader(fh)
    with tarfile.open(fileobj=reader, mode='r|') as tf:
        tf.extractall(dest, filter='data')
PY
        fi
    done
    rm -rf "${work}/unpacked"
}

mkdir -p "${PREFIX}"
while read -r subdir name want; do
    [ -n "${name}" ] || continue
    cached="${work}/${name}"
    if [ ! -f "${cached}" ]; then
        log "downloading ${name}"
        curl -fsSL -A "zahlen-release-toolchain" "${BASE_URL}/${subdir}/${name}" -o "${cached}"
    fi
    got=$(sha256sum "${cached}" | awk '{print $1}')
    if [ "${got}" != "${want}" ]; then
        {
            echo "error: checksum mismatch for ${name}" >&2
            echo "       expected ${want}" >&2
            echo "       got      ${got}" >&2
        } >&2
        exit 1
    fi
    extract_conda "${cached}" "${PREFIX}"
done <<< "${ARTIFACTS}"

# The packages are built for the target triple `x86_64-conda-linux-gnu`, so the
# driver looks for multiarch headers and start files under that name. Point it
# at the host's directories instead: with --sysroot=/ in the wrappers below this
# makes it a native compiler against jammy's glibc, not a cross compiler against
# a bundled 2.17 sysroot.
# Determined without a compiler on purpose: this runs on a bare jammy image, where
# there is no gcc yet -- asking one is what broke the first container build.
detect_multiarch() {
    if command -v dpkg-architecture >/dev/null 2>&1; then
        local m
        m=$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || true)
        if [ -n "${m}" ]; then printf '%s\n' "${m}"; return 0; fi
    fi
    # dpkg is always there; the multiarch tuple follows from the architecture.
    if command -v dpkg >/dev/null 2>&1; then
        case $(dpkg --print-architecture 2>/dev/null) in
            amd64) printf 'x86_64-linux-gnu\n'; return 0 ;;
            arm64) printf 'aarch64-linux-gnu\n'; return 0 ;;
            i386) printf 'i386-linux-gnu\n'; return 0 ;;
            armhf) printf 'arm-linux-gnueabihf\n'; return 0 ;;
        esac
    fi
    # Last resort: the directory that actually holds the C library start files.
    local dir
    for dir in /usr/lib/*-linux-gnu*; do
        [ -e "${dir}/libc.so" ] || [ -e "${dir}/libc.so.6" ] || continue
        printf '%s\n' "${dir#/usr/lib/}"
        return 0
    done
    return 1
}
multiarch=$(detect_multiarch || true)
if [ -z "${multiarch}" ]; then
    echo "error: cannot determine the host multiarch directory (dpkg and /usr/lib/*-linux-gnu*)" >&2
    exit 1
fi
mkdir -p "${PREFIX}/x86_64-conda-linux-gnu"
ln -sfn "/usr/include/${multiarch}" "${PREFIX}/x86_64-conda-linux-gnu/include"
ln -sfn "/usr/lib/${multiarch}" "${PREFIX}/x86_64-conda-linux-gnu/lib"
log "multiarch: ${multiarch} (headers and libraries follow the host)"
if [ ! -d "/usr/include/${multiarch}" ]; then
    # Not fatal: libc6-dev may not be installed yet, and a dangling symlink is
    # resolved when it is. The probe below is what decides.
    log "note: /usr/include/${multiarch} does not exist yet (libc6-dev not installed)"
fi

mkdir -p "${PREFIX}/bin"
for pair in "gcc:x86_64-conda-linux-gnu-gcc" "g++:x86_64-conda-linux-gnu-g++"; do
    short=${pair%%:*}
    real=${pair#*:}
    cat > "${PREFIX}/bin/${short}-16" <<EOF
#!/bin/sh
# ${short}-16 -- conda-forge gcc ${GCC_VERSION} driver, aimed at the host's own glibc.
#
# --sysroot=/ is the whole trick: without it the driver uses the 2.17 sysroot it
# ships with and the release would be built against headers the host does not
# have. The LD_LIBRARY_PATH is for the driver's own runtime (cc1plus links
# against the libstdc++ from the same download).
prefix=\$(CDPATH= cd -- "\$(dirname -- "\$0")/.." && pwd)
LD_LIBRARY_PATH="\${prefix}/lib\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}" \\
    exec "\${prefix}/bin/${real}" --sysroot=/ "\$@"
EOF
    chmod 755 "${PREFIX}/bin/${short}-16"
done

# ---------------------------------------------------------------------------
# Verify what was installed. The last check is the one that matters: trunk GCC
# 16 refused Jolt's `static const StaticArray<Vec3, 1026>` member, and a
# compiler that cannot compile that header cannot build this engine.
# ---------------------------------------------------------------------------
step "verify"
version=$("${PREFIX}/bin/g++-16" -dumpfullversion -dumpversion 2>/dev/null | head -1)
if [ -z "${version}" ] || ! printf '%s\n%s\n' "${GCC_MIN}" "${version}" | sort -V -C; then
    echo "error: installed gcc is ${version:-unknown}, need >= ${GCC_MIN}" >&2
    exit 1
fi
log "g++-16: ${version} (>= ${GCC_MIN})"

probe="${work}/probe"
mkdir -p "${probe}"
cat > "${probe}/probe.cpp" <<'EOF'
#include <cstdio>
#include <vector>
#include <string>
int main() {
    std::vector<std::string> v{"gcc"};
    std::printf("%s %zu\n", v[0].c_str(), v.size());
    return 0;
}
EOF
cat > "${probe}/jolt.cpp" <<'EOF'
// The construct that trunk GCC 16 rejected: a static data member whose type is
// a class template instantiated while its element type is still incomplete.
template <typename T, int N>
struct StaticArray {
protected:
    int mSize = 0;
    union { T mElements[N]; unsigned char mDummy; };
};
class alignas(16) Vec3 {
public:
    static const StaticArray<Vec3, 1026> sUnitSphere;
    float x, y, z, w;
};
int main() { return 0; }
EOF

if ! "${PREFIX}/bin/g++-16" -std=c++26 -O2 "${probe}/probe.cpp" -o "${probe}/probe"; then
    echo "error: g++-16 cannot compile and link a trivial program" >&2
    exit 1
fi
if ! "${probe}/probe" >/dev/null; then
    echo "error: a program built with g++-16 does not run" >&2
    exit 1
fi
log "compiled, linked and ran a probe"

if ! "${PREFIX}/bin/g++-16" -std=c++26 -freflection -fsyntax-only "${probe}/jolt.cpp"; then
    {
        echo "error: this gcc cannot compile Jolt's StaticArray/static-member pattern," >&2
        echo "       which extern/JoltPhysics needs; the build would stop at the PCH." >&2
    } >&2
    exit 1
fi
log "compiles Jolt's static-member pattern (-freflection, -std=c++26)"

printf '\n== gcc: %s (%s/bin/g++-16)\n' "${version}" "${PREFIX}"
printf '   add %s/bin to PATH; the wrappers carry --sysroot=/ themselves.\n' "${PREFIX}"
