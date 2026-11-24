// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "vulkan/core/DeviceConfigurator.hpp"
#include "vulkan/core/PhysicalDevice.hpp"

namespace ZHLN::Vk {

// Pure verdicts: the caller decides what to say about them. Keeping them
// side-effect free is what lets OptionalMeshShaders/OptionalRayTracing use them
// as accept predicates (a captureless lambda cannot carry a ledger), and it is
// why a headless Lavapipe run no longer prints a warning per optional feature.
auto MeshShaderLimitsSufficient(const VkPhysicalDevice physical) noexcept -> bool {
    return MeshShaderLimitsSufficient(QueryMeshShaderLimits(physical));
}

auto RayTracingDriverUsable(const VkPhysicalDevice physical) noexcept -> bool {
    if (physical == VK_NULL_HANDLE || vkGetPhysicalDeviceProperties2 == nullptr) {
        return true;
    }
    VkPhysicalDeviceSubgroupProperties subgroup {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2        properties {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &subgroup};
    vkGetPhysicalDeviceProperties2(physical, &properties);
    // Mesa Lavapipe's PLOC acceleration-structure builder (vk_bvh) writes out of
    // bounds at 128-bit SIMD width (subgroupSize == 4, the default on AArch64
    // NEON and pre-AVX2 x86_64), faulting a compute worker thread inside
    // vkCmdBuildAccelerationStructuresKHR. Hardware GPUs and >=256-bit software
    // devices (subgroupSize >= 8) stay enabled.
    return !(properties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU && subgroup.subgroupSize > 0 && subgroup.subgroupSize < 8);
}

void ReportSubgroupSupport(const VkPhysicalDevice physical, const VkSubgroupFeatureFlags requiredOps, DiagnosticLedger& diagnostics) noexcept {
    if (physical == VK_NULL_HANDLE || vkGetPhysicalDeviceProperties2 == nullptr) {
        return;
    }
    VkPhysicalDeviceSubgroupProperties subgroup {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceProperties2        properties {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &subgroup};
    vkGetPhysicalDeviceProperties2(physical, &properties);
    if ((subgroup.supportedOperations & requiredOps) != requiredOps) {
        diagnostics.Add(DiagnosticSeverity::Info, "Subgroups",
            "Subgroup width {} supports operations {:#x}, missing requested operations {:#x}; using fallback paths where available.",
            subgroup.subgroupSize, subgroup.supportedOperations, requiredOps & ~subgroup.supportedOperations
        );
    }
}

} // namespace ZHLN::Vk
