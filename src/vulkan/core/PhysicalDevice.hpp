// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <cstdint>

namespace ZHLN::Vk {

struct PhysicalDeviceInfo {
    VkPhysicalDevice                  handle = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties2       properties {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceFeatures2         features {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceMemoryProperties2 memory {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    uint32_t                          graphics_family = UINT32_MAX;
    uint32_t                          present_family  = UINT32_MAX;
    uint32_t                          transfer_family = UINT32_MAX;
    uint32_t                          compute_family  = UINT32_MAX;
    bool                              has_graphics    = false;
    bool                              has_present     = false;
    bool                              has_transfer    = false;
    bool                              has_compute     = false;
};

using DeviceScoreFunction = int32_t (*)(const PhysicalDeviceInfo& info, const void* userdata) noexcept;

struct MeshShaderLimits {
    uint32_t max_mesh_output_vertices                  = 0;
    uint32_t max_mesh_output_primitives                = 0;
    uint32_t max_task_work_group_invocations           = 0;
    uint32_t max_mesh_work_group_invocations           = 0;
    uint32_t max_preferred_task_work_group_invocations = 0;
    uint32_t max_preferred_mesh_work_group_invocations = 0;
    bool     prefers_compact_vertex_output             = false;
    bool     supported                                 = false;
};

[[nodiscard]] auto SelectPhysicalDevice(VkInstance instance, VkSurfaceKHR surface, DeviceScoreFunction score = nullptr, const void* userdata = nullptr) noexcept
    -> PhysicalDeviceInfo;

[[nodiscard]] auto QueryMeshShaderLimits(VkPhysicalDevice physical) noexcept -> MeshShaderLimits;
[[nodiscard]] auto MeshShaderLimitsSufficient(const MeshShaderLimits& limits) noexcept -> bool;

} // namespace ZHLN::Vk
