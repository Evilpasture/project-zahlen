// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicalDevice.hpp"
#include "Extensions.hpp"
#include <Zahlen/Log.hpp>
#include <cstddef>
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
    if (!info.hasGraphics || (!info.hasPresent && info.presentFamily == UINT32_MAX)) {
        return -1;
    }

    const VkPhysicalDeviceProperties& properties = info.properties.properties;
    int32_t                           score      = 0;
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
                const VkDeviceSize memory_mb = std::min<VkDeviceSize>(heap.size / (static_cast<VkDeviceSize>(1024U * 1024U)), 16'384U);
                score += static_cast<int32_t>(memory_mb);
            }
        }
    }
    return score;
}

[[nodiscard]] auto QueryQueueFamilies(const VkPhysicalDevice physical, const VkSurfaceKHR surface) noexcept -> std::array<uint32_t, 4> {
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

auto EnumeratePhysicalDevices(const VkInstance instance) noexcept -> std::vector<VkPhysicalDevice> {
    if (instance == VK_NULL_HANDLE || vkEnumeratePhysicalDevices == nullptr) {
        return {};
    }

    for (;;) {
        uint32_t count = 0;
        if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0) {
            return {};
        }
        std::vector<VkPhysicalDevice> devices(count);
        const VkResult                result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
        if (result == VK_SUCCESS) {
            devices.resize(count);
            return devices;
        }
        if (result != VK_INCOMPLETE) {
            return {};
        }
    }
}

auto InspectPhysicalDevice(const VkPhysicalDevice physical, const VkSurfaceKHR surface) noexcept -> PhysicalDeviceInfo {
    PhysicalDeviceInfo info {.handle = physical};

    if (vkGetPhysicalDeviceProperties2 != nullptr) {
        vkGetPhysicalDeviceProperties2(physical, &info.properties);
    }
    if (vkGetPhysicalDeviceFeatures2 != nullptr) {
        vkGetPhysicalDeviceFeatures2(physical, &info.features);
    }
    if (vkGetPhysicalDeviceMemoryProperties2 != nullptr) {
        vkGetPhysicalDeviceMemoryProperties2(physical, &info.memory);
    }

    const auto queue_families = QueryQueueFamilies(physical, surface);
    return PhysicalDeviceInfo {
        .handle         = physical,
        .properties     = info.properties,
        .features       = info.features,
        .memory         = info.memory,
        .graphicsFamily = queue_families[0],
        .presentFamily  = queue_families[1],
        .transferFamily = queue_families[2],
        .computeFamily  = queue_families[3],
        .hasGraphics    = queue_families[0] != UINT32_MAX,
        .hasPresent     = queue_families[1] != UINT32_MAX,
        .hasTransfer    = queue_families[2] != UINT32_MAX,
        .hasCompute     = queue_families[3] != UINT32_MAX,
    };
}

} // namespace

auto SelectPhysicalDevice(const VkInstance instance, const VkSurfaceKHR surface, const DeviceScoreFunction score, const void* const userdata) noexcept
    -> PhysicalDeviceInfo
/* TODO(Evilpasture): Change to this pattern:
auto SelectPhysicalDevice(
    const VkInstance instance,
    const VkSurfaceKHR surface,
    const DeviceScoreFunction score,
    const void* const userdata
) noexcept -> std::optional<PhysicalDeviceInfo> {
    const auto devices = EnumeratePhysicalDevices(instance);
    if (devices.empty()) {
        return std::nullopt;
    }

    const auto evaluate_device = [surface, score, userdata](const VkPhysicalDevice physical) {
        const auto info = InspectPhysicalDevice(physical, surface);
        const int32_t device_score = score != nullptr ? score(info, userdata) : DefaultScore(info);
        return std::pair{info, device_score};
    };

    // Find the device with the highest score
    std::optional<PhysicalDeviceInfo> best_device = std::nullopt;
    int32_t best_score = -1;

    for (const VkPhysicalDevice physical : devices) {
        const auto [info, device_score] = evaluate_device(physical);
        if (device_score > best_score) {
            best_score = device_score;
            best_device = info;
        }
    }

    if (best_score >= 0 && best_device.has_value()) {
        ZHLN::Log(
            "[Vulkan] Selected physical device: {} ({})",
            best_device->properties.properties.deviceName,
            DeviceTypeName(best_device->properties.properties.deviceType)
        );
        return best_device;
    }

    return std::nullopt;
}
*/
{
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
    int32_t            best_score = -1;
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

        const std::array<uint32_t, 4> queue_families = QueryQueueFamilies(physical, surface);
        info.graphicsFamily                          = queue_families[0];
        info.presentFamily                           = queue_families[1];
        info.transferFamily                          = queue_families[2];
        info.computeFamily                           = queue_families[3];
        info.hasGraphics                             = info.graphicsFamily != UINT32_MAX;
        info.hasPresent                              = info.presentFamily != UINT32_MAX;
        info.hasTransfer                             = info.transferFamily != UINT32_MAX;
        info.hasCompute                              = info.computeFamily != UINT32_MAX;

        const int32_t device_score = score != nullptr ? score(info, userdata) : DefaultScore(info);
        if (device_score > best_score) {
            best_score = device_score;
            best       = info;
        }
    }

    if (best_score >= 0) {
        ZHLN::Log("[Vulkan] Selected physical device: {} ({})", best.properties.properties.deviceName, DeviceTypeName(best.properties.properties.deviceType));
        return best;
    }
    return empty;
}

auto QueryMeshShaderLimits(const VkPhysicalDevice physical) noexcept -> MeshShaderLimits {
    if (physical == VK_NULL_HANDLE || !IsDeviceExtensionSupported(physical, VK_EXT_MESH_SHADER_EXTENSION_NAME) || vkGetPhysicalDeviceProperties2 == nullptr) {
        return MeshShaderLimits {};
    }

    VkPhysicalDeviceMeshShaderPropertiesEXT mesh_properties {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT,
    };
    VkPhysicalDeviceProperties2 properties {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &mesh_properties,
    };
    vkGetPhysicalDeviceProperties2(physical, &properties);

    // Returned atomically and immutably via designated initializers. We prefer this pattern more.
    return MeshShaderLimits {
        .maxMeshOutputVertices                = mesh_properties.maxMeshOutputVertices,
        .maxMeshOutputPrimitives              = mesh_properties.maxMeshOutputPrimitives,
        .maxTaskWorkGroupInvocations          = mesh_properties.maxTaskWorkGroupInvocations,
        .maxMeshWorkGroupInvocations          = mesh_properties.maxMeshWorkGroupInvocations,
        .maxPreferredTaskWorkGroupInvocations = mesh_properties.maxPreferredTaskWorkGroupInvocations,
        .maxPreferredMeshWorkGroupInvocations = mesh_properties.maxPreferredMeshWorkGroupInvocations,
        .prefersCompactVertexOutput           = mesh_properties.prefersCompactVertexOutput == VK_TRUE,
        .supported                            = true,
    };
}

auto MeshShaderLimitsSufficient(const MeshShaderLimits& limits) noexcept -> bool {
    return limits.supported && limits.maxMeshOutputVertices >= 64U && limits.maxMeshOutputPrimitives >= 124U && limits.maxTaskWorkGroupInvocations >= 32U &&
           limits.maxMeshWorkGroupInvocations >= 64U;
}

} // namespace ZHLN::Vk
