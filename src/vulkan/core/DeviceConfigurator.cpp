// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DeviceConfigurator.hpp"
#include "PhysicalDevice.hpp"
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
