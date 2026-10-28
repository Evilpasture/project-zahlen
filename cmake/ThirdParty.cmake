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
if(ZHLN_BUILD_RENDER)
    add_subdirectory(${CMAKE_SOURCE_DIR}/extern/Vulkan-Headers EXCLUDE_FROM_ALL SYSTEM)
endif()

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
# Prefer a system GLFW 3.4+ package on Unix when available (Window.cpp and
# EngineGlobals.cpp require GLFW 3.4's runtime platform selection APIs:
# glfwGetPlatform, GLFW_PLATFORM_WAYLAND/X11, glfwInitHint(GLFW_PLATFORM, ...)),
# and fall back to building the vendored extern/glfw from source otherwise.
option(ZHLN_GLFW_VENDORED
    "Build and use the vendored extern/glfw, ignoring any system GLFW" OFF)

set(ZHLN_HAS_GLFW FALSE)
if(UNIX AND NOT ZHLN_GLFW_VENDORED)
    find_path(_ZHLN_PROBE_GLFW_H
        NAMES GLFW/glfw3.h
        HINTS "${GLFW_ROOT}" ENV GLFW_ROOT /opt/homebrew/opt/glfw /usr/local
        PATH_SUFFIXES include
        QUIET
    )
    if(NOT _ZHLN_PROBE_GLFW_H)
        unset(_ZHLN_PROBE_GLFW_H CACHE)
    endif()

    if(_ZHLN_PROBE_GLFW_H OR DEFINED ENV{PKG_CONFIG_PATH})
        find_package(glfw3 3.4 CONFIG QUIET)
        if(glfw3_FOUND AND TARGET glfw)
            set_target_properties(glfw PROPERTIES IMPORTED_GLOBAL TRUE)
            get_target_property(_zhln_sys_glfw_inc glfw INTERFACE_INCLUDE_DIRECTORIES)
            if(_zhln_sys_glfw_inc)
                target_include_directories(glfw SYSTEM INTERFACE ${_zhln_sys_glfw_inc})
            endif()
            set(ZHLN_HAS_GLFW TRUE)
            message(STATUS "Using system GLFW ${glfw3_VERSION} (CMake package)")
        endif()

        if(NOT ZHLN_HAS_GLFW)
            find_package(PkgConfig QUIET)
            if(PkgConfig_FOUND)
                pkg_check_modules(ZHLN_SYS_GLFW QUIET IMPORTED_TARGET glfw3>=3.4)
            endif()
            if(ZHLN_SYS_GLFW_FOUND AND TARGET PkgConfig::ZHLN_SYS_GLFW)
                add_library(glfw INTERFACE IMPORTED GLOBAL)
                target_link_libraries(glfw INTERFACE PkgConfig::ZHLN_SYS_GLFW)
                if(ZHLN_SYS_GLFW_INCLUDE_DIRS)
                    target_include_directories(glfw SYSTEM INTERFACE ${ZHLN_SYS_GLFW_INCLUDE_DIRS})
                elseif(_ZHLN_PROBE_GLFW_H)
                    target_include_directories(glfw SYSTEM INTERFACE "${_ZHLN_PROBE_GLFW_H}")
                endif()
                set(ZHLN_HAS_GLFW TRUE)
                message(STATUS "Using system GLFW ${ZHLN_SYS_GLFW_VERSION} (pkg-config)")
            endif()
        endif()

        if(NOT ZHLN_HAS_GLFW AND _ZHLN_PROBE_GLFW_H AND EXISTS "${_ZHLN_PROBE_GLFW_H}/GLFW/glfw3native.h")
            file(STRINGS "${_ZHLN_PROBE_GLFW_H}/GLFW/glfw3.h" _zhln_glfw_ver_lines
                REGEX "^#define GLFW_VERSION_(MAJOR|MINOR)[ \t]+[0-9]+")
            set(_zhln_glfw_major 0)
            set(_zhln_glfw_minor 0)
            foreach(_line IN LISTS _zhln_glfw_ver_lines)
                if(_line MATCHES "GLFW_VERSION_MAJOR[ \t]+([0-9]+)")
                    set(_zhln_glfw_major "${CMAKE_MATCH_1}")
                elseif(_line MATCHES "GLFW_VERSION_MINOR[ \t]+([0-9]+)")
                    set(_zhln_glfw_minor "${CMAKE_MATCH_1}")
                endif()
            endforeach()
            if("${_zhln_glfw_major}.${_zhln_glfw_minor}" VERSION_GREATER_EQUAL "3.4")
                find_library(_ZHLN_SYS_GLFW_LIB
                    NAMES glfw glfw3
                    HINTS "${GLFW_ROOT}" ENV GLFW_ROOT /opt/homebrew/opt/glfw /usr/local
                    QUIET
                )
                if(_ZHLN_SYS_GLFW_LIB)
                    find_package(Threads QUIET)
                    add_library(glfw INTERFACE IMPORTED GLOBAL)
                    target_include_directories(glfw SYSTEM INTERFACE "${_ZHLN_PROBE_GLFW_H}")
                    target_link_libraries(glfw INTERFACE "${_ZHLN_SYS_GLFW_LIB}" Threads::Threads ${CMAKE_DL_LIBS})
                    set(ZHLN_HAS_GLFW TRUE)
                    message(STATUS "Using system GLFW ${_zhln_glfw_major}.${_zhln_glfw_minor}: ${_ZHLN_SYS_GLFW_LIB}")
                else()
                    unset(_ZHLN_SYS_GLFW_LIB CACHE)
                endif()
            endif()
        endif()
    endif()
endif()

if(ZHLN_HAS_GLFW)
    target_compile_definitions(glfw INTERFACE GLFW_INCLUDE_NONE)

    if(UNIX AND NOT APPLE)
        # src/window/CMakeLists.txt gates ZHLN_WINDOW_NATIVE_{X11,WAYLAND} on
        # _zhln_glfw_{x11,wayland}, which makes Window.cpp include
        # <GLFW/glfw3native.h> and call glfwGet{X11,Wayland}{Display,Window}.
        # Probe both header availability and exported symbols on the system
        # library so native Vulkan surfaces match what system GLFW compiled in.
        include(CheckCSourceCompiles)

        set(_zhln_glfw_x11 FALSE)
        find_path(_ZHLN_PROBE_XLIB_H NAMES X11/Xlib.h QUIET)
        find_path(_ZHLN_PROBE_XRANDR_H NAMES X11/extensions/Xrandr.h QUIET)
        if(_ZHLN_PROBE_XLIB_H AND _ZHLN_PROBE_XRANDR_H)
            block(PROPAGATE ZHLN_SYS_GLFW_HAS_X11)
                set(CMAKE_REQUIRED_INCLUDES "${_ZHLN_PROBE_XLIB_H}" "${_ZHLN_PROBE_XRANDR_H}")
                set(CMAKE_REQUIRED_LIBRARIES glfw)
                check_c_source_compiles([=[
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_X11
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
int main(void) { return glfwGetX11Display() != 0; }
]=] ZHLN_SYS_GLFW_HAS_X11)
            endblock()
            if(ZHLN_SYS_GLFW_HAS_X11)
                set(_zhln_glfw_x11 TRUE)
                target_include_directories(glfw SYSTEM INTERFACE "${_ZHLN_PROBE_XLIB_H}" "${_ZHLN_PROBE_XRANDR_H}")
            endif()
        else()
            if(NOT _ZHLN_PROBE_XLIB_H)
                unset(_ZHLN_PROBE_XLIB_H CACHE)
            endif()
            if(NOT _ZHLN_PROBE_XRANDR_H)
                unset(_ZHLN_PROBE_XRANDR_H CACHE)
            endif()
        endif()

        set(_zhln_glfw_wayland FALSE)
        find_path(_ZHLN_PROBE_WAYLAND_H NAMES wayland-client.h QUIET)
        if(_ZHLN_PROBE_WAYLAND_H)
            block(PROPAGATE ZHLN_SYS_GLFW_HAS_WAYLAND)
                set(CMAKE_REQUIRED_INCLUDES "${_ZHLN_PROBE_WAYLAND_H}")
                set(CMAKE_REQUIRED_LIBRARIES glfw)
                check_c_source_compiles([=[
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WAYLAND
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
int main(void) { return glfwGetWaylandDisplay() != 0; }
]=] ZHLN_SYS_GLFW_HAS_WAYLAND)
            endblock()
            if(ZHLN_SYS_GLFW_HAS_WAYLAND)
                set(_zhln_glfw_wayland TRUE)
                target_include_directories(glfw SYSTEM INTERFACE "${_ZHLN_PROBE_WAYLAND_H}")
            endif()
        else()
            unset(_ZHLN_PROBE_WAYLAND_H CACHE)
        endif()

        message(STATUS "System GLFW native backends: X11=${_zhln_glfw_x11} Wayland=${_zhln_glfw_wayland}")
    endif()
else()
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    if(UNIX AND NOT APPLE)
        # GLFW 3.4+ always compiles the null platform. X11/Wayland are extra
        # backends that need their -dev packages; without them, glfwInit fails
        # over to TTY/KMS instead of failing configure.
        # Guard FindX11 (4 try_compiles + 30 find_library calls) and Wayland
        # pkg-config spawns behind fast header probes; unsetting NOTFOUND entries
        # keeps warm reconfigures sub-millisecond while still picking up newly
        # installed -dev packages without wiping the build directory.
        set(_zhln_glfw_x11 FALSE)
        find_path(_ZHLN_PROBE_XLIB_H NAMES X11/Xlib.h QUIET)
        if(_ZHLN_PROBE_XLIB_H)
            find_package(X11 QUIET)
            if(X11_FOUND
               AND X11_Xrandr_INCLUDE_PATH
               AND X11_Xinerama_INCLUDE_PATH
               AND X11_Xkb_INCLUDE_PATH
               AND X11_Xcursor_INCLUDE_PATH
               AND X11_Xi_INCLUDE_PATH
               AND X11_Xshape_INCLUDE_PATH)
                set(_zhln_glfw_x11 TRUE)
            endif()
        else()
            unset(_ZHLN_PROBE_XLIB_H CACHE)
        endif()

        set(_zhln_glfw_wayland FALSE)
        find_path(_ZHLN_PROBE_WAYLAND_H NAMES wayland-client.h QUIET)
        if(_ZHLN_PROBE_WAYLAND_H)
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
        else()
            unset(_ZHLN_PROBE_WAYLAND_H CACHE)
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
    target_compile_definitions(glfw INTERFACE GLFW_INCLUDE_NONE)

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
endif()

# --- LuaJIT ---
# Lua is strictly a sandboxed, reflection-driven state and configuration layer
# (plugins/lua/ReflectLua); FFI isolation is enforced at VM creation by never
# opening luaopen_ffi or luaopen_package.
#
# Prefer a system LuaJIT 2.1+ package on Unix when available (e.g. `luajit` on
# Arch/NixOS, `libluajit-5.1-dev` on Debian/Ubuntu, `luajit-devel` on Fedora),
# and fall back to building the vendored extern/LuaJIT from source otherwise.
option(ZHLN_LUAJIT_VENDORED
    "Build and use the vendored extern/LuaJIT, ignoring any system LuaJIT" OFF)

set(ZHLN_HAS_LUAJIT FALSE)
if(UNIX AND NOT ZHLN_LUAJIT_VENDORED)
    find_path(_ZHLN_PROBE_LUAJIT_H
        NAMES luajit.h
        HINTS "${LUAJIT_ROOT}" ENV LUAJIT_ROOT /opt/homebrew/opt/luajit /usr/local
        PATH_SUFFIXES luajit-2.1 include/luajit-2.1
        QUIET
    )
    if(NOT _ZHLN_PROBE_LUAJIT_H)
        unset(_ZHLN_PROBE_LUAJIT_H CACHE)
    endif()

    if(_ZHLN_PROBE_LUAJIT_H OR DEFINED ENV{PKG_CONFIG_PATH})
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(ZHLN_SYS_LUAJIT QUIET IMPORTED_TARGET luajit>=2.1)
        endif()

        if(ZHLN_SYS_LUAJIT_FOUND AND TARGET PkgConfig::ZHLN_SYS_LUAJIT)
            add_library(LuaJIT INTERFACE IMPORTED GLOBAL)
            target_link_libraries(LuaJIT INTERFACE PkgConfig::ZHLN_SYS_LUAJIT)
            if(ZHLN_SYS_LUAJIT_INCLUDE_DIRS)
                target_include_directories(LuaJIT SYSTEM INTERFACE ${ZHLN_SYS_LUAJIT_INCLUDE_DIRS})
            elseif(_ZHLN_PROBE_LUAJIT_H)
                target_include_directories(LuaJIT SYSTEM INTERFACE "${_ZHLN_PROBE_LUAJIT_H}")
            endif()
            set(ZHLN_HAS_LUAJIT TRUE)
            message(STATUS "Using system LuaJIT ${ZHLN_SYS_LUAJIT_VERSION} (pkg-config)")
        elseif(_ZHLN_PROBE_LUAJIT_H AND EXISTS "${_ZHLN_PROBE_LUAJIT_H}/lua.hpp")
            find_library(_ZHLN_SYS_LUAJIT_LIB
                NAMES luajit-5.1 luajit
                HINTS "${LUAJIT_ROOT}" ENV LUAJIT_ROOT /opt/homebrew/opt/luajit /usr/local
                QUIET
            )
            if(_ZHLN_SYS_LUAJIT_LIB)
                add_library(LuaJIT INTERFACE IMPORTED GLOBAL)
                target_include_directories(LuaJIT SYSTEM INTERFACE "${_ZHLN_PROBE_LUAJIT_H}")
                target_link_libraries(LuaJIT INTERFACE "${_ZHLN_SYS_LUAJIT_LIB}" ${CMAKE_DL_LIBS} m)
                set(ZHLN_HAS_LUAJIT TRUE)
                message(STATUS "Using system LuaJIT: ${_ZHLN_SYS_LUAJIT_LIB} (${_ZHLN_PROBE_LUAJIT_H})")
            else()
                unset(_ZHLN_SYS_LUAJIT_LIB CACHE)
            endif()
        endif()
    endif()
endif()

if(NOT ZHLN_HAS_LUAJIT)
    # Keep LUAJIT_DISABLE_FFI OFF because extern/LuaJIT/CMakeLists.txt hardcodes
    # `-D FFI` in DASM_FLAGS and omits LUAJIT_FEATURES_DEFINITIONS on
    # luaJIT_buildvm on POSIX.
    set(LUAJIT_DISABLE_FFI OFF CACHE BOOL "Keep LuaJIT buildvm and lib_buffer consistent on POSIX" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/extern/LuaJIT SYSTEM)
endif()

# --- Jolt Physics ---
option(ZHLN_ENABLE_THIRDPARTY_LTO "Enable interprocedural optimization (LTO) on vendored third-party libraries" OFF)
set(INTERPROCEDURAL_OPTIMIZATION ${ZHLN_ENABLE_THIRDPARTY_LTO} CACHE BOOL "Enable interprocedural optimizations" FORCE)
set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
set(USE_STD_INCLUDES ON CACHE BOOL "" FORCE)
set(USE_RTTI OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "Build Jolt Unit Tests" FORCE)
set(JPH_BUILD_TESTS OFF CACHE BOOL "Build Jolt Unit Tests" FORCE)
if(NOT ZHLN_BUILD_RENDER)
    set(JPH_USE_VK OFF CACHE BOOL "Use Vulkan" FORCE)
endif()

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

if(ZHLN_BUILD_RENDER)
    # --- VulkanMemoryAllocator ---
    add_subdirectory(${CMAKE_SOURCE_DIR}/extern/VulkanMemoryAllocator SYSTEM)
endif()

# --- meshoptimizer (VK_EXT_mesh_shader meshlet generation) ---
# Used by the offline cooker (zcook) and by the runtime glTF importer through
# include/Zahlen/Meshlet.hpp, so both produce identical meshlet streams.
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/extern/meshoptimizer EXCLUDE_FROM_ALL SYSTEM)
set_target_properties(meshoptimizer PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(ZHLN_BUILD_RENDER)
    # --- SPIRV-Reflect (vendored at extern/SPIRV-Reflect) ---
    # Reflection of compiled SPIR-V for zahlen_vulkan's pipeline layout builder.
    # Upstream's CMake builds its spirv-reflect command-line tools (and install
    # rules) by default; zahlen only embeds the static library, so everything
    # else is forced off before the upstream CMakeLists runs. The static target
    # upstream provides is spirv-reflect-static, not the spirv_reflect target
    # the old third_party/ wrapper defined, so consumers link that name.
    set(SPIRV_REFLECT_EXECUTABLE OFF CACHE BOOL "" FORCE)
    set(SPIRV_REFLECT_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SPIRV_REFLECT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SPIRV_REFLECT_STATIC_LIB ON CACHE BOOL "" FORCE)
    set(SPIRV_REFLECT_SHARED_LIB OFF CACHE BOOL "" FORCE)
    set(SPIRV_REFLECT_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/extern/SPIRV-Reflect EXCLUDE_FROM_ALL SYSTEM)
    # The old vendored wrapper built this as C11 + PIC; upstream's static target
    # sets neither. PIC is required: the archive is linked into the shared
    # zahlen_engine (through the static zahlen_vulkan).
    set_target_properties(spirv-reflect-static PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
    )
endif()

# --- simdjson ---
# plugins/json is its only consumer (via simdjson::simdjson). Prefer a system
# simdjson package on Unix when available, and fall back to building the
# vendored extern/simdjson from source otherwise (including bare Windows
# toolchains with no system package manager).
#
# simdjson's own C++26 static-reflection APIs must stay OFF even though the
# engine compiles with -freflection: since simdjson 5.0 they are auto-detected
# from feature-test macros in every TU that includes <simdjson.h> unless
# SIMDJSON_STATIC_REFLECTION=0 is defined (upstream's default AUTO mode omits
# that flag from installed simdjsonTargets.cmake), and simdjson/annotations.h
# contains `throw std::meta::exception(...)` in consteval functions which GCC
# rejects under -fno-exceptions. Zahlen uses its own reflection serializer
# (JSONSchema.hpp), so force SIMDJSON_STATIC_REFLECTION=0 in both the system
# and vendored paths.
option(ZHLN_SIMDJSON_VENDORED
    "Build and use the vendored extern/simdjson, ignoring any system simdjson" OFF)

set(ZHLN_HAS_SIMDJSON FALSE)
if(UNIX AND NOT ZHLN_SIMDJSON_VENDORED)
    find_path(_ZHLN_PROBE_SIMDJSON_H
        NAMES simdjson.h
        HINTS "${SIMDJSON_ROOT}" ENV SIMDJSON_ROOT /opt/homebrew/opt/simdjson /usr/local
        PATH_SUFFIXES include
        QUIET
    )
    if(NOT _ZHLN_PROBE_SIMDJSON_H)
        unset(_ZHLN_PROBE_SIMDJSON_H CACHE)
    endif()

    if(_ZHLN_PROBE_SIMDJSON_H OR DEFINED ENV{PKG_CONFIG_PATH})
        # Omit a version argument here: upstream simdjson writes
        # COMPATIBILITY SameMinorVersion into simdjson-config-version.cmake, so
        # `find_package(simdjson 3.0)` would reject 3.1+, 4.x, and 5.x.
        find_package(simdjson CONFIG QUIET)
        if(simdjson_FOUND AND TARGET simdjson::simdjson)
            set_target_properties(simdjson::simdjson PROPERTIES IMPORTED_GLOBAL TRUE)
            get_target_property(_zhln_sys_simdjson_inc simdjson::simdjson INTERFACE_INCLUDE_DIRECTORIES)
            if(_zhln_sys_simdjson_inc)
                target_include_directories(simdjson::simdjson SYSTEM INTERFACE ${_zhln_sys_simdjson_inc})
            endif()
            get_target_property(_zhln_sys_simdjson_defs simdjson::simdjson INTERFACE_COMPILE_DEFINITIONS)
            if(_zhln_sys_simdjson_defs)
                list(REMOVE_ITEM _zhln_sys_simdjson_defs "SIMDJSON_STATIC_REFLECTION=1")
                set_target_properties(simdjson::simdjson PROPERTIES
                    INTERFACE_COMPILE_DEFINITIONS "${_zhln_sys_simdjson_defs}")
            endif()
            target_compile_definitions(simdjson::simdjson INTERFACE SIMDJSON_STATIC_REFLECTION=0)
            set(ZHLN_HAS_SIMDJSON TRUE)
            message(STATUS "Using system simdjson ${simdjson_VERSION} (CMake package)")
        endif()

        if(NOT ZHLN_HAS_SIMDJSON)
            find_package(PkgConfig QUIET)
            if(PkgConfig_FOUND)
                pkg_check_modules(ZHLN_SYS_SIMDJSON QUIET IMPORTED_TARGET simdjson)
            endif()
            if(ZHLN_SYS_SIMDJSON_FOUND AND TARGET PkgConfig::ZHLN_SYS_SIMDJSON)
                add_library(simdjson::simdjson INTERFACE IMPORTED GLOBAL)
                target_link_libraries(simdjson::simdjson INTERFACE PkgConfig::ZHLN_SYS_SIMDJSON)
                if(ZHLN_SYS_SIMDJSON_INCLUDE_DIRS)
                    target_include_directories(simdjson::simdjson SYSTEM INTERFACE ${ZHLN_SYS_SIMDJSON_INCLUDE_DIRS})
                elseif(_ZHLN_PROBE_SIMDJSON_H)
                    target_include_directories(simdjson::simdjson SYSTEM INTERFACE "${_ZHLN_PROBE_SIMDJSON_H}")
                endif()
                target_compile_definitions(simdjson::simdjson INTERFACE SIMDJSON_STATIC_REFLECTION=0)
                set(ZHLN_HAS_SIMDJSON TRUE)
                message(STATUS "Using system simdjson ${ZHLN_SYS_SIMDJSON_VERSION} (pkg-config)")
            endif()
        endif()

        if(NOT ZHLN_HAS_SIMDJSON AND _ZHLN_PROBE_SIMDJSON_H)
            find_library(_ZHLN_SYS_SIMDJSON_LIB
                NAMES simdjson
                HINTS "${SIMDJSON_ROOT}" ENV SIMDJSON_ROOT /opt/homebrew/opt/simdjson /usr/local
                QUIET
            )
            if(_ZHLN_SYS_SIMDJSON_LIB)
                find_package(Threads QUIET)
                add_library(simdjson::simdjson INTERFACE IMPORTED GLOBAL)
                target_include_directories(simdjson::simdjson SYSTEM INTERFACE "${_ZHLN_PROBE_SIMDJSON_H}")
                target_link_libraries(simdjson::simdjson INTERFACE "${_ZHLN_SYS_SIMDJSON_LIB}" Threads::Threads)
                target_compile_definitions(simdjson::simdjson INTERFACE
                    SIMDJSON_THREADS_ENABLED=1
                    SIMDJSON_STATIC_REFLECTION=0
                )
                set(ZHLN_HAS_SIMDJSON TRUE)
                message(STATUS "Using system simdjson: ${_ZHLN_SYS_SIMDJSON_LIB} (${_ZHLN_PROBE_SIMDJSON_H})")
            else()
                unset(_ZHLN_SYS_SIMDJSON_LIB CACHE)
            endif()
        endif()
    endif()
endif()

if(NOT ZHLN_HAS_SIMDJSON)
    set(SIMDJSON_STATIC_REFLECTION_MODE "OFF" CACHE STRING "" FORCE)
    # C++26 guarantees <bit>; pre-seeding skips simdjson's try_compile check, and
    # disabling PCH on this single-TU library avoids building a throwaway .pch.
    set(SIMDJSON_HAS_BIT_HEADER TRUE CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/extern/simdjson EXCLUDE_FROM_ALL SYSTEM)
    set_target_properties(simdjson PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()

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
