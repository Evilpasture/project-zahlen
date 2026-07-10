// src/render/Features.inl
#pragma once
#include "Features.hpp"

namespace ZHLN::Vk {

// ============================================================================
// GetStructureType Implementation
// ============================================================================

template <typename T>
[[nodiscard]] constexpr auto GetStructureType() noexcept -> VkStructureType {
    if constexpr (std::is_same_v<T, VkPhysicalDeviceVulkan11Features>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceVulkan12Features>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceVulkan13Features>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceFeatures2>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceAccelerationStructureFeaturesKHR>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceRayQueryFeaturesKHR>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
    } else if constexpr (std::is_same_v<T, VkPhysicalDeviceRobustness2FeaturesEXT>) {
        return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
    } else {
        // C++23: Safe compile-time error only if an unregistered Type is instantiated
        static_assert(sizeof(T) == 0, "Vulkan structure type mapping not registered for this Type in GetStructureType().");
        return VK_STRUCTURE_TYPE_MAX_ENUM;
    }
}

// ============================================================================
// FeatureChain Implementation
// ============================================================================

template <typename... Ts>
FeatureChain<Ts...>::FeatureChain(VkPhysicalDevice physicalDevice, std::tuple<FeatureNode<Ts>...>&& t):
    _features(std::move(t)), _physicalDevice(physicalDevice) {
}

template <typename... Ts>
template <typename T, typename Func>
auto FeatureChain<Ts...>::Require(Func&& configure) && {
    T feature {};
    std::forward<Func>(configure)(feature);
    FeatureNode<T> node {.feature = feature, .active = true};
    return FeatureChain<Ts..., T>(_physicalDevice, std::tuple_cat(std::move(_features), std::make_tuple(node)));
}

// Helpers for automated runtime support checking
template <typename T>
[[nodiscard]] inline auto QueryFeatureSupport(VkPhysicalDevice physicalDevice) noexcept -> T {
    T features {};
    features.sType = GetStructureType<T>();
    features.pNext = nullptr;

    if (physicalDevice != VK_NULL_HANDLE) {
        VkPhysicalDeviceFeatures2 features2 {};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features2.pNext = &features;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
    }
    return features;
}

template <typename T>
[[nodiscard]] inline auto IsSubsetOf(const T& requested, const T& supported) noexcept -> bool {
    struct DummyHeader {
        VkStructureType sType;
        void*           pNext;
        VkBool32        firstFeature;
    };
    constexpr size_t start_offset = offsetof(DummyHeader, firstFeature);

    if constexpr (start_offset < sizeof(T)) {
        const size_t bool_count = (sizeof(T) - start_offset) / sizeof(VkBool32);
        const auto*  req_bools  = reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(&requested) + start_offset);
        const auto*  sup_bools  = reinterpret_cast<const VkBool32*>(reinterpret_cast<const char*>(&supported) + start_offset);

        for (size_t i = 0; i < bool_count; ++i) {
            if (req_bools[i] != VK_FALSE && sup_bools[i] == VK_FALSE) {
                return false;
            }
        }
    }
    return true;
}

template <typename... Ts>
template <typename T, typename Func>
auto FeatureChain<Ts...>::Optional(Func&& configure) && {
    T requested {};
    std::forward<Func>(configure)(requested);

    T    supported = QueryFeatureSupport<T>(_physicalDevice);
    bool condition = IsSubsetOf(requested, supported);

    FeatureNode<T> node {.feature = requested, .active = condition};
    return FeatureChain<Ts..., T>(_physicalDevice, std::tuple_cat(std::move(_features), std::make_tuple(node)));
}

template <typename... Ts>
FeatureChain<Ts...>& FeatureChain<Ts...>::Build() {
    return *this;
}

template <typename... Ts>
const VkPhysicalDeviceFeatures2* FeatureChain<Ts...>::GetRoot() {
    constexpr size_t n = sizeof...(Ts);
    if constexpr (n == 0) {
        return nullptr;
    }

    const VkPhysicalDeviceFeatures2* root_ptr = nullptr;

    std::apply(
        [&root_ptr](auto&... nodes) {
            std::array<void**, n> p_next_ptrs {};
            std::array<void*, n>  feature_ptrs {};
            size_t                active_count = 0;

            auto process_node = [&](auto& node) {
                if (node.active) {
                    using FeatureType  = std::remove_reference_t<decltype(node.feature)>;
                    node.feature.sType = GetStructureType<FeatureType>();

                    p_next_ptrs[active_count]  = reinterpret_cast<void**>(&node.feature.pNext);
                    feature_ptrs[active_count] = &node.feature;
                    active_count++;
                }
            };

            // Fold expression processes nodes sequentially (from first to last)
            (process_node(nodes), ...);

            // Safely chain active nodes in reverse order: Last -> Second-to-last -> ... -> First ->
            // nullptr
            for (size_t i = 0; i < active_count; ++i) {
                if (i > 0) {
                    *p_next_ptrs[i] = feature_ptrs[i - 1];
                } else {
                    *p_next_ptrs[0] = nullptr;
                }
            }

            if (active_count > 0) {
                root_ptr = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(feature_ptrs[active_count - 1]);
            }
        },
        _features
    );

    return root_ptr;
}

// ============================================================================
// FeatureChainBuilder Implementation
// ============================================================================

template <typename T, typename Func>
auto FeatureChainBuilder::Require(Func&& configure) {
    return FeatureChain<>(_physicalDevice, std::make_tuple()).template Require<T>(std::forward<Func>(configure));
}

template <typename T, typename Func>
auto FeatureChainBuilder::Optional(Func&& configure) {
    return FeatureChain<>(_physicalDevice, std::make_tuple()).template Optional<T>(std::forward<Func>(configure));
}

// ============================================================================
// FeatureFactory Implementation
// ============================================================================

template <typename T>
[[nodiscard]] constexpr auto FeatureFactory::Create(auto&& configure) noexcept -> T {
    T features {};
    features.sType = GetStructureType<T>();
    configure(features);
    return features;
}

} // namespace ZHLN::Vk
