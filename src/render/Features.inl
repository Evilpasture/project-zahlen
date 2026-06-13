#pragma once
#include "Features.hpp"

#include <type_traits>
#include <utility>
namespace ZHLN::Vk {

// ============================================================================
// GetStructureType Implementation
// ============================================================================

template <typename T> [[nodiscard]] constexpr auto GetStructureType() noexcept -> VkStructureType {
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
	} else {
		// C++23: Safe compile-time error only if an unregistered Type is instantiated
		static_assert(
			sizeof(T) == 0,
			"Vulkan structure type mapping not registered for this Type in GetStructureType().");
		return VK_STRUCTURE_TYPE_MAX_ENUM;
	}
}

// ============================================================================
// FeatureChain Implementation
// ============================================================================

template <typename... Ts>
FeatureChain<Ts...>::FeatureChain(std::tuple<Ts...>&& t) : _features(std::move(t)) {}

template <typename... Ts>
template <typename T, typename Func>
auto FeatureChain<Ts...>::Require(Func&& configure) && {
	T feature{};
	feature.sType = GetStructureType<T>();
	feature.pNext = nullptr;
	configure(feature);
	return FeatureChain<Ts..., T>(std::tuple_cat(std::move(_features), std::make_tuple(feature)));
}

template <typename... Ts> FeatureChain<Ts...>& FeatureChain<Ts...>::Build() {
	return *this;
}

template <typename... Ts> auto* FeatureChain<Ts...>::GetRoot() {
	if constexpr (sizeof...(Ts) > 1) {
		auto link = []<std::size_t... Is>(std::tuple<Ts...>& t, std::index_sequence<Is...>) {
			((std::get<sizeof...(Ts) - 1 - Is>(t).pNext = &std::get<sizeof...(Ts) - 2 - Is>(t)),
			 ...);
		};
		link(_features, std::make_index_sequence<sizeof...(Ts) - 1>{});
	}
	return &std::get<sizeof...(Ts) - 1>(_features);
}

// ============================================================================
// FeatureChainBuilder Implementation
// ============================================================================

template <typename T, typename Func> auto FeatureChainBuilder::Require(Func&& configure) {
	T feature{};
	feature.sType = GetStructureType<T>();
	feature.pNext = nullptr;
	configure(feature);
	return FeatureChain<T>(std::make_tuple(feature));
}

// ============================================================================
// FeatureFactory Implementation
// ============================================================================

template <typename T>
[[nodiscard]] constexpr auto FeatureFactory::Create(auto&& configure) noexcept -> T {
	T features{}; // Value-initialization (Forces 0 warnings, zeroed memory)
	features.sType = GetStructureType<T>(); // Stitched safely at compile-time

	configure(features);
	return features; // Optimized out via NRVO
}

} // namespace ZHLN::Vk
