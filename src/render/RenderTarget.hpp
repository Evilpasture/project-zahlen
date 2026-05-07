#pragma once
#include "RenderCore.hpp"
#include "RenderGraph.hpp"

namespace ZHLN::Vk {

/**
 * @brief Bundles a GPU Image, its View, and its RenderGraph State Tracker.
 * Perfect for Shadows, G-Buffers, Bloom, and other offscreen passes.
 */
template <VkFormat F> struct RenderTarget {
	Image image;
	ImageView view;
	GraphImage tracker;

	RenderTarget() = default;

	// Move-only semantics
	RenderTarget(const RenderTarget&) = delete;
	RenderTarget& operator=(const RenderTarget&) = delete;
	RenderTarget(RenderTarget&&) noexcept = default;
	RenderTarget& operator=(RenderTarget&&) noexcept = default;

	[[nodiscard]] static RenderTarget Create(Allocator& allocator, const Context& ctx,
											 VkExtent2D extent, VkImageUsageFlags usage,
											 VkImageAspectFlags aspect = FormatTraits<F>::aspect) {
		RenderTarget rt;

		const VkImageCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = F,
			.extent = {extent.width, extent.height, 1},
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = usage,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};

		rt.image = Image::Create(allocator.Get(), info, VMA_MEMORY_USAGE_GPU_ONLY);
		if (!rt.image)
			return {};

		rt.view = CreateView<F>(ctx.Device(), rt.image.Handle(), aspect, 1);
		rt.tracker = GraphImage::Create(rt.image.Handle(), rt.view.Get(), extent, aspect);

		return rt;
	}

	[[nodiscard]] bool Valid() const noexcept { return image.Valid() && view.Valid(); }
	explicit operator bool() const noexcept { return Valid(); }
};

} // namespace ZHLN::Vk