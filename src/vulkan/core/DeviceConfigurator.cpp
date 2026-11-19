// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vulkan/core/DeviceConfigurator.hpp"
#include "vulkan/core/PhysicalDevice.hpp"
#include <Zahlen/Log.hpp>

namespace ZHLN::Vk {

auto MeshShaderLimitsSufficient(const VkPhysicalDevice physical) noexcept -> bool {
    const MeshShaderLimits limits = QueryMeshShaderLimits(physical);
    if (!MeshShaderLimitsSufficient(limits)) {
        ZHLN::Log(
            "[Vulkan] Mesh shader limits insufficient (vertices={}, primitives={}, taskInvocations={}, meshInvocations={}); using vertex pipelines.",
            limits.maxMeshOutputVertices, limits.maxMeshOutputPrimitives, limits.maxTaskWorkGroupInvocations, limits.maxMeshWorkGroupInvocations
        );
        return false;
    }
    return true;
}

auto RayTracingDriverUsable(const VkPhysicalDevice physical) noexcept -> bool {
    if (physical == VK_NULL_HANDLE || vkGetPhysicalDeviceProperties2 == nullptr) {
        return true;
    }
    VkPhysicalDeviceSubgroupProperties subgroup {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2        properties {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &subgroup};
    vkGetPhysicalDeviceProperties2(physical, &properties);
    // Mesa Lavapipe's PLOC acceleration-structure builder (vk_bvh) writes out
    // of bounds at 128-bit SIMD width (subgroupSize == 4, default on AArch64
    // NEON and pre-AVX2 x86_64), faulting a compute worker thread inside
    // vkCmdBuildAccelerationStructuresKHR. Hardware GPUs and >=256-bit software
    // devices (subgroupSize >= 8) stay enabled.
    if (properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && subgroup.subgroupSize > 0 && subgroup.subgroupSize < 8) {
        ZHLN::Log(
            "[Vulkan] CPU device '{}' has subgroup width {} (< 8); disabling acceleration structures to avoid 128-bit Lavapipe BVH fault.",
            properties.properties.deviceName, subgroup.subgroupSize
        );
        return false;
    }
    return true;
}

void ReportSubgroupSupport(const VkPhysicalDevice physical, const VkSubgroupFeatureFlags requiredOps) noexcept {
    if (physical == VK_NULL_HANDLE || vkGetPhysicalDeviceProperties2 == nullptr) {
        return;
    }
    VkPhysicalDeviceSubgroupProperties subgroup {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2        properties {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &subgroup};
    vkGetPhysicalDeviceProperties2(physical, &properties);
    if ((subgroup.supportedOperations & requiredOps) != requiredOps) {
        ZHLN::LogWarning(
            "[Vulkan] subgroup width {} supports operations {:#x}, missing requested operations {:#x}; using fallback paths where available.",
            subgroup.subgroupSize, subgroup.supportedOperations, requiredOps & ~subgroup.supportedOperations
        );
    } else {
        ZHLN::Log("[Vulkan] Subgroup width {} supports requested operations {:#x}.", subgroup.subgroupSize, requiredOps);
    }
}

} // namespace ZHLN::Vk
