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

	RenderTarget(RenderTarget&& other) noexcept
		: image(std::move(other.image)), view(std::move(other.view)), extent(other.extent) {}

	auto operator=(RenderTarget&& other) noexcept -> RenderTarget& {
		if (this != &other) {
			image = std::move(other.image);
			view = std::move(other.view);
			extent = other.extent;
		}
		return *this;
	}

	~RenderTarget() = default;

	[[nodiscard]] auto State() const noexcept -> TypedImage<VK_IMAGE_LAYOUT_UNDEFINED> {
		return {.handle = image.Handle(),
				.view = view.Get(),
				.extent = extent,
				.aspect = GetFormatAspect(F)};
	}

	struct RenderTargetDescriptor {
		VkImageUsageFlags usage = 0;
		VkImageAspectFlags aspect = GetFormatAspect(F);
	};

	[[nodiscard]] static auto Create(Allocator& allocator, const Context& ctx, VkExtent2D extent,
									 RenderTargetDescriptor desc) -> RenderTarget {
		RenderTarget rt;
		rt.extent = extent;

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
			.usage = desc.usage,
			.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.queueFamilyIndexCount = 0,
			.pQueueFamilyIndices = nullptr,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};

		rt.image = Image::Create(allocator.Get(), info, VMA_MEMORY_USAGE_GPU_ONLY);
		if (rt.image.Valid()) {
			rt.view = CreateView<F>(ctx.Device(), rt.image.Handle(), desc.aspect, 1);
		}
		return rt;
	}

	[[nodiscard]] auto Valid() const noexcept -> bool { return image.Valid() && view.Valid(); }
	explicit operator bool() const noexcept { return Valid(); }
};

// Define the Transition overload here where RenderTarget is fully complete [3]
template <VkImageLayout TargetLayout, VkFormat F>
[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd, const RenderTarget<F>& rt,
										Tag<TargetLayout> /*unused*/) noexcept {
	return Transition<TargetLayout>(cmd, rt.State());
}

} // namespace ZHLN::Vk
