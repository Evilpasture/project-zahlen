// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Allocator.hpp>
#include <RenderCore.hpp>

// ============================================================================
// Texture
// ============================================================================
namespace ZHLN::Vk {
struct TextureAsset {
	ZHLN::Vk::Image image;
	ZHLN::Vk::ImageView view;

	// Explicitly Move-Only
	TextureAsset() = default;
	TextureAsset(const TextureAsset&) = delete;
	auto operator=(const TextureAsset&) -> TextureAsset& = delete;

	TextureAsset(TextureAsset&&) noexcept = default;
	auto operator=(TextureAsset&&) noexcept -> TextureAsset& = default;
	~TextureAsset() = default;

	// Helper to check validity
	explicit operator bool() const { return image.Valid() && view.Valid(); }
};

/**
 * @brief High-level Orchestrator for GPU Texture Uploads.
 * Handles staging, mip-generation, and resource synchronization.
 */
template <VkFormat F>
[[nodiscard]]
inline auto UploadTexture(Allocator& allocator, const Context& ctx,
						  const VkImageCreateInfo& baseInfo, const void* pixelData)
	-> TextureAsset {
	// TMP Safety Check: Prevent Apple/Metal R8G8B8 errors at compile-time
	if constexpr (F == VK_FORMAT_R8G8B8_UNORM || F == VK_FORMAT_B8G8R8_UNORM) {
		static_assert(F != VK_FORMAT_R8G8B8_UNORM && F != VK_FORMAT_B8G8R8_UNORM,
					  "ZHLN Error: 24-bit textures (RGB) are not supported on Apple Silicon/Metal. "
					  "Please use a 32-bit format like VK_FORMAT_R8G8B8A8_UNORM.");
	}

	TextureAsset result;
	const uint32_t textureW = baseInfo.extent.width;
	const uint32_t textureH = baseInfo.extent.height;

	const uint32_t mipLevels = std::bit_width(std::max(textureW, textureH));

	CommandPool batchPool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
	if (!batchPool.Allocate(1)) {
		return {};
	}
	VkCommandBuffer cmd = batchPool[0];

	VkImageCreateInfo texInfo = baseInfo;
	texInfo.format = F; // Force format from template
	texInfo.mipLevels = mipLevels;
	texInfo.usage |= (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

	result.image = Image::Create(allocator.Get(), texInfo, VMA_MEMORY_USAGE_GPU_ONLY);
	if (!result.image) {
		return {};
	}

	const size_t imageSize = static_cast<size_t>(textureW) * textureH * 4;
	Buffer staging = Buffer::Create(allocator.Get(), imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
									VMA_MEMORY_USAGE_CPU_ONLY);
	if (auto mapped = staging.Map(); mapped.data) {
		std::memcpy(mapped.data, pixelData, imageSize);
	}

	ZHLN_BeginCommandBuffer(cmd);

	const ZHLN_ImageBarrierDesc initialBarrier = {.image = result.image.Handle(),
												  .src_access = 0,
												  .dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
												  .src_layout = VK_IMAGE_LAYOUT_UNDEFINED,
												  .dst_layout =
													  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
												  .src_stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
												  .dst_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
												  .aspect = GetFormatAspect(F), // Deduced!
												  .base_mip = 0,
												  .mip_count = mipLevels};
	ZHLN_CmdImageBarrier(cmd, &initialBarrier);

	CopyBufferToImage(cmd, {.buffer = staging.Handle(),
							.image = result.image.Handle(),
							.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
							.width = textureW,
							.height = textureH,
							.buffer_offset = 0,
							.mip_level = 0,
							.base_array_layer = 0});

	ZHLN_GenerateMipmaps(cmd, result.image.Handle(), (int32_t)textureW, (int32_t)textureH,
						 mipLevels);

	ZHLN_EndCommandBuffer(cmd);

	const VkCommandBufferSubmitInfo subInfo = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cmd};
	const VkSubmitInfo2 submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
								  .commandBufferInfoCount = 1,
								  .pCommandBufferInfos = &subInfo};
	vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
	vkQueueWaitIdle(ctx.GraphicsQueue());

	// Final View Creation
	result.view = CreateView<F>(ctx.Device(), result.image.Handle(), GetFormatAspect(F), mipLevels);

	return result;
}

void UpdateBindlessTextureSlot(VkDevice device, uint32_t slotIndex, VkImageView view,
							   const ZHLN::DoubleBuffered<VkDescriptorSet>& bindlessSets,
							   uint32_t dstBinding = 0);

} // namespace ZHLN::Vk
