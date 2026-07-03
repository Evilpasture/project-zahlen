#pragma once
#include "Features.hpp"

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
	} else if constexpr (std::is_same_v<T, VkPhysicalDeviceAccelerationStructureFeaturesKHR>) {
		return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
	} else if constexpr (std::is_same_v<T, VkPhysicalDeviceRayQueryFeaturesKHR>) {
		return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
	} else if constexpr (std::is_same_v<T, VkPhysicalDeviceRobustness2FeaturesEXT>) {
		return VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
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
FeatureChain<Ts...>::FeatureChain(std::tuple<FeatureNode<Ts>...>&& t) : _features(std::move(t)) {}

template <typename... Ts>
template <typename T, typename Func>
auto FeatureChain<Ts...>::Require(Func&& configure) && {
	T feature{};
	configure(feature);
	FeatureNode<T> node{.feature = feature, .active = true};
	return FeatureChain<Ts..., T>(std::tuple_cat(std::move(_features), std::make_tuple(node)));
}

template <typename... Ts>
template <typename T, typename Func>
auto FeatureChain<Ts...>::Optional(bool condition, Func&& configure) && {
	T feature{};
	if (condition) {
		configure(feature);
	}
	FeatureNode<T> node{.feature = feature, .active = condition};
	return FeatureChain<Ts..., T>(std::tuple_cat(std::move(_features), std::make_tuple(node)));
}

template <typename... Ts> FeatureChain<Ts...>& FeatureChain<Ts...>::Build() {
	return *this;
}

template <typename... Ts> const VkPhysicalDeviceFeatures2* FeatureChain<Ts...>::GetRoot() {
	constexpr size_t N = sizeof...(Ts);
	if constexpr (N == 0) {
		return nullptr;
	}

	const VkPhysicalDeviceFeatures2* rootPtr = nullptr;

	std::apply(
		[&rootPtr](auto&... nodes) {
			std::array<void**, N> pNextPtrs{};
			std::array<void*, N> featurePtrs{};
			size_t activeCount = 0;

			auto processNode = [&](auto& node) {
				if (node.active) {
					using FeatureType = std::remove_reference_t<decltype(node.feature)>;
					node.feature.sType = GetStructureType<FeatureType>();

					pNextPtrs[activeCount] = reinterpret_cast<void**>(&node.feature.pNext);
					featurePtrs[activeCount] = &node.feature;
					activeCount++;
				}
			};

			// Fold expression processes nodes sequentially (from first to last)
			(processNode(nodes), ...);

			// Safely chain active nodes in reverse order: Last -> Second-to-last -> ... -> First ->
			// nullptr
			for (size_t i = 0; i < activeCount; ++i) {
				if (i > 0) {
					*pNextPtrs[i] = featurePtrs[i - 1];
				} else {
					*pNextPtrs[0] = nullptr;
				}
			}

			if (activeCount > 0) {
				rootPtr = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(
					featurePtrs[activeCount - 1]);
			}
		},
		_features);

	return rootPtr;
}

// ============================================================================
// FeatureChainBuilder Implementation
// ============================================================================

template <typename T, typename Func> auto FeatureChainBuilder::Require(Func&& configure) {
	return FeatureChain<>().template Require<T>(std::forward<Func>(configure));
}

template <typename T, typename Func>
auto FeatureChainBuilder::Optional(bool condition, Func&& configure) {
	return FeatureChain<>().template Optional<T>(condition, std::forward<Func>(configure));
}

// ============================================================================
// FeatureFactory Implementation
// ============================================================================

template <typename T>
[[nodiscard]] constexpr auto FeatureFactory::Create(auto&& configure) noexcept -> T {
	T features{};
	features.sType = GetStructureType<T>();
	configure(features);
	return features;
}

} // namespace ZHLN::Vk
