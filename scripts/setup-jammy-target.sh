#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build a Ubuntu 22.04 (glibc 2.35) *target* sysroot, so the release can be
# compiled wherever the build dependencies are cheap -- the Arch container, with
# its prebuilt dxc, cmake and Vulkan stack -- and still link the libc the project
# supports. `--sysroot` does the rest; nothing else about the build changes.
#
#   # step 1, inside ubuntu:22.04 (apt resolves the transitive libraries):
#   docker run --rm -v "$PWD/.jammy:/opt/jammy" ubuntu:22.04 \
#       bash scripts/setup-jammy-target.sh --export /opt/jammy
#   # step 2, inside the Arch builder image, same path:
#   docker run --rm -v "$PWD/.jammy:/opt/jammy" zahlen-builder:release \
#       bash scripts/build-release-toolchain.sh /dist
#
# The export directory is recorded in the generated toolchain file *by absolute
# path*, so export it where it will later be mounted; mounting it elsewhere points
# the build at a directory that does not exist. To move a finished sysroot,
# re-export it instead of copying: --from-rootfs <old> --export <new> rewrites the
# recorded path in seconds, because it only copies the tree and fixes the loader
# symlink.
#
# Why the compiler comes from here and not from the build host: the installed
# package config refuses a consumer whose compiler version differs from the one
# that built the toolchain, and a jammy user can only install what jammy has.
# This sysroot's g++-16 *is* that package (measured: 16.0.1 20260315), so the
# build and the consumer agree by construction even though the build host is Arch
# with GCC 16.2.1.
#
# Two details are load-bearing and were found the hard way:
#   * jammy's /lib64/ld-linux-x86-64.so.2 is an absolute symlink. Under
#     --sysroot it resolves against the *host*, so the linker mixes two ld.so
#     versions and dies with "__nptl_change_stack_perm@GLIBC_PRIVATE undefined".
#     It is rewritten relative in the exported tree.
#   * the relocated driver finds cc1/cc1plus and its crt objects at build-time
#     absolute paths, so every consumer needs -B for both gcc directories. The
#     generated toolchain file does that; nothing is installed into the host.
set -euo pipefail

export_dir=
from_rootfs=
keep_apt=0
with_x11=1
while [ "$#" -gt 0 ]; do
    case $1 in
        --export)
            shift
            export_dir=${1:?--export needs a directory}
            ;;
        --from-rootfs)
            # Reuse an already-populated tree instead of apt: used to re-export,
            # and to rebuild the sysroot without network access.
            shift
            from_rootfs=${1:?--from-rootfs needs a directory}
            ;;
        --keep-apt) keep_apt=1 ;;
        --no-x11)
            # Wayland-only target: GLFW then builds without its X11 backend, and
            # the engine records no libX11 dependency. See the package note above.
            with_x11=0
            ;;
        -h | --help)
            sed -n '2,44p' "$0"
            exit 0
            ;;
        *)
            echo "error: unknown argument '$1'" >&2
            exit 64
            ;;
    esac
    shift
done
[ -n "${export_dir}" ] || {
    echo "error: --export <dir> is required" >&2
    exit 64
}

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
GLIBC_BASELINE=${ZHLN_GLIBC_BASELINE:-2.35}
GCC_PPA_VERSION=16-20260315-1ubuntu1~22~ppa1

asroot() {
    if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo -E "$@"; fi
}

# Packages installed into the target. Anything the engine *links* must come from
# here rather than from the build host: linking the host's copy records the
# host's symbol versions in the artifact and moves the floor.
#
# glfw is deliberately absent (jammy ships 3.3, the engine requires >= 3.4): the
# probe falls back to extern/glfw, which is built statically for this target, so
# the toolchain carries its own GLFW and the runtime needs no libglfw at all.
# The Wayland and X11 *dev* packages below exist because that static build links
# them:
#
#   Wayland   the primary backend. libwayland-bin is not optional -- it carries
#             wayland-scanner, and GLFW generates its protocol bindings with it at
#             build time. libdecor-0-dev is the decoration protocol library GLFW
#             uses when the compositor has no server-side decorations.
#   X11       the fallback backend, kept because Ubuntu 22.04 still ships X11
#             sessions (NVIDIA's driver defaults to one) and the cost is one
#             NEEDED entry. Build with --no-x11 for a Wayland-only tree: the GLFW
#             probe then finds no X11 headers, compiles GLFW_BUILD_X11=OFF and the
#             engine records no libX11 dependency at all.
#
# Two more absences are equally deliberate:
#   slang           jammy has none, and zshader links libslang in-process, so it
#                   is a runtime dependency of a released tool. The build compiles
#                   the pinned extern/slang with this sysroot's compiler instead
#                   of taking the build host's binary.
#   libvulkan-dev   jammy's headers are 1.3.204 and the renderer targets 1.4, so
#                   the headers come from extern/Vulkan-Headers; the loader is
#                   only ever a NEEDED soname (see MODE_LIBRARY below).
packages=(
    pkg-config
    binutils
    "gcc-16=${GCC_PPA_VERSION}"
    "g++-16=${GCC_PPA_VERSION}"
    "libstdc++-16-dev=${GCC_PPA_VERSION}"
    libc6-dev
    linux-libc-dev
    libzstd-dev
    libluajit-5.1-dev
    libx11-dev
    libxrandr-dev
    libxinerama-dev
    libxcursor-dev
    libxi-dev
    libxext-dev
    libwayland-dev
    libwayland-bin
    libxkbcommon-dev
    wayland-protocols
    libdecor-0-dev
)

# A Wayland-only target: the X11 -dev packages are what make GLFW's probe enable
# its X11 backend, so dropping them is the whole switch. Nothing else in the tree
# names X11, and GLFW then compiles with GLFW_BUILD_X11=OFF.
if [ "${with_x11}" -eq 0 ]; then
    if [ -n "${from_rootfs}" ]; then
        echo "warning: --no-x11 cannot remove headers from ${from_rootfs}." >&2
        echo "         The re-exported tree still has X11: GLFW will build that" >&2
        echo "         backend. Build the tree from scratch to drop it." >&2
        with_x11=1
    fi
    filtered=()
    for pkg in "${packages[@]}"; do
        case ${pkg} in
            libx11-dev | libxrandr-dev | libxinerama-dev | libxcursor-dev | libxi-dev | libxext-dev) continue ;;
        esac
        filtered+=("${pkg}")
    done
    packages=("${filtered[@]}")
fi

if [ -z "${from_rootfs}" ]; then
    echo "== installing the jammy target toolchain (glibc ${GLIBC_BASELINE})"
    apt-get update -qq
    apt-get install -y --no-install-recommends ca-certificates curl gnupg software-properties-common binutils
    if ! grep -rqs "ubuntu-toolchain-r/test" /etc/apt/sources.list /etc/apt/sources.list.d/; then
        add-apt-repository -y ppa:ubuntu-toolchain-r/test
        apt-get update -qq
    fi
    apt-get install -y --no-install-recommends "${packages[@]}"
    root=/
else
    echo "== re-exporting from ${from_rootfs}"
    root=$(cd "${from_rootfs}" && pwd)
fi

# --- prove the toolchain before anything is exported ------------------------
# A native compile here is the same compile the Arch host will do through
# --sysroot, so a broken sysroot fails in the cheap place.
echo "== verifying the target toolchain"
smoke=$(mktemp -d)
cat >"${smoke}/smoke.cpp" <<'EOF'
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
int main() {
    volatile float x = 2.0f;
    const float     s = std::sqrt(x) + std::atan2(x, x);
    std::vector<std::string> v {"hello"};
    std::printf("%f %s %s\n", static_cast<double>(s), v[0].c_str(), std::filesystem::current_path().c_str());
    return s > 0.0F ? 0 : 1;
}
EOF
compiler=$(command -v g++-16 || printf '%s/usr/bin/g++-16' "${root%/}")
"${compiler}" -std=c++26 -O2 -o "${smoke}/smoke" "${smoke}/smoke.cpp"
"${script_dir}/check-glibc-floor.sh" "${GLIBC_BASELINE}" "${smoke}/smoke"
"${smoke}/smoke" >/dev/null || {
    echo "error: the smoke binary did not run" >&2
    exit 1
}
rm -rf "${smoke}"

# --- export a relocatable sysroot -------------------------------------------
echo "== exporting the sysroot into ${export_dir}"
asroot mkdir -p "${export_dir}"
# A full userland, minus the parts that only describe the container it came from
# (or change on every run) -- the linker needs the real /usr, /lib and /lib64
# trees, not a curated subset that is one missing library away from a broken link.
copy_args=()
for dir in usr lib lib64 bin sbin etc; do
    [ -e "${root}/${dir}" ] && copy_args+=("./${dir}")
done
asroot tar -C "${root}" -cf - \
    --exclude=./proc --exclude=./sys --exclude=./dev --exclude=./run \
    --exclude=./tmp --exclude=./var/cache --exclude=./var/lib/apt/lists \
    --exclude=./var/log --exclude=./etc/ld.so.cache \
    "${copy_args[@]}" | asroot tar -C "${export_dir}" -xf -

# Absolute symlinks escape the sysroot. The loader is the one that matters.
for link in "${export_dir}/lib64/ld-linux-x86-64.so.2" "${export_dir}/lib/ld-linux-x86-64.so.2"; do
    [ -L "${link}" ] || continue
    target=$(readlink "${link}")
    case ${target} in
        /*) asroot ln -sfn "../lib/x86_64-linux-gnu/$(basename "${target}")" "${link}" ;;
    esac
done
asroot chmod 755 "${export_dir}/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"

gcc_lib=${export_dir}/usr/lib/gcc/x86_64-linux-gnu/16
gcc_libexec=${export_dir}/usr/libexec/gcc/x86_64-linux-gnu/16
for dir in "${gcc_lib}" "${gcc_libexec}" "${export_dir}/usr/bin/g++-16"; do
    [ -e "${dir}" ] || {
        echo "error: ${dir} is missing from the export" >&2
        exit 1
    }
done

# Everything a consumer of this sysroot needs, in one file, so the workflow never
# re-derives the flags.
asroot tee "${export_dir}/jammy-target.cmake" >/dev/null <<EOF
# Generated by scripts/setup-jammy-target.sh -- do not edit.
set(ZHLN_JAMMY_SYSROOT "${export_dir}")
set(CMAKE_SYSROOT "\${ZHLN_JAMMY_SYSROOT}")
set(CMAKE_C_COMPILER   "\${ZHLN_JAMMY_SYSROOT}/usr/bin/gcc-16")
set(CMAKE_CXX_COMPILER "\${ZHLN_JAMMY_SYSROOT}/usr/bin/g++-16")
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)
# -B makes the relocated driver find its own subprograms and crt objects.
set(CMAKE_C_FLAGS_INIT   "-B\${ZHLN_JAMMY_SYSROOT}/usr/libexec/gcc/x86_64-linux-gnu/16/ -B\${ZHLN_JAMMY_SYSROOT}/usr/lib/gcc/x86_64-linux-gnu/16/")
set(CMAKE_CXX_FLAGS_INIT "\${CMAKE_C_FLAGS_INIT}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "--sysroot=\${ZHLN_JAMMY_SYSROOT}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "--sysroot=\${ZHLN_JAMMY_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH "\${ZHLN_JAMMY_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# LIBRARY is the one mode that may fall out of the sysroot, and only as a
# fallback: the target's copies are searched first, so every library the engine
# links resolves to jammy's. What the fallback exists for is the one thing a
# glibc-baseline sysroot cannot carry -- the Vulkan *loader*. jammy's headers for
# it are 1.3.204 while the renderer targets 1.4, so libvulkan-dev is deliberately
# not installed here (the headers come from extern/Vulkan-Headers); the loader is
# a NEEDED soname (libvulkan.so.1) either way, and the consumer supplies it.
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# pkg-config must resolve inside the sysroot too, or the probes for glfw/LuaJIT/
# simdjson find the build host's copies and drag its symbol versions in.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "\${ZHLN_JAMMY_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR} "\${ZHLN_JAMMY_SYSROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:\${ZHLN_JAMMY_SYSROOT}/usr/share/pkgconfig")
EOF

if [ "${keep_apt}" -eq 0 ] && [ -z "${from_rootfs}" ]; then
    # The export is what matters; the container's apt state is not part of it.
    asroot apt-get clean
fi

asroot du -sh "${export_dir}" | awk '{print "  sysroot size: " $1}'
printf 'jammy target ready: %s\n' "${export_dir}"
printf '  use: -DCMAKE_TOOLCHAIN_FILE=%s/jammy-target.cmake\n' "${export_dir}"
printf '  compiler: %s/usr/bin/g++-16 (%s, the package a jammy user installs)\n' "${export_dir}" \
    "$("${export_dir}/usr/bin/g++-16" --version | head -1)"
printf '  wayland-scanner: %s (%s)\n' "${export_dir}/usr/bin/wayland-scanner" \
    "$("${export_dir}/usr/bin/wayland-scanner" --version 2>&1 | head -1 || echo missing)"
if [ "${with_x11}" -eq 1 ]; then
    printf '  window backends: wayland + x11 (GLFW builds both; Wayland is selected at run time)\n'
else
    printf '  window backends: wayland only (--no-x11)\n'
fi
