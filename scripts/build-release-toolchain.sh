#!/usr/bin/env bash
# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build the release toolchain inside the project's Arch builder image, linked
# against the Ubuntu 22.04 sysroot instead of the container's own glibc.
#
#   docker run --rm \
#       -v "$PWD/.jammy:/opt/jammy" -v "$PWD/dist:/dist" \
#       zahlen-builder:release bash scripts/build-release-toolchain.sh /dist
#
# The rule this script exists to keep: *nothing the image prebuilt may reach the
# artifact*. The container is only where the build-time dependencies are cheap --
# cmake, ninja, python, dxc (Jolt's HLSL passes) and a Vulkan loader for the
# tests. Every library that ends up inside a package, or inside the NEEDED list
# of one, is either
#   * built here from source by the sysroot's g++-16 (engine, app, Jolt, zpkg,
#     ztest, and the two vendored trees below), or
#   * installed in the sysroot and therefore jammy's own (zstd, LuaJIT, libc,
#     libstdc++, X11/Wayland for GLFW).
# That is what makes the published floor a property of the flags rather than of
# the machine that happened to run the build.
#
# Two trees must come from source, and both for a *measured* reason:
#   slang  zshader links libslang in-process, so it is a runtime dependency of a
#          released tool. The image's slangc/libslang are Arch binaries (the
#          image's own engine measures GLIBC_2.43) and jammy has no slang at all.
#   glfw   jammy ships 3.3 and the engine needs 3.4's platform APIs, so the image's
#          3.5 would be linked instead -- as an Arch-built shared library the
#          consumer does not have. Vendored, it is static inside libzahlen_engine
#          and the release needs no libglfw on the target machine.
#
# The window backend is Wayland, and the build refuses to publish without it. GLFW
# generates its protocol bindings with wayland-scanner at build time, and that
# tool must be the *target's* (jammy's 1.20): the bindings it emits are compiled
# against the sysroot's libwayland, and a newer host scanner can generate calls a
# 1.20 client library does not export. It is also the step that silently degrades
# -- with no scanner on PATH and no way to reach the sysroot's, GLFW drops its
# Wayland backend and builds X11 instead, which is a release that cannot open a
# window on a modern desktop. So the scanner is passed explicitly, and the
# configure result is checked.
#
# The linker is pinned to BFD for the same class of reason; see the note at the
# configure call.
set -euo pipefail

out=${1:-/dist}
sysroot=${ZHLN_JAMMY_SYSROOT:-/opt/jammy}
build=${ZHLN_BUILD_DIR:-build-jammy}
glibc_baseline=${ZHLN_GLIBC_BASELINE:-2.35}

[ -f "${sysroot}/jammy-target.cmake" ] || {
    echo "error: ${sysroot} has no jammy-target.cmake." >&2
    echo "       Run scripts/setup-jammy-target.sh inside ubuntu:22.04 first." >&2
    exit 1
}
command -v openssl >/dev/null || {
    echo "error: openssl is required to sign the release" >&2
    exit 1
}

cd "${CMAKE_SOURCE_DIR:-/workspace}"

# The target's own scanner, verified here rather than trusted: -D skips CMake's
# find_program, and GLFW runs the scanner through execute_process, which does not
# fail the configure when the program is missing -- it fails later, as a missing
# Wayland backend. Cheap to check first.
wayland_scanner=${sysroot}/usr/bin/wayland-scanner
if [ ! -x "${wayland_scanner}" ]; then
    echo "error: ${wayland_scanner} is missing or not executable." >&2
    echo "       The sysroot needs libwayland-bin (scripts/setup-jammy-target.sh" >&2
    echo "       installs it); without a scanner GLFW would build X11-only." >&2
    exit 1
fi
echo "== wayland-scanner: $("${wayland_scanner}" --version 2>&1 | head -1) (from the sysroot, not the host)"

# jammy names its lavapipe manifest lvp_icd.x86_64.json, Arch names it lvp_icd.json.
# The tests run here on whatever this host has; the consumer job does the same
# lookup after installing the distro's drivers.
icd=
for candidate in /usr/share/vulkan/icd.d/lvp_icd*.json; do
    [ -f "${candidate}" ] && { icd=${candidate}; break; }
done
if [ -z "${icd}" ]; then
    echo "error: no lavapipe ICD in this image; ztest cannot create a device" >&2
    ls -la /usr/share/vulkan/icd.d/ >&2 || true
    exit 1
fi
export VK_DRIVER_FILES="${icd}"
export VK_ICD_FILENAMES="${icd}"
echo "== lavapipe ICD: ${icd}"

echo "== configuring (target: glibc ${glibc_baseline} via ${sysroot})"
cmake -B "${build}" -S . -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${sysroot}/jammy-target.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DZHLN_BUILD_TESTS=ON \
    -DZHLN_INSTALL_TOOLCHAIN_PACKAGES=ON \
    -DUSE_SANITIZERS=OFF \
    -DZHLN_HAS_BLENDER=OFF \
    -DZHLN_TEST_VULKAN_SANDBOX=OFF \
    -DZHLN_SLANG_VENDORED=ON \
    -DZHLN_GLFW_VENDORED=ON \
    -DWAYLAND_SCANNER_EXECUTABLE="${wayland_scanner}" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DZHLN_LINKER_OVERRIDE=bfd
# The last three are the sysroot's fault, not this project's:
#   ZHLN_SLANG_VENDORED / ZHLN_GLFW_VENDORED   see the header.
#   CMAKE_POSITION_INDEPENDENT_CODE            the vendored GLFW is static and
#       gets folded into libzahlen_engine.so, so its objects have to be PIC.
#   ZHLN_LINKER_OVERRIDE=bfd                   cmake/LinkerConfig.cmake would pick
#       mold here, and mold cannot parse the "INPUT ( AS_NEEDED ( -latomic ) )"
#       script that ships in GCC 16's own lib directory:
#         mold: fatal: <sysroot>/usr/lib/gcc/x86_64-linux-gnu/16/
#                      libatomic_asneeded.so:3: library not found: AS_NEEDED
#       The probe cannot see that, because it only asks whether the compiler can
#       drive mold. Pin what the sysroot can actually be linked with.

# A release must carry Wayland. This is the assertion that keeps a silent
# degradation to X11-only from being published: it fails here, in the cheap place,
# naming the reason.
wayland_backend=$(sed -n 's/^GLFW_BUILD_WAYLAND:BOOL=//p' "${build}/CMakeCache.txt")
x11_backend=$(sed -n 's/^GLFW_BUILD_X11:BOOL=//p' "${build}/CMakeCache.txt")
echo "== GLFW backends from this configure: wayland=${wayland_backend:-unset} x11=${x11_backend:-unset}"
if [ "${wayland_backend}" != "ON" ]; then
    echo "error: this configure did not enable GLFW's Wayland backend." >&2
    echo "       Expected GLFW_BUILD_WAYLAND=ON in ${build}/CMakeCache.txt." >&2
    echo "       Check that the sysroot has libwayland-dev, libxkbcommon-dev," >&2
    echo "       wayland-protocols and libwayland-bin, and that" >&2
    echo "       WAYLAND_SCANNER_EXECUTABLE=${wayland_scanner} runs." >&2
    exit 1
fi

echo "== building"
cmake --build "${build}"

# zpkg ships to the consumer, so it is built for the same target: it links
# libstdc++ statically (see utils/zpkg/CMakeLists.txt) and would otherwise need
# this container's newer glibc on the user's machine.
echo "== building and testing zpkg"
cmake -S utils/zpkg -B "${build}-zpkg" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${sysroot}/jammy-target.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DZPKG_BUILD_TESTS=ON
cmake --build "${build}-zpkg"
ctest --test-dir "${build}-zpkg" --output-on-failure

# The bytes published are these bytes: 2.35 code runs on anything newer, so
# running the suite in this container is a real smoke test of the release.
echo "== running ztest on this host's lavapipe"
"${build}/bin/ztest" --test-dir "${build}" -V

echo "== packaging"
ZPKG_BIN="${build}-zpkg/zpkg" scripts/package-toolchain.sh "${out}" "${build}"

# The floor is a property of the linked symbol versions, not of the build host.
# Both the packages and the loose tools are checked, because zpkg is a user-facing
# binary that ships outside the package set.
echo "== checking the glibc floor of everything that ships"
scripts/check-glibc-floor.sh "${glibc_baseline}" "${out}/packages" "${out}/tools"

# For the release notes: what a reader needs to know to run this, recorded where
# the publishing step can read it rather than inferred there.
printf 'wayland=%s x11=%s\n' "${wayland_backend}" "${x11_backend:-off}" >"${out}/window-backends.txt"

printf '\nrelease toolchain built against glibc %s\n' "${glibc_baseline}"
printf 'window stack: wayland=%s x11=%s (glfw vendored, static)\n' \
    "${wayland_backend}" "${x11_backend:-off}"
