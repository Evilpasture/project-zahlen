# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# HostBlitSwapchain.cpp is entirely inside `#if defined(__APPLE__)`, and the
# only edge to this target is root CMakeLists' `if(APPLE AND
# ZHLN_BUILD_RENDER) target_link_libraries(zahlen_engine PRIVATE
# HostBlitSwapchain)`. On Linux and Windows it therefore built a 332-line
# translation unit that compiled to nothing, was never linked, and -- POSITION
# _INDEPENDENT_CODE not being set -- was the one archive in the tree that would
# have failed to link into the shared engine had anybody done so. Declared
# once, where the sources arguably belong, in a directory that still exists on
# every platform so the file layout does not move.
if(NOT APPLE)
    return()
endif()

add_library(HostBlitSwapchain STATIC HostBlitSwapchain.cpp)

set_target_properties(HostBlitSwapchain PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    # zahlen_engine links this, and zahlen_engine is SHARED.
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(HostBlitSwapchain PRIVATE
    Vulkan-Headers
    zahlen_warnings
)
target_include_directories(HostBlitSwapchain SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/volk
)

# HostBlitSwapchain.cpp includes "vulkan/Rendering.hpp": rooted, like everything
# else, so this needs src/ as well as include/.
target_include_directories(HostBlitSwapchain PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(HostBlitSwapchain PRIVATE -fno-exceptions -fno-rtti)

# No `if(APPLE)` here any more: the guard at the top of the file is the one
# place the platform is decided. A second one inside the same file was how the
# two could drift.
find_package(OpenGL REQUIRED)
target_link_libraries(HostBlitSwapchain PRIVATE
    zahlen_vulkan
    OpenGL::GL
    glfw
)
