# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# ----------------------------------------------------------------------------
# zahlen_window — the windowing subsystem
# ----------------------------------------------------------------------------
# Owns everything that talks to a window system: the desktop ZHLN::Window, the
# two windowless presentation targets, and the PIMPL bridge that publishes what
# the OS gave us as an opaque NativeSurfaceHandle.
#
# This is the ONE place GLFW is linked for window creation, and it is where the
# engine-internal presentation seam lives: PresentationTarget.hpp declares
# PresentationTarget and NativeSurfaceHandle, and NativeSurfaceInternal.hpp
# defines the OS descriptors behind the latter. Neither is public API -- a client
# of the engine sees only ZHLN::Window, and it reaches a frame's destination by
# asking the kernel for an attachment (Kernel::AcquireTarget), which is the one
# thing outside src/window that asks for a target at all. It asks the host, never
# a window: src/engine has no include path into this subsystem, and the engine's
# one door here is PlatformHost's private access for the kernel.
#
# It knows no Vulkan. It publishes void* members of a variant, and
# src/render/PresentationSurface.cpp is what turns them into a VkSurfaceKHR: that
# is the one layer allowed to see both a window descriptor and the loader, which
# is what lets zahlen_vulkan stay a leaf that knows only VkSurfaceKHR.

set(RAW_WINDOW_SOURCES
    PlatformHost.cpp
    PresentationTarget.cpp
    NativeSurfaceHandle.cpp
    Window.cpp
)

# Reflection-capable compilers compile these files directly; other compilers
# receive their generated counterparts. Kept here so this directory owns its
# source list the way every other subsystem does.
zahlen_transpile_sources(WINDOW_SOURCES ${RAW_WINDOW_SOURCES})

add_library(zahlen_window STATIC ${WINDOW_SOURCES})
set_target_properties(zahlen_window PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Which native window backends this file may ask GLFW for. These mirror the
# backend selection the root CMakeLists made for GLFW itself from the X11 and
# Wayland development files it found, because <GLFW/glfw3native.h> includes
# <X11/Xlib.h> / <wayland-client.h> for whichever backend is exposed -- asking
# for one the machine has no headers for would fail the compile, and a box with
# neither is a supported configuration (GLFW builds its null platform there).
if(WIN32)
    target_compile_definitions(zahlen_window PRIVATE ZHLN_WINDOW_NATIVE_WIN32=1)
elseif(NOT APPLE)
    if(_zhln_glfw_wayland)
        target_compile_definitions(zahlen_window PRIVATE ZHLN_WINDOW_NATIVE_WAYLAND=1)
    endif()
    if(_zhln_glfw_x11)
        target_compile_definitions(zahlen_window PRIVATE ZHLN_WINDOW_NATIVE_X11=1)
    endif()
endif()

# Window's ZHLN_API methods are defined here, and the archive is merged into
# libzahlen_engine: without this, a Windows build would compile them as
# dllimport and export nothing.
target_compile_definitions(zahlen_window PRIVATE ZHLN_ENGINE_BUILD)

# Jolt's ABI macros must match every other target that reads <Zahlen/Types.hpp>
# -- <Zahlen/Window.hpp> reaches it. Drifting here is a silent layout mismatch,
# not a link error (see the note above zahlen_enable_reflection).
target_compile_definitions(zahlen_window PRIVATE
    JPH_DOUBLE_PRECISION
    JPH_NO_CUSTOM_DETERMINISTIC_OPERATIONS
    $<$<CONFIG:Debug>:JPH_DEBUG>
    $<$<CONFIG:Release>:JPH_RELEASE>
)

if(NOT MSVC)
    target_compile_options(zahlen_window PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
endif()

# GLFW is linked HERE, completely isolated from zahlen_render.
target_link_libraries(zahlen_window
    PUBLIC
        Jolt
    PRIVATE
        glfw
)

target_include_directories(zahlen_window
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        # The one deliberate seam: Window.cpp drives the Linux TTY/KMS backend,
        # whose implementation still lives in the engine (src/engine/tty) because
        # the crash handler's emergency restore calls it too. Header-only from
        # here; the symbols resolve inside libzahlen_engine, which holds both.
        ${PROJECT_SOURCE_DIR}/src/engine
)

target_include_directories(zahlen_window SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/JoltPhysics
)

target_link_libraries(zahlen_window PRIVATE zahlen_warnings)
