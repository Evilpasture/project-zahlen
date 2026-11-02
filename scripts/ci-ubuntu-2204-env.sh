#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Provision a plain Ubuntu 22.04 (jammy) machine for this tree. Nothing here is
# container-specific and nothing here is Arch-specific: the point of the profile
# split is that "a user with a laptop" and "the machine that builds the release"
# run the same script with different arguments, so the release is only ever
# tested against a real distro install of the runtime libraries.
#
#   --build      compiler, CMake >= 3.28, the libraries the engine links, the
#                shader compilers the cooker and Jolt need, and lavapipe.
#   --consumer   what a user needs to install a released toolchain with zpkg,
#                compile a consumer against it and run it headless.
#   --gcc <n>    compiler major to install (default 16: -freflection needs it).
#
# Deliberately NOT installed for --build, because jammy is older than what this
# tree targets and the tree already carries the correct copies:
#
#   libglfw3-dev      jammy ships 3.3; the probe requires >= 3.4 and falls back
#                     to extern/glfw. A release build always does that (see
#                     -DZHLN_GLFW_VENDORED=ON in scripts/build-release-toolchain.sh).
#   libsimdjson-dev   jammy has no 5.x; cmake/ThirdParty.cmake falls back to
#                     extern/simdjson and errors if neither is found.
#   libvulkan-dev     jammy headers are 1.3.204; the renderer targets Vulkan 1.4
#                     (the published image links a 1.4.363 loader), so the
#                     headers come from extern/Vulkan-Headers instead.
#
# The runtime loader (libvulkan1) IS installed in both profiles: volk dlopens it.
set -euo pipefail

PROG=${0##*/}
GCC_VERSION=16
PROFILE=
# The distro the release claims. Tools fetched as prebuilts must also run here,
# so this is the ceiling for them, not just for our own binaries.
GLIBC_BASELINE=${ZHLN_GLIBC_BASELINE:-2.35}
# Pinned to the newest upstream dxc prebuilt that still runs on the baseline; see
# the block that installs it for the measurement.
DXC_TAG=v1.8.2505.1
DXC_TARBALL=linux_dxc_2025_07_14.x86_64.tar.gz
# jammy's ninja is 1.10.1 and CMake refuses to generate build files for C++20 modules
# with anything older than 1.11 -- this tree has 28 module units, so the *build host*
# needs a modern ninja the distro does not carry. Upstream publishes static-ish
# release binaries built against very old glibc (measured: GLIBC_2.15), so it is
# downloaded rather than packaged.
NINJA_VERSION=${NINJA_VERSION:-1.13.2}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

usage() {
    cat <<EOF
usage: ${PROG} --build|--consumer [--gcc <major>] [--glibc-baseline <ver>]

  --build             toolchain + engine build dependencies
  --consumer          dependencies for installing a released toolchain and running it
  --gcc <n>           gcc/g++ major to install (default ${GCC_VERSION})
  --glibc-baseline <v> distro baseline fetched prebuilts must run on (default ${GLIBC_BASELINE})
EOF
}

while [ "$#" -gt 0 ]; do
    case $1 in
        --build | --consumer) PROFILE=${1#--} ;;
        --gcc)
            shift
            GCC_VERSION=${1:?--gcc needs a major version}
            ;;
        --glibc-baseline)
            shift
            GLIBC_BASELINE=${1:?--glibc-baseline needs a version, e.g. 2.35}
            ;;
        -h | --help)
            usage
            exit 0
            ;;
        *)
            echo "error: unknown argument '$1'" >&2
            usage >&2
            exit 64
            ;;
    esac
    shift
done
if [ -z "${PROFILE}" ]; then
    usage >&2
    exit 64
fi
case ${GCC_VERSION} in
    '' | *[!0-9]*)
        echo "error: --gcc needs a number, got '${GCC_VERSION}'" >&2
        exit 64
        ;;
esac

if [ "$(id -u)" -eq 0 ]; then
    SUDO=()
else
    SUDO=(sudo -E)
fi
asroot() { "${SUDO[@]}" "$@"; }
export DEBIAN_FRONTEND=noninteractive

step() { printf '\n== %s\n' "$*"; }

# ---------------------------------------------------------------------------
# apt helpers. Every install is guarded, so re-running this script is cheap and
# a partially provisioned runner converges instead of reinstalling the world.
# ---------------------------------------------------------------------------
apt_install() {
    local missing=() pkg
    for pkg in "$@"; do
        dpkg -s "${pkg}" >/dev/null 2>&1 || missing+=("${pkg}")
    done
    if [ "${#missing[@]}" -eq 0 ]; then
        return 0
    fi
    printf '  installing: %s\n' "${missing[*]}"
    asroot apt-get install -y --no-install-recommends "${missing[@]}"
}

step "base packages"
asroot apt-get update
apt_install ca-certificates curl gnupg lsb-release apt-transport-https software-properties-common python3 unzip

step "gcc ${GCC_VERSION} (jammy ships gcc-11; the tree needs -freflection)"
if ! dpkg -s "g++-${GCC_VERSION}" >/dev/null 2>&1; then
    # The Ubuntu toolchain PPA is the only place a GCC this new exists for jammy.
    # If the version is ever dropped there, the diagnostic below says so instead
    # of leaving a confusing "c++ compiler not found" from CMake later.
    if ! grep -rqs "ubuntu-toolchain-r/test" /etc/apt/sources.list /etc/apt/sources.list.d/; then
        asroot add-apt-repository -y ppa:ubuntu-toolchain-r/test
        asroot apt-get update
    fi
    if ! apt-cache show "g++-${GCC_VERSION}" >/dev/null 2>&1; then
        {
            echo "error: g++-${GCC_VERSION} is not available on this distro." >&2
            echo "       available toolchain versions:" >&2
            apt-cache search --names-only "^g\+\+-[0-9]+$" | sort >&2 || true
            echo "       re-run with --gcc <major> to pick one; without native" >&2
            echo "       -freflection the build falls back to scripts/transpile_reflection.py." >&2
        } >&2
        exit 1
    fi
    apt_install "gcc-${GCC_VERSION}" "g++-${GCC_VERSION}"
fi
asroot update-alternatives --set "gcc" "/usr/bin/gcc-${GCC_VERSION}" || true

step "cmake >= 3.28 (jammy ships 3.22; the tree and the consumer both require 3.28)"
cmake_ok() {
    command -v cmake >/dev/null || return 1
    local v
    v=$(cmake --version | awk '/^cmake version/{print $3; exit}')
    [ -n "${v}" ] && [ "$(printf '%s\n3.28\n' "${v}" | sort -V | head -1)" = "3.28" ]
}
if ! cmake_ok; then
    if ! grep -rqs "apt.kitware.com" /etc/apt/sources.list /etc/apt/sources.list.d/ 2>/dev/null; then
        curl -fsSL https://apt.kitware.com/keys/kitware-archive-latest.asc |
            asroot gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg
        echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ $(. /etc/os-release && printf '%s' "${UBUNTU_CODENAME:-jammy}") main" |
            asroot tee /etc/apt/sources.list.d/kitware.list >/dev/null
        asroot apt-get update
    fi
    apt_install cmake
fi
if ! cmake_ok; then
    # Kitware's repo is the documented path; pip is the fallback that has never
    # failed on a runner, and it installs a wheel built for this glibc.
    apt_install python3-pip
    "${SUDO[@]}" pip3 install --no-cache-dir --upgrade "cmake>=3.28"
fi
if ! cmake_ok; then
    echo "error: could not provision cmake >= 3.28" >&2
    cmake --version >&2 || true
    exit 1
fi

step "ninja >= 1.11 (jammy ships 1.10.1, which cannot scan C++20 modules)"
apt_install pkg-config
ninja_ok() {
    command -v ninja >/dev/null || return 1
    local v
    v=$(ninja --version 2>&1 | head -1)
    [ -n "${v}" ] && [ "$(printf '%s\n1.11\n' "${v}" | sort -V | head -1)" = "1.11" ]
}
if ! ninja_ok; then
    # /usr/local/bin precedes /usr/bin on every normal PATH, so installing here
    # shadows a distro ninja without removing it.
    tmp_ninja=$(mktemp -d)
    curl -fsSL -o "${tmp_ninja}/ninja.zip" \
        "https://github.com/ninja-build/ninja/releases/download/v${NINJA_VERSION}/ninja-linux.zip"
    unzip -q "${tmp_ninja}/ninja.zip" -d "${tmp_ninja}"
    # Same rule as every other fetched prebuilt: it has to run on the baseline.
    "${script_dir}/check-glibc-floor.sh" "${GLIBC_BASELINE}" "${tmp_ninja}/ninja" || {
        echo "error: ninja ${NINJA_VERSION} cannot run on glibc ${GLIBC_BASELINE}." >&2
        exit 1
    }
    asroot install -m755 "${tmp_ninja}/ninja" /usr/local/bin/ninja
    rm -rf "${tmp_ninja}"
    hash -r 2>/dev/null || true
fi
if ! ninja_ok; then
    echo "error: could not provision ninja >= 1.11 (found: $(ninja --version 2>&1 | head -1))" >&2
    echo "       CMake needs 1.11+ to generate build files for C++20 modules." >&2
    exit 1
fi
printf '  ninja: %s (%s)\n' "$(ninja --version 2>&1 | head -1)" "$(command -v ninja)"

step "vulkan runtime + lavapipe (CPU rendering)"
apt_install libvulkan1 mesa-vulkan-drivers vulkan-tools
# The manifest name is not portable: Arch ships the upstream lvp_icd.json, while
# Debian and Ubuntu suffix it with the architecture (lvp_icd.x86_64.json). Look
# for whichever exists and hand it to every later step, so nothing in this repo
# hardcodes one distro's spelling again.
icd=
for candidate in /usr/share/vulkan/icd.d/lvp_icd*.json; do
    if [ -f "${candidate}" ]; then
        icd=${candidate}
        break
    fi
done
if [ -z "${icd}" ]; then
    echo "error: no lavapipe ICD found after installing mesa-vulkan-drivers" >&2
    ls -la /usr/share/vulkan/icd.d/ >&2 || true
    exit 1
fi
export VK_DRIVER_FILES="${icd}"
export VK_ICD_FILENAMES="${icd}"
if [ -n "${GITHUB_ENV:-}" ]; then
    # Later workflow steps inherit this; a local run just exports it in the shell.
    {
        printf 'VK_DRIVER_FILES=%s\n' "${icd}"
        printf 'VK_ICD_FILENAMES=%s\n' "${icd}"
    } >>"${GITHUB_ENV}"
fi
printf '  lavapipe ICD: %s\n' "${icd}"
if command -v vulkaninfo >/dev/null 2>&1; then
    vulkaninfo --summary | grep -E 'deviceName|driverName|apiVersion' | head -6 || true
    # jammy's mesa is 23.x, which advertises Vulkan 1.3. The renderer enables
    # acceleration-structure features; if this ever trips, say so here instead of
    # leaving a device-selection failure twenty minutes later.
    if ! vulkaninfo --summary | grep -qE 'apiVersion *= *1\.([4-9]|[1-9][0-9])'; then
        echo '  warning: lavapipe reports a Vulkan API version below 1.4' >&2
    fi
fi

if [ "${PROFILE}" = build ]; then
    step "engine build dependencies"
    apt_install libzstd-dev zlib1g-dev libssl-dev libcurl4-openssl-dev libluajit-5.1-dev libgtest-dev
    apt_install glslang-tools spirv-tools
    # Needed by a *release* build specifically:
    #   git, ccache, openssl   the checkout, the compiler cache, and the release
    #                          signing key that scripts/package-toolchain.sh uses.
    apt_install git ccache openssl
    # The window stack, and the reason the release can be Wayland-only: GLFW is
    # vendored (jammy ships 3.3, the engine needs 3.4's platform APIs) and is built
    # here against these. libwayland-dev ships wayland-client.pc,
    # wayland-cursor.pc and wayland-egl.pc -- all three are what
    # cmake/ThirdParty.cmake's GLFW probe requires -- and wayland-scanner comes
    # from libwayland-bin.
    #
    # No X11 development package is installed, deliberately. GLFW's probe degrades
    # silently: with no X11 headers it builds Wayland, with neither it builds the
    # null platform, and with both it builds both. The release wants exactly one
    # answer, and the build asserts it after configure.
    apt_install libwayland-dev libwayland-bin libxkbcommon-dev wayland-protocols libdecor-0-dev

    # Jolt compiles its own HLSL compute shaders with dxc, which it locates by
    # rewriting glslc's path. Ubuntu has no dxc package, so take a pinned
    # upstream prebuilt and put it where cmake/ThirdParty.cmake's probe looks.
    #
    # The pin is capped by the baseline, not by features: dxc has to *run* on the
    # machine that builds the release. Measured from the published tarballs with
    # scripts/check-glibc-floor.sh:
    #
    #   v1.8.2505.1    (2025-07-14)  GLIBC_2.34   <- newest that runs on jammy
    #   v1.9.2602      (2026-02-20)  GLIBC_2.38
    #   v1.10.2605.37  (2026-08-11)  GLIBC_2.38
    #
    # Pinning a newer one is exactly what failed the first jammy run, with
    # "libc.so.6: version `GLIBC_2.38' not found" from the freshly installed
    # dxc. Do not bump this without re-measuring; the check below refuses to
    # install a dxc the baseline cannot run.
    step "dxc ${DXC_TAG} (Jolt's Vulkan compute shaders)"
    if ! command -v dxc >/dev/null; then
        tmp=$(mktemp -d)
        curl -fsSL -o "${tmp}/dxc.tar.gz" \
            "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_TAG}/${DXC_TARBALL}"
        tar -xzf "${tmp}/dxc.tar.gz" -C "${tmp}"
        dxc_bin=$(find "${tmp}" -type f -name dxc -perm -u+x | head -1)
        if [ -z "${dxc_bin}" ]; then
            echo "error: ${DXC_TARBALL} did not contain a dxc binary" >&2
            find "${tmp}" -maxdepth 3 >&2
            exit 1
        fi
        # The binary and the shared objects it ships with (libdxcompiler, libdxil)
        # are held to the release baseline before anything is installed.
        "${script_dir}/check-glibc-floor.sh" "${GLIBC_BASELINE}" \
            "$(dirname "${dxc_bin}")" "$(dirname "${dxc_bin}")/../lib" || {
            echo "error: dxc ${DXC_TAG} cannot run on glibc ${GLIBC_BASELINE}." >&2
            echo "       Pin an older release (see the measurements above) or build dxc from source." >&2
            exit 1
        }
        asroot install -m755 "${dxc_bin}" /usr/local/bin/dxc
        # dxc resolves its libraries through bin/../lib, so that layout matters:
        # install the libs next to the binary, not just the compiler library.
        for lib in "$(dirname "${dxc_bin}")/../lib"/*.so*; do
            [ -f "${lib}" ] || continue
            asroot install -m644 "${lib}" "/usr/local/lib/$(basename "${lib}")"
        done
        # ldconfig rather than LD_LIBRARY_PATH: the release build must not
        # depend on an environment variable that a user's shell will not have.
        asroot ldconfig
        rm -rf "${tmp}"
    fi
    dxc --version || {
        echo "error: dxc is installed but does not run" >&2
        ldd "$(command -v dxc)" >&2 || true
        exit 1
    }
fi

if [ "${PROFILE}" = consumer ]; then
    step "consumer dependencies (runtime libraries the packages dlopen/link)"
    # libzstd1/libatomic1/libcurl4 are DT_NEEDED of the engine package, and the
    # Wayland list is what the *vendored, static* GLFW resolves against once it is
    # inside libzahlen_engine.so -- distro runtime libraries, not -dev packages.
    #
    # There is deliberately no X11 runtime library here, and no libglfw3: the
    # release is Wayland-only and carries its own GLFW, so a machine with neither
    # must be able to load and run the engine. If a regression reintroduces an X11
    # or libglfw dependency, this list is what makes it fail loudly.
    #
    # libglfw3 is deliberately NOT installed any more. The release carries its own
    # GLFW, so a machine with no libglfw at all must be enough to load and run the
    # engine; installing jammy's 3.3 here would let a regression that needs 3.4's
    # platform APIs (glfwGetPlatform and friends) load and then fail at run time,
    # which is exactly the failure this job exists to catch at install time.
    apt_install libzstd1 libatomic1 netpbm libcurl4 \
        libwayland-client0 libwayland-cursor0 libwayland-egl1 libxkbcommon0 libdecor-0-0
fi

step "provisioned"
{
    printf 'profile:      %s\n' "${PROFILE}"
    printf 'distro:       %s\n' "$(. /etc/os-release && printf '%s %s' "${NAME}" "${VERSION_ID}")"
    printf 'glibc:        %s\n' "$(ldd --version | awk 'NR==1{print $NF}')"
    printf 'baseline:     glibc %s (fetched prebuilts are held to it)\n' "${GLIBC_BASELINE}"
    printf 'gcc:          %s\n' "$(g++-"${GCC_VERSION}" --version | awk 'NR==1{print $0}')"
    printf 'cmake:        %s\n' "$(cmake --version | awk 'NR==1{print $3}')"
    printf 'ninja:        %s\n' "$(ninja --version)"
    printf 'vulkan ICDs:  %s\n' "$(ls /usr/share/vulkan/icd.d/ | tr '\n' ' ')"
    if [ "${PROFILE}" = build ]; then
        printf 'dxc:          %s\n' "$(dxc --version)"
    fi
} | tee "${GITHUB_STEP_SUMMARY:-/dev/null}"
