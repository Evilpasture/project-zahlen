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
    target_compile_options(glfw PRIVATE -Wno-deprecated-declarations)
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

# --- test-harness toggles for vendor projects ---
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

if(APPLE)
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
