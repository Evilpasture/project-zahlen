// src/render/Features.hpp
#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

namespace ZHLN::Vk {

/**
 * @brief Compile-time Type-to-Enum mapping.
 * Mirrors the GetFormatAspect function structure without requiring macros.
 */
template <typename T> [[nodiscard]] constexpr auto GetStructureType() noexcept -> VkStructureType;

/**
 * @brief Wrapper to associate a runtime active/inactive flag with a compile-time feature struct.
 */
template <typename T> struct FeatureNode {
	T feature;
	bool active = true;
};

template <typename... Ts> class FeatureChain {
	std::tuple<FeatureNode<Ts>...> _features;
	VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;

  public:
	FeatureChain() = default;
	FeatureChain(VkPhysicalDevice physicalDevice, std::tuple<FeatureNode<Ts>...>&& t);

	template <typename T, typename Func> auto Require(Func&& configure) &&;

	template <typename T, typename Func> auto Optional(Func&& configure) &&;

	FeatureChain<Ts...>& Build();

	const VkPhysicalDeviceFeatures2* GetRoot();
};

class FeatureChainBuilder {
  public:
	explicit FeatureChainBuilder(VkPhysicalDevice physicalDevice) noexcept
		: _physicalDevice(physicalDevice) {}

	template <typename T, typename Func> auto Require(Func&& configure);

	template <typename T, typename Func> auto Optional(Func&& configure);

  private:
	VkPhysicalDevice _physicalDevice;
};

struct FeatureFactory {
	template <typename T>
	[[nodiscard]] static constexpr auto Create(auto&& configure) noexcept -> T;
};

} // namespace ZHLN::Vk

#include "Features.inl"
