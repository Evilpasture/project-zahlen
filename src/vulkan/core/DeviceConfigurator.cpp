// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DeviceConfigurator.hpp"
#include "RenderCore.h"
#include <Zahlen/Log.hpp>

namespace ZHLN::Vk {

auto MeshShaderLimitsSufficient(VkPhysicalDevice physical) noexcept -> bool {
    if (physical == VK_NULL_HANDLE) {
        return false;
    }
    VkPhysicalDeviceMeshShaderPropertiesEXT mesh {};
    mesh.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 properties {};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &mesh;
    vkGetPhysicalDeviceProperties2(physical, &properties);

    ZHLN_MeshShaderLimits limits {};
    limits.max_mesh_output_vertices = mesh.maxMeshOutputVertices;
    limits.max_mesh_output_primitives = mesh.maxMeshOutputPrimitives;
    limits.max_task_work_group_invocations = mesh.maxTaskWorkGroupInvocations;
    limits.max_mesh_work_group_invocations = mesh.maxMeshWorkGroupInvocations;
    limits.supported = true; // Caller checked that VK_EXT_mesh_shader is advertised.
    const bool sufficient = ZHLN_MeshShaderLimitsSufficient(&limits);
    if (!sufficient) {
        ZHLN::Log(
            "[Vulkan] Mesh shader limits insufficient (vertices={}, primitives={}, taskInvocations={}, meshInvocations={}); using vertex pipelines.",
            limits.max_mesh_output_vertices, limits.max_mesh_output_primitives, limits.max_task_work_group_invocations,
            limits.max_mesh_work_group_invocations
        );
    }
    return sufficient;
}

void ReportSubgroupSupport(VkPhysicalDevice physical, VkSubgroupFeatureFlags requiredOps) noexcept {
    if (physical == VK_NULL_HANDLE) {
        return;
    }
    VkPhysicalDeviceSubgroupProperties subgroup {};
    subgroup.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 properties {};
    properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    properties.pNext = &subgroup;
    vkGetPhysicalDeviceProperties2(physical, &properties);
    if ((subgroup.supportedOperations & requiredOps) != requiredOps) {
        ZHLN::Log(
            "[Vulkan] WARNING: subgroup width {} supports operations {:#x}, missing requested operations {:#x}; using fallback paths where available.",
            subgroup.subgroupSize, subgroup.supportedOperations, requiredOps & ~subgroup.supportedOperations
        );
    } else {
        ZHLN::Log("[Vulkan] Subgroup width {} supports requested operations {:#x}.", subgroup.subgroupSize, requiredOps);
    }
}

} // namespace ZHLN::Vk
