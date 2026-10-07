// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "RenderCore.hpp"
#include <algorithm>
#include <bit>

namespace ZHLN::Vk {

inline ScopedScissor::ScopedScissor(const VkCommandBuffer cmd, const ScissorDesc& desc) noexcept:
    commandRect(cmd), resetScissor(desc.fallback) {
    vkCmdSetScissor(commandRect, 0, 1, &desc.target);
}

inline ScopedScissor::~ScopedScissor() noexcept {
    vkCmdSetScissor(commandRect, 0, 1, &resetScissor);
}

inline void CopyImageToBuffer(
    const VkCommandBuffer cmd,
    const VkImage srcImage,
    const VkBuffer dstBuffer,
    const VkExtent2D extent,
    const VkImageLayout layout,
    const VkImageAspectFlags aspect
) noexcept {
    const VkBufferImageCopy2 region {
        .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferRowLength = extent.width,
        .bufferImageHeight = extent.height,
        .imageSubresource = {
            .aspectMask = aspect,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageExtent = {extent.width, extent.height, 1},
    };
    const VkCopyImageToBufferInfo2 info {
        .sType = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
        .srcImage = srcImage,
        .srcImageLayout = layout,
        .dstBuffer = dstBuffer,
        .regionCount = 1,
        .pRegions = &region,
    };
    vkCmdCopyImageToBuffer2(cmd, &info);
}

inline void ExecuteCommands(const VkCommandBuffer primary, const std::span<const VkCommandBuffer> secondaries) noexcept {
    if (!secondaries.empty()) {
        vkCmdExecuteCommands(primary, static_cast<uint32_t>(secondaries.size()), secondaries.data());
    }
}

constexpr auto GetMipLevels(const uint32_t width, const uint32_t height) noexcept -> uint32_t {
    return std::bit_width(std::max(width, height));
}

template <uint32_t Width, uint32_t Height>
consteval auto GetMipLevels() noexcept -> uint32_t {
    return GetMipLevels(Width, Height);
}

inline void GenerateMipmaps(
    const VkCommandBuffer cmd,
    const VkImage image,
    const uint32_t width,
    const uint32_t height,
    const VkPipelineStageFlags2 shaderReadStage
) noexcept {
    GenerateMipmaps(cmd, image, width, height, GetMipLevels(width, height), shaderReadStage);
}

inline void GenerateMipmaps(
    const VkCommandBuffer cmd,
    const VkImage image,
    const uint32_t width,
    const uint32_t height,
    const uint32_t mipLevels,
    const VkPipelineStageFlags2 shaderReadStage
) noexcept {
    if (cmd == VK_NULL_HANDLE || image == VK_NULL_HANDLE || width == 0 || height == 0 || mipLevels == 0) {
        return;
    }

    uint32_t mipWidth = width;
    uint32_t mipHeight = height;
    for (uint32_t mip = 1; mip < mipLevels; ++mip) {
        ImageBarrier(cmd, ImageBarrierDesc {
            .image = image,
            .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dst_access = VK_ACCESS_2_TRANSFER_READ_BIT,
            .src_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .dst_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dst_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
            .base_mip = mip - 1,
            .mip_count = 1,
            .base_array_layer = 0,
            .layer_count = 1,
        });

        const int32_t nextWidth = static_cast<int32_t>(std::max(mipWidth / 2, 1U));
        const int32_t nextHeight = static_cast<int32_t>(std::max(mipHeight / 2, 1U));
        const VkImageBlit blit {
            .srcSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = mip - 1, .layerCount = 1},
            .srcOffsets = {{0, 0, 0}, {static_cast<int32_t>(mipWidth), static_cast<int32_t>(mipHeight), 1}},
            .dstSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = mip, .layerCount = 1},
            .dstOffsets = {{0, 0, 0}, {nextWidth, nextHeight, 1}},
        };
        vkCmdBlitImage(
            cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR
        );

        ImageBarrier(cmd, ImageBarrierDesc {
            .image = image,
            .src_access = VK_ACCESS_2_TRANSFER_READ_BIT,
            .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
            .src_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .src_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dst_stage = shaderReadStage,
            .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
            .base_mip = mip - 1,
            .mip_count = 1,
            .base_array_layer = 0,
            .layer_count = 1,
        });

        mipWidth = std::max(mipWidth / 2, 1U);
        mipHeight = std::max(mipHeight / 2, 1U);
    }

    ImageBarrier(cmd, ImageBarrierDesc {
        .image = image,
        .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
        .src_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dst_stage = shaderReadStage,
        .aspect = VK_IMAGE_ASPECT_COLOR_BIT,
        .base_mip = mipLevels - 1,
        .mip_count = 1,
        .base_array_layer = 0,
        .layer_count = 1,
    });
}

} // namespace ZHLN::Vk
