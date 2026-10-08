# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# cmake/ThirdParty.cmake
#
# Every option and add_subdirectory for the vendored libraries under extern/
# and the small handful of tracked third_party/ projects. The root CMakeLists
# includes this once, before the core subsystems, so the vendor cache variables
# below are settled before any vendor CMakeLists runs.

# Vulkan-Headers is a vendored submodule. Its CMake project provides the
# Vulkan-Headers interface target; no installed SDK or loader is required.
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/Vulkan-Headers EXCLUDE_FROM_ALL SYSTEM)

# --- mimalloc ---
if(APPLE)
    set(MI_OVERRIDE OFF CACHE BOOL "" FORCE) # Avoid macOS zone interposition bugs
else()
    set(MI_OVERRIDE ON CACHE BOOL "" FORCE)  # Stable on Linux and Windows
endif()

set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE) # Build static lib for easy embedding
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/mimalloc EXCLUDE_FROM_ALL)

# --- Apple SDK Blocks support for C sources ---
# CoreAudio/AudioToolbox headers expose Blocks declarations. Miniaudio's
# implementation and GLFW's Cocoa backend are both C-family code, so probe the
# actual SDK headers with the project C compiler (the C++ -fblocks check does
# not cover it). If it cannot parse them, only the affected C/Objective-C
# targets are routed through Blocks-capable Clang; project C++ stays untouched.
if(APPLE)
    include(CheckCSourceCompiles)
    set(_zhln_apple_sdk_blocks_probe_source [=[
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
typedef void (^ZHLNBlock)(void);
int main(void) { return sizeof(ZHLNBlock) == 0; }
]=])
    set(_zhln_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    string(APPEND CMAKE_REQUIRED_FLAGS " -fblocks")
    check_c_source_compiles(
        "${_zhln_apple_sdk_blocks_probe_source}"
        ZHLN_C_BLOCKS_SUPPORTED
    )
    set(CMAKE_REQUIRED_FLAGS "${_zhln_saved_required_flags}")
    unset(_zhln_saved_required_flags)

    set(_zhln_use_blocks_clang FALSE)
    set(ZHLN_BLOCKS_C_COMPILER_LAUNCHER "")
    if(NOT ZHLN_C_BLOCKS_SUPPORTED)
        # Migrate an override cached by the former GLFW-only fallback.
        set(ZHLN_BLOCKS_C_COMPILER "" CACHE FILEPATH
            "Blocks-capable C compiler used only for Apple C/Objective-C sources that require Blocks")
        if(NOT ZHLN_BLOCKS_C_COMPILER AND ZHLN_GLFW_C_COMPILER)
            set(ZHLN_BLOCKS_C_COMPILER "${ZHLN_GLFW_C_COMPILER}" CACHE FILEPATH
                "Blocks-capable C compiler used only for Apple C/Objective-C sources that require Blocks" FORCE)
        endif()
        if(NOT ZHLN_BLOCKS_C_COMPILER)
            find_program(_zhln_blocks_clang
                NAMES clang
                HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin
            )
            if(_zhln_blocks_clang)
                set(ZHLN_BLOCKS_C_COMPILER "${_zhln_blocks_clang}" CACHE FILEPATH
                    "Blocks-capable C compiler used only for Apple C/Objective-C sources that require Blocks" FORCE)
            endif()
        endif()

        if(NOT ZHLN_BLOCKS_C_COMPILER)
            message(FATAL_ERROR
                "Apple CoreAudio/AudioToolbox C sources need a compiler with Blocks support. "
                "No Clang was found; install Apple/Homebrew Clang or set "
                "-DZHLN_BLOCKS_C_COMPILER=/path/to/clang."
            )
        endif()

        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/CMakeFiles")
        set(_zhln_apple_sdk_blocks_probe "${CMAKE_BINARY_DIR}/CMakeFiles/zhln-apple-sdk-blocks-probe.c")
        file(WRITE "${_zhln_apple_sdk_blocks_probe}" "${_zhln_apple_sdk_blocks_probe_source}")
        set(_zhln_apple_sdk_blocks_probe_flags -std=c11 -fblocks)
        if(CMAKE_OSX_SYSROOT)
            list(APPEND _zhln_apple_sdk_blocks_probe_flags -isysroot "${CMAKE_OSX_SYSROOT}")
        endif()
        foreach(_zhln_apple_sdk_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
            list(APPEND _zhln_apple_sdk_blocks_probe_flags -arch "${_zhln_apple_sdk_arch}")
        endforeach()
        execute_process(
            COMMAND "${ZHLN_BLOCKS_C_COMPILER}" ${_zhln_apple_sdk_blocks_probe_flags} -fsyntax-only "${_zhln_apple_sdk_blocks_probe}"
            RESULT_VARIABLE _zhln_apple_sdk_blocks_result
            ERROR_VARIABLE _zhln_apple_sdk_blocks_error
        )
        file(REMOVE "${_zhln_apple_sdk_blocks_probe}")
        if(NOT "${_zhln_apple_sdk_blocks_result}" STREQUAL "0")
            message(FATAL_ERROR
                "${ZHLN_BLOCKS_C_COMPILER} was selected for Apple SDK C sources but cannot parse "
                "CoreAudio/AudioToolbox with -fblocks:\n${_zhln_apple_sdk_blocks_error}"
            )
        endif()

        if(NOT CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
            message(FATAL_ERROR
                "Routing only the Blocks-dependent C/Objective-C targets through a separate compiler "
                "requires a Ninja or Makefile generator (compiler launchers are not supported by ${CMAKE_GENERATOR})."
            )
        endif()

        set(_zhln_use_blocks_clang TRUE)
        set(ZHLN_BLOCKS_C_COMPILER_LAUNCHER
            "${CMAKE_CURRENT_LIST_DIR}/BlocksCCompilerLauncher.sh;${ZHLN_BLOCKS_C_COMPILER}"
        )
        message(STATUS
            "Apple SDK C/Objective-C sources needing Blocks will compile with ${ZHLN_BLOCKS_C_COMPILER} (-fblocks)"
        )
    endif()
endif()

# --- GLFW ---
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE)
    # GLFW 3.4+ always compiles the null platform. X11/Wayland are extra
    # backends that need their -dev packages; without them, glfwInit fails
    # over to TTY/KMS instead of failing configure.
    find_package(X11 QUIET)
    set(_zhln_glfw_x11 FALSE)
    if(X11_FOUND
       AND X11_Xrandr_INCLUDE_PATH
       AND X11_Xinerama_INCLUDE_PATH
       AND X11_Xkb_INCLUDE_PATH
       AND X11_Xcursor_INCLUDE_PATH
       AND X11_Xi_INCLUDE_PATH
       AND X11_Xshape_INCLUDE_PATH)
        set(_zhln_glfw_x11 TRUE)
    endif()

    set(_zhln_glfw_wayland FALSE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(ZHLN_WAYLAND QUIET
            wayland-client>=0.2.7
            wayland-cursor>=0.2.7
            wayland-egl>=0.2.7
            xkbcommon>=0.5.0)
        find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)
        if(ZHLN_WAYLAND_FOUND AND WAYLAND_SCANNER_EXECUTABLE)
            set(_zhln_glfw_wayland TRUE)
        endif()
    endif()

    set(GLFW_BUILD_X11 ${_zhln_glfw_x11} CACHE BOOL "Build GLFW with X11" FORCE)
    set(GLFW_BUILD_WAYLAND ${_zhln_glfw_wayland} CACHE BOOL "Build GLFW with Wayland" FORCE)
    if(_zhln_glfw_x11 OR _zhln_glfw_wayland)
        message(STATUS "GLFW window backends: X11=${_zhln_glfw_x11} Wayland=${_zhln_glfw_wayland}")
    else()
        message(STATUS "No X11/Wayland development files; GLFW builds the null platform only.")
    endif()
endif()
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/glfw SYSTEM)

# The macOS SDK marks sprintf deprecated and the vendored cocoa joystick
# still uses it; silence the deprecation noise on the vendored target only.
if(APPLE)
    target_compile_options(glfw PRIVATE
        -Wno-deprecated-declarations
        "$<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:OBJC>>:-fblocks>"
    )
    if(_zhln_use_blocks_clang)
        set_property(TARGET glfw PROPERTY C_COMPILER_LAUNCHER "${ZHLN_BLOCKS_C_COMPILER_LAUNCHER}")
        set_property(TARGET glfw PROPERTY OBJC_COMPILER_LAUNCHER "${ZHLN_BLOCKS_C_COMPILER_LAUNCHER}")
    endif()
endif()

# --- LuaJIT ---
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/LuaJIT SYSTEM)

# LuaJIT's CMake produces the static library only. The CLI is what drives the
# vendored Fennel compiler (tools/fennelc.lua); host `fennel` is not required.
if(TARGET LuaJIT AND EXISTS "${CMAKE_SOURCE_DIR}/extern/LuaJIT/src/luajit.c" AND NOT TARGET luajit)
    add_executable(luajit "${CMAKE_SOURCE_DIR}/extern/LuaJIT/src/luajit.c")
    target_link_libraries(luajit PRIVATE LuaJIT)
    set_target_properties(luajit PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS ON
    )
    if(UNIX AND NOT APPLE)
        target_link_libraries(luajit PRIVATE dl m)
    elseif(UNIX)
        target_link_libraries(luajit PRIVATE m)
    endif()
endif()

# --- Jolt Physics ---
set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
set(USE_STD_INCLUDES ON CACHE BOOL "" FORCE)
set(USE_RTTI OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "Build Jolt Unit Tests" FORCE)
set(JPH_BUILD_TESTS OFF CACHE BOOL "Build Jolt Unit Tests" FORCE)

if(APPLE)
    # Jolt enables its Metal backend by default, which adds Objective-C++ files
    # that include modern MetalKit/ModelIO SDK headers and use Blocks. Probe the
    # actual OBJCXX frontend; if it cannot parse these headers, leave Jolt's
    # Vulkan/CPU compute choices intact and omit only the optional Metal backend.
    include(CheckSourceCompiles)
    block()
        set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
        string(APPEND CMAKE_REQUIRED_FLAGS " -fblocks")
        check_source_compiles(OBJCXX [=[
#import <MetalKit/MetalKit.h>
#import <ModelIO/ModelIO.h>
#include <simd/simd.h>
#include <dispatch/dispatch.h>

typedef void (^ZHLNMetalBlockProbe)(void);

int main(void) {
    ZHLNMetalBlockProbe block = nullptr;
    (void)block;
    return sizeof(vector_float2) == 0;
}
]=] ZHLN_JOLT_MTL_OBJCXX_SUPPORTED)
    endblock()

    if(NOT ZHLN_JOLT_MTL_OBJCXX_SUPPORTED)
        set(JPH_USE_MTL OFF CACHE BOOL "Use Metal" FORCE)
        message(STATUS
            "Disabling Jolt Metal compute: ${CMAKE_OBJCXX_COMPILER_ID} (${CMAKE_OBJCXX_COMPILER}) cannot compile the MetalKit/ModelIO Objective-C++ headers with -fblocks. "
            "Jolt's other configured compute backends are unchanged."
        )
    else()
        message(STATUS
            "Jolt Metal Objective-C++ SDK probe passed with ${CMAKE_OBJCXX_COMPILER_ID} (${CMAKE_OBJCXX_COMPILER})."
        )
    endif()
endif()

# Force the debug renderer to compile globally, even in Release configurations
add_compile_definitions(JPH_DEBUG_RENDERER)

set(JPH_BUILD_SHARED_LIBS ON CACHE BOOL "Build Jolt as shared library" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/JoltPhysics/Build SYSTEM)

# Force Jolt to export all symbols
target_compile_definitions(Jolt PUBLIC JPH_SHARED_LIBRARY)
set_target_properties(Jolt PROPERTIES
    CXX_VISIBILITY_PRESET default
    C_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

# --- VulkanMemoryAllocator ---
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/VulkanMemoryAllocator SYSTEM)

# --- meshoptimizer (VK_EXT_mesh_shader meshlet generation) ---
# Used by the offline cooker (zcook) and by the runtime glTF importer through
# include/Zahlen/Meshlet.hpp, so both produce identical meshlet streams.
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/meshoptimizer EXCLUDE_FROM_ALL SYSTEM)
set_target_properties(meshoptimizer PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- SPIRV-Reflect (tracked in third_party/) ---
add_subdirectory(${CMAKE_SOURCE_DIR}/third_party/SPIRV-Reflect)

# --- simdjson (vendored at extern/simdjson) ---
# plugins/json is its only consumer. Provisioned here like every other extern/
# entry so no platform needs a system simdjson: pkg-config discovery has no
# story on a bare Windows toolchain, which is exactly what used to break
# plugins/json there. As a subproject simdjson turns its developer mode and
# install rules off by itself; BUILD_SHARED_LIBS is OFF project-wide, so its
# simdjson target is a static archive, aliased as simdjson::simdjson.
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/simdjson EXCLUDE_FROM_ALL SYSTEM)

# --- test-harness toggles for vendor projects ---
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Clang-specific warning groups. GCC silently accepts unknown -Wno-* flags
    # until another diagnostic makes it print an "unrecognized option" note.
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-enum-enum-conversion -Wno-deprecated-anon-enum-enum-conversion")
endif()

# --- zstd ---
find_library(ZSTD_LIBRARY NAMES zstd zstd_static HINTS "/opt/homebrew/lib" "/usr/local/lib" "/usr/lib")
if(NOT ZSTD_LIBRARY)
    message(FATAL_ERROR "Could not find zstd library. Please install it (e.g., 'brew install zstd').")
endif()

if(APPLE)
    # Ensure CMake searches Homebrew's prefix for packages, headers, and libraries
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
    link_directories(/opt/homebrew/lib)
endif()

# libseat and libevdev are loaded at runtime by src/engine/tty (SharedLibrary).
# They are not link-time or configure-time dependencies: a machine without the
# -dev packages can still compile, and a machine without the .so simply cannot
# take the TTY display fallback.
