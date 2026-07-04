#pragma once

#include "RenderTarget.hpp"

namespace ZHLN::Vk {

// ============================================================================
// RenderTarget Implementation
// ============================================================================

template <VkFormat F>
inline RenderTarget<F>::RenderTarget(RenderTarget&& other) noexcept
	: image(std::move(other.image)), view(std::move(other.view)), extent(other.extent) {}

template <VkFormat F>
inline auto RenderTarget<F>::operator=(RenderTarget&& other) noexcept -> RenderTarget& {
	if (this != &other) {
		image = std::move(other.image);
		view = std::move(other.view);
		extent = other.extent;
	}
	return *this;
}

template <VkFormat F>
inline auto RenderTarget<F>::State() const noexcept -> TypedImage<VK_IMAGE_LAYOUT_UNDEFINED> {
	return {.handle = image.Handle(),
			.view = view.Get(),
			.extent = extent,
			.aspect = GetFormatAspect(F),
			.format = F};
}

template <VkFormat F>
inline auto RenderTarget<F>::Create(Allocator& allocator, const Context& ctx, VkExtent2D extent,
									RenderTargetDescriptor desc) -> RenderTarget {
	RenderTarget rt;
	rt.extent = extent;

	const VkImageCreateInfo info = {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.pNext = nullptr,
		.flags = static_cast<VkImageCreateFlags>((desc.arrayLayers >= 6) *
												 VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT),
		.imageType = VK_IMAGE_TYPE_2D,
		.format = F,
		.extent = {.width = extent.width, .height = extent.height, .depth = 1},
		.mipLevels = 1,
		.arrayLayers = desc.arrayLayers,
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
		if (desc.arrayLayers > 1) {
			rt.view = CreateView2DArray<F>(ctx.Device(), rt.image.Handle(), 0, desc.arrayLayers,
										   desc.aspect, 1);
		} else {
			rt.view = CreateView<F>(ctx.Device(), rt.image.Handle(), desc.aspect, 1);
		}
	}
	return rt;
}

template <VkFormat F> inline auto RenderTarget<F>::Valid() const noexcept -> bool {
	return image.Valid() && view.Valid();
}

template <VkFormat F> inline RenderTarget<F>::operator bool() const noexcept {
	return Valid();
}

// ============================================================================
// Transition Helpers
// ============================================================================

namespace detail {

struct LayoutSyncInfo {
	VkPipelineStageFlags2 stage;
	VkAccessFlags2 access;
};

// Maps Vulkan Image Layouts to Stage and Access flags using Synchronization2
constexpr auto GetSyncInfo(VkImageLayout layout, bool isSource) noexcept -> LayoutSyncInfo {
	switch (layout) {
		case VK_IMAGE_LAYOUT_UNDEFINED:
			return {.stage = VK_PIPELINE_STAGE_2_NONE, .access = 0};

		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			return {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
					.access = isSource ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
									   : (VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
										  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT)};

		case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL:
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
			return {.stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
							 VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
					.access = isSource ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
									   : (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
										  VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)};

		case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			return {.stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
							 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
					.access = VK_ACCESS_2_SHADER_READ_BIT};

		case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
			return {.stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .access = 0};

		case VK_IMAGE_LAYOUT_GENERAL:
			return {.stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					.access = isSource
								  ? (VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT)
								  : (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT)};

		default:
			return {.stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
					.access =
						isSource ? VK_ACCESS_2_MEMORY_WRITE_BIT : VK_ACCESS_2_MEMORY_READ_BIT};
	}
}

// Uniformly unpacks RenderTarget<F> or TypedImage<Layout> configurations
template <typename T> struct ResourceTraits;

template <VkImageLayout Layout> struct ResourceTraits<TypedImage<Layout>> {
	static constexpr VkImageLayout old_layout = Layout;
	static constexpr auto GetImage(const TypedImage<Layout>& res) noexcept { return res.handle; }
	static constexpr auto GetView(const TypedImage<Layout>& res) noexcept { return res.view; }
	static constexpr auto GetExtent(const TypedImage<Layout>& res) noexcept { return res.extent; }
	static constexpr auto GetAspect(const TypedImage<Layout>& res) noexcept { return res.aspect; }
	static constexpr auto GetFormat(const TypedImage<Layout>& res) noexcept { return res.format; }
};

template <VkFormat F> struct ResourceTraits<RenderTarget<F>> {
	static constexpr VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	static constexpr auto GetImage(const RenderTarget<F>& res) noexcept {
		return res.image.Handle();
	}
	static constexpr auto GetView(const RenderTarget<F>& res) noexcept { return res.view.Get(); }
	static constexpr auto GetExtent(const RenderTarget<F>& res) noexcept { return res.extent; }
	static constexpr auto GetAspect(const RenderTarget<F>& /*res*/) noexcept {
		return GetFormatAspect(F);
	}
	static constexpr auto GetFormat(const RenderTarget<F>&) noexcept { return F; }
};

} // namespace detail

// ============================================================================
// Transition Implementation
// ============================================================================

template <VkImageLayout TargetLayout, VkFormat F>
[[nodiscard]] constexpr auto Transition(VkCommandBuffer cmd, const RenderTarget<F>& rt,
										Tag<TargetLayout> /*unused*/) noexcept {
	return TransitionSingle<TargetLayout>(cmd, rt);
}

// Overrides the old loop-based TransitionBatch to issue a single grouped pipeline barrier
template <VkImageLayout TargetLayout, typename... Resources>
[[nodiscard]] constexpr auto TransitionBatch(VkCommandBuffer cmd,
											 const Resources&... resources) noexcept {
	constexpr size_t count = sizeof...(Resources);
	if constexpr (count == 0) {
		return std::tuple<>{};
	} else {
		std::array<VkImageMemoryBarrier2, count> barriers{};
		size_t idx = 0;

		auto populate_barrier = [&](const auto& res) {
			using Traits = detail::ResourceTraits<std::decay_t<decltype(res)>>;
			constexpr VkImageLayout old_layout = Traits::old_layout;

			auto src_sync = detail::GetSyncInfo(old_layout, true);
			auto dst_sync = detail::GetSyncInfo(TargetLayout, false);

			barriers[idx++] = VkImageMemoryBarrier2{
				.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
				.pNext = nullptr,
				.srcStageMask = src_sync.stage,
				.srcAccessMask = src_sync.access,
				.dstStageMask = dst_sync.stage,
				.dstAccessMask = dst_sync.access,
				.oldLayout = old_layout,
				.newLayout = TargetLayout,
				.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
				.image = Traits::GetImage(res),
				.subresourceRange = {.aspectMask = Traits::GetAspect(res),
									 .baseMipLevel = 0,
									 .levelCount = VK_REMAINING_MIP_LEVELS,
									 .baseArrayLayer = 0,
									 .layerCount = VK_REMAINING_ARRAY_LAYERS}};
		};

		(populate_barrier(resources), ...);

		VkDependencyInfo depInfo = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
									.pNext = nullptr,
									.dependencyFlags = 0,
									.memoryBarrierCount = 0,
									.pMemoryBarriers = nullptr,
									.bufferMemoryBarrierCount = 0,
									.pBufferMemoryBarriers = nullptr,
									.imageMemoryBarrierCount = static_cast<uint32_t>(count),
									.pImageMemoryBarriers = barriers.data()};

		vkCmdPipelineBarrier2(cmd, &depInfo);

		auto make_typed = [&](const auto& res) {
			using Traits = detail::ResourceTraits<std::decay_t<decltype(res)>>;
			return TypedImage<TargetLayout>{.handle = Traits::GetImage(res),
											.view = Traits::GetView(res),
											.extent = Traits::GetExtent(res),
											.aspect = Traits::GetAspect(res),
											.format = Traits::GetFormat(res)};
		};

		return std::make_tuple(make_typed(resources)...);
	}
}

} // namespace ZHLN::Vk
