#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the release inside a real Ubuntu 22.04 install.
#
#   docker run --rm -v "$PWD:/workspace" -v "$PWD/dist:/dist" \
#       -v "$PWD/.ccache:/ccache" -e CCACHE_DIR=/ccache \
#       zahlen-release-builder:jammy bash scripts/build-release-toolchain.sh /dist
#
# The host *is* the target. This runs inside docker/release-jammy.Dockerfile, which
# is ubuntu:22.04 plus the jammy packages a build needs, so the glibc floor of every
# published byte is the floor of the machine that produced it. There is no --sysroot,
# no -B, no loader wrapper and no foreign binutils: the failures that design caused
# (a compiler that could not find its own crt objects, a wayland-scanner that could
# not find libxml2, and an `ar` linked against a libbfd that only existed inside the
# sysroot) do not exist here to be worked around.
#
# The rule this script exists to keep: *nothing prebuilt by the image may reach the
# artifact*. Everything that ships is either
#   * built here from source by gcc-16 (engine, app, Jolt, zpkg, ztest, and the two
#     vendored trees below), or
#   * a jammy package, and therefore what a jammy user already has (zstd, LuaJIT,
#     libstdc++, libwayland, libxkbcommon).
# That is what makes the published floor a property of the flags rather than of
# whichever machine happened to run the build.
#
# One tree must come from source, for a measured reason:
#   glfw   jammy ships 3.3 and the engine needs 3.4's platform APIs. Vendored, it is
#          static inside libzahlen_engine.so, so the release needs no libglfw on the
#          target machine.
# Slang used to be the second, and is not any more: zshader is a build tool with no
# install rule, so libslang cooks shaders during the build and reaches no package.
# It comes from upstream's prebuilt tarball instead -- see section 3.
#
# The window backend is Wayland and the build refuses to publish without it. GLFW's
# probe degrades silently -- no X11 headers means Wayland, neither means the null
# platform -- so the configure result is asserted below rather than assumed.
#
# The linker is pinned to BFD: not because anything here needs it, but so that a
# mold installed into some future image cannot silently change what the release was
# linked with. See cmake/LinkerConfig.cmake.
#
# No output is discarded here. The CI log is the only place a failure explains
# itself, and nobody reads it until the run is already red -- by which point a
# swallowed error is unrecoverable. The only redirects allowed are existence
# probes (command -v, dpkg -s, grep -q), which have no output worth keeping.
# If something is too noisy to print, make it quieter; do not hide it.
set -Eeuo pipefail

# Every failure names itself. Without this, a step whose output is redirected -- or
# a command that exits 127 because a program is missing -- leaves the log stopping
# at the last unrelated line.
trap 'status=$?; printf "\nerror: exit %s at line %s\n  command: %s\n" \
    "${status}" "${LINENO}" "${BASH_COMMAND}" >&2' ERR

out=${1:-/dist}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
# The pinned slang is built here and its CMake reads the version from git tags,
# which a submodule checkout inside a Docker context does not carry. The workflow
# resolves the tag and passes it; without it slang reports 0.0.0-unknown.
slang_version=${ZHLN_SLANG_VERSION:-}
build=${ZHLN_BUILD_DIR:-build-jammy}
glibc_baseline=${ZHLN_GLIBC_BASELINE:-2.35}

# ------------------------------------------------------- 1. the host is the target
if [ -r /etc/os-release ]; then
    . /etc/os-release
fi
if [ "${ID:-}" != "ubuntu" ] || [ "${VERSION_ID:-}" != "22.04" ]; then
    echo "error: this script builds inside Ubuntu 22.04; this is ${PRETTY_NAME:-an unknown system}." >&2
    echo "       Use docker/release-jammy.Dockerfile. Cross-building against a sysroot" >&2
    echo "       is precisely what this replaced: every failure in that design came" >&2
    echo "       from running another distro's binaries on this one." >&2
    exit 1
fi
host_glibc=$(ldd --version | awk 'NR==1{print $NF}')
echo "== host: ${PRETTY_NAME}, glibc ${host_glibc}, release floor ${glibc_baseline}"
if [ -n "${host_glibc}" ] && [ -n "${glibc_baseline}" ] && [ "${host_glibc}" != "${glibc_baseline}" ]; then
    printf '   note: host glibc and the claimed floor differ; the floor check at the end decides.\n'
fi

CC=${ZHLN_CC:-gcc-16}
CXX=${ZHLN_CXX:-g++-16}
for tool in "${CC}" "${CXX}" wayland-scanner openssl; do
    command -v "${tool}" >/dev/null || {
        echo "error: ${tool} is not installed." >&2
        echo "       scripts/ci-ubuntu-2204-env.sh --build installs the compiler, the" >&2
        echo "       Wayland tools and openssl (jammy ships gcc-11; the tree needs" >&2
        echo "       -freflection, which is why a newer compiler is downloaded at all)." >&2
        exit 1
    }
done
# Both compilers explicitly: the env script points the `gcc` alternative at gcc-16,
# but a configure that resolves `c++` on its own would silently pick jammy's g++-11
# and fail much later, on -freflection.
# A *released* GCC 16, not the trunk snapshot in jammy's PPA. `16-20260315` is the
# only 16 apt offers for this distro and it rejects Jolt's
# `static const StaticArray<Vec3, 1026>` member -- instantiated while Vec3 is still
# incomplete -- which stops the build in the PCH of three targets. Released 14.2 and
# 16.2 both defer that instantiation, so the version floor is the cheap detector;
# scripts/install-gcc-prebuilt.sh compiles the construct as a self-test as well.
GCC_MIN=${ZHLN_GCC_MIN:-16.1.1}
cxx_version=$("${CXX}" -dumpfullversion -dumpversion 2>/dev/null | head -1)
if [ -z "${cxx_version}" ] || ! printf '%s\n%s\n' "${GCC_MIN}" "${cxx_version}" | sort -V -C; then
    {
        echo "error: ${CXX} is ${cxx_version:-unversioned}; this release needs >= ${GCC_MIN}."
        echo "       Older 16.x is a trunk snapshot that cannot compile Jolt; see"
        echo "       scripts/install-gcc-prebuilt.sh for the pinned download."
    } >&2
    exit 1
fi
# The assembler is half of the compiler pair, and the half jammy gets wrong: GCC 16
# emits `.base64` for large data blobs, GAS only learned that directive in 2.41, and
# jammy ships 2.38. Passing the driver a snippet that triggers it costs milliseconds
# and fails here instead of 300 files into the build.
as_bin=$("${CXX}" -print-prog-name=as 2>/dev/null || true)
as_probe=$(mktemp)
printf '\t.base64 "EjRWeJA="\n' >"${as_probe}.s"
if ! "${as_bin}" "${as_probe}.s" -o "${as_probe}.o" >/dev/null 2>&1; then
    {
        echo "error: ${as_bin:-no assembler} cannot assemble .base64, which GCC 16 emits." >&2
        echo "       GAS needs to be 2.41 or newer; jammy ships 2.38. scripts/" >&2
        echo "       install-gcc-prebuilt.sh installs a matching binutils." >&2
    } >&2
    rm -f "${as_probe}.s" "${as_probe}.o"
    exit 1
fi
rm -f "${as_probe}.s" "${as_probe}.o"
echo "== assembler: $("${as_bin}" --version | head -1) (${as_bin})"
echo "== compiler: $("${CXX}" --version | head -1)"
echo "== c compiler: $("${CC}" --version | head -1)"
echo "== wayland-scanner: $(wayland-scanner --version 2>&1 | head -1)"
echo "== linker: $(ld --version | head -1)"
# CMake refuses to generate build files for C++20 modules with ninja < 1.11, and this
# tree has 28 module units -- so a distro ninja is not good enough. Fail here, before
# six minutes of configure, if the host has the wrong one.
ninja_version=$(ninja --version 2>&1 | head -1)
if [ -z "${ninja_version}" ] || [ "$(printf '%s\n1.11\n' "${ninja_version}" | sort -V | head -1)" != "1.11" ]; then
    echo "error: ninja ${ninja_version:-not found} cannot generate C++20 module builds." >&2
    echo "       CMake needs 1.11 or newer. scripts/ci-ubuntu-2204-env.sh installs a" >&2
    echo "       downloaded one into /usr/local/bin (jammy has 1.10.1)." >&2
    exit 1
fi
echo "== ninja: ${ninja_version} ($(command -v ninja))"

# lavapipe, for the test suite below. The ICD filename is not portable (Debian and
# Ubuntu suffix it with the architecture), so find it rather than hardcode it.
if [ -z "${VK_DRIVER_FILES:-}" ]; then
    for candidate in /usr/share/vulkan/icd.d/lvp_icd*.json; do
        [ -f "${candidate}" ] || continue
        export VK_DRIVER_FILES=${candidate}
        export VK_ICD_FILENAMES=${candidate}
        break
    done
fi
echo "== lavapipe ICD: ${VK_DRIVER_FILES:-none found}"

cd "${CMAKE_SOURCE_DIR:-/workspace}"

# ------------------------------------------------------------- 2. git ownership
# The container runs as root and the mounted checkout belongs to the runner, so git
# refuses every repository in the tree:
#
#   fatal: detected dubious ownership in repository at '/workspace/extern/slang'
#
# That reads like noise from Slang's optional "user skills" step. It is not: Slang's
# CMake derives SLANG_VERSION_NUMERIC and SLANG_VERSION_FULL from `git describe`
# (cmake/GitVersion.cmake, before project()), so when git refuses, those variables
# come out empty and every set_target_properties in Slang is handed one argument
# fewer than it expects:
#
#   CMake Error at extern/slang/source/slang-rt/CMakeLists.txt:20 (set_target_properties):
#     set_target_properties called with incorrect number of arguments.
#
# which is what a whole round was spent on. The wildcard form of safe.directory needs
# a newer git than jammy's 2.34, so each repository is marked by path.
while IFS= read -r gitpath; do
    git config --global --add safe.directory "$(cd "$(dirname "${gitpath}")" && pwd)" || true
done < <(find . -maxdepth 6 -name .git -print 2>/dev/null | sort -u)
# Prove it, in one line, instead of letting Slang's CMake fail with a message that
# names a file nobody has open.
if [ -d extern/slang/.git ] || [ -f extern/slang/.git ]; then
    slang_git=$(git -C extern/slang rev-parse --short HEAD 2>&1) || {
        echo "error: git cannot read extern/slang: ${slang_git}" >&2
        echo "       Slang takes its version from git describe; without it the configure" >&2
        echo "       dies in set_target_properties with a misleading argument-count error." >&2
        exit 1
    }
    # A shallow submodule has no tags, so `git describe` exits 128 here and that is
    # fine -- it must not look like a failure, which is exactly what it did when this
    # was a command substitution inside an echo under `set -o pipefail`.
    if slang_tag=$(git -C extern/slang describe --tags --abbrev=0 2>&1); then
        echo "== extern/slang at ${slang_git} (${slang_tag})"
    else
        echo "== extern/slang at ${slang_git} (no tag in this checkout)"
    fi
fi

# -------------------------------------------------------------------- 3. slang
# Slang is a build-host dependency, not a released one: utils/zshader is a build tool
# with no install rule, so it cooks shaders with slangc and nothing it links reaches a
# package. Building the pinned extern/slang tree therefore costs ~25 minutes of every
# run that is not a ccache hit to produce two things nobody ships.
#
# The prebuilt comes from docker/release-jammy.Dockerfile, which installs upstream's
# release tarball -- built against glibc 2.17, measured -- plus the CMake config
# upstream does not ship (scripts/install-slang-prebuilt.sh). -DZHLN_SLANG_VENDORED=ON
# builds the pinned tree instead, for anyone who wants the exact submodule revision.
slang_mode=${ZHLN_SLANG_VENDORED:-OFF}
slang_prefix=${ZHLN_SLANG_PREFIX:-/opt/slang}
slang_args=()
if [ "${slang_mode}" = "ON" ]; then
    echo "== slang: building the pinned extern/slang tree (ZHLN_SLANG_VENDORED=ON)"
    slang_args+=(-DZHLN_SLANG_VENDORED=ON)
    if [ -n "${slang_version}" ]; then
        slang_args+=(-DSLANG_VERSION_FULL="${slang_version}")
    fi
else
    [ -x "${slang_prefix}/bin/slangc" ] || {
        echo "error: no prebuilt slangc at ${slang_prefix}/bin/slangc." >&2
        echo "       scripts/install-slang-prebuilt.sh installs it; the release image" >&2
        echo "       does that at build time. Or set ZHLN_SLANG_VENDORED=ON to build" >&2
        echo "       the pinned tree instead." >&2
        exit 1
    }
    [ -f "${slang_prefix}/lib/cmake/slang/slang-config.cmake" ] || {
        echo "error: ${slang_prefix} has slangc but no CMake config package." >&2
        echo "       cmake/ShaderCompilation.cmake resolves Slang with" >&2
        echo "       find_package(slang CONFIG); re-run scripts/install-slang-prebuilt.sh." >&2
        exit 1
    }
    echo "== slang: prebuilt $("${slang_prefix}/bin/slangc" -version 2>&1 | head -1) at ${slang_prefix}"
    slang_args+=(-DCMAKE_PREFIX_PATH="${slang_prefix}" -DSLANG_EXECUTABLE="${slang_prefix}/bin/slangc")
fi

# ---------------------------------------------------------------- 2. ccache
# ccache is why a re-run costs minutes instead of an engine rebuild. The cache
# directory is a bind mount, so it survives the container; the workflow restores and
# saves it across runs. BASEDIR + NOHASHDIR are what make the cache usable at all
# here: the sources are at /workspace inside the container and somewhere else on the
# runner, and the build directory name is not part of a translation unit.
#
# ccache's boolean environment variables have a rule that is easy to get wrong: from
# the environment, "0", "false", "no" and "disable" are *errors*, and every other
# value means true. So `CCACHE_NODIRECT=0` -- "direct mode off" in the author's head,
# an abort of every invocation in ccache's -- and `CCACHE_CPP2=0` right behind it.
# Neither setting had any business being there: direct mode on and CPP2 off are the
# defaults, and the fix for a setting with no business being set is to stop setting
# it.
export CCACHE_DIR=${CCACHE_DIR:-/ccache}
export CCACHE_BASEDIR=${CCACHE_BASEDIR:-${PWD}}
export CCACHE_NOHASHDIR=true
export CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-3G}
export CCACHE_COMPILERCHECK=${CCACHE_COMPILERCHECK:-content}
ccache_launcher=()
if [ -d "${CCACHE_DIR}" ] && command -v ccache >/dev/null; then
    # Not `|| true`: caching is a requirement of this lane, and a ccache that cannot
    # even read its own environment must fail here, in one second, rather than turn
    # into a mysteriously slow build (or a wall of failing compiles).
    ccache --set-config=max_size="${CCACHE_MAXSIZE}"
    echo "== ccache: ${CCACHE_DIR} (max ${CCACHE_MAXSIZE}), stats before this build:"
    ccache --show-stats
    ccache_launcher=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
else
    echo "warning: ccache not available; this build will not reuse anything" >&2
fi

# ------------------------------------------------------- 3. toolchain probe
# Compile, archive and link a probe before the engine build starts, and check the
# result against the floor. Cheap, and it means a broken toolchain costs seconds
# instead of an hour.
probe_dir=${build}-probe
mkdir -p "${probe_dir}"
cat >"${probe_dir}/probe.cpp" <<'EOF'
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
int main() {
    std::vector<std::string> v {"zahlen"};
    std::printf("%s %s\n", v[0].c_str(), std::filesystem::current_path().c_str());
    return 0;
}
EOF
echo "== toolchain probe: compile, archive and link"
"${CXX}" -std=c++17 -O2 -c "${probe_dir}/probe.cpp" -o "${probe_dir}/probe.o"
ar rcs "${probe_dir}/libprobe.a" "${probe_dir}/probe.o"
"${CXX}" -std=c++17 -O2 "${probe_dir}/probe.cpp" "${probe_dir}/libprobe.a" -o "${probe_dir}/probe"
"${script_dir}/check-glibc-floor.sh" "${glibc_baseline}" "${probe_dir}/probe" || {
    echo "error: a probe built by this toolchain does not meet the glibc baseline." >&2
    exit 1
}
# Run it, not just link it. The engine's own build tools (zshader, zpkg) are built by
# this compiler and started by the build, so a toolchain whose runtime the host
# cannot load fails at the first cooked shader -- 90 steps in, which is where this
# was found. One execution here turns that into a five-second failure.
probe_out=$("${probe_dir}/probe" 2>&1) || {
    {
        echo "error: a probe built by this toolchain does not run on this host." >&2
        printf '%s\n' "${probe_out}" >&2
        echo "       Programs from this compiler need its runtime (GLIBCXX_3.4.36," >&2
        echo "       GCC_13.0.0); jammy ships older ones. scripts/install-gcc-prebuilt.sh" >&2
        echo "       registers ${ZHLN_GCC_PREFIX:-/opt/gcc16}/lib with ldconfig." >&2
    } >&2
    exit 1
}
ldconfig_bin=$(command -v ldconfig || printf '/sbin/ldconfig')
libstdcxx_in_use=$("${ldconfig_bin}" -p 2>/dev/null | awk '/libstdc\+\+\.so\.6 /{print $NF; exit}' || true)
echo "== probe: ${probe_out}"
echo "== loader libstdc++: ${libstdcxx_in_use:-unknown}"

# ccache, verified instead of assumed: two identical compiles, and the second must
# be a hit. A ccache that is installed but not caching is indistinguishable from a
# working one until you time the build.
if [ -d "${CCACHE_DIR}" ] && command -v ccache >/dev/null; then
    ccache_probe=${build}-ccache-probe
    mkdir -p "${ccache_probe}"
    printf 'int zahlen_ccache_probe;\n' >"${ccache_probe}/probe.c"
    compile_ccache_probe() {
        CCACHE_LOGFILE="${ccache_probe}/log" \
            ccache "${CC}" -O2 -c "${ccache_probe}/probe.c" -o "${ccache_probe}/probe.o"
    }
    compile_ccache_probe
    : >"${ccache_probe}/log"
    compile_ccache_probe
    if grep -q 'Result: .*cache_hit' "${ccache_probe}/log"; then
        echo "== ccache: verified here (the second identical compile was a hit)"
    else
        echo "error: ccache is not caching -- the second identical compile missed." >&2
        sed -n 's/^/  ccache: /p' "${ccache_probe}/log" | tail -20 >&2
        exit 1
    fi
    rm -rf "${ccache_probe}"
fi
rm -rf "${probe_dir}"

# --------------------------------------------------------------- 4. simdjson
# ZHLN_INSTALL_TOOLCHAIN_PACKAGES=ON requires a simdjson *CONFIG* package (see
# cmake/Toolchains.cmake:20: the vendored single-header fallback is not exportable),
# and jammy has no simdjson at all. Build the pinned tree with the native compiler
# and install it into the system prefix -- exactly what a jammy user gets from
# building it, and it keeps the header-only nature of the dependency (nothing of it
# ships in the artifact; only the export needs to resolve).
simdjson_prefix=/usr/local
simdjson_cmakedir=
for dir in "${simdjson_prefix}/lib/cmake/simdjson" \
    "${simdjson_prefix}/lib/x86_64-linux-gnu/cmake/simdjson" \
    "${simdjson_prefix}/share/cmake/simdjson"; do
    [ -f "${dir}/simdjson-config.cmake" ] && simdjson_cmakedir=${dir} && break
done
if [ -z "${simdjson_cmakedir}" ]; then
    echo "== building simdjson into ${simdjson_prefix}"
    [ -f extern/simdjson/CMakeLists.txt ] || {
        echo "error: extern/simdjson is not checked out." >&2
        echo "       It is a submodule; fetch it with --recursive." >&2
        exit 1
    }
    cmake -S extern/simdjson -B "${build}-simdjson" -G Ninja \
        -DCMAKE_C_COMPILER="${CC}" -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DSIMDJSON_DEVELOPER_MODE=OFF \
        -DSIMDJSON_BUILD_STATIC=ON \
        -DBUILD_SHARED_LIBS=OFF \
        "${ccache_launcher[@]}"
    cmake --build "${build}-simdjson"
    cmake --install "${build}-simdjson" --prefix "${simdjson_prefix}"
    for dir in "${simdjson_prefix}/lib/cmake/simdjson" \
        "${simdjson_prefix}/lib/x86_64-linux-gnu/cmake/simdjson" \
        "${simdjson_prefix}/share/cmake/simdjson"; do
        [ -f "${dir}/simdjson-config.cmake" ] && simdjson_cmakedir=${dir} && break
    done
    [ -n "${simdjson_cmakedir}" ] || {
        echo "error: simdjson installed but no simdjson-config.cmake was produced." >&2
        exit 1
    }
fi
# The version literal lives in the generated version file, not the config.
simdjson_version=$(sed -n 's/^set(PACKAGE_VERSION "\([^"]*\)".*/\1/p' \
    "${simdjson_cmakedir}/simdjson-config-version.cmake" | head -1)
echo "== simdjson: ${simdjson_version:-unknown} CONFIG package in ${simdjson_cmakedir}"

# ------------------------------------------------------------- 5. configure
echo "== configuring (native ${PRETTY_NAME:-jammy}, floor glibc ${glibc_baseline})"
if [ "${slang_mode}" = "ON" ]; then
    if [ -n "${slang_version}" ]; then
        echo "== vendored slang version: ${slang_version}"
    else
        echo "warning: no SLANG_VERSION_FULL given; slang will report 0.0.0-unknown" >&2
    fi
fi
cmake -B "${build}" -S . -G Ninja \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DZHLN_BUILD_TESTS=ON \
    -DZHLN_INSTALL_TOOLCHAIN_PACKAGES=ON \
    -DUSE_SANITIZERS=OFF \
    -DZHLN_HAS_BLENDER=OFF \
    -DZHLN_TEST_VULKAN_SANDBOX=OFF \
    -DZHLN_GLFW_VENDORED=ON \
    "${slang_args[@]}" \
    -DWAYLAND_SCANNER_EXECUTABLE="$(command -v wayland-scanner)" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DZHLN_LINKER_OVERRIDE=bfd \
    "${ccache_launcher[@]}" \
    -DVulkan_INCLUDE_DIR="${PWD}/extern/Vulkan-Headers/include" \
    -DZHLN_BUILD_SVG=OFF
# Why each of the flags that is not obvious is here:
#   ZHLN_SLANG_VENDORED / ZHLN_GLFW_VENDORED   see the header: jammy has no slang,
#       and its GLFW is 3.3 where the engine needs 3.4's platform APIs.
#   CMAKE_POSITION_INDEPENDENT_CODE            the vendored GLFW is static and gets
#       folded into libzahlen_engine.so, so its objects have to be PIC.
#   WAYLAND_SCANNER_EXECUTABLE                 explicit rather than found: GLFW
#       invokes the scanner through execute_process, where a program that cannot run
#       does not fail the configure -- it surfaces later as missing generated headers.
#   ZHLN_LINKER_OVERRIDE=bfd                   determinism; see the header.
#   Vulkan_INCLUDE_DIR        jammy deliberately has no libvulkan-dev: its headers
#       are 1.3 and the renderer targets 1.4, so the vendored headers are the only
#       correct ones. Without this, find_package(Vulkan) fails and
#       cmake/ThirdParty.cmake's Jolt block -- which seeds Vulkan_GLSLC_EXECUTABLE
#       with dxc -- is skipped, leaving every Jolt shader rule to run a program named
#       "Vulkan_GLSLC_EXECUTABLE-NOTFOUND". Naming the vendored headers is enough to
#       make Vulkan_FOUND true; the loader is a NEEDED soname the consumer supplies.
#   ZHLN_BUILD_SVG=OFF        resvg is not packaged for jammy and building it needs
#       cargo; without this the configure drops the plugin with a warning on every
#       run. It is not one of the 16 released components.
#   SLANG_VERSION_FULL        the pinned slang is built here, and its CMake can only
#       read its version from git tags -- which a submodule checkout in a Docker
#       context does not have. See the workflow, which passes the tag.

# A release must carry Wayland. This is the assertion that keeps a silent
# degradation to X11-only -- or to no window backend at all -- from being published.
# It fails here, in the cheap place, naming the reason.
# A CMake BOOL cache entry is whatever was assigned to it -- TRUE from a set(...)
# with a boolean variable, ON from -D on the command line -- and both mean the same
# thing. Comparing the raw string to "ON" is how a correct configure (wayland=TRUE,
# x11=FALSE) was reported as "did not enable Wayland", which is a red build spent on
# a spelling.
cmake_bool() {
    case $(printf '%s' "${1:-}" | tr '[:lower:]' '[:upper:]') in
        ON | TRUE | 1 | YES | Y) printf 'ON\n' ;;
        OFF | FALSE | 0 | NO | N) printf 'OFF\n' ;;
        '') printf 'unset\n' ;;
        *) printf '%s\n' "${1}" ;;
    esac
}
wayland_raw=$(sed -n 's/^GLFW_BUILD_WAYLAND:BOOL=//p' "${build}/CMakeCache.txt")
x11_raw=$(sed -n 's/^GLFW_BUILD_X11:BOOL=//p' "${build}/CMakeCache.txt")
wayland_backend=$(cmake_bool "${wayland_raw}")
x11_backend=$(cmake_bool "${x11_raw}")
echo "== GLFW backends from this configure: wayland=${wayland_backend} x11=${x11_backend}" \
    "(raw: ${wayland_raw:-absent} / ${x11_raw:-absent})"
if [ "${x11_backend}" = "ON" ]; then
    echo "error: this release build has GLFW's X11 backend enabled." >&2
    echo "       The release is Wayland-only by design, and the image installs no X11" >&2
    echo "       development packages (scripts/ci-ubuntu-2204-env.sh --build). If they" >&2
    echo "       appeared, find out what pulled them in." >&2
    exit 1
fi
if [ "${wayland_backend}" != "ON" ]; then
    echo "error: this configure did not enable GLFW's Wayland backend." >&2
    echo "       Found GLFW_BUILD_WAYLAND=${wayland_raw:-<absent>} in ${build}/CMakeCache.txt," >&2
    echo "       which does not mean enabled." >&2
    echo "       Check that libwayland-dev, libxkbcommon-dev, wayland-protocols and" >&2
    echo "       libwayland-bin are installed, and that" >&2
    echo "       $(command -v wayland-scanner) runs." >&2
    exit 1
fi

echo "== building"
cmake --build "${build}"

# zpkg ships to the consumer, so it is built for the same target. It links libstdc++
# statically (see utils/zpkg/CMakeLists.txt), so it runs on a jammy machine whose
# libstdc++ is older than the PPA compiler's.
echo "== building and testing zpkg"
cmake -S utils/zpkg -B "${build}-zpkg" -G Ninja \
    -DCMAKE_C_COMPILER="${CC}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_BUILD_TYPE=Release -DZPKG_BUILD_TESTS=ON \
    "${ccache_launcher[@]}"
cmake --build "${build}-zpkg"
ctest --test-dir "${build}-zpkg" --output-on-failure

# The bytes published are these bytes: 2.35 code runs on anything newer, so running
# the suite here is a real smoke test of the release.
echo "== running ztest on this host's lavapipe"
ztest_log="${out}/ztest.log"
mkdir -p "${out}"
if ! "${build}/bin/ztest" --test-dir "${build}" -V 2>&1 | tee "${ztest_log}"; then
    # A worker crash is reported as "Stack trace (2 frames)" ending in __sigaction:
    # ztest's worker installs its own handler, and the handler cannot unwind a stack
    # that is the thing being corrupted. The framework's own answer is
    # `--allow-debugger`, which needs a terminal and therefore cannot be used here;
    # this is the same replay without the prompt. Each case is selected by node ID
    # and run without --ztest-worker, so the signal reaches gdb instead of the
    # handler and the frames below the fault are real.
    echo "== ztest failed; replaying the crashed cases under gdb"
    echo "   (the worker's crash handler is what truncates its own traces)"
    # ztest colours its output even when stdout is a pipe, so the escape sequences
    # have to go before the lines can be matched: without this the parser finds
    # nothing and reports "no crashed cases" about a log full of them.
    crashed=$(sed -e 's/\x1b\[[0-9;]*m//g' "${ztest_log}" | sed -n 's/^  \[ CRASH \] \([^ ]*\)$/\1/p' | sort -u)
    if [ -z "${crashed}" ]; then
        echo "warning: no crashed cases were named in the log; nothing to replay" >&2
    elif ! command -v gdb >/dev/null 2>&1; then
        echo "warning: gdb is not installed; scripts/ci-ubuntu-2204-env.sh --build installs it" >&2
    else
        # A case that only crashes next to its neighbours is itself the finding, so
        # a serial run of just the failures comes first: if that is clean, the crash
        # needs the parallel session and the per-case replay below would say nothing.
        if "${build}/bin/ztest" --test-dir "${build}" --lf --jobs 1 2>&1 | tail -n 20; then
            echo "note: the failures did not reproduce serially -- they need the parallel session"
        fi
        # Bounded: six traces are enough to tell one bug from two, and a wedged case
        # must not hold the runner. The list goes through a file rather than a pipe,
        # because `printf | head` would SIGPIPE the writer and pipefail would end the
        # script with 141 before the first trace. follow-fork-mode covers a replay that
        # still shards into a worker; with a single node it is a no-op.
        printf '%s\n' "${crashed}" > "${out}/crashed-cases.txt"
        head -n 6 "${out}/crashed-cases.txt" > "${out}/crashed-cases-replayed.txt"
        while read -r case_id; do
            echo
            echo "== gdb: ${case_id}"
            timeout 180 gdb -batch -nx \
                -ex 'set pagination off' \
                -ex 'set confirm off' \
                -ex 'set follow-fork-mode child' \
                -ex 'set detach-on-fork on' \
                -ex 'handle SIGPIPE nostop noprint pass' \
                -ex run \
                -ex 'echo \n== backtrace\n' \
                -ex 'bt 40' \
                -ex 'info registers rip rsp rbp' \
                -ex 'x/6i $pc-16' \
                --args "${build}/bin/ztest" --test-dir "${build}" "${case_id}" 2>&1 | tail -n 90 \
                || echo "   (no backtrace: the case did not fault this time)"
        done < "${out}/crashed-cases-replayed.txt"
    fi
    exit 1
fi

echo "== packaging"
ZPKG_BIN="${build}-zpkg/zpkg" scripts/package-toolchain.sh "${out}" "${build}"

# The floor is a property of the linked symbol versions, not of the build host. Both
# the packages and the loose tools are checked, because zpkg is a user-facing binary
# that ships outside the package set.
echo "== checking the glibc floor of everything that ships"
scripts/check-glibc-floor.sh "${glibc_baseline}" "${out}/packages" "${out}/tools"

# For the release notes: what a reader needs to know to run this, recorded where the
# publishing step can read it rather than inferred there.
printf 'wayland=%s x11=%s\n' "${wayland_backend}" "${x11_backend:-off}" >"${out}/window-backends.txt"

printf '\nrelease built on %s for glibc %s\n' "${PRETTY_NAME:-jammy}" "${glibc_baseline}"
printf 'window stack: wayland=%s x11=%s (glfw vendored, static)\n' \
    "${wayland_backend}" "${x11_backend:-off}"
