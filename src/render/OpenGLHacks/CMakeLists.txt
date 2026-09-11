# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# HostBlitSwapchain is a real OpenGL presenter only on macOS, whose lack of
# native Vulkan WSI requires the host-GL fallback. Other platforms build its
# inert implementation so the target remains uniformly addressable without
# discovering OpenGL, linking it, or including its headers.
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

if(NOT MSVC)
    target_compile_options(HostBlitSwapchain PRIVATE -fno-exceptions -fno-rtti)
endif()

if(APPLE)
    find_package(OpenGL REQUIRED)

    target_include_directories(HostBlitSwapchain PRIVATE
        ${PROJECT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src/vulkan
    )

    target_link_libraries(HostBlitSwapchain PRIVATE
        zahlen_render
        OpenGL::GL
        glfw
    )
endif()
