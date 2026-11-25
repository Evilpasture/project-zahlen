# Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.28)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Must run BEFORE project() so platform modules don't bake -isystem into CXX module scanners
if(APPLE)
    if(NOT _ZHLN_CACHED_MAC_SDK OR NOT EXISTS "${_ZHLN_CACHED_MAC_SDK}")
        execute_process(
        COMMAND xcrun --show-sdk-path
        OUTPUT_VARIABLE _ZHLN_MAC_SDK_OUT
        OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(_ZHLN_CACHED_MAC_SDK "${_ZHLN_MAC_SDK_OUT}" CACHE INTERNAL "macOS SDK path")
    endif()
    set(_ZHLN_MAC_SDK "${_ZHLN_CACHED_MAC_SDK}")

    # Tell CMake NEVER to pass the SDK's usr/include as -isystem
    list(APPEND CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES "${_ZHLN_MAC_SDK}/usr/include")
    list(APPEND CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES "${_ZHLN_MAC_SDK}/usr/include")

    # Extra safety: prevent CMake target dependency scanners from auto-system-promoting SDK directories
    set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)
endif()


# The engine version, and the only place a version number is written by hand.
#
# Everything else derives from PROJECT_VERSION: include/Zahlen/Version.hpp.in is
# configured from it, cmake/Toolchains.cmake uses it for the toolchain id and the
# exported package version, .github/workflows/release.yml takes the release tag
# from it, and tests/version_test.py fails the build when a copy disagrees.
project(zahlen VERSION 0.6.0 LANGUAGES C CXX ASM)

option(ZHLN_INSTALL_TOOLCHAIN_PACKAGES "Install independently consumable toolchain packages" OFF)

# There is deliberately no minimum-compiler-version variable here. What the
# tree needs from a compiler is behaviour, and the one case that has actually
# bitten -- GCC PR c++/125280, which rejects Jolt's incomplete static member
# under -freflection -- was fixed on a branch that reports the same version
# before and after the fix, so no comparison against a version string can
# express the gate. cmake/ToolchainCapabilities.cmake compiles
# cmake/probes/pr125280-static-member.cpp instead, with this build's own
# reflection flags, and the installed package configs compile it again in the
# consumer's toolchain. scripts/install-gcc-prebuilt.sh pins a GCC version to
# *download*, which is provisioning rather than a capability claim.

# CMake's built-in "Link What You Use": link with --no-as-needed and have the
# linker report every direct dependency that contributes no symbol. It is the
# same evidence as the symbol audit that produced the link cleanup, but it runs
# on every link and costs one extra ldd pass per executable, so it stays opt-in.
#
# Run it before trusting a link line:
#   cmake -S . -B build/gcc -DZHLN_LINK_WHAT_YOU_USE=ON
#   ninja -C build/gcc
# then relink only what you need to inspect -- removing a target's binary is
# enough, because removing more schedules the whole prerequisite graph:
#   rm build/gcc/bin/zahlen_cube && ninja -C build/gcc bin/zahlen_cube
#
# Anything reported for a *shared* library (libatomic, libsimdjson) is the
# static-library flattening artifact described in cmake/ThirdParty.cmake, not a
# defect: CMake propagates a static library's link dependencies verbatim to every
# final link, so the consumer names a library that one of its archives needs.
option(ZHLN_LINK_WHAT_YOU_USE "Report direct dependencies that contribute no symbols" OFF)
if(ZHLN_LINK_WHAT_YOU_USE)
    set(CMAKE_LINK_WHAT_YOU_USE ON)
endif()
if(ZHLN_INSTALL_TOOLCHAIN_PACKAGES)
    # Jolt is compiled privately inside libzahlen_engine.so and never exposed publicly.
    set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
endif()


if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(
    FATAL_ERROR
      "zahlen requires a GNU-style toolchain (gcc/g++, or clang/clang++ with "
      "the GNU driver). MSVC and clang-cl cannot build C++26 static "
      "reflection; reconfigure with -DCMAKE_C_COMPILER=clang "
      "-DCMAKE_CXX_COMPILER=clang++.")
endif()

option(ZHLN_USE_CUSTOM_LIBCXX "Link against Bloomberg custom libc++" OFF)

# --- Apple SDK / OBJCXX / build type, before ANY compiler probe -------------
#
# This is the first thing that has to be true about the toolchain, so it is set
# before the first zhln_check_cxx_compiler_flag() below. Three of these change
# what a probe *means* or *measuring* it too late changes the answer:
#
#   CMAKE_SYSROOT     decides which SDK headers a probe compiles against, so a
#                     probe run before it answers about the wrong SDK.
#   CMAKE_BUILD_TYPE  selects CMAKE_CXX_FLAGS_<CONFIG>, which every flag probe
#                     feeds to the compiler. Setting it afterwards left the
#                     probes measuring an empty flag set and made
#                     CMAKE_BUILD_TYPE the one cache entry that disagreed with
#                     the probes that had already run.
#   OBJCXX            Jolt's Metal backend is Objective-C++; a language enabled
#                     halfway down the file is invisible to every probe above
#                     it, and CompilerFlagProbe is above it.
if(APPLE)
    set(MACOS_SDK_PATH "${_ZHLN_MAC_SDK}")
    set(CMAKE_SYSROOT ${MACOS_SDK_PATH})
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -fno-rtti -fno-exceptions")
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE
      "Release"
      CACHE STRING "Choose the type of build." FORCE)
endif()

include(CompilerFlagProbe)
zhln_check_cxx_compiler_flag("-Qunused-arguments" ZHLN_HAS_QUNUSED_ARGUMENTS)
zhln_check_cxx_compiler_flag("-fblocks" ZHLN_HAS_FBLOCKS)
zhln_check_cxx_compiler_flag("-fannotation-attributes"
                             ZHLN_HAS_ANNOTATION_ATTRIBUTES)

if(ZHLN_USE_CUSTOM_LIBCXX)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-stdlib=libc++" ZHLN_HAS_STDLIB_LIBCXX)
endif()

if(WIN32)
    include(CheckSymbolExists)
    set(CMAKE_REQUIRED_LIBRARIES ws2_32)
    check_symbol_exists(WSAStartup "winsock2.h" ZHLN_HAVE_WINSOCK)
    unset(CMAKE_REQUIRED_LIBRARIES)
else()
    set(ZHLN_HAVE_WINSOCK FALSE)
endif()

if(ZHLN_HAS_QUNUSED_ARGUMENTS)
    add_compile_options(-Qunused-arguments)
    add_link_options(-Qunused-arguments)
endif()

if(NOT CMAKE_CXX_FLAGS_RELEASE MATCHES "(^| )-g")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g")
endif()
if(NOT CMAKE_C_FLAGS_RELEASE MATCHES "(^| )-g")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -g")
endif()

if(UNIX OR APPLE)
    set(CMAKE_ASM_COMPILE_OBJECT
      "<CMAKE_ASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -o <OBJECT> -c <SOURCE>"
      CACHE STRING "ASM compile command" FORCE)
endif()


set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ${CMAKE_SOURCE_DIR}/compile_commands.json is a convenience symlink for clangd,
# which only ever reads one of them. Configuring a second tree used to replace
# it silently, so the editor then described whatever was configured last -- a
# 40-target Release build while the developer was working in a Debug tree. With
# several build directories in play the link is now first-writer-wins: an
# existing link to a *different* build tree is left alone and reported, and
# removing it (or configuring once) takes it back.
if(NOT WIN32)
    set(_zhln_compdb_target "${CMAKE_SOURCE_DIR}/compile_commands.json")
    set(_zhln_compdb_source "${CMAKE_BINARY_DIR}/compile_commands.json")
    set(_zhln_compdb_skip FALSE)
    if(IS_SYMLINK "${_zhln_compdb_target}")
        file(READ_SYMLINK "${_zhln_compdb_target}" _zhln_compdb_existing)
        if(NOT _zhln_compdb_existing STREQUAL _zhln_compdb_source)
            set(_zhln_compdb_skip TRUE)
            message(STATUS
                "clangd: ${_zhln_compdb_target} already points at ${_zhln_compdb_existing}; "
                "leaving it. Remove the link to point it at ${_zhln_compdb_source}.")
        endif()
    endif()
    if(NOT _zhln_compdb_skip)
        file(CREATE_LINK "${_zhln_compdb_source}" "${_zhln_compdb_target}"
           SYMBOLIC RESULT _zhln_compdb_link)
        if(_zhln_compdb_link)
            message(STATUS
                "clangd: could not symlink compile_commands.json (${_zhln_compdb_link})")
        endif()
    endif()
endif()

include(CompilerWarnings)
include(HostToolRuntime)
include(GitRevision)
include(LinkerConfig)
include(Sanitizers)
include(VulkanSandbox)
include(PythonProbe)
# The in-tree asset pipeline (Blender probe + cook_assets graph) is opt-in:
# the default engine build compiles no game assets and never probes for
# Blender. Turn it on only for the demo/host binaries that consume
# data/base.pak from this tree.
option(ZHLN_COOK_IN_TREE_ASSETS "Cook this tree's game assets during the engine build (requires the cooker CLI)" OFF)
option(ZHLN_BUILD_COOKER "Build the optional zcook CLI (cooking library: Zahlen::AssetCooking)" ON)
if(ZHLN_COOK_IN_TREE_ASSETS)
    include(BlenderProbe)
else()
    set(ZHLN_HAS_BLENDER FALSE)
endif()

# The generated half of <Zahlen/Config.hpp>. Every target that can see
# include/Zahlen/Config.hpp gets this directory first on its include path: it is
# the engine's own targets through the existing `include` entry below, and it is
# the toolchain packages through _zhln_toolchain_include() in cmake/Toolchains.cmake.
set(ZHLN_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/include")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/include/Zahlen/Version.hpp.in"
    "${ZHLN_GENERATED_INCLUDE_DIR}/Zahlen/Version.hpp" @ONLY)
include_directories(BEFORE SYSTEM "${ZHLN_GENERATED_INCLUDE_DIR}")

# PROJECT_VERSION is the only hand-written version number; the copies other tools
# need (zpkg's ToolVersion, the PKGBUILD, packaging/README.md) are checked here
# rather than trusted. The check is cheap and errors at configure time, which is
# the last moment before a wrong number ends up inside a published package.
if(ZHLN_HAS_PYTHON)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/version_test.py" --check
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _zhln_version_result
        OUTPUT_VARIABLE _zhln_version_output
        ERROR_VARIABLE _zhln_version_error)
    if(NOT _zhln_version_result EQUAL 0)
        message(FATAL_ERROR "version copies disagree with PROJECT_VERSION ${PROJECT_VERSION}:\n${_zhln_version_output}${_zhln_version_error}")
    endif()
endif()

include(GovernanceChecks)
include(ReflectionConfig)

# The C++ runtime for the whole build, decided once, before the first target exists.
#
# Directory-scoped on purpose: add_compile_options/link_directories/add_link_options
# below are inherited by every target created in every subdirectory, including
# extern/JoltPhysics, whose CMakeLists.txt knows nothing about the branch toolchain.
# A per-target helper cannot reach that dylib, and a dylib left on the platform
# libc++ is enough to abort a process inside malloc when a std::locale crosses the
# boundary. Inert unless ZHLN_USE_CUSTOM_LIBCXX is ON.
zahlen_configure_custom_libcxx()
include(ToolchainCapabilities)

option(ZHLN_DEV_MODE "Enable raw loose-file asset loading for development" ON)
if(ZHLN_DEV_MODE)
    add_compile_definitions(ZHLN_DEV_MODE)
endif()

# --- packaging lanes pin the shipped runtime, not just its interface ---------
#
# ZHLN_DEV_MODE defaults ON because the in-tree workflow wants loose-file asset
# loading. A package is the other case: its payload is a shared library that was
# *already compiled* with whatever this option said, so withholding the define
# from zahlen_engine's INTERFACE (cmake/Toolchains.cmake) only fixed half of it
# -- a consumer configured against the package would read Config::isDev == false
# and then load libzahlen_engine.so, whose Config::isDev is true, which is worse
# than either answer alone.
#
# So a toolchain build states the two explicitly. The packaging lane passes
# ZHLN_DEV_MODE=OFF and nothing here fires; a developer who configured the
# packaging lane from a tree that left the default alone gets a configure error
# naming the flag rather than a package with a split personality.
if(ZHLN_INSTALL_TOOLCHAIN_PACKAGES AND ZHLN_DEV_MODE AND NOT ZHLN_DEV_MODE_IN_INSTALLED_PACKAGES)
    message(FATAL_ERROR
        "ZHLN_INSTALL_TOOLCHAIN_PACKAGES=ON with ZHLN_DEV_MODE=ON (the default) would ship a "
        "library compiled for loose-file development loading. Configure the packaging lane with "
        "-DZHLN_DEV_MODE=OFF, or pass -DZHLN_DEV_MODE_IN_INSTALLED_PACKAGES=ON to ship dev mode "
        "deliberately.")
endif()

string(REPLACE "\\" "/" ZHLN_PROJECT_ROOT_FWD "${PROJECT_SOURCE_DIR}")
add_compile_definitions(ZHLN_PROJECT_ROOT="${ZHLN_PROJECT_ROOT_FWD}")

option(ZHLN_IN_DOCKER "Build running inside a Docker container" OFF)
if(ZHLN_IN_DOCKER)
    add_compile_definitions(ZHLN_IN_DOCKER=1)
    message(STATUS "Docker container build mode enabled.")
endif()

option(ZHLN_NO_ANNOTATION_EXTRACT
       "Disable compile-time reflection annotation extraction" OFF)
if(ZHLN_NO_ANNOTATION_EXTRACT)
    add_compile_definitions(ZHLN_NO_ANNOTATION_EXTRACT)
endif()

option(ZHLN_BUILD_TESTS "Build engine test suite" ON)
option(ZHLN_BUILD_SAMPLES "Build engine sample applications" ON)
option(ZHLN_BUILD_EXTRAS
       "Build optional asset plugins, engine extensions, and gameplay systems" ON)
option(ZHLN_BUILD_RENDER
       "Build the Vulkan descriptor-heap renderer (OFF links the zero-overhead world-only null RenderContext)"
       ON)
option(ZHLN_ENABLE_PCH
       "Enable precompiled headers on high-TU non-module GCC targets" ON)
option(ZHLN_SYNTAX_ONLY
       "Check C/C++ syntax only (-fsyntax-only) without emitting object code or linking"
       OFF)

# --- the Kconfig menu as a front end to the cache ---------------------------
#
# Kconfiglib's menuconfig is wired in as a preset (CMakePresets.json) and
# scripted by scripts/menuconfig.sh; it writes <build>/.config, and
# cmake/Kconfig.cmake reads it back. The path is declared and validated here,
# next to the other cache variables, so a typo fails before anything expensive
# runs -- the apply itself has to wait for the option() declarations, which are
# spread down this file and into cmake/ShaderCompilation.cmake.
#
# Inert unless ZHLN_KCACHE_CONFIG is set: a tree configured by hand, by CI or by
# an IDE reads no .config and behaves exactly as it did before this existed.
set(ZHLN_KCACHE_CONFIG "" CACHE FILEPATH
    "Apply this Kconfig .config to the cache (set by the *-menuconfig presets)")
if(ZHLN_KCACHE_CONFIG AND NOT EXISTS "${ZHLN_KCACHE_CONFIG}")
    message(FATAL_ERROR
        "ZHLN_KCACHE_CONFIG points at '${ZHLN_KCACHE_CONFIG}', which does not exist. "
        "Run ./scripts/menuconfig.sh to create it, or unset ZHLN_KCACHE_CONFIG "
        "to configure without a .config.")
endif()


if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(ZHLN_DEBUG=1)
endif()

# Early apply for the one menu entry ThirdParty reads before the full apply
# below can run (see cmake/Kconfig.cmake). Without it the first configure
# after a menuconfig run still sees the option() default here.
if(ZHLN_KCACHE_CONFIG)
    include(Kconfig)
    zhln_kconfig_apply_early("${ZHLN_KCACHE_CONFIG}" ZHLN_JOLT_SLANG_FALLBACK)
endif()

include(ThirdParty)
add_subdirectory(extensions/Image)

add_subdirectory(src/presentation)
add_subdirectory(src/window)
if(ZHLN_BUILD_RENDER)
    add_subdirectory(src/vulkan)
else()
    message(STATUS "ZHLN_BUILD_RENDER=OFF: skipping Vulkan, Slang shader compilation, and zahlen_render GPU passes (linking world-only null RenderContext)")
endif()
add_subdirectory(src/threading)
add_subdirectory(src/filesystem)
add_subdirectory(src/physics)
add_subdirectory(src/ecs)
add_subdirectory(src/audio)

# --- what `zahlen --version` reports as the build's flags --------------------
#
# This string is baked into extensions/CommandLine and is the only description
# of the compile line a user ever sees, so it has to name the flags the build
# actually used -- not the ones this file happened to know about at the point it
# was assembled.
#
# It used to be built here, which is *before* ThirdParty.cmake (line 248) appends
# the Apple flags to CMAKE_CXX_FLAGS, before the sanitizer options from
# cmake/Sanitizers.cmake (included at 193) reach anything, and before
# zahlen_enable_reflection adds -fannotation-attributes per target. The result
# reported neither -stdlib=libc++, nor any sanitizer flag, nor
# -fconstexpr-steps, nor -fannotation-attributes -- and it also left out
# -std=c++26, which is the single most load-bearing item.
#
# Assembled late instead -- see zahlen_assemble_compiler_flags() below, called
# after ThirdParty.cmake and after the reflection helpers exist, so every
# contributor has already run.

# The flags above record what the driver accepts. These probes record what it
# produces, with the same flags, before anything is built: Jolt's incomplete
# static data member (which is why a *released* GCC is required at all) and
# `.base64` assembly (which is why the compiler's own binutils must be the one
# that answers). See cmake/ToolchainCapabilities.cmake.
set(_zhln_reflection_probe_flags "")
if(COMPILER_HAS_REFLECTION)
    set(_zhln_reflection_probe_flags "-freflection")
    if(COMPILER_HAS_PARAMETER_REFLECTION_FLAG)
        string(APPEND _zhln_reflection_probe_flags " -fparameter-reflection")
    elseif(COMPILER_HAS_REFLECTION_LATEST_FLAG)
        string(APPEND _zhln_reflection_probe_flags " -freflection-latest")
    endif()
endif()
zhln_probe_toolchain_capabilities("${_zhln_reflection_probe_flags}")

# The installed package configs re-run the PR 125280 probe in the consumer's
# toolchain, which is a different compiler from this one. They get the source
# and the flags by substitution so there is exactly one copy of each.
set(ZHLN_TOOLCHAIN_REFLECTION_FLAGS "${_zhln_reflection_probe_flags}" CACHE INTERNAL
    "reflection flags an installed package should probe a consumer toolchain with")
file(READ "${PROJECT_SOURCE_DIR}/cmake/probes/pr125280-static-member.cpp" _zhln_pr125280_source)
# PackageConfig.cmake.in embeds the source in a [==[ ... ]==] argument.
# Refuse a premature closing delimiter rather than shipping a broken config.
string(FIND "${_zhln_pr125280_source}" "]==]" _zhln_probe_delimiter)
if(NOT _zhln_probe_delimiter EQUAL -1)
    message(FATAL_ERROR "PR 125280 probe contains the CMake bracket delimiter ]==]")
endif()
unset(ZHLN_PR125280_PROBE_SOURCE CACHE)
set(ZHLN_PR125280_PROBE_SOURCE "${_zhln_pr125280_source}")

# --- the reported flag set, assembled now that every contributor has run -----
#
# Sources, in order:
#   * CMAKE_CXX_FLAGS and the configuration's own CMAKE_CXX_FLAGS_<CONFIG>,
#     which ThirdParty.cmake has finished appending Apple flags to by now;
#   * CMAKE_CXX_COMPILE_OPTIONS_<LANG>, the same variable CMake itself reads
#     when it builds a compile line, so directory-scoped add_compile_options
#     (the sanitizer set, -stdlib=libc++, -Qunused-arguments) is included
#     without this file keeping a second copy of it;
#   * -std=, from the CXX_STANDARD the targets are actually built with;
#   * the project warning set, the exception/RTTI policy, and the reflection
#     flags -- including -fannotation-attributes, which is decided per target
#     but is the same decision everywhere it applies.
set(ZHLN_REPORTED_FLAGS "")

macro(_zhln_report_flags)
    string(APPEND ZHLN_REPORTED_FLAGS " ${ARGN}")
endmacro()

_zhln_report_flags("${CMAKE_CXX_FLAGS}")
if(CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" UPPER_BUILD_TYPE)
    _zhln_report_flags("${CMAKE_CXX_FLAGS_${UPPER_BUILD_TYPE}}")
else()
    _zhln_report_flags("${CMAKE_CXX_FLAGS_DEBUG}")
endif()
# add_compile_options() writes the COMPILE_OPTIONS *directory property*; it does
# NOT write CMAKE_CXX_COMPILE_OPTIONS_CXX, which is a platform variable and is
# empty in this project. Reading the wrong one silently dropped every flag
# applied that way -- the sanitizer set, -stdlib=libc++, -Qunused-arguments.
#
# The property values are still generator expressions at this point
# ($<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>), so they are reported verbatim
# rather than evaluated: what `--version` prints is the option list as the build
# applies it, and a language-conditioned flag reads correctly as one.
get_directory_property(_zhln_dir_compile_options COMPILE_OPTIONS)
if(_zhln_dir_compile_options)
    string(JOIN " " _zhln_dir_compile_options_str ${_zhln_dir_compile_options})
    _zhln_report_flags("${_zhln_dir_compile_options_str}")
endif()
_zhln_report_flags("${ZHLN_COMPILE_WARNINGS_STR} -fno-exceptions -fno-rtti")

# There is no CXX_STANDARD property on the interface targets this describes, so
# the value is stated once here rather than read back per target.
_zhln_report_flags("-std=c++26")

if(COMPILER_HAS_REFLECTION)
    _zhln_report_flags("-freflection")
endif()
if(COMPILER_HAS_PARAMETER_REFLECTION_FLAG)
    _zhln_report_flags("-fparameter-reflection")
elseif(COMPILER_HAS_REFLECTION_LATEST_FLAG)
    _zhln_report_flags("-freflection-latest")
endif()
if(COMPILER_HAS_CONSTEXPR_STEPS_FLAG)
    _zhln_report_flags("-fconstexpr-steps=33554432")
endif()
# zahlen_enable_reflection() applies this inside its `if(COMPILER_HAS_REFLECTION)`
# branch, so a toolchain that has the flag but not -freflection never passes it
# to a compiler. Reporting it there too would describe a flag the build omits.
if(COMPILER_HAS_REFLECTION AND ZHLN_HAS_ANNOTATION_ATTRIBUTES)
    _zhln_report_flags("-fannotation-attributes")
endif()

string(STRIP "${ZHLN_REPORTED_FLAGS}" COMPILER_FLAGS_LIST)
string(REGEX REPLACE " +" " " COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST}")

add_library(zahlen_engine SHARED)

add_subdirectory(src/core)
add_subdirectory(src/engine)

if(ZHLN_BUILD_RENDER)
    include(ShaderCompilation)
endif()

# The apply half of the Kconfig lane declared above. This runs after
# include(ShaderCompilation), which is the last place in the tree that declares an
# option a menu entry can name.
#
# The lane list names entries the menu always offers but this configuration may
# not declare: ZHLN_SLANG_* lives inside include(ShaderCompilation), which only
# runs when ZHLN_BUILD_RENDER is on. They are listed unconditionally -- that is
# the point. With the renderer off they are "offered but not built", not "invented",
# and the apply has to be able to tell those apart.
set(ZHLN_KCONFIG_LANE_SYMBOLS
    ZHLN_SLANG_VENDORED
    ZHLN_SLANG_FROM_PREBUILT
    ZHLN_SLANG_REQUIRE_PREBUILT)
if(ZHLN_KCACHE_CONFIG)
    include(Kconfig)
    zhln_kconfig_apply("${ZHLN_KCACHE_CONFIG}" ${ZHLN_KCONFIG_LANE_SYMBOLS})
endif()

add_subdirectory(src/render)

# The engine shared library seals its dynamic symbol table in three layers:
#
#   1. Compile-time hidden visibility (C/CXX/OBJC/OBJCXX + VISIBILITY_INLINES_HIDDEN)
#      on zahlen_engine, every internal sub-library whose objects enter it, and
#      every vendored static dependency (Jolt, miniaudio, meshoptimizer,
#      SPIRV-Reflect, vendored GLFW).
#   2. Archive sealing (-Wl,--exclude-libs,ALL on ELF and MinGW; sub-libraries
#      that own ZHLN_API definitions -- zahlen_threading, zahlen_filesystem,
#      are OBJECT targets so their exports enter directly while
#      every STATIC archive's symbols are forced hidden, including system .a
#      dependencies and C++ stdlib weak helpers).
#   3. Explicit linker export allowlist (cmake/zahlen_engine.map on ELF,
#      __declspec(dllexport) on PE, hidden visibility + -unexported_symbol
#      guards on Mach-O), granting only the COM factories and the intentional
#      Core/Runtime ZHLN_API surface (and closing private members of ZHLN_API
#      classes). configure/check_engine_exports.py enforces bidirectional parity
#      between ZHLN_API declarations and cmake/zahlen_engine.map at configure
#      time and audits the linked DSO with nm at post-build time, so the two
#      can never drift out of sync.
set_target_properties(zahlen_engine PROPERTIES C_VISIBILITY_PRESET hidden
                                               CXX_VISIBILITY_PRESET hidden
                                               OBJC_VISIBILITY_PRESET hidden
                                               OBJCXX_VISIBILITY_PRESET hidden
                                               VISIBILITY_INLINES_HIDDEN ON)

set_target_properties(zahlen_engine PROPERTIES CXX_STANDARD 26
                                               CXX_STANDARD_REQUIRED ON)
zahlen_set_bin_output(zahlen_engine)

target_compile_definitions(
  zahlen_engine
  PRIVATE ZHLN_ENGINE_BUILD
          JPH_DOUBLE_PRECISION
          JPH_DEBUG_RENDERER)

set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/engine/EngineGlobals.cpp
  PROPERTIES SKIP_PRECOMPILE_HEADERS ON
             COMPILE_DEFINITIONS
             "ZHLN_GIT_COMMIT_HASH=\"${ZHLN_GIT_HASH}\"")

# Bloomberg clang-p2996's PCH ASTReader crashes when deserializing P2996
# reflection expressions inside variable-template initializers; keep PCH on GCC.
if(ZHLN_ENABLE_PCH AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_precompile_headers(
      zahlen_engine PRIVATE
      "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/include/Zahlen/Math/Math3D.hpp>"
      "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/include/Zahlen/Log.hpp>"
      "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/include/Zahlen/Error.hpp>")
endif()


target_link_libraries(
  zahlen_engine
  PRIVATE zahlen_render
          zahlen_window
          zahlen_physics
          zahlen_threading
          zahlen_filesystem
          zahlen_ecs
          zahlen_audio
          meshoptimizer
          glfw
          ${ZSTD_LIBRARY}
          zahlen_warnings)


# Every target whose objects are linked into libzahlen_engine compiles the
# engine's own public headers, so all of them must see ZHLN_API as dllexport.
# On Windows Common.h maps ZHLN_API to dllimport without ZHLN_ENGINE_BUILD, and
# then a call between two of these objects -- say EntityCommandBuffer.cpp in
# zahlen_ecs calling ZHLN::ECS::Registry::Create defined in ECS.cpp in the same
# target -- is emitted as an import of __imp__ZN4ZHLN3ECS8Registry6CreateEv:
# a reference the DLL was always meant to satisfy from its own text, which
# MinGW ld reports as an undefined reference while linking it. The visibility
# macro is ignored outside Windows (Common.h uses gnu::visibility), so this
# changes nothing on the other platforms.
set(ZHLN_ENGINE_LIB_TARGETS
    zahlen_render
    zahlen_window
    zahlen_physics
    zahlen_threading
    zahlen_filesystem
    zahlen_ecs
    zahlen_audio
    zahlen_miniaudio)
if(ZHLN_BUILD_RENDER)
    list(APPEND ZHLN_ENGINE_LIB_TARGETS zahlen_vulkan)
    if(TARGET zahlen_shader_bytecode)
        list(APPEND ZHLN_ENGINE_LIB_TARGETS zahlen_shader_bytecode)
    endif()
    if(TARGET HostBlitSwapchain)
        list(APPEND ZHLN_ENGINE_LIB_TARGETS HostBlitSwapchain)
    endif()
endif()
foreach(ZHLN_ENGINE_LIB_TARGET IN LISTS ZHLN_ENGINE_LIB_TARGETS)
    target_compile_definitions(${ZHLN_ENGINE_LIB_TARGET} PRIVATE ZHLN_ENGINE_BUILD)
    # Every object and archive folded into libzahlen_engine shares its PIC and
    # hidden-visibility contract so only explicitly annotated ZHLN_API symbols
    # reach default visibility across C, C++, Objective-C, and Objective-C++.
    set_target_properties(${ZHLN_ENGINE_LIB_TARGET} PROPERTIES POSITION_INDEPENDENT_CODE ON
                                                              C_VISIBILITY_PRESET hidden
                                                              CXX_VISIBILITY_PRESET hidden
                                                              OBJC_VISIBILITY_PRESET hidden
                                                              OBJCXX_VISIBILITY_PRESET hidden
                                                              VISIBILITY_INLINES_HIDDEN ON)
endforeach()

# Seal symbols from PRIVATE static archives and restrict DSO exports to the
# verified ZHLN_API surface (cmake/zahlen_engine.map).
if(UNIX AND NOT APPLE)
    set(ZHLN_ENGINE_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/zahlen_engine.map")
    target_link_options(zahlen_engine PRIVATE
        "LINKER:--exclude-libs,ALL"
        "LINKER:--version-script=${ZHLN_ENGINE_VERSION_SCRIPT}"
    )
    set_property(TARGET zahlen_engine APPEND PROPERTY LINK_DEPENDS "${ZHLN_ENGINE_VERSION_SCRIPT}")
elseif(MINGW)
    target_link_options(zahlen_engine PRIVATE "LINKER:--exclude-libs,ALL")
elseif(APPLE)
    foreach(_zhln_apple_unexport IN ITEMS
        "__ZN3JPH*" "__ZNK3JPH*" "__ZTVN3JPH*" "__ZTIN3JPH*" "__ZTSN3JPH*"
        "_ma_*" "_mi_*" "__mi_*" "_meshopt_*" "_Clay_*"
        "_vma*" "__ZN3Vma*" "__ZN13Vma*" "_volk*" "_spvReflect*"
        "_glfw*" "__glfw*" "_ZSTD_*" "_FSE_*" "_HUF_*"
        "__ZSt*" "__ZNSt*" "__ZNKSt*" "__ZTVNSt*" "__ZTINSt*" "__ZTSNSt*" "__ZGVNSt*")
        target_link_options(zahlen_engine PRIVATE "LINKER:-unexported_symbol,${_zhln_apple_unexport}")
    endforeach()
    unset(_zhln_apple_unexport)
endif()

if(ZHLN_HAS_PYTHON AND UNIX AND NOT ZHLN_SYNTAX_ONLY)
    add_custom_command(TARGET zahlen_engine POST_BUILD
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/configure/check_engine_exports.py"
                --dso "$<TARGET_FILE:zahlen_engine>"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Auditing dynamic symbol exports of $<TARGET_FILE_NAME:zahlen_engine>..."
        VERBATIM
    )
endif()

# Windows: the MinGW link surface the engine's own sources need.
#
#   stdc++exp  <bits/print.h>'s vprint_unicode calls std::__open_terminal and
#              std::__write_to_terminal when _WIN32 is defined (the POSIX branch
#              delegates to vprint_nonunicode instead), and those two definitions
#              live in libstdc++exp, not in libstdc++. PUBLIC because the same
#              header instantiation lands in the consumers' own objects: the
#              engine's Log.cpp, CommandLine.cpp, RenderCore.cpp, Mutex.cpp and
#              RenderDocInit.cpp all print to a FILE*, and so do the tools,
#              tests and samples that link this library. utils/zshader carries
#              the same expression for libslang's in-process passes.
#   dbghelp    the symbol resolver in src/engine/diagnostics/StackTrace.cpp; its
#              #pragma comment(lib, "dbghelp.lib") is MSVC-only, so MinGW needs
#              the import library named.
#   atomic     16-byte std::atomic instantiations on x86_64 call libatomic's
#              __atomic_load_16/__atomic_store_16 under GCC unless -mcx16 is on
#              -- it is not, this tree asks for -mavx2 and friends rather than
#              an ISA level.
target_link_libraries(
  zahlen_engine
  PUBLIC $<$<AND:$<BOOL:${MINGW}>,$<CXX_COMPILER_ID:GNU>>:stdc++exp>)
if(MINGW)
    target_link_libraries(zahlen_engine PRIVATE dbghelp atomic)
endif()

if(APPLE AND ZHLN_BUILD_RENDER)
    target_link_libraries(zahlen_engine PRIVATE HostBlitSwapchain)
endif()

# Jolt is built as a private hidden-visibility PIC static archive folded into
# libzahlen_engine.so; zero JPH::* symbols or Jolt headers cross the COM boundary.
target_link_libraries(zahlen_engine PRIVATE Jolt)

target_include_directories(
  zahlen_engine SYSTEM
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/extern/JoltPhysics
          ${CMAKE_CURRENT_SOURCE_DIR}/third_party/renderdoc)
target_include_directories(
  zahlen_engine SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${ZHLN_GENERATED_INCLUDE_DIR})
target_include_directories(
  zahlen_engine SYSTEM
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/extern/cgltf
)

target_compile_options(
  zahlen_engine
  PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
  PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>)
if(WIN32)
    # -Wno-microsoft-anon-tag and -Wno-microsoft-enum-forward-reference are clang
    # diagnostics and now come from the probe in cmake/CompilerWarnings.cmake, so a
    # GNU driver is not handed flags it does not know. -Wno-unused-parameter is
    # already probed there too, for every target that links zahlen_warnings.
    target_compile_options(zahlen_engine PRIVATE -Wno-unknown-pragmas -D_CRT_SECURE_NO_WARNINGS)
endif()

include(PluginTarget)
add_subdirectory(extensions/Signal)
add_subdirectory(extensions/CommandLine)
# Screenshot encoding and its optional COM plugin are available even in a core-only build.
add_subdirectory(extensions/Screenshot)

add_executable(zahlen app/main.cpp)


# The engine's DLL is written next to it; Jolt's is not, and PE has no rpath to
# find it with. This is the binary a person runs, so the DLLs go beside it.
zahlen_add_runtime_dlls(zahlen)

set_target_properties(zahlen PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON
                                                        CXX_EXTENSIONS OFF)
zahlen_set_bin_output(zahlen)

add_executable(zahlen_cube app/cube.cpp)
zahlen_add_runtime_dlls(zahlen_cube)
set_target_properties(zahlen_cube PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON
                                                        CXX_EXTENSIONS OFF)
zahlen_set_bin_output(zahlen_cube)



include(ECSCompiler)

add_custom_target(
  link_resources ALL
  COMMAND
    ${CMAKE_COMMAND} -D SRC="${CMAKE_CURRENT_SOURCE_DIR}/resources" -D
    DST="${CMAKE_BINARY_DIR}/resources" -P
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CreateSymlinks.cmake"
  COMMENT "Creating zero-copy symlinks for resources...")

add_dependencies(zahlen link_resources)

if(USE_SANITIZERS)
    zahlen_install_sanitizer_runtime(zahlen)
endif()

zahlen_enable_reflection(zahlen_window)
zahlen_enable_reflection(zahlen_render)
zahlen_enable_reflection(zahlen_engine)
zahlen_enable_reflection(zahlen_audio)
zahlen_enable_reflection(zahlen_physics)
zahlen_enable_reflection(zahlen_ecs)
zahlen_enable_reflection(zahlen_threading)
zahlen_enable_reflection(zahlen_filesystem)
zahlen_enable_reflection(zahlen)
zahlen_enable_reflection(zahlen_cube)

if(ZHLN_BUILD_RENDER)
    zahlen_enable_reflection(zahlen_vulkan)
    if(TARGET zahlen_shader_bytecode)
        zahlen_enable_reflection(zahlen_shader_bytecode)
    endif()
    zahlen_enable_reflection(zshader)
    # Only declared on Apple (src/render/OpenGLHacks/CMakeLists.txt), and not
    # needing reflection anyway -- it reaches no reflected header. Kept behind
    # the TARGET check so a non-Apple configure does not call this on nothing.
    if(TARGET HostBlitSwapchain)
        zahlen_enable_reflection(HostBlitSwapchain)
    endif()
endif()

if(ZHLN_BUILD_EXTRAS)
    # Every target in this tree names its sources. Nothing is discovered by
    # glob, anywhere: the build is the list of what is real, and a file the
    # build does not list is a file the compiler never sees -- which is a
    # fact about the build, not something a script should be inspecting for.
    #
    # Keep the optional layers distinct in both the source tree and build graph.
    # Plugins establish the shared serializer and ReflectLua state layer first;
    # extensions then provide the reusable runtime/network systems; gameplay
    # composes those targets.
    add_subdirectory(plugins)
    add_subdirectory(extensions)
    add_subdirectory(gameplay)

    # UITOML.hpp names GUI::UINode in its signatures, and it used to be repaid
    # with a PUBLIC zahlen_ui_schema link -- which put libzahlen_ui_schema.a on
    # the link line of all 16 serialization consumers, including fidelity, the
    # glTF inspector and the CDN map sample, none of which call into it. UINode
    # is a plain aggregate and ReflectTOML::Parse<> only walks its fields, so the
    # edge was a *header* requirement, and plugins/toml already publishes the
    # repository's extensions root (plugins/CMakeLists.txt), which is where
    # <UI/UITree.hpp> lives. The targets that do call GUI:: symbols -- the UI
    # editor and Extras_UI -- link zahlen_ui_schema themselves.
    #
    # There was a `zahlen_extras` INTERFACE target here linking 19 of the above.
    # Nothing in the project ever linked it: its only reader was the
    # `ZHLN_BUILD_EXTRAS AND TARGET zahlen_extras` guard in tests/CMakeLists.txt,
    # which is already inside `if(ZHLN_BUILD_EXTRAS)` and so tested nothing. It
    # was also wrong about what "extras" means -- zahlen_module is built
    # unconditionally -- and incomplete, omitting bsp, widgets, scripting,
    # console, editor, benchmark, lua, http, remote_asset, cdn, github and svg.
    # A list that nothing consumes and that is wrong in both directions is worse
    # than no list: it reads as an inventory. Removed; the guard in
    # tests/CMakeLists.txt now tests the option alone.

    # Build the UI-document editor only when its UI schema, widget, TOML
    # serializer, and baked-font targets are all present (zero #ifdefs in source).
    if(TARGET zahlen_ui_schema AND TARGET zahlen_widgets AND TARGET zahlen_serialization AND TARGET zahlen_fonts)
        add_executable(zahlen_ui_editor app/UIEditor.cpp)
        set_target_properties(zahlen_ui_editor PROPERTIES
            CXX_STANDARD 26
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
        )
        zahlen_set_bin_output(zahlen_ui_editor)
        target_link_libraries(zahlen_ui_editor PRIVATE
            zahlen_ui_schema
            zahlen_widgets
            zahlen_serialization
            zahlen_fonts
            zahlen_warnings
        )
        add_dependencies(zahlen_ui_editor link_resources)
        zahlen_add_runtime_dlls(zahlen_ui_editor)
        zahlen_enable_reflection(zahlen_ui_editor)
        if(USE_SANITIZERS)
            zahlen_install_sanitizer_runtime(zahlen_ui_editor)
        endif()
    else()
        message(STATUS "Skipping zahlen_ui_editor: requires zahlen_ui_schema, zahlen_widgets, zahlen_serialization, and zahlen_fonts")
    endif()
else()
    message(STATUS "Skipping zahlen_ui_editor: optional UI extensions are disabled (ZHLN_BUILD_EXTRAS=OFF)")
endif()

# Reusable high-level game frontend. Its implementation (and optional-module
# links) live above Core, so games can link one target without composing the
# engine themselves. Available even in a core-only build.
add_subdirectory(app)

# The optional project-light client uses App's CLI, process and frame
# lifecycle, but keeps a separate executable and its own gameplay modules.
# Register it only after zahlen_application exists, and only when the optional
# client (zlib + CharacterController + Camera) was configured above.
if(ZHLN_BUILD_EXTRAS AND TARGET zahlen_project_light_client)
    add_executable(zahlen_project_light_app EXCLUDE_FROM_ALL app/ProjectLightClientMain.cpp)
    set_target_properties(zahlen_project_light_app PROPERTIES
        CXX_STANDARD 26
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        OUTPUT_NAME "project-light-client"
    )
    zahlen_set_bin_output(zahlen_project_light_app)
    # zahlen_application is PUBLIC zahlen_engine, which publishes core COM usage
    # requirements through zahlen_jolt_headers.
    target_link_libraries(zahlen_project_light_app PRIVATE
        zahlen_application
        zahlen_project_light_client
        zahlen_character_controller
        zahlen_camera
        zahlen_warnings
    )
    add_dependencies(zahlen_project_light_app link_resources)
    zahlen_add_runtime_dlls(zahlen_project_light_app)
    zahlen_enable_reflection(zahlen_project_light_app)
    if(USE_SANITIZERS)
        zahlen_install_sanitizer_runtime(zahlen_project_light_app)
    endif()
endif()

# Both shipping host executables drive libzahlen_engine through the unified
# ZHLN::App host without linking immediate-mode UI or optional gameplay archives.
target_link_libraries(zahlen PRIVATE zahlen_application zahlen_warnings)
target_link_libraries(zahlen_cube PRIVATE zahlen_application zahlen_warnings)

# Tool-specific dependencies stay on tool targets. The core-only build still
# produces a callable-driven game; extras add standalone editor, scene host and
# windowless benchmark executables instead of bloating app/main.cpp.
if(ZHLN_BUILD_EXTRAS)
    # The cooker CLI is an extras tool like utils/scene and utils/fidelity:
    # it links plugins/ targets (zahlen_asset_cooking, zahlen_serialization)
    # that only exist in this configuration.
    if(ZHLN_BUILD_COOKER AND TARGET zahlen_asset_cooking AND TARGET zahlen_serialization)
        add_subdirectory(utils/zcook)
    endif()
    if(TARGET zahlen_serialization)
        add_subdirectory(utils/scene)
    endif()
    if(TARGET zahlen_editor AND TARGET zahlen_freecam AND TARGET zahlen_scene_documents AND TARGET zahlen_fallback_scene)
        add_subdirectory(utils/editor)
    endif()
    if(TARGET zahlen_benchmark)
        add_subdirectory(utils/benchmark)
    endif()
endif()

if(ZHLN_BUILD_TESTS)
    include(cmake/ZTest.cmake)
    # Without extras, plugins/ is never configured -- but the CPU test binary
    # cooks radiance assets, so configure the cooking library on its own
    # (AssetCooking's CMakeLists supports being configured from the root).
    if(NOT ZHLN_BUILD_EXTRAS)
        add_subdirectory(plugins/AssetCooking)
    endif()
    add_subdirectory(tests)
    zahlen_finalize_ztest_manifest()
endif()

# Opt-in in-tree asset cooking: purely the AssetPipeline helper graph.
# zcook (above) is its only tool dependency, and it must exist by now.
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME AND NOT ZHLN_SYNTAX_ONLY AND ZHLN_COOK_IN_TREE_ASSETS)
    if(NOT TARGET zcook)
        message(FATAL_ERROR "ZHLN_COOK_IN_TREE_ASSETS requires ZHLN_BUILD_EXTRAS=ON and ZHLN_BUILD_COOKER=ON")
    endif()
    include(AssetPipeline)
    zahlen_configure_game_assets("${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(ZHLN_BUILD_SAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples")
    add_subdirectory(samples)
endif()

if(TARGET zahlen_serialization AND EXISTS
                                   "${CMAKE_CURRENT_SOURCE_DIR}/utils/fidelity")
    add_subdirectory(utils/fidelity utils/fidelity)
endif()

if(ZHLN_SYNTAX_ONLY)
    include(SyntaxOnly)
endif()

if(ZHLN_INSTALL_TOOLCHAIN_PACKAGES)
    include(Toolchains)
endif()

# libzahlen_engine.so is the single shared object the Engine package installs
# (cmake/Toolchains.cmake: zhln_toolchain_package(Engine TARGETS ...)). With
# ZHLN_GLIBC_BASELINE set -- the release lane sets it -- `--target glibc-floor`
# holds them to the floor they ship with, before anything is packaged or signed.
include(GlibcFloor)
set(_zhln_glibc_targets zahlen_engine)
if(TARGET zcook)
    list(APPEND _zhln_glibc_targets zcook)
endif()
foreach(_zhln_plugin_target IN ITEMS
    zahlen_plugin_freecam
    zahlen_plugin_screenshot
    zahlen_plugin_character_controller
    zahlen_plugin_animation
    zahlen_plugin_terrain
    zahlen_plugin_vfx
    zahlen_plugin_gltf
    zahlen_plugin_bsp
    zahlen_plugin_interaction
    zahlen_plugin_discord_rpc
    zahlen_plugin_svg
    zahlen_plugin_http)
    if(TARGET ${_zhln_plugin_target})
        list(APPEND _zhln_glibc_targets ${_zhln_plugin_target})
    endif()
endforeach()
zhln_add_glibc_floor_check(${_zhln_glibc_targets})
unset(_zhln_glibc_targets)
unset(_zhln_plugin_target)

if(TARGET zahlen_plugin_freecam)
    # Build-order only: zahlen never links or includes FreeCam, and still runs
    # cleanly if libzahlen_plugin_freecam is removed at runtime.
    add_dependencies(zahlen zahlen_plugin_freecam)
endif()
