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
	};

	[[nodiscard]] static auto Create(Allocator& allocator, const Context& ctx, VkExtent2D extent,
									 RenderTargetDescriptor desc) -> RenderTarget;

	[[nodiscard]] auto Valid() const noexcept -> bool;
	explicit operator bool() const noexcept;
};

// Define the Transition overload here where RenderTarget is fully complete [3]
template <VkImageLayout TargetLayout, VkFormat F>
[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd, const RenderTarget<F>& rt,
										Tag<TargetLayout>) noexcept;

} // namespace ZHLN::Vk

#include "RenderTarget.inl"
