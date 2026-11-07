#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Install upstream's prebuilt Slang for the *build host*.
#
#   usage: scripts/install-slang-prebuilt.sh [version] [prefix]
#
# Why not build the pinned extern/slang tree: nothing in the release ships Slang.
# utils/zshader is a build tool with no install rule, so libslang and its public
# headers are build-host dependencies only -- zshader links the library, cooks
# every shader in-process and then has no reason to exist in a package. Building
# it from source costs ~25 minutes of every run that is not a ccache hit, for a
# dependency that never reaches a package.
#
# Why upstream's tarball and not a Python wheel: the `slangpy` wheel on PyPI does
# carry libslang-compiler.so and slang.h, but neither slang-com-helper.h nor
# slang-com-ptr.h, which utils/zshader includes. Upstream publishes release
# tarballs built against deliberately old glibc for exactly this purpose, with
# lib/libslang-compiler.so and the complete include/ tree. This one measures
# GLIBC_2.17 (see the check below), so it runs on jammy with margin. The tarball
# also contains a standalone compiler binary; this build never uses it.
#
# What it adds on top of the tarball is the CMake config package upstream does not
# ship: cmake/SlangHost/ resolves libslang with find_package(slang CONFIG), which
# is how the Arch image's distro package was used for every non-release build.
# Fifteen lines that describe what is already on disk is cheaper than making the
# tree learn a second way to find Slang.
set -Eeuo pipefail

trap 'status=$?; printf "\nerror: exit %s at line %s\n  command: %s\n" \
    "${status}" "${LINENO}" "${BASH_COMMAND}" >&2' ERR

version=${1:-2026.20}
prefix=${2:-/opt/slang}
baseline=${ZHLN_GLIBC_BASELINE:-2.35}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

# Upstream builds one tarball per glibc ABI. The old one is the point: a build host
# only has to be *newer* than the binaries it runs.
tarball=slang-${version}-linux-x86_64-glibc-2.28.tar.gz
url=https://github.com/shader-slang/slang/releases/download/v${version}/${tarball}

if [ -f "${prefix}/include/slang.h" ] && compgen -G "${prefix}/lib/libslang-compiler.so*" >/dev/null; then
    echo "== slang: libslang + headers already installed at ${prefix}"
    exit 0
fi

tmp=$(mktemp -d)
trap 'status=$?; rm -rf "${tmp}"; printf "\nerror: exit %s at line %s\n  command: %s\n" \
    "${status}" "${LINENO}" "${BASH_COMMAND}" >&2' ERR

echo "== slang: fetching ${tarball}"
curl -fsSL -o "${tmp}/${tarball}" "${url}"
tar -xzf "${tmp}/${tarball}" -C "${tmp}"

# The binaries have to run on the machine that builds the release, and Slang is a
# prebuilt we did not compile, so it gets held to the same baseline as everything
# else before it is installed.
"${script_dir}/check-glibc-floor.sh" "${baseline}" "${tmp}/lib" || {
    echo "error: slang ${version} cannot run on glibc ${baseline}." >&2
    echo "       Pick an older release; the *-glibc-2.27 / -glibc-2.28 tarballs are" >&2
    echo "       the ones built for old hosts." >&2
    exit 1
}

echo "== slang: installing into ${prefix}"
mkdir -p "${prefix}"
cp -a "${tmp}/include" "${tmp}/lib" "${tmp}/LICENSE" "${prefix}/" 2>/dev/null ||
    cp -a "${tmp}/include" "${tmp}/lib" "${prefix}/"
# The tarball ships libslang-compiler.so.0.<version> and a bare .so symlink, but not
# the plain libslang.so name some link lines expect.
[ -e "${prefix}/lib/libslang.so" ] ||
    ln -sfn "libslang-compiler.so.0.${version}" "${prefix}/lib/libslang.so"

# The config package upstream does not ship. It describes what is already installed;
# nothing is generated or compiled here.
mkdir -p "${prefix}/lib/cmake/slang"
cat >"${prefix}/lib/cmake/slang/slang-config.cmake" <<EOF
# Written by scripts/install-slang-prebuilt.sh -- do not edit.
# Upstream's release tarballs carry the binaries, the libraries and the headers, but
# no CMake package, and cmake/ShaderCompilation.cmake resolves Slang with
# find_package(slang CONFIG). This is the smallest target that answers it.
set(slang_FOUND TRUE)
set(slang_VERSION ${version})
if(NOT TARGET slang::slang)
    add_library(slang::slang SHARED IMPORTED)
    set_target_properties(slang::slang PROPERTIES
        IMPORTED_LOCATION "${prefix}/lib/libslang-compiler.so.0.${version}"
        INTERFACE_INCLUDE_DIRECTORIES "${prefix}/include")
endif()
EOF
cat >"${prefix}/lib/cmake/slang/slang-config-version.cmake" <<EOF
set(PACKAGE_VERSION "${version}")
set(PACKAGE_VERSION_COMPATIBLE TRUE)
if(PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
endif()
EOF

rm -rf "${tmp}"

# Prove the two things the build actually needs are present: the shared library
# zshader links, and the public headers it compiles against. Code generation is
# verified by zshader itself once the build has produced bin/zshader.
[ -f "${prefix}/include/slang.h" ] || {
    echo "error: ${prefix}/include/slang.h is missing." >&2
    exit 1
}
[ -e "${prefix}/lib/libslang.so" ] || {
    echo "error: ${prefix}/lib/libslang.so is missing." >&2
    exit 1
}
echo "== slang: installed libslang + headers into ${prefix}"
