# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(HostBlitSwapchain STATIC HostBlitSwapchain.cpp)

set_target_properties(HostBlitSwapchain PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_link_libraries(HostBlitSwapchain PRIVATE
    Vulkan-Headers
    zahlen_warnings
)
target_include_directories(HostBlitSwapchain SYSTEM PRIVATE
    ${PROJECT_SOURCE_DIR}/extern/volk
)

# HostBlitSwapchain.cpp includes "vulkan/Rendering.hpp": rooted, like everything
# else, which needs src/ on the non-Apple path as well -- the block below is
# Apple-only and used to be the sole place this target could find the RHI.
target_include_directories(HostBlitSwapchain PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

target_compile_options(HostBlitSwapchain PRIVATE -fno-exceptions -fno-rtti)

if(APPLE)
    find_package(OpenGL REQUIRED)

    target_include_directories(HostBlitSwapchain PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(HostBlitSwapchain PRIVATE
        zahlen_vulkan
        OpenGL::GL
        glfw
    )
endif()
