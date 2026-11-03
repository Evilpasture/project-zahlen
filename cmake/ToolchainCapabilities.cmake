# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/ToolchainCapabilities.cmake
#
# Behavioural compiler probes.
#
# CompilerFlagProbe.cmake answers "does the driver accept this flag". This module
# answers the question after it: "does the compiler, invoked the way this build
# invokes it, produce what the tree needs".
#
# Both probes used to be shell. scripts/install-gcc-prebuilt.sh compiled them
# while validating a download and scripts/build-release-toolchain.sh compiled
# them again before a release build, so the answer existed on the two machines
# that ran those scripts and nowhere else: anyone pointing CMake at the
# ubuntu-toolchain-r/test `gcc-16`, or at a clang-p2996 fork, was told the
# configuration was fine and then met an incomplete-type error in the Jolt
# precompiled header minutes later.
#
# Neither probe runs a compiled program. Parts of this tree are cross-compiled
# (MinGW, MSVC), and a check whose answer depends on the host executing the
# target's output is the wrong check -- which is why `try_run` appears nowhere in
# this tree.
#
# ZHLN_SKIP_TOOLCHAIN_CAPABILITY_PROBES=ON configures without the probes. It is
# there for a toolchain that is known to work but cannot be probed, not as a way
# past a failure the probes are reporting correctly.

include_guard(GLOBAL)
include(CheckCXXSourceCompiles)

# zhln_probe_toolchain_capabilities(<reflection flags>)
#
# <reflection flags> is the flag string this build passes for C++26 reflection:
# "-freflection" for GCC, and for a clang-p2996 fork whatever ReflectionConfig
# settled on (possibly with -fparameter-reflection or -freflection-latest). Pass
# "" when reflection comes from the transpiler fallback: the probes then do not
# run at all, because they would be testing a compiler mode this build is not
# using, and a fallback build is legitimate on a compiler that fails them.
function(zhln_probe_toolchain_capabilities REFLECTION_FLAGS)
    if(ZHLN_SKIP_TOOLCHAIN_CAPABILITY_PROBES)
        message(STATUS "toolchain probes: skipped (ZHLN_SKIP_TOOLCHAIN_CAPABILITY_PROBES)")
        return()
    endif()
    if(NOT REFLECTION_FLAGS)
        message(STATUS "toolchain probes: skipped (reflection comes from the transpiler)")
        return()
    endif()

    # check_cxx_source_compiles caches its verdict, and a verdict from a different
    # flag string is the one stale answer that would let the wrong compiler pass.
    if(DEFINED ZHLN_CAPABILITY_PROBE_FLAGS AND NOT ZHLN_CAPABILITY_PROBE_FLAGS STREQUAL REFLECTION_FLAGS)
        unset(ZHLN_COMPILER_HANDLES_INCOMPLETE_STATIC_MEMBER CACHE)
        unset(ZHLN_COMPILER_EMITS_ASSEMBLABLE_BASE64 CACHE)
    endif()
    set(ZHLN_CAPABILITY_PROBE_FLAGS "${REFLECTION_FLAGS}" CACHE INTERNAL
        "reflection flags the toolchain capability probes ran with")

    set(_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    set(_saved_required_includes "${CMAKE_REQUIRED_INCLUDES}")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} ${REFLECTION_FLAGS}")

    # ------------------------------------------------------------- Jolt ------
    # The construct trunk GCC 16 rejects: a static data member whose type is a
    # class template instantiated while the element type is still incomplete.
    # extern/JoltPhysics declares exactly that -- `static const
    # StaticArray<Vec3, 1026> sUnitSphere;` inside Vec3 -- and depends on the
    # instantiation being deferred. Released GCC 14 and 16 defer it; the
    # ubuntu-toolchain-r/test snapshot of 16.x does not. This is the probe that
    # makes the compiler version floor a fact rather than a version string.
    check_cxx_source_compiles("
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
int main() { return 0; }"
        ZHLN_COMPILER_HANDLES_INCOMPLETE_STATIC_MEMBER)
    if(NOT ZHLN_COMPILER_HANDLES_INCOMPLETE_STATIC_MEMBER)
        # CMake 3.31 prints each line of a FATAL_ERROR as its own paragraph, with a
        # blank line between, so each paragraph is a single (long) source line and
        # CMake does the wrapping.
        set(_msg "\n")
        string(APPEND _msg "This compiler cannot compile extern/JoltPhysics.\n")
        string(APPEND _msg "It rejects a static data member whose type is a class template instantiated while the element type is still incomplete, which is how Jolt declares `static const StaticArray<Vec3, 1026> sUnitSphere;` inside Vec3.\n")
        string(APPEND _msg "Released GCC 14 and 16 defer that instantiation; the ubuntu-toolchain-r/test `gcc-16` trunk snapshot does not, and the build would stop at the Jolt precompiled header of three targets.\n")
        string(APPEND _msg "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} (${CMAKE_CXX_COMPILER})\n")
        string(APPEND _msg "Use a released compiler -- scripts/install-gcc-prebuilt.sh installs a pinned GCC 16.2.0 -- or set ZHLN_SKIP_TOOLCHAIN_CAPABILITY_PROBES=ON if this verdict is known to be wrong for your toolchain.")
        message(FATAL_ERROR "${_msg}")
    endif()
    message(STATUS "toolchain probes: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} compiles Jolt's static-member pattern")

    # -------------------------------------------------------- assembler ------
    # GCC 16 emits `.base64` for large constant data and GAS only learned the
    # directive in binutils 2.41, where jammy ships 2.38. The assembler is reached
    # through PATH or -B, so a compiler that is correct on its own still fails 300
    # files into a build when the distro's `as` is the one that answers. Compiling
    # a translation unit like this one, rather than invoking `as` on a snippet as
    # the release lane used to, tests the pair as the build actually forms it.
    #
    # The top-level `asm` is what makes the verdict reliable. Whether a particular
    # release chooses `.base64` for a particular constant is its own business, and
    # a probe that passes because the compiler happened not to use the directive
    # is worse than no probe: it would report a broken binutils as fine. The
    # array below reproduces the shape that triggered this in CI; the directive is
    # emitted unconditionally, so the assembler has to accept it either way.
    set(_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/zhln-capabilities")
    file(MAKE_DIRECTORY "${_probe_dir}")
    # 600 distinct values from a fixed seed, so the translation unit is identical
    # on every machine and the cached verdict stays meaningful. Distinct because
    # repeated constants let the assembler merge them, which makes the blob
    # smaller than the one that trips this in practice.
    set(_seed 7)
    set(_blob "")
    foreach(_index RANGE 599)
        math(EXPR _seed "(${_seed} * 1103515245 + 12345) & 0x7fffffff")
        # OUTPUT_FORMAT HEXADECIMAL already writes the 0x prefix.
        math(EXPR _value "${_seed}" OUTPUT_FORMAT HEXADECIMAL)
        string(APPEND _blob "\t${_value}ULL,\n")
    endforeach()
    file(WRITE "${_probe_dir}/base64-values.inc" "${_blob}")
    set(CMAKE_REQUIRED_INCLUDES "${CMAKE_REQUIRED_INCLUDES}" "${_probe_dir}")
    check_cxx_source_compiles("
#include <cstdint>
asm(R\"(.base64 \"EjRWeJA=\")\");
namespace {
const std::uint64_t kTable[] = {
#include \"base64-values.inc\"
};
}
extern \"C\" unsigned long long zahlen_probe_first() { return kTable[0]; }
int main() { return static_cast<int>(zahlen_probe_first() & 1u); }"
        ZHLN_COMPILER_EMITS_ASSEMBLABLE_BASE64)
    if(NOT ZHLN_COMPILER_EMITS_ASSEMBLABLE_BASE64)
        set(_msg "\n")
        string(APPEND _msg "This toolchain cannot assemble what it emits.\n")
        string(APPEND _msg "GCC 16 writes `.base64` for large constant data, and the assembler that answers for this compiler does not accept it. That is normally a binutils older than 2.41 reached through PATH -- Ubuntu 22.04 ships 2.38 -- rather than the binutils installed alongside the compiler.\n")
        string(APPEND _msg "Compiler: ${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_VERSION}). The compiler's own output is in ${CMAKE_BINARY_DIR}/CMakeFiles/CMakeConfigureLog.yaml.\n")
        string(APPEND _msg "Put the compiler's own binutils first on PATH -- scripts/install-gcc-prebuilt.sh installs a matching 2.46.1 and verifies PATH with -print-prog-name=as -- or set ZHLN_SKIP_TOOLCHAIN_CAPABILITY_PROBES=ON if this verdict is known to be wrong.")
        message(FATAL_ERROR "${_msg}")
    endif()
    message(STATUS "toolchain probes: the assembler accepts GCC 16's .base64 output")

    set(CMAKE_REQUIRED_FLAGS "${_saved_required_flags}")
    set(CMAKE_REQUIRED_INCLUDES "${_saved_required_includes}")
endfunction()
