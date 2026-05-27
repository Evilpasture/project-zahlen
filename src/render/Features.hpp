#pragma once
#include <vulkan/vulkan.h>
#include <type_traits>

namespace ZHLN::Vk {

/**
 * @brief Compile-time Type-to-Enum mapping.
 * Mirrors the GetFormatAspect function structure without requiring macros.
 */
template <typename T>
[[nodiscard]] constexpr auto GetStructureType() noexcept -> VkStructureType {
    if constexpr (std::is_same_v<T, VkPhysicalDeviceVulkan11Features>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    } 
    else if constexpr (std::is_same_v<T, VkPhysicalDeviceVulkan12Features>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    } 
    else if constexpr (std::is_same_v<T, VkPhysicalDeviceVulkan13Features>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    } 
    else if constexpr (std::is_same_v<T, VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
    }
    else if constexpr (std::is_same_v<T, VkPhysicalDeviceFeatures2>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    } 
    else {
        // C++23: Safe compile-time error only if an unregistered Type is instantiated
        static_assert(false, "Vulkan structure type mapping not registered for this Type in GetStructureType().");
        return VK_STRUCTURE_TYPE_MAX_ENUM;
    }
}

struct FeatureFactory {
    template <typename T>
    [[nodiscard]] static constexpr auto Create(auto&& configure) noexcept -> T {
        T features{}; // Value-initialization (Forces 0 warnings, zeroed memory)
        features.sType = GetStructureType<T>(); // Stitched safely at compile-time
        
        configure(features);
        return features; // Optimized out via NRVO
    }
};

} // namespace ZHLN::Vk