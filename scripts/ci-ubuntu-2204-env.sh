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
#                     to extern/glfw, which is the 3.5 tree CI already builds.
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
DXC_TAG=v1.10.2605.37
DXC_TARBALL=linux_dxc_2026_08_11.x86_64.tar.gz

usage() {
    cat <<EOF
usage: ${PROG} --build|--consumer [--gcc <major>]

  --build       toolchain + engine build dependencies
  --consumer    dependencies for installing a released toolchain and running it
  --gcc <n>     gcc/g++ major to install (default ${GCC_VERSION})
EOF
}

while [ "$#" -gt 0 ]; do
    case $1 in
        --build | --consumer) PROFILE=${1#--} ;;
        --gcc)
            shift
            GCC_VERSION=${1:?--gcc needs a major version}
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
asroot apt-get update -qq
apt_install ca-certificates curl gnupg lsb-release apt-transport-https software-properties-common python3

step "gcc ${GCC_VERSION} (jammy ships gcc-11; the tree needs -freflection)"
if ! dpkg -s "g++-${GCC_VERSION}" >/dev/null 2>&1; then
    # The Ubuntu toolchain PPA is the only place a GCC this new exists for jammy.
    # If the version is ever dropped there, the diagnostic below says so instead
    # of leaving a confusing "c++ compiler not found" from CMake later.
    if ! grep -rqs "ubuntu-toolchain-r/test" /etc/apt/sources.list /etc/apt/sources.list.d/; then
        asroot add-apt-repository -y ppa:ubuntu-toolchain-r/test
        asroot apt-get update -qq
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
asroot update-alternatives --set "gcc" "/usr/bin/gcc-${GCC_VERSION}" >/dev/null 2>&1 || true

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
        asroot apt-get update -qq
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

step "ninja + pkg-config"
apt_install ninja-build pkg-config

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
    vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverName|apiVersion' | head -6 || true
    # jammy's mesa is 23.x, which advertises Vulkan 1.3. The renderer enables
    # acceleration-structure features; if this ever trips, say so here instead of
    # leaving a device-selection failure twenty minutes later.
    if ! vulkaninfo --summary 2>/dev/null | grep -qE 'apiVersion *= *1\.([4-9]|[1-9][0-9])'; then
        echo '  warning: lavapipe reports a Vulkan API version below 1.4' >&2
    fi
fi

if [ "${PROFILE}" = build ]; then
    step "engine build dependencies"
    apt_install libzstd-dev zlib1g-dev libssl-dev libcurl4-openssl-dev libluajit-5.1-dev libgtest-dev
    apt_install glslang-tools spirv-tools

    # Jolt compiles its own HLSL compute shaders with dxc, which it locates by
    # rewriting glslc's path. Ubuntu has no dxc package, so take the pinned
    # upstream release and put it where cmake/ThirdParty.cmake's probe looks.
    step "dxc ${DXC_TAG} (Jolt's Vulkan compute shaders)"
    if ! command -v dxc >/dev/null; then
        tmp=$(mktemp -d)
        curl -fsSL -o "${tmp}/dxc.tar.gz" \
            "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_TAG}/${DXC_TARBALL}"
        tar -xzf "${tmp}/dxc.tar.gz" -C "${tmp}"
        # The tarball nests bin/ and lib/ one level down.
        dxc_bin=$(find "${tmp}" -type f -name dxc -perm -u+x | head -1)
        dxc_lib=$(find "${tmp}" -type f -name "libdxcompiler.so*" | head -1)
        if [ -z "${dxc_bin}" ] || [ -z "${dxc_lib}" ]; then
            echo "error: ${DXC_TARBALL} did not contain dxc + libdxcompiler.so" >&2
            find "${tmp}" -maxdepth 3 >&2
            exit 1
        fi
        asroot install -m755 "${dxc_bin}" /usr/local/bin/dxc
        asroot install -m644 "${dxc_lib}" "/usr/local/lib/$(basename "${dxc_lib}")"
        # ldconfig rather than LD_LIBRARY_PATH: the release build must not
        # depend on an environment variable that a user's shell will not have.
        asroot ldconfig
        rm -rf "${tmp}"
    fi
    dxc --version >/dev/null || {
        echo "error: dxc is installed but does not run (missing libdxcompiler?)" >&2
        ldd "$(command -v dxc)" >&2 || true
        exit 1
    }
fi

if [ "${PROFILE}" = consumer ]; then
    step "consumer dependencies (runtime libraries the packages dlopen/link)"
    # libglfw3/libzstd1/libatomic1 are DT_NEEDED of the engine package; netpbm
    # converts the captured PPM into something a browser can show.
    apt_install libglfw3 libzstd1 libatomic1 netpbm
fi

step "provisioned"
{
    printf 'profile:      %s\n' "${PROFILE}"
    printf 'distro:       %s\n' "$(. /etc/os-release && printf '%s %s' "${NAME}" "${VERSION_ID}")"
    printf 'glibc:        %s\n' "$(ldd --version | awk 'NR==1{print $NF}')"
    printf 'gcc:          %s\n' "$(g++-"${GCC_VERSION}" --version | awk 'NR==1{print $0}')"
    printf 'cmake:        %s\n' "$(cmake --version | awk 'NR==1{print $3}')"
    printf 'ninja:        %s\n' "$(ninja --version)"
    printf 'vulkan ICDs:  %s\n' "$(ls /usr/share/vulkan/icd.d/ | tr '\n' ' ')"
    if [ "${PROFILE}" = build ]; then
        printf 'dxc:          %s\n' "$(dxc --version)"
    fi
} | tee "${GITHUB_STEP_SUMMARY:-/dev/null}"
