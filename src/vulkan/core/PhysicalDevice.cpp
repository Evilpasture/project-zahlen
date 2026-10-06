// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicalDevice.hpp"
#include "Extensions.hpp"
#include <Zahlen/Log.hpp>
#include <vector>

namespace ZHLN::Vk {

namespace {

[[nodiscard]] auto DeviceTypeName(const VkPhysicalDeviceType type) noexcept -> std::string_view {
    switch (type) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            return "discrete GPU";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            return "integrated GPU";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            return "virtual GPU";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            return "CPU Vulkan device";
        default:
            return "other Vulkan device";
    }
}

[[nodiscard]] auto DefaultScore(const PhysicalDeviceInfo& info) noexcept -> int32_t {
    if (!info.has_graphics || (!info.has_present && info.present_family == UINT32_MAX)) {
        return -1;
    }

    const VkPhysicalDeviceProperties& properties = info.properties.properties;
    int32_t score = 0;
    switch (properties.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            score = 1'000'000;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            score = 500'000;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            score = 250'000;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
        default:
            score = 0;
            break;
    }

    if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) {
        const VkPhysicalDeviceMemoryProperties& memory = info.memory.memoryProperties;
        for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
            const VkMemoryHeap& heap = memory.memoryHeaps[i];
            if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                const VkDeviceSize memoryMb = std::min<VkDeviceSize>(heap.size / (1024U * 1024U), 16'384U);
                score += static_cast<int32_t>(memoryMb);
            }
        }
    }
    return score;
}

[[nodiscard]] auto QueryQueueFamilies(const VkPhysicalDevice physical, const VkSurfaceKHR surface) noexcept
    -> std::array<uint32_t, 4> {
    std::array<uint32_t, 4> selected {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    if (vkGetPhysicalDeviceQueueFamilyProperties == nullptr) {
        return selected;
    }

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    if (count != 0) {
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        families.resize(count);
    }

    // Prefer transfer-only queues, then transfer queues without graphics.
    for (uint32_t i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_TRANSFER_BIT) != 0 && (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) == 0) {
            selected[2] = i;
            break;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) != 0 && (flags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            selected[3] = i;
            break;
        }
    }
    if (selected[2] == UINT32_MAX) {
        for (uint32_t i = 0; i < count; ++i) {
            const VkQueueFlags flags = families[i].queueFlags;
            if ((flags & VK_QUEUE_TRANSFER_BIT) != 0 && (flags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                selected[2] = i;
                break;
            }
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && selected[0] == UINT32_MAX) {
            selected[0] = i;
            if (selected[2] == UINT32_MAX) {
                selected[2] = i;
            }
            if (selected[3] == UINT32_MAX) {
                selected[3] = i;
            }
        }
        if (surface != VK_NULL_HANDLE && selected[1] == UINT32_MAX && vkGetPhysicalDeviceSurfaceSupportKHR != nullptr) {
            VkBool32 supported = VK_FALSE;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &supported) == VK_SUCCESS && supported == VK_TRUE) {
                selected[1] = i;
            }
        }
    }

    if (surface == VK_NULL_HANDLE && selected[0] != UINT32_MAX) {
        selected[1] = selected[0];
    }
    return selected;
}

} // namespace

auto SelectPhysicalDevice(
    const VkInstance instance,
    const VkSurfaceKHR surface,
    const DeviceScoreFunction score,
    const void* const userdata
) noexcept -> PhysicalDeviceInfo {
    PhysicalDeviceInfo empty {};
    if (instance == VK_NULL_HANDLE || vkEnumeratePhysicalDevices == nullptr) {
        return empty;
    }

    std::vector<VkPhysicalDevice> devices;
    for (;;) {
        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
            return empty;
        }
        devices.resize(count);
        const VkResult result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
        if (result == VK_SUCCESS) {
            devices.resize(count);
            break;
        }
        if (result != VK_INCOMPLETE) {
            return empty;
        }
    }

    PhysicalDeviceInfo best {};
    int32_t bestScore = -1;
    for (const VkPhysicalDevice physical: devices) {
        PhysicalDeviceInfo info {};
        info.handle = physical;
        if (vkGetPhysicalDeviceProperties2 != nullptr) {
            vkGetPhysicalDeviceProperties2(physical, &info.properties);
        }
        if (vkGetPhysicalDeviceFeatures2 != nullptr) {
            vkGetPhysicalDeviceFeatures2(physical, &info.features);
        }
        if (vkGetPhysicalDeviceMemoryProperties2 != nullptr) {
            vkGetPhysicalDeviceMemoryProperties2(physical, &info.memory);
        }

        const std::array<uint32_t, 4> queueFamilies = QueryQueueFamilies(physical, surface);
        info.graphics_family = queueFamilies[0];
        info.present_family  = queueFamilies[1];
        info.transfer_family = queueFamilies[2];
        info.compute_family  = queueFamilies[3];
        info.has_graphics    = info.graphics_family != UINT32_MAX;
        info.has_present     = info.present_family != UINT32_MAX;
        info.has_transfer    = info.transfer_family != UINT32_MAX;
        info.has_compute     = info.compute_family != UINT32_MAX;

        const int32_t deviceScore = score != nullptr ? score(info, userdata) : DefaultScore(info);
        if (deviceScore > bestScore) {
            bestScore = deviceScore;
            best      = info;
        }
    }

    if (bestScore >= 0) {
        ZHLN::Log("[Vulkan] Selected physical device: {} ({})", best.properties.properties.deviceName,
                  DeviceTypeName(best.properties.properties.deviceType));
        return best;
    }
    return empty;
}

auto QueryMeshShaderLimits(const VkPhysicalDevice physical) noexcept -> MeshShaderLimits {
    MeshShaderLimits result {};
    if (physical == VK_NULL_HANDLE || !IsDeviceExtensionSupported(physical, VK_EXT_MESH_SHADER_EXTENSION_NAME) ||
        vkGetPhysicalDeviceProperties2 == nullptr) {
        return result;
    }

    VkPhysicalDeviceMeshShaderPropertiesEXT meshProperties {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT,
    };
    VkPhysicalDeviceProperties2 properties {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &meshProperties,
    };
    vkGetPhysicalDeviceProperties2(physical, &properties);

    result.max_mesh_output_vertices = meshProperties.maxMeshOutputVertices;
    result.max_mesh_output_primitives = meshProperties.maxMeshOutputPrimitives;
    result.max_task_work_group_invocations = meshProperties.maxTaskWorkGroupInvocations;
    result.max_mesh_work_group_invocations = meshProperties.maxMeshWorkGroupInvocations;
    result.max_preferred_task_work_group_invocations = meshProperties.maxPreferredTaskWorkGroupInvocations;
    result.max_preferred_mesh_work_group_invocations = meshProperties.maxPreferredMeshWorkGroupInvocations;
    result.prefers_compact_vertex_output = meshProperties.prefersCompactVertexOutput == VK_TRUE;
    result.supported = true;
    return result;
}

auto MeshShaderLimitsSufficient(const MeshShaderLimits& limits) noexcept -> bool {
    return limits.supported && limits.max_mesh_output_vertices >= 64U && limits.max_mesh_output_primitives >= 124U &&
        limits.max_task_work_group_invocations >= 32U && limits.max_mesh_work_group_invocations >= 64U;
}

} // namespace ZHLN::Vk
