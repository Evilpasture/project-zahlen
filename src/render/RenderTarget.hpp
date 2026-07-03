// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

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

// --- Graph Helper ---
template <VkImageLayout L, VkFormat F>
Vk::TypedImage<L> AssumeLayout(const Vk::RenderTarget<F>& rt,
							   VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
	return {rt.image.Handle(), rt.view.Get(), rt.extent, aspect};
}

template <VkImageLayout TargetLayout, typename T>
constexpr auto TransitionSingle(VkCommandBuffer cmd, const T& res) noexcept {
	// Simply fetch from index 0 of our unified batch transition
	return std::get<0>(TransitionBatch<TargetLayout>(cmd, res));
}

template <VkImageLayout TargetLayout, typename... Resources>
[[nodiscard]] constexpr auto TransitionBatch(VkCommandBuffer cmd,
											 const Resources&... resources) noexcept;

/**
 * @brief Batch transitions a tuple/pack of Vulkan images to a target layout using std::apply.
 */
template <VkImageLayout L, typename Tuple>
[[nodiscard]] auto TransitionAllTo(VkCommandBuffer cmd, const Tuple& atts) {
	return std::apply([&](const auto&... a) { return Vk::TransitionBatch<L>(cmd, a...); }, atts);
}

template <typename... Targets> struct RenderTargetBundle {
	std::tuple<Targets&...> targets;

	constexpr explicit RenderTargetBundle(Targets&... t) noexcept : targets(t...) {}

	// 1. Batch Recreate (Resize)
	void Recreate(Allocator& alloc, const Context& ctx, VkExtent2D extent) const {
		std::apply(
			[&](auto&... t) {
				((t = std::remove_cvref_t<decltype(t)>::Create(alloc, ctx, extent, {})), ...);
			},
			targets);
	}

	// 2. Batch Transition
	template <VkImageLayout TargetLayout>
	[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd) const noexcept {
		return std::apply(
			[&](const auto&... t) { return TransitionBatch<TargetLayout>(cmd, t...); }, targets);
	}
};

// CTAD Factory
template <typename... Ts> [[nodiscard]] constexpr auto TieTargets(Ts&... tgts) noexcept {
	return RenderTargetBundle<Ts...>(tgts...);
}

/**
 * @brief Automatically ties, transitions, clears, and prepares a color attachment group.
 */
template <typename... Images>
[[nodiscard]] auto ClearAndPrepareGroup(VkCommandBuffer cmd, VkExtent2D extent, Color4 clear,
										Images&... imgs) {
	auto bundle = Vk::TieTargets(imgs...);
	auto atts = bundle.template Transition<VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL>(cmd);
	Vk::DynamicPass(extent)
		.AddColorGroup(atts, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, clear)
		.Execute(cmd, []() {});
	return TransitionAllTo<VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(cmd, atts);
}

} // namespace ZHLN::Vk

#include "RenderTarget.inl"
