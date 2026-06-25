// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Allocator.hpp"
#include "RenderCore.hpp"

namespace ZHLN::Vk {

template <VkFormat F> struct RenderTarget {
	Image image;
	ImageView view;
	VkExtent2D extent{};

	RenderTarget() = default;

	RenderTarget(const RenderTarget&) = delete;
	auto operator=(const RenderTarget&) -> RenderTarget& = delete;

	RenderTarget(RenderTarget&& other) noexcept;
	auto operator=(RenderTarget&& other) noexcept -> RenderTarget&;

	~RenderTarget() = default;

	[[nodiscard]] auto State() const noexcept -> TypedImage<VK_IMAGE_LAYOUT_UNDEFINED>;

	struct RenderTargetDescriptor {
		VkImageUsageFlags usage = 0;
		VkImageAspectFlags aspect = GetFormatAspect(F);
		uint32_t arrayLayers = 1;
	};

	[[nodiscard]] static auto Create(Allocator& allocator, const Context& ctx, VkExtent2D extent,
									 RenderTargetDescriptor desc) -> RenderTarget;

	[[nodiscard]] auto Valid() const noexcept -> bool;
	explicit operator bool() const noexcept;
};

// Define the Transition overload here where RenderTarget is fully complete
template <VkImageLayout TargetLayout, VkFormat F>
[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd, const RenderTarget<F>& rt,
										Tag<TargetLayout> /*unused*/) noexcept;

template <typename T> struct TargetFormat;

template <VkFormat F> struct TargetFormat<Vk::RenderTarget<F>> {
	static constexpr VkFormat value = F;
};

template <typename... Targets> struct GBufferLayout {
	static constexpr size_t count = sizeof...(Targets);

	template <size_t Index> using TargetTypeAt = Targets...[Index];

	template <size_t Index> static constexpr VkFormat get() {
		static_assert(Index < count, "GBuffer layout index out of bounds.");
		return TargetFormat<TargetTypeAt<Index>>::value;
	}

	static constexpr std::array<VkFormat, count> array = {TargetFormat<Targets>::value...};
};

} // namespace ZHLN::Vk

#include "RenderTarget.inl"
