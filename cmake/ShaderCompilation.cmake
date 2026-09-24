# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Initialize the global generated shader tracking lists
set(ALL_GENERATED_SPVS "")
# MACRO=<module.spv> pairs: what the shader catalog generator is handed, one
# per cooked module. Kept beside the -D definitions because it is the same
# fact.
set(ALL_SHADER_MACRO_PATHS "")
# MACRO=<entry slang>,<entry point>,<slang stage> triples: how the catalog
# generator replays each cook in-process for reflection. Slang reflects the
# source; the cooked SPIR-V only votes on what survived (see Reflect.cpp).
set(ALL_SHADER_SLANG_SOURCES "")
# MACRO=<NAME[=VALUE]> pairs: the -D preprocessor definitions of each cook,
# stripped of the flag itself. Same accumulation shape as the sources above.
set(ALL_SHADER_SLANG_DEFINES "")
# Bare entry-point paths: not an argument, only the catalog command's DEPENDS
# half of the slang inputs, so editing a shader re-runs the reflection.
set(ALL_SHADER_ENTRY_SOURCES "")

set(SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders")
set(SHADER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
set(GEN_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
file(MAKE_DIRECTORY ${GEN_INCLUDE_DIR})

# The shared slang the entry points import: every cook and the catalog's
# in-process replay read these, so both commands depend on them. Stated once
# because the alternative is two thirteen-file lists drifting apart -- the
# last file added to one and not the other is a stale catalog nobody notices.
set(ZHLN_SHADER_COMMON_SOURCES
    "${SHADER_SRC_DIR}/uniforms.slang"
    "${SHADER_SRC_DIR}/pbr_helpers.slang"
    "${SHADER_SRC_DIR}/hash.slang"
    "${SHADER_SRC_DIR}/common.slang"
    "${SHADER_SRC_DIR}/descriptor_heap_layout.slang"
    "${SHADER_SRC_DIR}/cluster_grid.slang"
    "${SHADER_SRC_DIR}/cluster_math.slang"
    "${SHADER_SRC_DIR}/sampling.slang"
    "${SHADER_SRC_DIR}/vertex_format.slang"
    "${SHADER_SRC_DIR}/particles.slang"
    "${SHADER_SRC_DIR}/material_model.slang"
    "${SHADER_SRC_DIR}/instance_data.slang"
    "${SHADER_SRC_DIR}/volumetric_grid.slang"
)

# The vendored Slang's shape, stated once: both fallbacks below -- the slangc
# compiler search and the libslang library search -- build this same tree, and
# add_subdirectory runs a single time (the second search sees the targets and
# stands down). Keep the tree to the compiler and its library: no RHI/tests,
# no DXC fetch, no LLVM download.
set(SLANG_ENABLE_EXAMPLES OFF)
set(SLANG_ENABLE_TESTS OFF)
set(SLANG_ENABLE_GFX OFF)
set(SLANG_ENABLE_SLANG_RHI OFF)
set(SLANG_ENABLE_SLANGD OFF)
set(SLANG_ENABLE_SLANGI OFF)
set(SLANG_ENABLE_REPLAYER OFF)
set(SLANG_ENABLE_DXIL OFF)
# The gpu-types and catalog modes compile Slang->SPIR-V in-process via libslang.
# On non-Windows libslang dlopen's a versioned slang-glslang-<VERSION> library
# which provides both glslang and spirv-opt (see slang-glslang-compiler.cpp).
# Upstream defaults it ON, but stating it guards the hermetic build against a
# packaging that flipped it OFF (shader-slang/slang#3469 macOS arm64 missing
# libslang-glslang.dylib).
set(SLANG_ENABLE_SLANG_GLSLANG ON CACHE BOOL "" FORCE)
set(SLANG_ENABLE_SLANGRT ON CACHE BOOL "" FORCE)
set(SLANG_SLANG_LLVM_FLAVOR DISABLE)

# ----------------------------------------------------------------------------
# The Slang the cooks and the catalog replay run. A host slangc (PATH, Vulkan
# SDK, SLANG_BIN, or -DSLANG_EXECUTABLE) is preferred, and the library comes
# from the matching config package when one is visible -- so the compiler and
# the library the replay links always agree. The vendored submodule is the
# hermetic fallback: one pinned source builds both. It is also the escape from
# a host/SDK Slang whose compiler works but whose library misbehaves --
# shader-slang/slang#9500, silenced upstream: the 1.4.341.1 SDK's
# libslang-compiler.0.2026.1 null-derefs in its C API SPIR-V path on builtin
# arithmetic that its own slangc cooks fine, so zshader's in-process replay
# segfaults where the cooks succeed. ZHLN_SLANG_VENDORED=ON takes the pinned
# route on purpose.
# ----------------------------------------------------------------------------
option(ZHLN_SLANG_VENDORED
    "Build and use the pinned extern/slang for both slangc and libslang, ignoring any host/SDK Slang" OFF)

# Prefer a host slangc (PATH, Vulkan SDK, SLANG_BIN, or -DSLANG_EXECUTABLE).
# If none is available, build the vendored Slang submodule and use its slangc.
if(ZHLN_SLANG_VENDORED)
    # find_program caches its answer, so a build directory that was configured
    # once without this option -- the default -- still holds the host slangc in
    # CMakeCache.txt, and the "Found host slangc" branch below would take it.
    # That contradicts what this option promises, and it is not harmless: the
    # known-bad gate reads a version off whichever slangc wins, so the pinned
    # tree would be refused on the SDK's version number while the library it
    # actually links was already the pinned one. Shadowing rather than
    # unsetting leaves the cache alone, so dropping -DZHLN_SLANG_VENDORED later
    # goes back to the host compiler without a reconfigure from scratch.
    set(SLANG_EXECUTABLE "")
else()
    find_program(SLANG_EXECUTABLE NAMES slangc PATHS "$ENV{VULKAN_SDK}/bin" "$ENV{SLANG_BIN}")
endif()
set(SLANG_COMPILER_DEPENDS "")
if(SLANG_EXECUTABLE)
    message(STATUS "Found host slangc: ${SLANG_EXECUTABLE}")
elseif(TARGET slangc)
    # A Slang is already in this configure -- a parent project built one, or an
    # earlier add_subdirectory did. Adding the submodule again would collide with
    # the targets it defines (CMP0002, "another target with the same name already
    # exists"), and its compiler is the one this build should cook with anyway.
    message(STATUS "Using the slangc target already in this build; not adding extern/slang again")
    set(SLANG_EXECUTABLE "$<TARGET_FILE:slangc>")
    set(SLANG_COMPILER_DEPENDS slangc)
else()
    set(SLANG_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/slang")
    if(EXISTS "${SLANG_SOURCE_DIR}/CMakeLists.txt")
        if(ZHLN_SLANG_VENDORED)
            message(STATUS "ZHLN_SLANG_VENDORED: building slangc from the pinned ${SLANG_SOURCE_DIR}")
        else()
            message(STATUS "Host slangc not found; building vendored Slang from ${SLANG_SOURCE_DIR}")
        endif()
        add_subdirectory("${SLANG_SOURCE_DIR}" EXCLUDE_FROM_ALL)
        set(SLANG_EXECUTABLE "$<TARGET_FILE:slangc>")
        set(SLANG_COMPILER_DEPENDS slangc)
    else()
        message(FATAL_ERROR
            "slangc not found on PATH and ${SLANG_SOURCE_DIR} is missing. "
            "Install slangc (Vulkan SDK / a Slang release) or run "
            "'git submodule update --init --recursive'.")
    endif()
endif()

# libslang for zshader's gpu-types mode, which compiles the ABI module
# in-process: a config package (Vulkan SDK or a Slang release -- the Dockerfile
# puts the SDK's lib/cmake on CMAKE_PREFIX_PATH) when one is visible, else the
# vendored tree. A tree the compiler search above already added wins without a
# second lookup, so the library always matches the slangc that cooks the passes.
# Either way ZHLN_SLANG_TARGET names the target tools/zshader links.
if(TARGET slang::slang)
    set(ZHLN_SLANG_TARGET slang::slang)
elseif(TARGET slang)
    set(ZHLN_SLANG_TARGET slang)
elseif(TARGET slangc)
    # Reachable two ways: this configure just added the tree and it exported no
    # library target, or a parent project supplied slangc and nothing to link.
    message(FATAL_ERROR
        "A slangc target is in this build but no slang or slang::slang target is, so zshader has no "
        "libslang to link. Point CMAKE_PREFIX_PATH at a Slang package, or drop that existing Slang "
        "and let the pinned submodule provide both.")
else()
    if(NOT ZHLN_SLANG_VENDORED)
        find_package(slang CONFIG QUIET)
    endif()
    if(slang_FOUND AND TARGET slang::slang)
        set(ZHLN_SLANG_TARGET slang::slang)
        message(STATUS "Found libslang (config): ${slang_DIR}")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/slang/CMakeLists.txt")
        if(ZHLN_SLANG_VENDORED)
            message(STATUS "ZHLN_SLANG_VENDORED: building libslang from the pinned ${CMAKE_CURRENT_SOURCE_DIR}/extern/slang")
        else()
            message(STATUS "libslang config not found; building vendored Slang from ${CMAKE_CURRENT_SOURCE_DIR}/extern/slang")
        endif()
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/extern/slang" EXCLUDE_FROM_ALL)
        if(TARGET slang::slang)
            set(ZHLN_SLANG_TARGET slang::slang)
        elseif(TARGET slang)
            set(ZHLN_SLANG_TARGET slang)
        else()
            message(FATAL_ERROR "The vendored Slang exports neither a slang nor a slang::slang target")
        endif()
    else()
        message(FATAL_ERROR
            "libslang not found: no slang CMake config is visible and ${CMAKE_CURRENT_SOURCE_DIR}/extern/slang is missing. "
            "Install the Vulkan SDK / a Slang release or run 'git submodule update --init --recursive'.")
    endif()
endif()

# The directory that actually resolves Slang's public headers, stated once so
# the consumer (tools/zshader) does not re-derive the layout. A config package
# is allowed to advertise the parent of the real location -- the Vulkan SDK
# ships its headers one level down (include/slang/) while naming include/ --
# and a moved or partial install is a class of failure this probe turns from a
# compiler error into a configure error that names the directories it saw.
set(ZHLN_SLANG_INCLUDE_DIR "")
get_target_property(ZHLN_SLANG_ADVERTISED_INCLUDES ${ZHLN_SLANG_TARGET} INTERFACE_INCLUDE_DIRECTORIES)
foreach(SLANG_INCLUDE_CANDIDATE IN LISTS ZHLN_SLANG_ADVERTISED_INCLUDES)
    # A built tree advertises $<BUILD_INTERFACE:...> wrappers; the filesystem
    # probe runs at configure time, before any of them would be evaluated.
    set(ZHLN_SLANG_PROBE_DIR "${SLANG_INCLUDE_CANDIDATE}")
    if(ZHLN_SLANG_PROBE_DIR MATCHES "^\\$<(BUILD_INTERFACE|INSTALL_INTERFACE):(.*)>$")
        set(ZHLN_SLANG_PROBE_DIR "${CMAKE_MATCH_2}")
    endif()
    if(EXISTS "${ZHLN_SLANG_PROBE_DIR}/slang-com-helper.h"
       AND EXISTS "${ZHLN_SLANG_PROBE_DIR}/slang-com-ptr.h"
       AND EXISTS "${ZHLN_SLANG_PROBE_DIR}/slang.h")
        set(ZHLN_SLANG_INCLUDE_DIR "${ZHLN_SLANG_PROBE_DIR}")
        break()
    endif()
endforeach()
if(NOT ZHLN_SLANG_INCLUDE_DIR)
    # The SDK layout: the same headers, one level below the advertised dir.
    foreach(SLANG_INCLUDE_CANDIDATE IN LISTS ZHLN_SLANG_ADVERTISED_INCLUDES)
        set(ZHLN_SLANG_PROBE_DIR "${SLANG_INCLUDE_CANDIDATE}")
        if(ZHLN_SLANG_PROBE_DIR MATCHES "^\\$<(BUILD_INTERFACE|INSTALL_INTERFACE):(.*)>$")
            set(ZHLN_SLANG_PROBE_DIR "${CMAKE_MATCH_2}")
        endif()
        if(EXISTS "${ZHLN_SLANG_PROBE_DIR}/slang/slang-com-helper.h"
           AND EXISTS "${ZHLN_SLANG_PROBE_DIR}/slang/slang-com-ptr.h"
           AND EXISTS "${ZHLN_SLANG_PROBE_DIR}/slang/slang.h")
            set(ZHLN_SLANG_INCLUDE_DIR "${ZHLN_SLANG_PROBE_DIR}/slang")
            break()
        endif()
    endforeach()
endif()
if(NOT ZHLN_SLANG_INCLUDE_DIR)
    message(FATAL_ERROR
        "The libslang target ${ZHLN_SLANG_TARGET} advertises the include directories "
        "${ZHLN_SLANG_ADVERTISED_INCLUDES}, and neither they nor a 'slang/' subdirectory of "
        "any of them contains slang-com-helper.h, slang-com-ptr.h and slang.h. Delete the "
        "build directory to rerun the slang discovery against a complete install.")
endif()
message(STATUS "Slang headers: ${ZHLN_SLANG_INCLUDE_DIR}")
# The directory that holds the built slang and slang-glslang dylibs, for
# RUNPATH and DYLD_LIBRARY_PATH at zshader build time (dlopen probe).
set(ZHLN_SLANG_LIB_DIR "$<TARGET_FILE_DIR:${ZHLN_SLANG_TARGET}>")
set(ZHLN_SLANG_GLSLANG_DIR "")
set(ZHLN_SLANG_GLSLANG_DEPENDS "")
if(TARGET slang-glslang)
    set(ZHLN_SLANG_GLSLANG_DIR "$<TARGET_FILE_DIR:slang-glslang>")
    set(ZHLN_SLANG_GLSLANG_DEPENDS "slang-glslang")
endif()

# ----------------------------------------------------------------------------
# The known-bad gate. shader-slang/slang#9500 (silenced upstream): the C API
# SPIR-V path null-derefs on builtin arithmetic (SPIRVEmitContext::
# _arithmeticOpCodeConvert with a null basicType) while slangc's legacy path
# cooks the same modules fine. A bad Slang therefore does not fail the cook --
# it segfaults zshader's in-process replay mid-build, with no explanation.
# The families below are proven bad on this engine (2025.24.2 per the issue,
# 2026.1 per the 1.4.341.1 SDK's libslang-compiler.0.2026.1); the fix has no
# pinned upstream release, so a matching version is refused here, at configure
# time, with the resolutions stated. A future 2026.1.x that ships the fix goes
# off this list. The vendored tree is pinned past the window and is exempt.
#
# Which Slang that leaves is decided by what can be *read*, not by where it came
# from: the gate speaks for a packaged Slang, and a Slang this build makes from
# source has no package location to read. Every other route into this file --
# including a host with no slangc and no slang config, which falls back to the
# tree silently -- reaches the gate through the same two questions, so none of
# them can skip it by accident.
# ----------------------------------------------------------------------------
option(ZHLN_SLANG_ALLOW_KNOWN_BAD
    "Proceed even when the resolved Slang matches a known-bad family (shader-slang/slang#9500)" OFF)

# --- what this configure can read a version from ---
# slangc: a build target has no executable to run yet, which is why its version
# arrives as a generator expression ("$<TARGET_FILE:slangc>") rather than a path.
set(ZHLN_SLANGC_PROBEABLE ON)
if(SLANG_EXECUTABLE MATCHES "^\\$<")
    set(ZHLN_SLANGC_PROBEABLE OFF)
endif()
# libslang: only an imported target owns a file at configure time, and only there
# may LOCATION be read at all -- CMake refuses it on a target this build has not
# built yet ("use $<TARGET_FILE>"), because there is no file to name. For an
# imported one it is the property to use: it applies CMake's own config mapping,
# so it answers with the file a link would pick, where IMPORTED_LOCATION alone is
# NOTFOUND on a package written by install(EXPORT) with per-config locations.
get_target_property(ZHLN_SLANG_IMPORTED ${ZHLN_SLANG_TARGET} IMPORTED)

if(ZHLN_SLANGC_PROBEABLE OR ZHLN_SLANG_IMPORTED)
    # slangc: -version is the only flag (--version is rejected), automated
    # builds print a git-describe string ("2026.1-52-gc8ddf20bb"), local
    # builds print "unknown" or a bare number, and the line has been seen on
    # either stream -- so capture both and only trust a dotted version.
    set(ZHLN_SLANGC_VERSION "unknown")
    if(ZHLN_SLANGC_PROBEABLE)
        execute_process(COMMAND ${SLANG_EXECUTABLE} -version
            RESULT_VARIABLE ZHLN_SLANGC_VERSION_RESULT
            OUTPUT_VARIABLE ZHLN_SLANGC_VERSION_OUT
            ERROR_VARIABLE ZHLN_SLANGC_VERSION_ERR
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_STRIP_TRAILING_WHITESPACE)
        string(STRIP "${ZHLN_SLANGC_VERSION_OUT} ${ZHLN_SLANGC_VERSION_ERR}" ZHLN_SLANGC_VERSION_LINE)
        # CMake's regex engine has no {n} intervals: spell the four digits out.
        if(ZHLN_SLANGC_VERSION_RESULT EQUAL 0
           AND ZHLN_SLANGC_VERSION_LINE MATCHES "([0-9][0-9][0-9][0-9]\\.[0-9]+)")
            set(ZHLN_SLANGC_VERSION "${CMAKE_MATCH_1}")
        endif()
    endif()
    # libslang: the versioned filename is the only version the target carries
    # (libslang-compiler.0.2026.1.dylib, libslang-compiler.so.0.2025.21;
    # unversioned on Windows, where the slangc answer carries the check).
    set(ZHLN_SLANG_LIB_VERSION "unknown")
    if(ZHLN_SLANG_IMPORTED)
        get_target_property(ZHLN_SLANG_LIB_LOCATION ${ZHLN_SLANG_TARGET} LOCATION)
        if(ZHLN_SLANG_LIB_LOCATION AND NOT ZHLN_SLANG_LIB_LOCATION MATCHES "NOTFOUND")
            get_filename_component(ZHLN_SLANG_LIB_NAME "${ZHLN_SLANG_LIB_LOCATION}" NAME)
            if(ZHLN_SLANG_LIB_NAME MATCHES "([0-9][0-9][0-9][0-9]\\.[0-9]+)")
                set(ZHLN_SLANG_LIB_VERSION "${CMAKE_MATCH_1}")
            endif()
        endif()
    endif()

    set(ZHLN_SLANG_KNOWN_BAD "")
    foreach(ZHLN_SLANG_PROBE_VERSION IN LISTS ZHLN_SLANGC_VERSION ZHLN_SLANG_LIB_VERSION)
        if(ZHLN_SLANG_PROBE_VERSION MATCHES "^(2025\\.24|2026\\.1)([^0-9]|$)")
            list(APPEND ZHLN_SLANG_KNOWN_BAD ${ZHLN_SLANG_PROBE_VERSION})
        endif()
    endforeach()
    list(REMOVE_DUPLICATES ZHLN_SLANG_KNOWN_BAD)

    message(STATUS "Slang versions: slangc ${ZHLN_SLANGC_VERSION}, libslang ${ZHLN_SLANG_LIB_VERSION}")
    if(NOT ZHLN_SLANG_KNOWN_BAD
       AND NOT ZHLN_SLANGC_VERSION STREQUAL "unknown"
       AND NOT ZHLN_SLANG_LIB_VERSION STREQUAL "unknown"
       AND NOT ZHLN_SLANGC_VERSION STREQUAL ZHLN_SLANG_LIB_VERSION)
        message(WARNING
            "slangc reports ${ZHLN_SLANGC_VERSION} but libslang is ${ZHLN_SLANG_LIB_VERSION}: "
            "the cooks and the in-process replay would run different compilers.")
    endif()

    if(ZHLN_SLANG_KNOWN_BAD)
        if(ZHLN_SLANG_ALLOW_KNOWN_BAD)
            message(WARNING
                "Slang ${ZHLN_SLANG_KNOWN_BAD} is known-bad for zshader's in-process replay "
                "(shader-slang/slang#9500); proceeding because ZHLN_SLANG_ALLOW_KNOWN_BAD=ON.")
        else()
            message(FATAL_ERROR
"Slang ${ZHLN_SLANG_KNOWN_BAD} is known to segfault zshader's in-process reflection replay "
"(shader-slang/slang#9500, silenced upstream: the C API SPIR-V path null-derefs on builtin "
"arithmetic that slangc itself cooks fine -- the cooks pass, the replay dies mid-build). "
"Proven bad on this engine: 2025.24.2 (the issue) and 2026.1 (the 1.4.341.1 SDK's "
"libslang-compiler.0.2026.1). Resolutions:
  1. Update the Vulkan SDK to a release bundling a Slang past this family, verify with
     \"$VULKAN_SDK/bin/slangc -version\", then reconfigure.
  2. Install a recent Slang release binary (https://github.com/shader-slang/slang/releases --
     the latest, e.g. v2026.18, is well past the window; pick the zip for your platform):
     put its bin/ on PATH and its root directory on CMAKE_PREFIX_PATH, and the discovery
     above picks up both slangc and the matching libslang config.
  3. Build the pinned Slang this engine is developed against (verified against #9500):
       git submodule update --init --recursive
       rm -rf build/<your tag>
       tools/build.sh -DZHLN_SLANG_VENDORED=ON
  If this exact build of Slang is actually fine for you, override with "
  "-DZHLN_SLANG_ALLOW_KNOWN_BAD=ON.")
        endif()
    endif()
else()
    # Nothing to read here: this build's Slang comes from source -- the pinned
    # submodule, or one a parent project already added -- so it is the one Slang
    # the gate exempts by pin. Said out loud, because a gate that went quiet
    # silently would read as a gate that passed.
    message(STATUS
        "Slang versions: not probeable (${ZHLN_SLANG_TARGET} is built in this tree, and slangc is one this build makes); "
        "the known-bad version gate does not apply")
endif()

# ----------------------------------------------------------------------------
# compile_slang: compiles a single Slang entry point to SPIR-V.
# Sets ${OUTPUT_VAR} in the parent scope to the resulting .spv path.
# ----------------------------------------------------------------------------
function(compile_slang SHADER_PATH ENTRY STAGE OUTPUT_VAR)
    get_filename_component(FILE_NAME ${SHADER_PATH} NAME_WE)
    set(OUTPUT_SPV "${GEN_INCLUDE_DIR}/${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv")
    set(EXTRA_ARGS ${ARGN})

    # Map HLSL-style profiles to Slang's stage names
    set(SLANG_STAGE ${STAGE})
    if(STAGE MATCHES "^vs_")
        set(SLANG_STAGE "vertex")
    elseif(STAGE MATCHES "^ps_")
        set(SLANG_STAGE "fragment")
    elseif(STAGE MATCHES "^cs_")
        set(SLANG_STAGE "compute")
    elseif(STAGE MATCHES "^as_")
        # VK_EXT_mesh_shader amplification (task) stage
        set(SLANG_STAGE "amplification")
    elseif(STAGE MATCHES "^ms_")
        # VK_EXT_mesh_shader mesh stage
        set(SLANG_STAGE "mesh")
    endif()

    add_custom_command(
        OUTPUT ${OUTPUT_SPV}
        COMMAND ${SLANG_EXECUTABLE} ${SHADER_PATH}
                -entry ${ENTRY}
                -stage ${SLANG_STAGE}
                -target spirv
                -fvk-use-entrypoint-name
                -matrix-layout-column-major
                -I "${SHADER_SRC_DIR}"
                -I "${SHADER_INCLUDE_DIR}"
                ${EXTRA_ARGS}
                -o ${OUTPUT_SPV}
        DEPENDS ${SHADER_PATH}
                ${SLANG_COMPILER_DEPENDS}
                ${ZHLN_SHADER_COMMON_SOURCES}
        COMMENT "Slang: Generating ${FILE_NAME}.${ENTRY}.${OUTPUT_VAR}.spv"
        VERBATIM
    )
    set(${OUTPUT_VAR} ${OUTPUT_SPV} PARENT_SCOPE)

    # The reflection replay of this cook: the catalog generator compiles the
    # same entry point, with the same defines, in-process (see the catalog
    # command below and tools/zshader/Reflect.cpp). Reported the way the
    # cooked path is -- one record per call for the caller to accumulate --
    # because OUTPUT_VAR is the macro name, so the records key themselves.
    # Every flag the callers can pass today is a -D -- per-target EXTRA_ARGS
    # and the STAGES fifth field alike -- and the replay understands nothing
    # else; a new flag kind fails here, at configure time, instead of
    # silently reflecting a different module than the cook compiled.
    set(ZHLN_SLANG_SOURCE_RECORD "${OUTPUT_VAR}=${SHADER_PATH},${ENTRY},${SLANG_STAGE}" PARENT_SCOPE)
    set(ZHLN_SLANG_DEFINE_RECORDS "")
    foreach(EXTRA_ARG IN LISTS EXTRA_ARGS)
        if(EXTRA_ARG MATCHES "^-D(.+)$")
            list(APPEND ZHLN_SLANG_DEFINE_RECORDS "${OUTPUT_VAR}=${CMAKE_MATCH_1}")
        else()
            message(FATAL_ERROR "compile_slang: ${OUTPUT_VAR} passes '${EXTRA_ARG}', which is not a -D define; "
                "the catalog's in-process reflection replay only forwards -D flags (see ALL_SHADER_SLANG_DEFINES)")
        endif()
    endforeach()
    set(ZHLN_SLANG_DEFINE_RECORDS "${ZHLN_SLANG_DEFINE_RECORDS}" PARENT_SCOPE)
endfunction()

# ----------------------------------------------------------------------------
# add_shader_target: Boilerplate killer with stage-specific flag support.
# ----------------------------------------------------------------------------
function(add_shader_target TARGET_SUFFIX)
    cmake_parse_arguments(ARG "" "" "STAGES;EXTRA_ARGS" ${ARGN})

    set(OUTPUTS "")
    foreach(STAGE_DEF IN LISTS ARG_STAGES)
        string(REPLACE "|" ";" PARTS "${STAGE_DEF}")
        list(GET PARTS 0 SHADER_PATH)
        list(GET PARTS 1 ENTRY)
        list(GET PARTS 2 PROFILE)
        list(GET PARTS 3 MACRO)

        # Check for optional 5th parameter (stage-specific preprocessor flags)
        list(LENGTH PARTS PARTS_LEN)
        set(STAGE_SPECIFIC_ARGS "")
        if(PARTS_LEN GREATER 4)
            list(GET PARTS 4 STAGE_SPECIFIC_ARGS)
            # Split the space-separated flags into a proper CMake list
            string(REPLACE " " ";" STAGE_SPECIFIC_ARGS "${STAGE_SPECIFIC_ARGS}")
        endif()

        # Pass ${MACRO} as the unique output variable name to prevent collisions
        compile_slang("${SHADER_PATH}" ${ENTRY} ${PROFILE} ${MACRO} ${ARG_EXTRA_ARGS} ${STAGE_SPECIFIC_ARGS})

        list(APPEND OUTPUTS ${${MACRO}})
        list(APPEND ALL_SHADER_MACRO_PATHS "${MACRO}=${${MACRO}}")
        # The replay records of this cook, accumulated the way the macro path
        # above is. The defines arrive unquoted: most cooks pass none, and an
        # empty expansion appends nothing.
        list(APPEND ALL_SHADER_SLANG_SOURCES ${ZHLN_SLANG_SOURCE_RECORD})
        list(APPEND ALL_SHADER_SLANG_DEFINES ${ZHLN_SLANG_DEFINE_RECORDS})
        list(APPEND ALL_SHADER_ENTRY_SOURCES "${SHADER_PATH}")

        # Export the cooked path under the macro name the catalog knows it by.
        # A source file can then #embed it (src/render/GpuAbi.hpp reads
        # gpu_abi.slang's own bytes at compile time), which #embed requires to
        # be a preprocessing-time file name -- a string constant is too late.
        set(${MACRO} ${${MACRO}} PARENT_SCOPE)
    endforeach()

    set(TGT zahlen_engine_${TARGET_SUFFIX})
    add_custom_target(${TGT} ALL DEPENDS ${OUTPUTS})
    add_dependencies(zahlen_engine ${TGT})

    list(APPEND ALL_GENERATED_SPVS ${OUTPUTS})
    set(ALL_SHADER_MACRO_PATHS ${ALL_SHADER_MACRO_PATHS} PARENT_SCOPE)
    set(ALL_GENERATED_SPVS ${ALL_GENERATED_SPVS} PARENT_SCOPE)
    set(ALL_SHADER_SLANG_SOURCES ${ALL_SHADER_SLANG_SOURCES} PARENT_SCOPE)
    set(ALL_SHADER_SLANG_DEFINES ${ALL_SHADER_SLANG_DEFINES} PARENT_SCOPE)
    set(ALL_SHADER_ENTRY_SOURCES ${ALL_SHADER_ENTRY_SOURCES} PARENT_SCOPE)
endfunction()

# ----------------------------------------------------------------------------
# compile_shaders: bulk VS+PS compile for the "simple" shader set
# ----------------------------------------------------------------------------
function(compile_shaders TARGET_NAME)
    set(SHADER_FILES ${ARGN})
    set(ALL_SPV_OUTPUTS "")
    set(STAGE_EXTS     "VS"     "PS")
    set(STAGE_ENTRIES  "VSMain" "PSMain")
    set(STAGE_PROFILES "vs_6_5" "ps_6_5")

    foreach(SHADER_SRC IN LISTS SHADER_FILES)
        get_filename_component(FILE_NAME ${SHADER_SRC} NAME_WLE)

        foreach(i RANGE 1)
            list(GET STAGE_EXTS     ${i} EXT)
            list(GET STAGE_ENTRIES  ${i} ENTRY)
            list(GET STAGE_PROFILES ${i} PROFILE)

            # Clean, native Slang macro generation
            string(MAKE_C_IDENTIFIER "SHADER_${FILE_NAME}_SLANG_${EXT}_PATH" MACRO_NAME)
            string(TOUPPER ${MACRO_NAME} MACRO_NAME)

            compile_slang("${SHADER_SRC}" ${ENTRY} ${PROFILE} ${MACRO_NAME})

            list(APPEND ALL_SPV_OUTPUTS ${${MACRO_NAME}})
            list(APPEND ALL_SHADER_MACRO_PATHS "${MACRO_NAME}=${${MACRO_NAME}}")
            # The replay records, as above: this family passes no -D flags, so
            # the defines expansion is always empty here.
            list(APPEND ALL_SHADER_SLANG_SOURCES ${ZHLN_SLANG_SOURCE_RECORD})
            list(APPEND ALL_SHADER_SLANG_DEFINES ${ZHLN_SLANG_DEFINE_RECORDS})
            list(APPEND ALL_SHADER_ENTRY_SOURCES "${SHADER_SRC}")
            set(${MACRO_NAME} ${${MACRO_NAME}} PARENT_SCOPE)
        endforeach()
    endforeach()

    add_custom_target(${TARGET_NAME}_shader_gen ALL DEPENDS ${ALL_SPV_OUTPUTS})
    add_dependencies(${TARGET_NAME} ${TARGET_NAME}_shader_gen)

    set(ALL_SHADER_MACRO_PATHS ${ALL_SHADER_MACRO_PATHS} PARENT_SCOPE)
    set(ALL_GENERATED_SPVS ${ALL_GENERATED_SPVS} ${ALL_SPV_OUTPUTS} PARENT_SCOPE)
    set(ALL_SHADER_SLANG_SOURCES ${ALL_SHADER_SLANG_SOURCES} PARENT_SCOPE)
    set(ALL_SHADER_SLANG_DEFINES ${ALL_SHADER_SLANG_DEFINES} PARENT_SCOPE)
    set(ALL_SHADER_ENTRY_SOURCES ${ALL_SHADER_ENTRY_SOURCES} PARENT_SCOPE)
endfunction()

# --- EXECUTE COMPILATIONS ---

compile_shaders(zahlen_engine
    "${SHADER_SRC_DIR}/blit.slang"
    "${SHADER_SRC_DIR}/taa.slang"
    "${SHADER_SRC_DIR}/ui.slang"
    "${SHADER_SRC_DIR}/fxaa.slang"
    "${SHADER_SRC_DIR}/mlaa.slang"
    "${SHADER_SRC_DIR}/punctual_shadows.slang"
)

# --- Scene shaders (basic.slang hosts the GlobalSceneRegistry ParameterBlock).
# The engine reflects the authoritative bindless layout out of these modules. ---

add_shader_target(basic_shader
    STAGES
        "${SHADER_SRC_DIR}/basic.slang|VSMain|vs_6_5|SHADER_BASIC_SLANG_VS_PATH"
        "${SHADER_SRC_DIR}/basic.slang|PSMain|ps_6_5|SHADER_BASIC_SLANG_PS_PATH"
)

# --- VK_EXT_mesh_shader stages ---
# basic_task.slang / basic_mesh.slang replace the input assembler + vertex stage
# of the geometry passes. They reuse basic.slang's fragment shaders verbatim
# through the shared VSOutput interface declared in common.slang.
add_shader_target(basic_mesh_shader
    STAGES
        "${SHADER_SRC_DIR}/basic_task.slang|TaskMain|as_6_5|SHADER_BASIC_SLANG_TASK_PATH"
        "${SHADER_SRC_DIR}/basic_mesh.slang|MeshMain|ms_6_5|SHADER_BASIC_SLANG_MESH_PATH"
)

# --- Single-stage compute/pixel targets ---

add_shader_target(culling_shader
    STAGES "${SHADER_SRC_DIR}/culling.slang|CSMain|cs_6_0|SHADER_CULLING_SLANG_CS_PATH"
)

add_shader_target(hiz_generate_shader
    STAGES "${SHADER_SRC_DIR}/hiz_generate.slang|CSMain|cs_6_0|SHADER_HIZ_GENERATE_SLANG_CS_PATH"
)

# Ray-traced sun shadow mask and its edge-avoiding A-Trous wavelet denoiser.
# The mask is traced at 1-2 SPP with blue-noise/Vogel-disk jitter and then
# smoothed by `denoiserPasses` iterations of the wavelet before the lighting
# pass reads a single load instead of a ray.
add_shader_target(rt_shadow_shader
    STAGES "${SHADER_SRC_DIR}/rt_shadow.slang|CSMain|cs_6_0|SHADER_RT_SHADOW_SLANG_CS_PATH"
)

add_shader_target(shadow_denoise_atrous_shader
    STAGES "${SHADER_SRC_DIR}/shadow_denoise_atrous.slang|CSMain|cs_6_0|SHADER_SHADOW_DENOISE_ATROUS_SLANG_CS_PATH"
)

# Color A-Trous wavelet over the composited HDR scene color, run between the
# reflection/forward passes and bloom to integrate the 1 SPP RT grain.
add_shader_target(hdr_denoise_atrous_shader
    STAGES "${SHADER_SRC_DIR}/hdr_denoise_atrous.slang|CSMain|cs_6_0|SHADER_HDR_DENOISE_ATROUS_SLANG_CS_PATH"
)

# Half-resolution RT reflection tracing for the VNDF roughness band; the
# reflection pass upsamples the composed result.
add_shader_target(rtr_half_shader
    STAGES "${SHADER_SRC_DIR}/rtr_half.slang|CSMain|cs_6_0|SHADER_RTR_HALF_SLANG_CS_PATH"
)

# Half-resolution GTAO horizon search for the AO-only GI modes; the lighting
# pass depth-weighted-upsamples the R8 result.
add_shader_target(ao_gtao_shader
    STAGES "${SHADER_SRC_DIR}/ao_gtao.slang|CSMain|cs_6_0|SHADER_AO_GTAO_SLANG_CS_PATH"
)

# Per-pass scene variants. Each pass compiles its own named entry points
# against its own varying struct (common.slang), so the geometry and fragment
# stages of one pipeline agree on the SPIR-V interface by construction.
# Resource::GetSceneShaders() still enforces the pairing on the C++ side.
add_shader_target(shadow_shader
    STAGES
        "${SHADER_SRC_DIR}/basic.slang|VSMainShadow|vs_6_5|SHADER_BASIC_SLANG_VS_SHADOW_PATH"
        "${SHADER_SRC_DIR}/basic.slang|PSShadow|ps_6_0|SHADER_SHADOW_SLANG_PS_PATH"
        "${SHADER_SRC_DIR}/basic_mesh.slang|MeshMainShadow|ms_6_5|SHADER_BASIC_SLANG_MESH_SHADOW_PATH"
)

add_shader_target(cluster_bounds
    STAGES "${SHADER_SRC_DIR}/cluster_bounds.slang|CSMain|cs_6_0|SHADER_CLUSTER_BOUNDS_CS_PATH"
)

add_shader_target(cluster_cull
    STAGES "${SHADER_SRC_DIR}/cluster_culling.slang|CSMain|cs_6_0|SHADER_CLUSTER_CULLING_CS_PATH"
)

add_shader_target(skinning_shader
    STAGES "${SHADER_SRC_DIR}/skinning.slang|CSMain|cs_6_0|SHADER_SKINNING_SLANG_CS_PATH"
)

add_shader_target(forward_shader
    STAGES
        "${SHADER_SRC_DIR}/basic.slang|VSMainForward|vs_6_5|SHADER_BASIC_SLANG_VS_FORWARD_PATH"
        "${SHADER_SRC_DIR}/basic.slang|PSForward|ps_6_0|SHADER_FORWARD_SLANG_PS_PATH"
        "${SHADER_SRC_DIR}/basic_mesh.slang|MeshMainForward|ms_6_5|SHADER_BASIC_SLANG_MESH_FORWARD_PATH"
)

add_shader_target(hang_gpu_shader
    STAGES "${SHADER_SRC_DIR}/hang_gpu.slang|CSMain|cs_6_0|SHADER_HANG_GPU_SLANG_CS_PATH"
)

# --- DUAL KAWASE BLOOM (single compute dispatch chain) ---
# One file per entry point: the heap binding tables are reflected positionally
# from each module's set-0 declaration order, so every pipeline must see
# exactly the bindings it consumes.

add_shader_target(bloom_threshold_cs
    STAGES "${SHADER_SRC_DIR}/bloom_threshold_cs.slang|CSMain|cs_6_0|SHADER_BLOOM_THRESHOLD_CS_SLANG_CS_PATH"
)

add_shader_target(bloom_down_cs
    STAGES "${SHADER_SRC_DIR}/bloom_down_cs.slang|CSMain|cs_6_0|SHADER_BLOOM_DOWN_CS_SLANG_CS_PATH"
)

add_shader_target(bloom_up_cs
    STAGES "${SHADER_SRC_DIR}/bloom_up_cs.slang|CSMain|cs_6_0|SHADER_BLOOM_UP_CS_SLANG_CS_PATH"
)

add_shader_target(procedural_bake
    STAGES "${SHADER_SRC_DIR}/procedural_bake.slang|CSMain|cs_6_0|SHADER_PROCEDURAL_BAKE_SLANG_CS_PATH"
)

add_shader_target(brdf_lut
    STAGES "${SHADER_SRC_DIR}/brdf_lut.slang|CSMain|cs_6_0|SHADER_BRDF_LUT_CS_PATH"
)

add_shader_target(ibl_specular
    STAGES "${SHADER_SRC_DIR}/ibl_bake.slang|SpecularMain|cs_6_0|SHADER_IBL_SPECULAR_CS_PATH"
)

add_shader_target(ibl_sh
    STAGES "${SHADER_SRC_DIR}/ibl_bake.slang|SHMain|cs_6_0|SHADER_IBL_SH_CS_PATH"
)

add_shader_target(smaa_lut
    STAGES "${SHADER_SRC_DIR}/smaa_lut.slang|CSMain|cs_6_0|SHADER_SMAA_LUT_CS_PATH"
)

add_shader_target(gpu_scene
    STAGES "${SHADER_SRC_DIR}/gpu_scene.slang|CompactMain|cs_6_0|SHADER_GPU_SCENE_CS_PATH"
)

add_shader_target(vol_clear_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_clear.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_CLEAR_SLANG_CS_PATH"
)

add_shader_target(vol_fog_inject_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_fog_inject.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_FOG_INJECT_CS_PATH"
)

add_shader_target(vol_light_inject_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_light_inject.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_LIGHT_INJECT_CS_PATH"
)

add_shader_target(vol_integrate_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_integration.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_INTEGRATION_SLANG_CS_PATH"
)

add_shader_target(vol_temporal_shader
    STAGES "${SHADER_SRC_DIR}/volumetric_temporal.slang|CSMain|cs_6_0|SHADER_VOLUMETRIC_TEMPORAL_CS_PATH"
)

# --- GPU PARTICLE SHADERS ---

add_shader_target(particle_update_shader
    STAGES "${SHADER_SRC_DIR}/particle_update.slang|CSMain|cs_6_0|SHADER_PARTICLE_UPDATE_CS_PATH"
)

add_shader_target(particle_render_shader
    STAGES
        "${SHADER_SRC_DIR}/particle_render.slang|VSMain|vs_6_5|SHADER_PARTICLE_RENDER_VS_PATH"
        "${SHADER_SRC_DIR}/particle_render.slang|PSMain|ps_6_5|SHADER_PARTICLE_RENDER_PS_PATH"
)

# --- 3D MESH PARTICLE SHADERS ---

add_shader_target(mesh_particle_update_shader
    STAGES "${SHADER_SRC_DIR}/mesh_particle_update.slang|CSMain|cs_6_0|SHADER_MESH_PARTICLE_UPDATE_CS_PATH"
)

add_shader_target(mesh_particle_render_shader
    STAGES
        "${SHADER_SRC_DIR}/mesh_particle_render.slang|VSMain|vs_6_5|SHADER_MESH_PARTICLE_RENDER_VS_PATH"
        "${SHADER_SRC_DIR}/mesh_particle_render.slang|PSMain|ps_6_5|SHADER_MESH_PARTICLE_RENDER_PS_PATH"
)

# Compiles mesh_particle_render.slang with -DSHADOW_PASS for depth-only rendering
add_shader_target(mesh_particle_shadow_shader
    STAGES
        "${SHADER_SRC_DIR}/mesh_particle_render.slang|VSMain|vs_6_5|SHADER_MESH_PARTICLE_SHADOW_VS_PATH|-DSHADOW_PASS"
        "${SHADER_SRC_DIR}/mesh_particle_render.slang|PSShadow|ps_6_5|SHADER_MESH_PARTICLE_SHADOW_PS_PATH|-DSHADOW_PASS"
)

# --- Multi-stage (VS+PS) targets, RT vs NoRT variants ---

add_shader_target(reflection_shader
    STAGES
        "${SHADER_SRC_DIR}/reflection.slang|VSMain|vs_6_5|SHADER_REFLECTION_SLANG_VS_PATH"
        "${SHADER_SRC_DIR}/reflection.slang|PSMain|ps_6_5|SHADER_REFLECTION_SLANG_PS_PATH"
)

add_shader_target(reflection_nort_shader
    STAGES
        "${SHADER_SRC_DIR}/reflection.slang|VSMain|vs_6_5|SHADER_REFLECTION_NORT_SLANG_VS_PATH"
        "${SHADER_SRC_DIR}/reflection.slang|PSMain|ps_6_5|SHADER_REFLECTION_NORT_SLANG_PS_PATH"
    EXTRA_ARGS -DDISABLE_RTR
)

add_shader_target(lighting_shader
    STAGES
        "${SHADER_SRC_DIR}/lighting.slang|VSMain|vs_6_5|SHADER_LIGHTING_SLANG_VS_PATH"
        "${SHADER_SRC_DIR}/lighting.slang|PSMain|ps_6_5|SHADER_LIGHTING_SLANG_PS_PATH"
)

add_shader_target(lighting_nort_shader
    STAGES
        "${SHADER_SRC_DIR}/lighting.slang|VSMain|vs_6_5|SHADER_LIGHTING_NORT_SLANG_VS_PATH"
        "${SHADER_SRC_DIR}/lighting.slang|PSMain|ps_6_5|SHADER_LIGHTING_NORT_SLANG_PS_PATH"
    EXTRA_ARGS -DDISABLE_RTR
)

# --- Integrated stage-specific defines for SMAA ---
add_shader_target(smaa_shaders
    STAGES
        "${SHADER_SRC_DIR}/SMAA.slang|SmaaEdgeVS|vs_6_5|SHADER_SMAA_EDGE_VS_PATH|-DEDGE_PASS"
        "${SHADER_SRC_DIR}/SMAA.slang|SmaaEdgePS|ps_6_5|SHADER_SMAA_EDGE_PS_PATH|-DEDGE_PASS"
        "${SHADER_SRC_DIR}/SMAA.slang|SmaaWeightVS|vs_6_5|SHADER_SMAA_WEIGHT_VS_PATH|-DWEIGHT_PASS"
        "${SHADER_SRC_DIR}/SMAA.slang|SmaaWeightPS|ps_6_5|SHADER_SMAA_WEIGHT_PS_PATH|-DWEIGHT_PASS"
        "${SHADER_SRC_DIR}/SMAA.slang|SmaaBlendVS|vs_6_5|SHADER_SMAA_BLEND_VS_PATH|-DBLEND_PASS"
        "${SHADER_SRC_DIR}/SMAA.slang|SmaaBlendPS|ps_6_5|SHADER_SMAA_BLEND_PS_PATH|-DBLEND_PASS"
)

# --- DECAL SHADER ---
add_shader_target(decal_shader
    STAGES
        "${SHADER_SRC_DIR}/decal.slang|VSMain|vs_6_5|SHADER_DECAL_VS_PATH"
        "${SHADER_SRC_DIR}/decal.slang|PSMain|ps_6_5|SHADER_DECAL_PS_PATH"
)

# --- THE SHADER CATALOG ---
# zshader reflects the cooked modules into two generated files:
#
#   * ShaderBindings.hpp -- the declarations every render source compiles
#     against: one type per module (entry point, stage, its bindings with their
#     descriptor types and numbers, its push-constant layout) and one
#     Vk::ShaderSet<...> per descriptor block. No bytes, so nothing outside the
#     generated bytecode builds SPIR-V into every translation unit;
#
#   * ShaderBytecode.cpp -- the one translation unit in the project that
#     #embeds the cooked modules (and the data blobs the renderer ships inside
#     the binary). It defines each module's Bytes() and asserts, per module,
#     that the generated lists are what the module's own bytes say -- the check
#     that keeps this generator honest.
#
# It runs after the cooks and only when a module or the tool changed, so a
# shader edit recompiles one translation unit and relinks.

set(ZHLN_SHADER_CATALOG_HEADER "${GEN_INCLUDE_DIR}/ShaderBindings.hpp")
set(ZHLN_SHADER_CATALOG_SOURCE "${GEN_INCLUDE_DIR}/ShaderBytecode.cpp")

# Every cooked module, as Type=<macro>: the name the engine knows it by and the
# macro the cook defined for its .spv. A type carries the module's entry point,
# its stage, its cooked path, its byte size and its own declarations, so a
# pipeline is built from it and a descriptor write is checked against it --
# there is no second place where the engine spells what a module is. The loop
# below holds this list against the cooks.
set(ZHLN_SHADER_CATALOG_MODULES
    "BasicVS=SHADER_BASIC_SLANG_VS_PATH"
    "BasicPS=SHADER_BASIC_SLANG_PS_PATH"
    "BasicTask=SHADER_BASIC_SLANG_TASK_PATH"
    "BasicMesh=SHADER_BASIC_SLANG_MESH_PATH"
    "BasicVSShadow=SHADER_BASIC_SLANG_VS_SHADOW_PATH"
    "BasicMeshShadow=SHADER_BASIC_SLANG_MESH_SHADOW_PATH"
    "BasicVSForward=SHADER_BASIC_SLANG_VS_FORWARD_PATH"
    "BasicMeshForward=SHADER_BASIC_SLANG_MESH_FORWARD_PATH"
    "BlitVS=SHADER_BLIT_SLANG_VS_PATH"
    "BlitPS=SHADER_BLIT_SLANG_PS_PATH"
    "TaaVS=SHADER_TAA_SLANG_VS_PATH"
    "TaaPS=SHADER_TAA_SLANG_PS_PATH"
    "UiVS=SHADER_UI_SLANG_VS_PATH"
    "UiPS=SHADER_UI_SLANG_PS_PATH"
    "LightingVS=SHADER_LIGHTING_SLANG_VS_PATH"
    "LightingPS=SHADER_LIGHTING_SLANG_PS_PATH"
    "ReflectionVS=SHADER_REFLECTION_SLANG_VS_PATH"
    "ReflectionPS=SHADER_REFLECTION_SLANG_PS_PATH"
    "ReflectionNortVS=SHADER_REFLECTION_NORT_SLANG_VS_PATH"
    "ReflectionNortPS=SHADER_REFLECTION_NORT_SLANG_PS_PATH"
    "FxaaVS=SHADER_FXAA_SLANG_VS_PATH"
    "FxaaPS=SHADER_FXAA_SLANG_PS_PATH"
    "MlaaVS=SHADER_MLAA_SLANG_VS_PATH"
    "MlaaPS=SHADER_MLAA_SLANG_PS_PATH"
    "SmaaEdgeVS=SHADER_SMAA_EDGE_VS_PATH"
    "SmaaEdgePS=SHADER_SMAA_EDGE_PS_PATH"
    "SmaaWeightVS=SHADER_SMAA_WEIGHT_VS_PATH"
    "SmaaWeightPS=SHADER_SMAA_WEIGHT_PS_PATH"
    "SmaaBlendVS=SHADER_SMAA_BLEND_VS_PATH"
    "SmaaBlendPS=SHADER_SMAA_BLEND_PS_PATH"
    "BloomThresholdCS=SHADER_BLOOM_THRESHOLD_CS_SLANG_CS_PATH"
    "BloomDownCS=SHADER_BLOOM_DOWN_CS_SLANG_CS_PATH"
    "BloomUpCS=SHADER_BLOOM_UP_CS_SLANG_CS_PATH"
    "PunctualShadowsVS=SHADER_PUNCTUAL_SHADOWS_SLANG_VS_PATH"
    "PunctualShadowsPS=SHADER_PUNCTUAL_SHADOWS_SLANG_PS_PATH"
    "LightingNortVS=SHADER_LIGHTING_NORT_SLANG_VS_PATH"
    "LightingNortPS=SHADER_LIGHTING_NORT_SLANG_PS_PATH"
    "VolumetricClearCS=SHADER_VOLUMETRIC_CLEAR_SLANG_CS_PATH"
    "HdrDenoiseAtrousCS=SHADER_HDR_DENOISE_ATROUS_SLANG_CS_PATH"
    "RtrHalfCS=SHADER_RTR_HALF_SLANG_CS_PATH"
    "GtaoCS=SHADER_AO_GTAO_SLANG_CS_PATH"
    "VolumetricFogInjectCS=SHADER_VOLUMETRIC_FOG_INJECT_CS_PATH"
    "VolumetricLightInjectCS=SHADER_VOLUMETRIC_LIGHT_INJECT_CS_PATH"
    "VolumetricIntegrationCS=SHADER_VOLUMETRIC_INTEGRATION_SLANG_CS_PATH"
    "VolumetricTemporalCS=SHADER_VOLUMETRIC_TEMPORAL_CS_PATH"
    "ParticleUpdateCS=SHADER_PARTICLE_UPDATE_CS_PATH"
    "ParticleRenderVS=SHADER_PARTICLE_RENDER_VS_PATH"
    "ParticleRenderPS=SHADER_PARTICLE_RENDER_PS_PATH"
    "DecalVS=SHADER_DECAL_VS_PATH"
    "DecalPS=SHADER_DECAL_PS_PATH"
    "MeshParticleUpdateCS=SHADER_MESH_PARTICLE_UPDATE_CS_PATH"
    "MeshParticleRenderVS=SHADER_MESH_PARTICLE_RENDER_VS_PATH"
    "MeshParticleRenderPS=SHADER_MESH_PARTICLE_RENDER_PS_PATH"
    "MeshParticleShadowVS=SHADER_MESH_PARTICLE_SHADOW_VS_PATH"
    "MeshParticleShadowPS=SHADER_MESH_PARTICLE_SHADOW_PS_PATH"
    "CullingCS=SHADER_CULLING_SLANG_CS_PATH"
    "HizGenerateCS=SHADER_HIZ_GENERATE_SLANG_CS_PATH"
    "ShadowPS=SHADER_SHADOW_SLANG_PS_PATH"
    "ClusterBoundsCS=SHADER_CLUSTER_BOUNDS_CS_PATH"
    "ClusterCullingCS=SHADER_CLUSTER_CULLING_CS_PATH"
    "SkinningCS=SHADER_SKINNING_SLANG_CS_PATH"
    "ForwardPS=SHADER_FORWARD_SLANG_PS_PATH"
    "HangGpuCS=SHADER_HANG_GPU_SLANG_CS_PATH"
    "ProceduralBakeCS=SHADER_PROCEDURAL_BAKE_SLANG_CS_PATH"
    "BrdfLutCS=SHADER_BRDF_LUT_CS_PATH"
    "IblSpecularCS=SHADER_IBL_SPECULAR_CS_PATH"
    "IblShCS=SHADER_IBL_SH_CS_PATH"
    "SmaaLutCS=SHADER_SMAA_LUT_CS_PATH"
    "GpuSceneCS=SHADER_GPU_SCENE_CS_PATH"
    "RtShadowCS=SHADER_RT_SHADOW_SLANG_CS_PATH"
    "ShadowDenoiseAtrousCS=SHADER_SHADOW_DENOISE_ATROUS_SLANG_CS_PATH"
)

# A type whose macro no cook produced would reach the generator as `Type=` and
# be rejected there with a bare argv; caught here it names the macro instead.
foreach(CATALOG_MODULE IN LISTS ZHLN_SHADER_CATALOG_MODULES)
    string(REPLACE "=" ";" CATALOG_MODULE_PARTS "${CATALOG_MODULE}")
    list(GET CATALOG_MODULE_PARTS 1 CATALOG_MACRO)
    set(CATALOG_MACRO_FOUND FALSE)
    foreach(MACRO_PATH IN LISTS ALL_SHADER_MACRO_PATHS)
        if(MACRO_PATH MATCHES "^${CATALOG_MACRO}=")
            set(CATALOG_MACRO_FOUND TRUE)
        endif()
    endforeach()
    if(NOT CATALOG_MACRO_FOUND)
        message(FATAL_ERROR
            "zshader: catalog type ${CATALOG_MODULE} names ${CATALOG_MACRO}, which no cooked shader defines")
    endif()
endforeach()

# One descriptor block per pass, as Set=<Type>[,<Type>...]: the modules whose
# bindings that block serves. Configurations that share a block (Lighting and
# LightingNort, SMAA's three passes, the four bakes) are why this is stated
# rather than derived -- the names are the engine's, the bindings are the
# modules'.
set(ZHLN_SHADER_CATALOG_SETS
    "Hiz=HizGenerateCS"
    "Culling=CullingCS"
    "ClusterBounds=ClusterBoundsCS"
    "ClusterCulling=ClusterCullingCS"
    "Bake=ProceduralBakeCS,BrdfLutCS,IblSpecularCS,SmaaLutCS"
    "VolumetricClear=VolumetricClearCS"
    "VolumetricFogInject=VolumetricFogInjectCS"
    "VolumetricLightInject=VolumetricLightInjectCS"
    "VolumetricIntegration=VolumetricIntegrationCS"
    "VolumetricTemporal=VolumetricTemporalCS"
    "BloomThreshold=BloomThresholdCS"
    "BloomDown=BloomDownCS"
    "BloomUp=BloomUpCS"
    "HdrDenoise=HdrDenoiseAtrousCS"
    "RtrHalf=RtrHalfCS"
    "Gtao=GtaoCS"
    "Taa=TaaPS"
    "Fxaa=FxaaPS"
    "Mlaa=MlaaPS"
    "SmaaEdge=SmaaEdgePS"
    "SmaaWeight=SmaaWeightPS"
    "SmaaBlend=SmaaBlendPS"
    "Blit=BlitPS"
    "Lighting=LightingPS,LightingNortPS"
    "Reflection=ReflectionPS,ReflectionNortPS"
)

# The generator itself: tools/zshader/CMakeLists.txt. It is a directory of its
# own because its sources are transpiled like the engine's are, and the
# transpiler registers source directories with include_directories() -- a scope
# of its own keeps that out of the rest of the build. Everything above this
# point in this file describes what the tool is *run with*, everything below it
# what it is run on.
add_subdirectory("${CMAKE_SOURCE_DIR}/tools/zshader" "${CMAKE_BINARY_DIR}/tools/zshader")

# The blue-noise tile is decoded here, at build time, rather than by the
# renderer at startup. It cannot come through the VFS: RenderContext::Create
# uploads it, and the Kernel builds its AssetManager and mounts data/base.pak
# only after that returns (src/engine/Kernel.cpp). What the renderer used to do
# was #embed the PNG and call stbi_load_from_memory on it every startup, which
# made it an image decoder; the cook below makes it a memcpy instead. For this
# tile that is cheaper at both ends -- 1024x1024 of noise compresses to within
# 8KB of its own raw size, and the decode disappears.
#
# Stdlib-only on purpose: this needs the interpreter GovernanceChecks found and
# no third-party package, because the build promises an interpreter and nothing
# else. See configure/cook_blue_noise.py.
set(ZHLN_BLUE_NOISE_SOURCE "${CMAKE_SOURCE_DIR}/src/render/LDR_RGBA_0.png")
set(ZHLN_BLUE_NOISE_COOKED "${GEN_INCLUDE_DIR}/blue_noise.rgba")
add_custom_command(
    OUTPUT "${ZHLN_BLUE_NOISE_COOKED}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_SOURCE_DIR}/configure/cook_blue_noise.py"
        -i "${ZHLN_BLUE_NOISE_SOURCE}"
        -o "${ZHLN_BLUE_NOISE_COOKED}"
    DEPENDS "${CMAKE_SOURCE_DIR}/configure/cook_blue_noise.py" "${ZHLN_BLUE_NOISE_SOURCE}"
    COMMENT "cook_blue_noise: decoding the blue-noise tile to raw RGBA"
    VERBATIM
)

# Bytes the renderer ships inside the binary that are not shaders. All three are
# already in the layout the renderer reads: two cooked DDS tables and one
# decoded tile, none of them a container to parse or an image to decode.
set(ZHLN_SHADER_BLOBS
    "ltc_mat=${CMAKE_SOURCE_DIR}/src/render/ltc_mat.dds"
    "ltc_amp=${CMAKE_SOURCE_DIR}/src/render/ltc_amp.dds"
    "blue_noise_rgba=${ZHLN_BLUE_NOISE_COOKED}"
)

# gpu_abi.slang is not a pass: nothing builds a pipeline from it and the engine
# never loads its bytes. It exists so the host types can be held against a real
# compilation of them, and that check reads the module in a constant expression
# (src/render/GpuAbi.hpp) -- which is why its bytes are embedded there and not
# in the generated bytecode: one copy, in the translation units that check it.
# The gpu-types command below compiles the module in-process and emits its
# SPIR-V itself, so no slangc cook produces it and no catalog entry carries it.

set(ZSHADER_ARGS
    --out-header "${ZHLN_SHADER_CATALOG_HEADER}"
    --out-source "${ZHLN_SHADER_CATALOG_SOURCE}"
)
foreach(MACRO_PATH IN LISTS ALL_SHADER_MACRO_PATHS)
    list(APPEND ZSHADER_ARGS --bytes "${MACRO_PATH}")
endforeach()
foreach(MODULE IN LISTS ZHLN_SHADER_CATALOG_MODULES)
    list(APPEND ZSHADER_ARGS --module "${MODULE}")
endforeach()
foreach(SET IN LISTS ZHLN_SHADER_CATALOG_SETS)
    list(APPEND ZSHADER_ARGS --set "${SET}")
endforeach()
foreach(BLOB IN LISTS ZHLN_SHADER_BLOBS)
    list(APPEND ZSHADER_ARGS --blob "${BLOB}")
endforeach()
# The reflection replay: one --slang-source per cooked module (the entry the
# cook compiled, replays in-process), one --slang-define per -D it compiled
# with, and the same two -I search roots the cook passes slangc. zshader
# refuses modules whose replay inputs are missing or orphaned, so a cook that
# stops recording its source fails the build here rather than reflecting
# nothing.
foreach(SLANG_SOURCE IN LISTS ALL_SHADER_SLANG_SOURCES)
    list(APPEND ZSHADER_ARGS --slang-source "${SLANG_SOURCE}")
endforeach()
foreach(SLANG_DEFINE IN LISTS ALL_SHADER_SLANG_DEFINES)
    list(APPEND ZSHADER_ARGS --slang-define "${SLANG_DEFINE}")
endforeach()
list(APPEND ZSHADER_ARGS --slang-search "${SHADER_SRC_DIR}")
list(APPEND ZSHADER_ARGS --slang-search "${SHADER_INCLUDE_DIR}")

add_custom_command(
    OUTPUT "${ZHLN_SHADER_CATALOG_HEADER}" "${ZHLN_SHADER_CATALOG_SOURCE}"
    COMMAND ${CMAKE_COMMAND} -E env "DYLD_LIBRARY_PATH=${ZHLN_SLANG_LIB_DIR}:${ZHLN_SLANG_GLSLANG_DIR}:$ENV{DYLD_LIBRARY_PATH}" "LD_LIBRARY_PATH=${ZHLN_SLANG_LIB_DIR}:${ZHLN_SLANG_GLSLANG_DIR}:$ENV{LD_LIBRARY_PATH}" $<TARGET_FILE:zshader> ${ZSHADER_ARGS}
    DEPENDS
        zshader
        ${ZHLN_SLANG_GLSLANG_DEPENDS}
        ${ALL_GENERATED_SPVS}
        ${ALL_SHADER_ENTRY_SOURCES}
        ${ZHLN_SHADER_COMMON_SOURCES}
        "${CMAKE_SOURCE_DIR}/src/render/ltc_mat.dds"
        "${CMAKE_SOURCE_DIR}/src/render/ltc_amp.dds"
        "${ZHLN_BLUE_NOISE_COOKED}"
    COMMENT "zshader: reflecting the cooked shaders into the catalog"
    VERBATIM
)
add_custom_target(zahlen_shader_catalog
    DEPENDS "${ZHLN_SHADER_CATALOG_HEADER}" "${ZHLN_SHADER_CATALOG_SOURCE}"
)

# --- THE GPU HOST TYPES ---
# zshader's second mode compiles the gpu_abi module in-process -- the same
# module src/render/GpuAbi.hpp holds the host structs against -- and walks its
# reflected layout into the generated host structs (GeneratedGpuTypes.hpp):
# every struct gpu_abi.slang wraps, minus GPUMeshlet, whose ABI is the raw
# word protocol rather than the declared layout (see GpuTypes.cpp). The same
# compile emits the module's SPIR-V, which is what GpuAbi.hpp embeds, so the
# header and the bytes it is checked against are never more than one build
# apart. <Zahlen/Render/GpuLayout.hpp> includes the header and re-exports the
# structs under their engine names, so a Slang edit re-emits the host side on the
# next build; an unmappable edit fails here, naming the member, instead of
# compiling against skewed layouts.
#
# The header is generated, so every target compiling a translation unit that
# reaches it -- directly or through GpuLayout.hpp -- orders itself after the
# target below: the engine here, the renderer, the RHI and the GUI in their
# own directory files. Tests and zcook link the engine, which orders them.
# The SPIR-V rides the same edge: it is the command's second output, so the
# compile definition in src/render/CMakeLists.txt never names a file the build
# has not produced yet.
set(ZHLN_GPU_TYPES_HEADER "${GEN_INCLUDE_DIR}/GeneratedGpuTypes.hpp")
set(SHADER_GPU_ABI_CS_PATH "${GEN_INCLUDE_DIR}/gpu_abi.spv")
add_custom_command(
    OUTPUT "${ZHLN_GPU_TYPES_HEADER}" "${SHADER_GPU_ABI_CS_PATH}"
    COMMAND ${CMAKE_COMMAND} -E env "DYLD_LIBRARY_PATH=${ZHLN_SLANG_LIB_DIR}:${ZHLN_SLANG_GLSLANG_DIR}:$ENV{DYLD_LIBRARY_PATH}" "LD_LIBRARY_PATH=${ZHLN_SLANG_LIB_DIR}:${ZHLN_SLANG_GLSLANG_DIR}:$ENV{LD_LIBRARY_PATH}" $<TARGET_FILE:zshader>
        --slang-module gpu_abi
        --slang-search "${SHADER_SRC_DIR}"
        --slang-search "${SHADER_INCLUDE_DIR}"
        --out-gpu-types "${ZHLN_GPU_TYPES_HEADER}"
        --out-abi-spv "${SHADER_GPU_ABI_CS_PATH}"
    DEPENDS
        zshader
        ${ZHLN_SLANG_GLSLANG_DEPENDS}
        "${SHADER_SRC_DIR}/gpu_abi.slang"
        "${SHADER_SRC_DIR}/cluster_grid.slang"
        "${SHADER_SRC_DIR}/cluster_math.slang"
        "${SHADER_SRC_DIR}/cxx_abi.slang"
        "${SHADER_SRC_DIR}/descriptor_heap_layout.slang"
        "${SHADER_SRC_DIR}/instance_data.slang"
        "${SHADER_SRC_DIR}/particles.slang"
        "${SHADER_SRC_DIR}/uniforms.slang"
        "${SHADER_SRC_DIR}/vertex_format.slang"
    COMMENT "zshader: compiling the ABI module into host types and SPIR-V"
    VERBATIM
)
add_custom_target(zahlen_gpu_types
    DEPENDS "${ZHLN_GPU_TYPES_HEADER}" "${SHADER_GPU_ABI_CS_PATH}"
)
add_dependencies(zahlen_engine zahlen_gpu_types)

# The consumer claims the generated files: a custom command's outputs are only
# known in the directory that declared them, so src/render/CMakeLists.txt marks
# them GENERATED and depends on the target above. ALL_SHADER_MACRO_PATHS and
# ALL_GENERATED_SPVS are exported to the parent scope by the functions above.
