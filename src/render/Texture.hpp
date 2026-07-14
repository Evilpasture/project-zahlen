// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/render/Rendering.hpp> before including any other Zahlen render headers."
#endif

// ============================================================================
// Texture
// ============================================================================
namespace ZHLN::Vk {
struct TextureCreativeWork {
    ZHLN::Vk::Image     image;
    ZHLN::Vk::ImageView view;

    // Explicitly Move-Only
    TextureCreativeWork()                                              = default;
    TextureCreativeWork(const TextureCreativeWork&)                    = delete;
    auto operator=(const TextureCreativeWork&) -> TextureCreativeWork& = delete;

    TextureCreativeWork(TextureCreativeWork&&) noexcept                    = default;
    auto operator=(TextureCreativeWork&&) noexcept -> TextureCreativeWork& = default;
    ~TextureCreativeWork()                                                 = default;

    // Helper to check validity
    explicit operator bool() const {
        return image.Valid() && view.Valid();
    }
};

/**
 * @brief High-level Orchestrator for GPU Texture Uploads.
 * Handles staging, mip-generation, and resource synchronization.
 */
template <VkFormat F>
[[nodiscard]]
inline auto UploadTexture(Allocator& allocator, const Context& ctx, const VkImageCreateInfo& baseInfo, const void* pixelData) -> TextureCreativeWork {
    // TMP Safety Check: Prevent Apple/Metal R8G8B8 errors at compile-time
    if constexpr (F == VK_FORMAT_R8G8B8_UNORM || F == VK_FORMAT_B8G8R8_UNORM) {
        static_assert(
            F != VK_FORMAT_R8G8B8_UNORM && F != VK_FORMAT_B8G8R8_UNORM, "ZHLN Error: 24-bit textures (RGB) are not supported on Apple Silicon/Metal. "
                                                                        "Please use a 32-bit format like VK_FORMAT_R8G8B8A8_UNORM."
        );
    }

    TextureCreativeWork result;
    const uint32_t      texture_w = baseInfo.extent.width;
    const uint32_t      texture_h = baseInfo.extent.height;

    const uint32_t mip_levels = std::bit_width(std::max(texture_w, texture_h));

    CommandPool batch_pool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
    if (!batch_pool.Allocate(1)) {
        return {};
    }
    VkCommandBuffer cmd = batch_pool[0];

    VkImageCreateInfo tex_info = baseInfo;
    tex_info.format            = F; // Force format from template
    tex_info.mipLevels         = mip_levels;
    tex_info.usage |= (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    auto img_res = Image::Create(allocator.Get(), tex_info, VMA_MEMORY_USAGE_GPU_ONLY);
    if (!img_res.has_value()) {
        return {};
    }
    result.image = std::move(img_res.value());

    const size_t image_size  = static_cast<size_t>(texture_w) * texture_h * 4;
    auto         staging_res = Buffer::Create(allocator.Get(), image_size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
    if (!staging_res.has_value()) {
        return {};
    }
    Buffer staging = std::move(staging_res.value());
    if (auto mapped = staging.Map(); mapped.data) {
        std::memcpy(mapped.data, pixelData, image_size);
    }

    {
        CommandBufferGuard          guard(cmd);
        const ZHLN_ImageBarrierDesc initial_barrier = {
            .image      = result.image.Handle(),
            .src_access = 0,
            .dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .src_layout = VK_IMAGE_LAYOUT_UNDEFINED,
            .dst_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .src_stage  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
            .dst_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .aspect     = GetFormatAspect(F),
            .base_mip   = 0,
            .mip_count  = mip_levels
        };
        ZHLN_CmdImageBarrier(cmd, &initial_barrier);

        CopyBufferToImage(
            cmd, {.buffer           = staging.Handle(),
                  .image            = result.image.Handle(),
                  .layout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  .width            = texture_w,
                  .height           = texture_h,
                  .buffer_offset    = 0,
                  .mip_level        = 0,
                  .base_array_layer = 0}
        );

        ZHLN_GenerateMipmaps(cmd, result.image.Handle(), (int32_t) texture_w, (int32_t) texture_h, mip_levels);
    }

    const VkCommandBufferSubmitInfo sub_info = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext         = {},
        .commandBuffer = cmd,
        .deviceMask    = {},
    };
    const VkSubmitInfo2 submit = {
        .sType                    = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .pNext                    = {},
        .flags                    = {},
        .waitSemaphoreInfoCount   = {},
        .pWaitSemaphoreInfos      = {},
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &sub_info,
        .signalSemaphoreInfoCount = {},
        .pSignalSemaphoreInfos    = {},
    };
    vkQueueSubmit2(ctx.GraphicsQueue(), 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(ctx.GraphicsQueue());

    // Final View Creation
    result.view = CreateView<F>(ctx.Device(), result.image.Handle(), GetFormatAspect(F), mip_levels);

    return result;
}

void UpdateBindlessTextureSlot(
    VkDevice                                     device,
    uint32_t                                     slotIndex,
    VkImageView                                  view,
    const ZHLN::DoubleBuffered<VkDescriptorSet>& bindlessSets,
    uint32_t                                     dstBinding = 0
);

} // namespace ZHLN::Vk
