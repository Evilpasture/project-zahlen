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
    uint32_t                          graphicsFamily = UINT32_MAX;
    uint32_t                          presentFamily  = UINT32_MAX;
    uint32_t                          transferFamily = UINT32_MAX;
    uint32_t                          computeFamily  = UINT32_MAX;
    bool                              hasGraphics    = false;
    bool                              hasPresent     = false;
    bool                              hasTransfer    = false;
    bool                              hasCompute     = false;
};

using DeviceScoreFunction = int32_t (*)(const PhysicalDeviceInfo& info, const void* userdata) noexcept;

struct MeshShaderLimits {
    uint32_t maxMeshOutputVertices                = 0;
    uint32_t maxMeshOutputPrimitives              = 0;
    uint32_t maxTaskWorkGroupInvocations          = 0;
    uint32_t maxMeshWorkGroupInvocations          = 0;
    uint32_t maxPreferredTaskWorkGroupInvocations = 0;
    uint32_t maxPreferredMeshWorkGroupInvocations = 0;
    bool     prefersCompactVertexOutput           = false;
    bool     supported                            = false;
};

[[nodiscard]] auto SelectPhysicalDevice(VkInstance instance, VkSurfaceKHR surface, DeviceScoreFunction score = nullptr, const void* userdata = nullptr) noexcept
    -> PhysicalDeviceInfo;

[[nodiscard]] auto QueryMeshShaderLimits(VkPhysicalDevice physical) noexcept -> MeshShaderLimits;
[[nodiscard]] auto MeshShaderLimitsSufficient(const MeshShaderLimits& limits) noexcept -> bool;

} // namespace ZHLN::Vk
