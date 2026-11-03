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

# Pinned artifacts: the C compiler, the C++ compiler + libstdc++, the libstdc++
# runtime, the C++ headers, the crt objects, and binutils.
#
# binutils is pinned because GCC 16 emits `.base64` for large data blobs -- a
# directive GAS only learned in 2.41 -- and jammy ships 2.38:
#
#     /tmp/ccyIOJ8t.s:90159: Error: unknown pseudo-op: `.base64'
#
# That is the same mistake as taking ninja's version from apt: one half of the
# compiler pair pinned, the other half whatever the distro had. 2.46.1 is the
# version the conda-forge gcc 16.2.0 packages were built against.
#
# The glibc headers stay the host's. Checksums are of the exact files this script was tested
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
linux-64 binutils_impl_linux-64-2.46.1-default_hfdba357_103.conda 98f9ddc6dc5d56a824b7eaea5bab6ad4bb47642288fc70d6b1b4024ac39d5595
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
# driver looks for multiarch headers and start files under that name. Point it at the
# host's directories, by flag rather than by symlink: these packages ship real
# directories at x86_64-conda-linux-gnu/{include,lib} (binutils puts ldscripts
# there), so a symlink at that path is not available -- and unpacking through one
# would write into /usr as root.
#
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
    # Last resort: the directory that actually holds the C library.
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
log "multiarch: ${multiarch} (headers and libraries come from the host)"
if [ ! -d "/usr/include/${multiarch}" ]; then
    # Not fatal: libc6-dev may not be installed yet. The probe below is what decides.
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
# --sysroot=/ is the first half of the trick: without it the driver uses the 2.17
# sysroot these packages expect, not the host's glibc. The rest point the target
# triple's search paths at the host's multiarch directories, replacing the symlinks
# earlier versions of this script used (see the note above).
#
# PATH matters for the assembler: it is looked up as x86_64-conda-linux-gnu-as, and
# jammy's 2.38 cannot assemble what this compiler emits (.base64).
PATH="\${prefix}/bin:\${prefix}/x86_64-conda-linux-gnu/bin\${PATH:+:\${PATH}}" \\
LD_LIBRARY_PATH="\${prefix}/lib\${LD_LIBRARY_PATH:+:\${LD_LIBRARY_PATH}}" \\
    exec "\${prefix}/bin/${real}" --sysroot=/ \\
        -isystem "/usr/include/__MULTIARCH__" \\
        -B"/usr/lib/__MULTIARCH__/" \\
        -L"/usr/lib/__MULTIARCH__" "\$@"
EOF
    sed -i "s|__MULTIARCH__|${multiarch}|g" "${PREFIX}/bin/${short}-16"
    chmod 755 "${PREFIX}/bin/${short}-16"
done

# ---------------------------------------------------------------------------
# The compiler's runtime is newer than the distro's, and that is not a detail:
# jammy ships GLIBCXX_3.4.30 and GCC_12, a program from this compiler can need
# GLIBCXX_3.4.36 and GCC_13.0.0, and every build tool the engine runs during the
# build is such a program:
#
#     bin/zshader: /lib/x86_64-linux-gnu/libstdc++.so.6: version `GLIBCXX_3.4.36'
#                  not found; ... `GCC_13.0.0' not found
#
# LD_LIBRARY_PATH is not a fix: the build sets it itself for shader cooking, which
# silently replaces whatever we exported. Register the runtime with the dynamic
# loader instead, so it is found through the cache no matter who sets what.
if [ ! -e "${PREFIX}/lib/libgcc_s.so.1" ]; then
    gcc_lib=$(ls -d "${PREFIX}"/lib/gcc/*/*/libgcc_s.so.1 2>/dev/null | head -1 || true)
    if [ -n "${gcc_lib}" ]; then
        ln -s "${gcc_lib#"${PREFIX}/lib/"}" "${PREFIX}/lib/libgcc_s.so.1"
    fi
fi
if [ "$(id -u)" -eq 0 ] && [ -d /etc/ld.so.conf.d ]; then
    # The "00-" is load-bearing. These files are included in alphabetical order by
    # /etc/ld.so.conf, the distro's own `x86_64-linux-gnu.conf` lists /usr/local and
    # /lib and sorts after plain names, and the loader takes the *first* cache entry
    # for a soname -- so a file called `zahlen-gcc16.conf` loses to the distro's and
    # the runtime is silently the old one. Verified below rather than assumed.
    conf=/etc/ld.so.conf.d/00-zahlen-gcc16.conf
    printf '%s\n' "${PREFIX}/lib" >"${conf}"
    ldconfig_bin=$(command -v ldconfig || printf '/sbin/ldconfig')
    if [ -x "${ldconfig_bin}" ]; then
        "${ldconfig_bin}"
        runtime=$("${ldconfig_bin}" -p 2>/dev/null | awk '/libstdc\+\+\.so\.6 /{print $NF; exit}')
        gcc_runtime=$("${ldconfig_bin}" -p 2>/dev/null | awk '/libgcc_s\.so\.1 /{print $NF; exit}')
        log "runtime registered with the loader (${conf})"
        log "loader resolves libstdc++.so.6 -> ${runtime:-unknown}"
        log "loader resolves libgcc_s.so.1   -> ${gcc_runtime:-unknown}"
        case "${runtime}" in
            "${PREFIX}"/*) ;;
            *)
                {
                    echo "error: the loader still resolves libstdc++.so.6 to" >&2
                    echo "       ${runtime:-nothing}, not to ${PREFIX}/lib." >&2
                    echo "       Something earlier in /etc/ld.so.conf.d wins; see the" >&2
                    echo "       comment above ${conf}." >&2
                } >&2
                exit 1
                ;;
        esac
    fi
else
    {
        echo "note: not running as root, so the runtime was not registered." >&2
        echo "      Programs built with this compiler need GLIBCXX_3.4.36 and GCC_13.0.0," >&2
        echo "      which the distro does not have. Either run this script as root, or" >&2
        echo "      export LD_LIBRARY_PATH=${PREFIX}/lib before running them." >&2
    } >&2
fi

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
# std::format is deliberate: it makes the probe link against the *newer* runtime
# symbols, so "compiles and runs" here also means the loader can find the runtime
# this compiler needs. A probe that only uses <cstdio> passes on a machine that
# cannot run the build tools.
cat > "${probe}/probe.cpp" <<'EOF'
#include <cstdio>
#include <format>
#include <string>
#include <vector>
int main() {
    std::vector<std::string> v{"gcc"};
    std::printf("%s\n", std::format("{} {} runtime", v[0], v.size()).c_str());
    return 0;
}
EOF

if ! "${PREFIX}/bin/g++-16" -std=c++26 -O2 "${probe}/probe.cpp" -o "${probe}/probe"; then
    echo "error: g++-16 cannot compile and link a trivial program" >&2
    exit 1
fi
if ! "${probe}/probe" >/dev/null; then
    echo "error: a program built with g++-16 does not run" >&2
    exit 1
fi
log "compiled, linked and ran a probe (std::format, so the new runtime resolved)"

# The assembler must come from here, not from the distro: the pair is verified
# together or not at all.
as_path=$("${PREFIX}/bin/g++-16" -print-prog-name=as 2>/dev/null || true)
as_version=$("${as_path}" --version 2>/dev/null | head -1 || true)
ld_path=$("${PREFIX}/bin/g++-16" -print-prog-name=ld 2>/dev/null || true)
log "linker: $("${ld_path}" --version 2>/dev/null | head -1) (${ld_path})"
case "${as_path}" in
    "${PREFIX}"/*) log "assembler: ${as_version} (${as_path})" ;;
    *)
        {
            echo "error: g++-16 resolves its assembler to ${as_path:-nothing}," >&2
            echo "       which is outside ${PREFIX}; jammy's 2.38 cannot assemble" >&2
            echo "       GCC 16 output (.base64). Keep ${PREFIX}/bin first on PATH." >&2
        } >&2
        exit 1
        ;;
esac

# Two behavioural checks are deliberately *not* here: whether this gcc accepts
# Jolt's static-member pattern, and whether its assembler accepts the `.base64`
# directive. Both are questions about a compiler, and cmake/
# ToolchainCapabilities.cmake now asks them at configure time, with the flags the
# build actually uses -- so they answer for the engine, for the SDK, and for
# anyone who points CMake at this prefix, not only for the machine that ran this
# script. What stays here is what configure cannot see: that this prefix's own
# binutils is the one that answers, which is the `as` path check above.

printf '\n== gcc: %s (%s/bin/g++-16)\n' "${version}" "${PREFIX}"
printf '   add %s/bin to PATH; the wrappers carry --sysroot=/ themselves.\n' "${PREFIX}"
