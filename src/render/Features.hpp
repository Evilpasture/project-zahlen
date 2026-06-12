// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <tuple>
#include <type_traits>
#include <utility>
#include <vulkan/vulkan.h>

namespace ZHLN::Vk {

/**
 * @brief Compile-time Type-to-Enum mapping.
 * Mirrors the GetFormatAspect function structure without requiring macros.
 */
template <typename T> [[nodiscard]] constexpr auto GetStructureType() noexcept -> VkStructureType;

template <typename... Ts> class FeatureChain {
	std::tuple<Ts...> _features;

  public:
	FeatureChain() = default;
	FeatureChain(std::tuple<Ts...>&& t);

	template <typename T, typename Func> auto Require(Func&& configure) &&;

	FeatureChain<Ts...>& Build();

	auto* GetRoot();
};

class FeatureChainBuilder {
  public:
	template <typename T, typename Func> auto Require(Func&& configure);
};

struct FeatureFactory {
	template <typename T>
	[[nodiscard]] static constexpr auto Create(auto&& configure) noexcept -> T;
};

} // namespace ZHLN::Vk

#include "Features.inl"
