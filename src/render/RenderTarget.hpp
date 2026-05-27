#pragma once
#include "Allocator.hpp"
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

	// Rule of Five: Enforce move-only semantics and satisfy static analysis
	RenderTarget(const RenderTarget&) = delete;
	auto operator=(const RenderTarget&) -> RenderTarget& = delete;
	RenderTarget(RenderTarget&&) noexcept = default;
	auto operator=(RenderTarget&&) noexcept -> RenderTarget& = default;
	~RenderTarget() = default; // <-- Defaulted destructor safely completes the Rule of Five

	[[nodiscard]] static auto Create(Allocator& allocator, const Context& ctx, VkExtent2D extent,
									 VkImageUsageFlags usage,
									 VkImageAspectFlags aspect = GetFormatAspect(F))
		-> RenderTarget {
		RenderTarget renderTarget;

		const VkImageCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = F,
			.extent = {.width = extent.width, .height = extent.height, .depth = 1},
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = usage,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.queueFamilyIndexCount = 0,
			.pQueueFamilyIndices = nullptr,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};

		renderTarget.image = Image::Create(allocator.Get(), info, VMA_MEMORY_USAGE_GPU_ONLY);
		if (!renderTarget.image) {
			return {};
		}

		renderTarget.view = CreateView<F>(ctx.Device(), renderTarget.image.Handle(), aspect, 1);
		renderTarget.tracker = GraphImage::Create(renderTarget.image.Handle(),
												  renderTarget.view.Get(), extent, aspect);

		return renderTarget;
	}

	[[nodiscard]] auto Valid() const noexcept -> bool { return image.Valid() && view.Valid(); }
	explicit operator bool() const noexcept { return Valid(); }
};

} // namespace ZHLN::Vk
