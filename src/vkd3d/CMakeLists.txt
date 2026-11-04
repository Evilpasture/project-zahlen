# Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
# SPDX-License-Identifier: GPL-3.0-or-later

# Joint Vulkan/D3D12 address contract (ZHLN::VkD3D::DeviceAddress).
# Renderer integration is intentionally deferred, so src/render continues to
# link and use zahlen_vulkan directly for now. This is a concrete target so
# CI can compile-check the bridge independently of the heavy renderer target.
#
# GpuAddress.hpp bridges exactly two backend headers. It reaches them ONLY
# by linking the two header-only seam targets
#   zahlen_d3d12_address   -> <d3d12/memory/BufferAddress.hpp>
#   zahlen_vulkan_address  -> <vulkan/memory/DeviceAddress.hpp>
# each of which exports a single src/<backend>/include root. Deliberately
# NOT ${PROJECT_SOURCE_DIR}/src (and not ${PROJECT_SOURCE_DIR}): that one
# -I would make every private header in every src/ subsystem includable
# from here, which is the leak check_subsystem_boundaries.py rejects.
if(NOT TARGET zahlen_vulkan_address)
    message(FATAL_ERROR "zahlen_vkd3d requires ZHLN_BUILD_RENDER=ON so the zahlen_vulkan_address seam (src/vulkan) is available")
endif()

add_library(zahlen_vkd3d STATIC GpuAddress.cpp Enums.cpp)
add_library(Zahlen::VkD3D ALIAS zahlen_vkd3d)
set_target_properties(zahlen_vkd3d PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
target_compile_features(zahlen_vkd3d PRIVATE cxx_std_26)
target_include_directories(zahlen_vkd3d PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_options(zahlen_vkd3d PRIVATE -fno-exceptions -fno-rtti)
# Select packed-address arithmetic only at the joint backend boundary; engine
# and pass sources do not branch on this definition.
target_compile_definitions(zahlen_vkd3d PUBLIC ZHLN_GPU_ADDRESS_D3D12=1)
target_link_libraries(zahlen_vkd3d PRIVATE
    zahlen_d3d12_address
    zahlen_vulkan_address
    zahlen_warnings
)
