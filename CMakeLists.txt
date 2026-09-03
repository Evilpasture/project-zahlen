# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.28)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# 1. Force the compiler selection BEFORE project()
if(WIN32 AND NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
project(zahlen LANGUAGES C CXX ASM)

# Vulkan-Headers is a vendored submodule. Its CMake project provides the
# Vulkan-Headers interface target; no installed SDK or loader is required.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/Vulkan-Headers EXCLUDE_FROM_ALL SYSTEM)

include(CompilerWarnings)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Qunused-arguments)
    add_link_options(-Qunused-arguments)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

# --- DWARF IN RELEASE, ALWAYS ---
# An optimized build without debug info produces stacks that cannot be
# symbolized, which makes release-mode crashes and profiler output worthless.
# Plain Release therefore carries the same -g that RelWithDebInfo already has.
# Must be set before any add_subdirectory: directory scope snapshots the flags
# variables when the subdirectory is processed. Plain (non-cache) set, so a
# user who deliberately passes -DCMAKE_CXX_FLAGS_RELEASE keeps their value and
# merely gains -g on top of it.
if(NOT CMAKE_CXX_FLAGS_RELEASE MATCHES "(^| )-g")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -g")
endif()
if(NOT CMAKE_C_FLAGS_RELEASE MATCHES "(^| )-g")
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -g")
endif()

# --- RETRIEVE GIT COMMIT HASH ---
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE ZHLN_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

if(NOT ZHLN_GIT_HASH)
    set(ZHLN_GIT_HASH "unknown")
endif()

if(UNIX OR APPLE)
    set(CMAKE_ASM_COMPILE_OBJECT "<CMAKE_ASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -o <OBJECT> -c <SOURCE>" CACHE STRING "ASM compile command" FORCE)
endif()

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_FLAGS "${CMAKE_OBJCXX_FLAGS} -fno-rtti -fno-exceptions")
    execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE MACOS_SDK_PATH OUTPUT_STRIP_TRAILING_WHITESPACE)
    set(CMAKE_SYSROOT ${MACOS_SDK_PATH})
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- DETECT C++26 LANGUAGE FEATURE FLAGS ---
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-freflection" COMPILER_HAS_REFLECTION)

if(COMPILER_HAS_REFLECTION)
    message(STATUS "C++26 Static Reflection supported via -freflection")
else()
    message(STATUS "C++26 Static Reflection not supported by current compiler. Using generated script for source code flattening.")
endif()

# --- PYTHON INTERPRETER SELECTION ---
# Prioritize local virtual environment (.venv) if present
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.venv")
    set(Python3_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.venv")
    set(Python3_FIND_REGISTRY LAST)
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# Enforce the one-way dependency boundary: optional extras may consume core,
# while src/, include/, and modules/ must never consume extras.
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_core_extras_boundary.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE ZHLN_CORE_EXTRAS_BOUNDARY_RESULT
    OUTPUT_VARIABLE ZHLN_CORE_EXTRAS_BOUNDARY_OUTPUT
    ERROR_VARIABLE ZHLN_CORE_EXTRAS_BOUNDARY_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT ZHLN_CORE_EXTRAS_BOUNDARY_RESULT EQUAL 0)
    message(FATAL_ERROR "${ZHLN_CORE_EXTRAS_BOUNDARY_ERROR}")
endif()
message(STATUS "${ZHLN_CORE_EXTRAS_BOUNDARY_OUTPUT}")

# Tests may only include public headers. src/ is private implementation.
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_tests_public_api.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE ZHLN_TESTS_PUBLIC_API_RESULT
    OUTPUT_VARIABLE ZHLN_TESTS_PUBLIC_API_OUTPUT
    ERROR_VARIABLE ZHLN_TESTS_PUBLIC_API_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT ZHLN_TESTS_PUBLIC_API_RESULT EQUAL 0)
    message(FATAL_ERROR "${ZHLN_TESTS_PUBLIC_API_ERROR}")
endif()
message(STATUS "${ZHLN_TESTS_PUBLIC_API_OUTPUT}")

# Reflection internals live in include/Zahlen/Core/Reflection.hpp and nowhere
# else; module interface units must never declare a detail namespace -- module
# internals are internal by not being exported, and a detail namespace only
# exists to be exported by accident. Both are configured-invariants: a stray
# std::meta/^^/[:...:] or a detail namespace would otherwise survive as a
# permanent public surface.
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_reflection_boundary.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE ZHLN_REFLECTION_BOUNDARY_RESULT
    OUTPUT_VARIABLE ZHLN_REFLECTION_BOUNDARY_OUTPUT
    ERROR_VARIABLE ZHLN_REFLECTION_BOUNDARY_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_STRIP_TRAILING_WHITESPACE
)
if(NOT ZHLN_REFLECTION_BOUNDARY_RESULT EQUAL 0)
    message(FATAL_ERROR "${ZHLN_REFLECTION_BOUNDARY_ERROR}")
endif()
message(STATUS "${ZHLN_REFLECTION_BOUNDARY_OUTPUT}")

set(ZHLN_TRANSPILER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/tools/transpile_reflection.py")

function(zahlen_transpile_sources OUT_SOURCES_VAR)
    if(COMPILER_HAS_REFLECTION)
        # Native support: Return sources untouched
        set(${OUT_SOURCES_VAR} ${ARGN} PARENT_SCOPE)
        return()
    endif()

    set(PROCESSED_SRCS "")
    set(ORIG_SOURCE_DIRS "")

    foreach(SRC IN LISTS ARGN)
        get_filename_component(ABS_SRC "${SRC}" ABSOLUTE)
        get_filename_component(ABS_DIR "${ABS_SRC}" DIRECTORY)
        list(APPEND ORIG_SOURCE_DIRS "${ABS_DIR}")

        file(RELATIVE_PATH REL_SRC "${CMAKE_SOURCE_DIR}" "${ABS_SRC}")
        set(OUT_SRC "${CMAKE_BINARY_DIR}/transpiled/${REL_SRC}")
        get_filename_component(OUT_DIR "${OUT_SRC}" DIRECTORY)

        file(MAKE_DIRECTORY "${OUT_DIR}")

        # At build time, run the Python transpiler using the compilation database
        add_custom_command(
            OUTPUT "${OUT_SRC}"
            COMMAND Python3::Interpreter "${ZHLN_TRANSPILER_SCRIPT}"
                    --input "${ABS_SRC}"
                    --output "${OUT_SRC}"
                    --compdb "${CMAKE_BINARY_DIR}"
            DEPENDS "${ABS_SRC}" "${ZHLN_TRANSPILER_SCRIPT}"
            COMMENT "Flattening Reflection in ${REL_SRC}..."
            VERBATIM
        )
        list(APPEND PROCESSED_SRCS "${OUT_SRC}")
    endforeach()

    # Register original source directories so transpiled files in build/ can locate relative headers
    list(REMOVE_DUPLICATES ORIG_SOURCE_DIRS)
    include_directories(${ORIG_SOURCE_DIRS})

    set(${OUT_SOURCES_VAR} ${PROCESSED_SRCS} PARENT_SCOPE)
endfunction()

execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -dumpmachine
    OUTPUT_VARIABLE ZHLN_TARGET_TRIPLE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT ZHLN_TARGET_TRIPLE)
    set(ZHLN_TARGET_TRIPLE "${CMAKE_SYSTEM_PROCESSOR}-unknown-${CMAKE_SYSTEM_NAME}")
    string(TOLOWER "${ZHLN_TARGET_TRIPLE}" ZHLN_TARGET_TRIPLE)
endif()

if(NOT MSVC)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -fuse-ld=mold -Wl,--version ERROR_QUIET OUTPUT_VARIABLE MOLD_VERSION)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -fuse-ld=lld -Wl,--version ERROR_QUIET OUTPUT_VARIABLE LLD_VERSION)
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -fuse-ld=gold -Wl,--version ERROR_QUIET OUTPUT_VARIABLE GOLD_VERSION)

    if(MOLD_VERSION MATCHES "mold")
        message(STATUS "Using MOLD linker")
        add_link_options(-fuse-ld=mold)
        set(ZHLN_LINKER "mold")
    elseif(LLD_VERSION MATCHES "LLD")
        message(STATUS "Using LLD linker")
        add_link_options(-fuse-ld=lld)
        set(ZHLN_LINKER "lld")
    elseif(GOLD_VERSION MATCHES "GNU gold")
        message(STATUS "Using GOLD linker")
        add_link_options(-fuse-ld=gold)
        set(ZHLN_LINKER "gold")
    else()
        message(STATUS "Using default BFD linker")
        set(ZHLN_LINKER "bfd")
    endif()
    if(UNIX AND NOT APPLE)
        add_link_options(-rdynamic)
    endif()
else()
    set(ZHLN_LINKER "link.exe")
endif()

option(USE_SANITIZERS "Enable ASan and UBSan" OFF)

# Shared CTest environment. Keep this list independent of the sanitizer option:
# the Vulkan sandbox must reach every test in a normal build as well.
set(ZHLN_TEST_ENVIRONMENT "")

if(USE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
    # Marks the whole sanitizer profile. Tests whose consteval code builds
    # std::strings are broken by GCC bug 71962 (-fsanitize=undefined rejects
    # pointer null-checks during constant evaluation); see
    # tests/core/TestReflection.cpp and tests/extras/TestJSON.cpp.
    add_definitions(
        -D__ASAN_ENABLED__
        -DZHLN_SANITIZER_BUILD=1
        -DZHLN_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
    )

    list(APPEND ZHLN_TEST_ENVIRONMENT
        "ASAN_OPTIONS=protect_shadow_gap=0:detect_leaks=1:symbolize=1:halt_on_error=1"
        "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp:print_suppressions=0"
        "UBSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/ubsan.supp:print_stacktrace=1:halt_on_error=1"
        "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan.supp:halt_on_error=1"
    )
endif()

option(ZHLN_DEV_MODE "Enable raw loose-file asset loading for development" ON)
if(ZHLN_DEV_MODE)
    add_compile_definitions(ZHLN_DEV_MODE)
endif()

# Absolute project root (forward slashes also on Windows) for code that reads
# or writes repo-root files at runtime, e.g. the performance baseline cache
# (tests/extras/profile/PerfBaseline.hpp). Defined once here so every target shares it
# instead of each consumer inventing its own path plumbing.
string(REPLACE "\\" "/" ZHLN_PROJECT_ROOT_FWD "${PROJECT_SOURCE_DIR}")
add_compile_definitions(ZHLN_PROJECT_ROOT="${ZHLN_PROJECT_ROOT_FWD}")

option(ZHLN_IN_DOCKER "Build running inside a Docker container" OFF)
if(ZHLN_IN_DOCKER)
    add_compile_definitions(ZHLN_IN_DOCKER=1)
    message(STATUS "Docker container build mode enabled.")
endif()

# --- OPT-IN HERMETIC VULKAN TEST ENVIRONMENT ---
# The checked-in lavapipe ICD makes a test run independent of whatever ICDs and
# validation layers happen to be installed on the host. It is OPT-IN, because
# VK_DRIVER_FILES *replaces* the loader's ICD search path instead of extending
# it: pinning it hides real hardware completely, so an ordinary bare-metal dev
# machine (Arch + Hyprland + a discrete GPU, no container anywhere) would
# silently run every render suite on the CPU rasteriser and print
# "[VULKAN] Selected physical device: llvmpipe" while a perfectly good GPU sat
# in the same box. Device scoring was never the problem — the GPU simply was
# never enumerated (see ZHLN_Internal_DefaultScoreFn in src/render/RenderCore.c,
# which ranks a discrete GPU at 1'000'000 and a CPU device at 0).
#
# Default OFF: tests use the host Vulkan installation. Turn it ON for a hermetic
# CPU-only run. Docker never needs it — the Dockerfile points the distro's
# Lavapipe at the whole image — and ZHLN_IN_DOCKER keeps this block off so the
# two mechanisms never stack.
#
# The pin applies to the CPU test groups only. tests/render declares itself
# "local hardware required", so the GPU groups always see the host drivers.
option(ZHLN_TEST_VULKAN_SANDBOX
    "Pin the CPU test groups to the vendored lavapipe ICD instead of the host Vulkan drivers (hides real GPUs)"
    OFF)

set(ZHLN_VULKAN_SANDBOX_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan_sandbox/linux-x86_64")
set(ZHLN_VULKAN_SANDBOX_ENVIRONMENT "")

if(ZHLN_TEST_VULKAN_SANDBOX AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT ZHLN_IN_DOCKER)
    find_file(ZHLN_VULKAN_SANDBOX_ICD
        NAMES lvp_icd.json
        PATHS "${ZHLN_VULKAN_SANDBOX_ROOT}/icd.d"
        NO_DEFAULT_PATH
    )

    if(ZHLN_VULKAN_SANDBOX_ICD)
        list(APPEND ZHLN_VULKAN_SANDBOX_ENVIRONMENT
            "VK_DRIVER_FILES=${ZHLN_VULKAN_SANDBOX_ICD}"
            "VK_ICD_FILENAMES=${ZHLN_VULKAN_SANDBOX_ICD}"
            "VK_LAYER_PATH=${ZHLN_VULKAN_SANDBOX_ROOT}/explicit_layer.d"
            "LD_LIBRARY_PATH=${ZHLN_VULKAN_SANDBOX_ROOT}/lib"
        )
        message(STATUS
            "Using vendored Vulkan sandbox for the CPU test groups: "
            "${ZHLN_VULKAN_SANDBOX_ROOT} (GPU test groups still use the host drivers)")
    else()
        message(WARNING
            "ZHLN_TEST_VULKAN_SANDBOX=ON but no ICD was found at "
            "${ZHLN_VULKAN_SANDBOX_ROOT}/icd.d/lvp_icd.json; "
            "tests will use the host Vulkan installation.")
    endif()
else()
    message(STATUS
        "Tests will use the host Vulkan installation and its real GPUs "
        "(-DZHLN_TEST_VULKAN_SANDBOX=ON pins the CPU test groups to the vendored lavapipe ICD).")
endif()

# Workaround for Clang P2996 revisions where copying a reflected
# annotation value fails constant evaluation ('read of object outside
# its lifetime' in libc++'s optional, observed on macOS/arm64): error
# messages fall back to plain enumerator names.
option(ZHLN_NO_ANNOTATION_EXTRACT "Disable compile-time reflection annotation extraction" OFF)
if(ZHLN_NO_ANNOTATION_EXTRACT)
    add_compile_definitions(ZHLN_NO_ANNOTATION_EXTRACT)
endif()

option(ZHLN_BUILD_TESTS "Build engine test suite" ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(ZHLN_DEBUG=1)
endif()

if(APPLE)
    set(MI_OVERRIDE OFF CACHE BOOL "" FORCE) # Avoid macOS zone interposition bugs
else()
    set(MI_OVERRIDE ON CACHE BOOL "" FORCE)  # Stable on Linux and Windows
endif()

set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE) # Build static lib for easy embedding
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/mimalloc EXCLUDE_FROM_ALL)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/glfw SYSTEM)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/LuaJIT SYSTEM)

set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
set(USE_STD_INCLUDES ON CACHE BOOL "" FORCE)
set(USE_RTTI OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "Build Jolt Unit Tests" FORCE)
set(JPH_BUILD_TESTS OFF CACHE BOOL "Build Jolt Unit Tests" FORCE)

# Force the debug renderer to compile globally, even in Release configurations
add_compile_definitions(JPH_DEBUG_RENDERER)

set(JPH_BUILD_SHARED_LIBS ON CACHE BOOL "Build Jolt as shared library" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/JoltPhysics/Build SYSTEM)

# Force Jolt to export all symbols
target_compile_definitions(Jolt PUBLIC JPH_SHARED_LIBRARY)
set_target_properties(Jolt PROPERTIES
    CXX_VISIBILITY_PRESET default
    C_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/VulkanMemoryAllocator SYSTEM)

# --- meshoptimizer (VK_EXT_mesh_shader meshlet generation) ---
# Used by the offline cooker (zcook) and by the runtime glTF importer through
# include/Zahlen/Meshlet.hpp, so both produce identical meshlet streams.
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/meshoptimizer EXCLUDE_FROM_ALL SYSTEM)
set_target_properties(meshoptimizer PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/SPIRV-Reflect)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui SYSTEM)
set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(YOGA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YOGA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/yoga EXCLUDE_FROM_ALL SYSTEM)

if(APPLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-enum-enum-conversion -Wno-deprecated-anon-enum-enum-conversion")
endif()



set(RAW_ENGINE_SOURCES
    src/engine/AssertHandler.cpp
    src/engine/DefaultPreset.cpp
    src/engine/GUIEditor.cpp
    src/engine/graphics/RenderFrame.cpp
    src/engine/graphics/RenderGraphBuilder.cpp
    src/engine/graphics/RenderDrawCommands.cpp
    src/engine/graphics/RenderInit.cpp
    src/engine/graphics/init/RenderInitDevice.cpp
    src/engine/graphics/init/RenderInitHeaps.cpp
    src/engine/graphics/init/RenderInitTargets.cpp
    src/engine/graphics/init/RenderInitScenePipelines.cpp
    src/engine/graphics/init/RenderInitPostProcess.cpp
    src/engine/graphics/init/RenderInitUI.cpp
    src/engine/graphics/RenderResources.cpp
    src/engine/Engine.cpp
    src/engine/CreativeWorksFactory.cpp
    src/engine/graphics/MeshBuilder.cpp
    src/engine/graphics/TextureManager.cpp
    src/engine/system/TextureSystem.cpp
    src/engine/system/AnimationSystem.cpp
    src/engine/CreativeWorksManager.cpp
    src/engine/Window.cpp
    src/engine/Platform.cpp
    src/engine/Console.cpp
    src/engine/Profiler.cpp
    src/engine/Text.cpp
    src/engine/graphics/Resources.cpp
    src/engine/system/RenderSystem.cpp
    src/engine/system/GraphicsSettingsSync.cpp
    src/engine/system/DecalSystem.cpp
    src/engine/system/MovementSystem.cpp
    src/engine/system/AudioSystem.cpp
    src/engine/system/ArticulationSystem.cpp
    src/engine/system/CullingSystem.cpp
    src/engine/graphics/Sky.cpp
    src/engine/graphics/BRDFLUT.cpp
    src/engine/system/PhysicsSystem.cpp
    src/engine/system/TerrainSystem.cpp
    src/engine/system/LODSystem.cpp
    src/engine/system/ParticleSystem.cpp
    src/engine/CommandLine.cpp
    src/engine/system/TargetCameraSystem.cpp
    src/engine/TTYBackend.cpp
    src/engine/system/TransformSystem.cpp
    src/engine/system/PhysicsStateSystem.cpp
    src/engine/system/LightingSystem.cpp
    src/engine/system/InputSystem.cpp
    src/engine/system/CameraSystem.cpp
    src/engine/graphics/RenderDocInit.cpp
    src/engine/system/InteractionSystem.cpp
    src/engine/system/UIRenderSystem.cpp
    src/engine/system/UIInteractionSystem.cpp
    src/engine/WindowSurface.cpp
    src/engine/graphics/RenderProcedural.cpp
    src/engine/graphics/RenderPasses.cpp
    src/engine/graphics/RenderSetup.cpp
    src/engine/Scene.cpp
    src/engine/ScriptRunner.cpp
)

find_library(ZSTD_LIBRARY NAMES zstd zstd_static HINTS "/opt/homebrew/lib" "/usr/local/lib" "/usr/lib")
if(NOT ZSTD_LIBRARY)
    message(FATAL_ERROR "Could not find zstd library. Please install it (e.g., 'brew install zstd').")
endif()

if(APPLE)
    # Ensure CMake searches Homebrew's prefix for packages, headers, and libraries
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
    link_directories(/opt/homebrew/lib)
endif()

# Find PkgConfig on both Linux and macOS
find_package(PkgConfig REQUIRED)

# Linux-only dependencies
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    pkg_check_modules(LIBEVDEV REQUIRED IMPORTED_TARGET libevdev)
    pkg_check_modules(LIBSEAT REQUIRED IMPORTED_TARGET libseat)
endif()

# Dependencies needed on both Linux and macOS.
#
# simdjson is deliberately NOT here: JSON lives in extras/json, so the core
# engine has no JSON dependency and a core-only build needs no JSON library
# installed. extras/CMakeLists.txt finds it when the extras layer is built.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
    pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/render)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/threading)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/physics)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/ecs)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src/audio)

# --- ASSEMBLE COMPILER FLAGS METADATA ---
set(COMPILER_FLAGS_LIST "${CMAKE_CXX_FLAGS}")
if(CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" UPPER_BUILD_TYPE)
    set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} ${CMAKE_CXX_FLAGS_${UPPER_BUILD_TYPE}}")
else()
    set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} ${CMAKE_CXX_FLAGS_DEBUG}")
endif()

set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} ${ZHLN_COMPILE_WARNINGS_STR} -fno-exceptions -fno-rtti")

if(COMPILER_HAS_REFLECTION)
    set(COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST} -freflection")
endif()

string(STRIP "${COMPILER_FLAGS_LIST}" COMPILER_FLAGS_LIST)
string(REGEX REPLACE " +" " " COMPILER_FLAGS_LIST "${COMPILER_FLAGS_LIST}")

zahlen_transpile_sources(ENGINE_SOURCES ${RAW_ENGINE_SOURCES})

add_library(zahlen_engine SHARED ${ENGINE_SOURCES})

# The renderer (and Volk inside it) must bind to THIS image's symbols:
# executables linking zahlen_render's static archive alongside the engine
# (e.g. the extras GPU tests via zahlen_extras) embed their own copies of
# volkInitialize & friends, and those preempt the engine's PLT calls --
# leaving the engine's per-image dispatch table NULL after a "successful"
# ZHLN_EnsureVulkanLoader(). The version script localizes the renderer's
# symbols in this .so so they bind at link time. See cmake/zahlen_engine.map.
if(UNIX AND NOT APPLE)
    target_link_options(zahlen_engine PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/zahlen_engine.map"
    )
endif()

target_precompile_headers(zahlen_engine PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/src/render/Rendering.hpp>"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set_target_properties(zahlen_engine PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
endif()

set_target_properties(zahlen_engine PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
)

# --- COMPILE-TIME PROFILING ---
option(ZHLN_PROFILE_RENDER_FRAME_COMPILE "Enable GCC compile-time profiling for RenderFrame.cpp" OFF)

if(ZHLN_PROFILE_RENDER_FRAME_COMPILE)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set_source_files_properties(src/engine/graphics/RenderFrame.cpp PROPERTIES
            COMPILE_OPTIONS "-ftime-report"
        )
        message(STATUS "GCC compile-time profiling enabled for RenderFrame.cpp")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set_source_files_properties(src/engine/graphics/RenderFrame.cpp PROPERTIES
            COMPILE_OPTIONS "-ftime-trace"
        )
        message(STATUS "Clang time-trace profiling enabled for RenderFrame.cpp")
    endif()
endif()

target_compile_definitions(zahlen_engine PRIVATE
    ZHLN_ENGINE_BUILD
    JPH_DOUBLE_PRECISION
    JPH_SHARED_LIBRARY
    ZHLN_GIT_COMMIT_HASH="${ZHLN_GIT_HASH}"
    ZHLN_COMPILER_FLAGS="${COMPILER_FLAGS_LIST}"
    ZHLN_META_BUILDER="CMake ${CMAKE_VERSION}"
    ZHLN_BUILD_TOOL="${CMAKE_GENERATOR}"
    ZHLN_TARGET_TRIPLE="${ZHLN_TARGET_TRIPLE}"
    ZHLN_LINKER_NAME="${ZHLN_LINKER}"
    ZHLN_COMPILED_SCRIPTS_DIR="${CMAKE_BINARY_DIR}"
)

target_sources(zahlen_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/stbi_impl.c
)

# List all module interface files
set(ZHLN_MODULE_FILES
    modules/zahlen-core.cppm
    modules/zahlen-threading.cppm
    modules/zahlen-math.cppm
    modules/zahlen-ecs.cppm
    modules/zahlen-physics.cppm
    modules/zahlen-render.cppm
    modules/zahlen-audio.cppm
    modules/zahlen-scripting.cppm
    modules/zahlen-engine.cppm
    modules/zahlen.cppm
)

# Prevent CMake from injecting PCH before 'module;' on .cppm files
set_source_files_properties(${ZHLN_MODULE_FILES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Register C++20/26 module file set
target_sources(zahlen_engine
    PUBLIC
        FILE_SET CXX_MODULES
        TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/modules
        FILES ${ZHLN_MODULE_FILES}
)

target_link_libraries(zahlen_engine PRIVATE
    zahlen_render
    zahlen_physics
    zahlen_threading
    zahlen_ecs
    zahlen_audio
    glfw
    spirv_reflect
    imgui
    mimalloc-static
    meshoptimizer
    yogacore
    ${ZSTD_LIBRARY}
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(zahlen_engine PRIVATE PkgConfig::LIBEVDEV PkgConfig::LIBSEAT)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR APPLE)
    target_link_libraries(zahlen_engine PRIVATE PkgConfig::FONTCONFIG)
endif()

target_link_libraries(zahlen_engine PUBLIC Jolt)

target_include_directories(zahlen_engine SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/JoltPhysics
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/renderdoc
)
target_include_directories(zahlen_engine SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/yoga
    ${CMAKE_BINARY_DIR}/generated_shaders
)
target_include_directories(zahlen_engine SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/cgltf
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# --- SHADER COMPILATION ---
include(ShaderCompilation)
include(ECSCompiler)

# --- FENNEL COMPILER INTEGRATION & EXPORTED HELPERS ---
find_program(FENNEL_BIN NAMES fennel)

function(zahlen_compile_fennel TARGET_NAME OUTPUT_LUA_FILES_VAR)
    set(LUA_OUTPUTS "")
    foreach(FNL_SRC IN LISTS ARGN)
        get_filename_component(FILE_NAME_WE ${FNL_SRC} NAME_WLE)
        get_filename_component(FILE_DIR ${FNL_SRC} DIRECTORY)

        file(RELATIVE_PATH REL_PATH "${CMAKE_SOURCE_DIR}" "${FILE_DIR}")
        set(OUT_DIR "${CMAKE_BINARY_DIR}/${REL_PATH}")
        file(MAKE_DIRECTORY "${OUT_DIR}")

        set(OUTPUT_LUA "${OUT_DIR}/${FILE_NAME_WE}.lua")

        if(FENNEL_BIN)
            add_custom_command(
                OUTPUT ${OUTPUT_LUA}
                COMMAND ${FENNEL_BIN} --use-bit-lib --compile ${FNL_SRC} > ${OUTPUT_LUA}
                DEPENDS ${FNL_SRC}
                COMMENT "Fennel: Compiling ${FNL_SRC} -> ${OUTPUT_LUA}"
                VERBATIM
            )
            list(APPEND LUA_OUTPUTS ${OUTPUT_LUA})
        else()
            message(FATAL_ERROR "Fennel compiler ('fennel') not found in PATH. Cannot compile ${FNL_SRC}")
        endif()
    endforeach()
    set(${OUTPUT_LUA_FILES_VAR} ${LUA_OUTPUTS} PARENT_SCOPE)
endfunction()


# --- ENGINE LAUNCHER & EDITOR EXECUTABLE ---
# The composition root. It lives outside src/ on purpose: wiring an engine
# together means naming the optional layers it runs with, and src/ is the core
# library, which may not know extras exist. See tools/check_core_extras_boundary.py.
add_executable(zahlen
    app/main.cpp
)
target_link_libraries(zahlen PRIVATE zahlen_engine imgui Jolt)
target_compile_definitions(zahlen PRIVATE JPH_DOUBLE_PRECISION)

# The composition root reaches engine internals (engine/Platform.hpp,
# physics/PhysicsWorld.hpp). While it lived at src/main.cpp those resolved
# relative to the file; from app/ the path has to be explicit.
target_include_directories(zahlen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

set_target_properties(zahlen PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
)

# --- ASSET COMPILER (ZCOOK) ---
# zcook lives in tools/, not src/: it is the offline asset pipeline, and the
# off-line GLB emitter consumes extras/json's reflection serializer. Core
# (src/, include/, modules/) must never depend on extras -- see
# tools/check_core_extras_boundary.py -- so the cooker is outside core roots.
add_executable(zcook
    tools/zcook/main.cpp
    tools/zcook/Transform.cpp
    tools/zcook/GLB.cpp
    tools/zcook/Cook.cpp
)
target_link_libraries(zcook PRIVATE zahlen_engine meshoptimizer)
target_include_directories(zcook SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/cgltf
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/stb
    ${CMAKE_CURRENT_SOURCE_DIR}/extras
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/zcook
)


# --- ASSET COMPILER (ZCOOK) ---
set(ZHLN_SHARED_ASSET_DIR "${CMAKE_SOURCE_DIR}/build/shared_assets" CACHE PATH "Shared cooked asset cache")

function(zahlen_configure_game_assets GAME_SOURCE_DIR)
    file(MAKE_DIRECTORY "${ZHLN_SHARED_ASSET_DIR}")

    set(SHARED_ZCOOK "${ZHLN_SHARED_ASSET_DIR}/zcook")

    # 1. Symlink the current build's zcook to a STABLE shared location
    add_custom_command(
        TARGET zcook POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink "$<TARGET_FILE:zcook>" "${SHARED_ZCOOK}"
        COMMENT "Updating stable shared zcook symlink..."
    )

    # 2. Generate assets.ninja using the FIXED path (${SHARED_ZCOOK})
    add_custom_command(
        OUTPUT "${ZHLN_SHARED_ASSET_DIR}/assets.ninja"
        COMMAND python3 "${zahlen_SOURCE_DIR}/configure_assets.py"
                "${ZHLN_SHARED_ASSET_DIR}/assets.ninja"
                "${SHARED_ZCOOK}"
                "${GAME_SOURCE_DIR}"
        WORKING_DIRECTORY "${ZHLN_SHARED_ASSET_DIR}"
        DEPENDS "${zahlen_SOURCE_DIR}/configure_assets.py"
                "${zahlen_SOURCE_DIR}/tools/export_metadata.py"
        COMMENT "Scanning asset folders and generating assets.ninja..."
    )

    # 3. Use symlink for base.pak
    if(WIN32)
        set(PAK_LINK_CMD ${CMAKE_COMMAND} -E copy "${ZHLN_SHARED_ASSET_DIR}/data/base.pak" "$<TARGET_FILE_DIR:zahlen>/data/base.pak")
    else()
        set(PAK_LINK_CMD ${CMAKE_COMMAND} -E create_symlink "${ZHLN_SHARED_ASSET_DIR}/data/base.pak" "$<TARGET_FILE_DIR:zahlen>/data/base.pak")
    endif()

    add_custom_target(cook_assets ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:zahlen>/data"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${ZHLN_SHARED_ASSET_DIR}/build_assets"
        # Run ninja inside the shared directory with persistent .ninja_log
        COMMAND ninja -C "${ZHLN_SHARED_ASSET_DIR}" -f "${ZHLN_SHARED_ASSET_DIR}/assets.ninja"
        COMMAND ${PAK_LINK_CMD}
        DEPENDS "${ZHLN_SHARED_ASSET_DIR}/assets.ninja" zcook
        WORKING_DIRECTORY "${ZHLN_SHARED_ASSET_DIR}"
        COMMENT "Cooking assets in parallel using Ninja..."
        USES_TERMINAL
    )
    add_dependencies(zahlen cook_assets)
endfunction()

# If building engine standalone (not as a submodule), cook local engine assets
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    zahlen_configure_game_assets("${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(NOT MSVC)
    # Module producers and every transitive consumer must agree on these ABI
    # modes. Publish them from the engine target so core/extras PCMs, tools,
    # samples, and tests cannot drift into incompatible configurations.
    target_compile_options(zahlen_engine
        PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
        PRIVATE
            $<$<AND:$<CXX_COMPILER_ID:GNU>,$<COMPILE_LANGUAGE:CXX>>:${GCC_ONLY_FLAGS}>
            $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
    )
    if(WIN32)
        target_link_options(zahlen_engine PRIVATE -Xlinker /subsystem:console)
        target_compile_options(zahlen_engine PRIVATE -Wno-microsoft-anon-tag -Wno-unknown-pragmas -Wno-unused-parameter)
        target_compile_options(zahlen_engine PRIVATE -Wno-microsoft-anon-tag -Wno-unknown-pragmas -Wno-unused-parameter -Wno-microsoft-enum-forward-reference)
        target_compile_options(zahlen_engine PRIVATE -D_CRT_SECURE_NO_WARNINGS)
    endif()
endif()

target_compile_options(zahlen_render PRIVATE
    $<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU>>:-freflection>
)

add_custom_target(link_resources ALL
    COMMAND ${CMAKE_COMMAND}
        -D SRC="${CMAKE_CURRENT_SOURCE_DIR}/resources"
        -D DST="${CMAKE_BINARY_DIR}/resources"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CreateSymlinks.cmake"
    COMMENT "Creating zero-copy symlinks for resources..."
)

add_dependencies(zahlen link_resources)

if(USE_SANITIZERS)
    get_filename_component(CLANG_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
    find_file(ASAN_DLL
        NAMES "clang_rt.asan_dynamic-x86_64.dll"
        PATHS "${CLANG_BIN_DIR}/../lib/clang/23/lib/windows" "${CLANG_BIN_DIR}/../lib/clang/22/lib/windows"
    )
    if(ASAN_DLL)
        add_custom_command(TARGET zahlen POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy "${ASAN_DLL}" "$<TARGET_FILE_DIR:zahlen>"
            COMMENT "Copying ASan Runtime for zahlen..."
        )
    endif()
endif()

# --- APPLY TARGET-SPECIFIC REFLECTION FLAGS ---

function(zahlen_use_custom_libcxx TARGET_NAME)
    option(ZHLN_USE_CUSTOM_LIBCXX "Link against Bloomberg custom libc++" OFF)

    # Only apply custom LLVM libc++ when explicitly enabled on Clang
    if(NOT ZHLN_USE_CUSTOM_LIBCXX OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        return()
    endif()

    get_filename_component(_DEFAULT_LLVM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../llvm-p2996" ABSOLUTE)
    set(LLVM_BLOOMBERG_ROOT "${_DEFAULT_LLVM_ROOT}" CACHE PATH "Path to Bloomberg LLVM monorepo")
    set(LLVM_BLOOMBERG_BUILD "${LLVM_BLOOMBERG_ROOT}/build" CACHE PATH "Path to Bloomberg LLVM build directory")

    if(NOT EXISTS "${LLVM_BLOOMBERG_ROOT}")
        message(FATAL_ERROR "ZHLN_USE_CUSTOM_LIBCXX is ON, but LLVM root was not found at: ${LLVM_BLOOMBERG_ROOT}\n"
                           "Pass -DLLVM_BLOOMBERG_ROOT=/path/to/llvm-p2996 to configure manually.")
    endif()

    file(GLOB _LLVM_LIB_SUBDIRS LIST_DIRECTORIES true "${LLVM_BLOOMBERG_BUILD}/lib/*")
    set(_ALL_LLVM_LIB_DIRS "${LLVM_BLOOMBERG_BUILD}/lib")
    foreach(_DIR IN LISTS _LLVM_LIB_SUBDIRS)
        if(IS_DIRECTORY "${_DIR}")
            list(APPEND _ALL_LLVM_LIB_DIRS "${_DIR}")
        endif()
    endforeach()

    target_compile_options(${TARGET_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>
    )

    foreach(_LIB_DIR IN LISTS _ALL_LLVM_LIB_DIRS)
        target_link_options(${TARGET_NAME} PRIVATE
            $<$<PLATFORM_ID:Linux>:-L${_LIB_DIR}>
            $<$<PLATFORM_ID:Linux>:-Wl,-rpath,${_LIB_DIR}>
        )
    endforeach()

    target_link_options(${TARGET_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>
    )

    target_link_libraries(${TARGET_NAME} PRIVATE
        c++
        c++abi
        unwind
    )
endfunction()

function(zahlen_enable_reflection TARGET_NAME)
    if(COMPILER_HAS_REFLECTION)
        target_compile_options(${TARGET_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-freflection>")
        if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_compile_options(${TARGET_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-fannotation-attributes>")
        endif()
        target_compile_definitions(${TARGET_NAME} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:__cpp_impl_reflection=202603L>")
    endif()
    zahlen_use_custom_libcxx(${TARGET_NAME})
endfunction()

zahlen_enable_reflection(zahlen_render)
zahlen_enable_reflection(zahlen_engine)
zahlen_enable_reflection(zahlen_audio)
zahlen_enable_reflection(zahlen_physics)
zahlen_enable_reflection(zahlen_ecs)
zahlen_enable_reflection(zahlen_threading)
zahlen_enable_reflection(zahlen)
zahlen_enable_reflection(zcook)

option(ZHLN_BUILD_EXTRAS "Build first-party engine feature modules and VFX toolkits" ON)

if(ZHLN_BUILD_EXTRAS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extras")
    add_subdirectory(extras)

    # The engine itself has no scripting; the composition root installs a runtime
    # if one was built. Without it ScriptRunner forwards to nothing and the
    # engine runs C++-only.
    if(TARGET zahlen_scripting_lua)
        target_link_libraries(zahlen PRIVATE zahlen_scripting_lua)
        target_compile_definitions(zahlen PRIVATE ZHLN_HAS_SCRIPTING=1)
    endif()
endif()

# --- TESTS ---
if(ZHLN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# --- SAMPLES ---
option(ZHLN_BUILD_SAMPLES "Build engine sample applications" ON)

if(ZHLN_BUILD_SAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/samples")
    add_subdirectory(samples)
endif()


target_link_libraries(zahlen_engine PRIVATE zahlen_warnings)
