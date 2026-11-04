// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "RenderCore.hpp"
#include <algorithm>
#include <bit>

namespace ZHLN::Vk {

inline ScopedScissor::ScopedScissor(const VkCommandBuffer cmd, const ScissorDesc& desc) noexcept: commandRect(cmd), resetScissor(desc.fallback) {
    vkCmdSetScissor(commandRect, 0, 1, &desc.target);
}

inline ScopedScissor::~ScopedScissor() noexcept {
    vkCmdSetScissor(commandRect, 0, 1, &resetScissor);
}

inline void CopyImageToBuffer(
    const VkCommandBuffer    cmd,
    const VkImage            srcImage,
    const VkBuffer           dstBuffer,
    const VkExtent2D         extent,
    const Vk::ImageLayout      layout,
    const Vk::ImageAspect aspect
) noexcept {
    const VkBufferImageCopy2 region {
        .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferRowLength   = extent.width,
        .bufferImageHeight = extent.height,
        .imageSubresource =
            {
                .aspectMask     = ToVk(aspect),
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        .imageExtent = {extent.width, extent.height, 1},
    };
    const VkCopyImageToBufferInfo2 info {
        .sType          = VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2,
        .srcImage       = srcImage,
        .srcImageLayout = ToVk(layout),
        .dstBuffer      = dstBuffer,
        .regionCount    = 1,
        .pRegions       = &region,
    };
    vkCmdCopyImageToBuffer2(cmd, &info);
}

inline void CopyImage(
    const VkCommandBuffer cmd,
    const VkImage         srcImage,
    const Vk::ImageLayout   srcLayout,
    const VkImage         dstImage,
    const Vk::ImageLayout   dstLayout,
    const VkExtent3D      extent
) noexcept {
    const VkImageCopy2 region {
        .sType = VK_STRUCTURE_TYPE_IMAGE_COPY_2,
        .srcSubresource =
            {
                .aspectMask = ToVk(Vk::ImageAspect::Color),
                .layerCount = 1,
            },
        .dstSubresource =
            {
                .aspectMask = ToVk(Vk::ImageAspect::Color),
                .layerCount = 1,
            },
        .extent = extent,
    };
    const VkCopyImageInfo2 info {
        .sType          = VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2,
        .srcImage       = srcImage,
        .srcImageLayout = ToVk(srcLayout),
        .dstImage       = dstImage,
        .dstImageLayout = ToVk(dstLayout),
        .regionCount    = 1,
        .pRegions       = &region,
    };
    vkCmdCopyImage2(cmd, &info);
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
    const VkCommandBuffer       cmd,
    const VkImage               image,
    const uint32_t              width,
    const uint32_t              height,
    const Vk::PipelineStage shaderReadStage
) noexcept {
    GenerateMipmaps(cmd, image, width, height, GetMipLevels(width, height), shaderReadStage);
}

inline void GenerateMipmaps(
    const VkCommandBuffer       cmd,
    const VkImage               image,
    const uint32_t              width,
    const uint32_t              height,
    const uint32_t              mipLevels,
    const Vk::PipelineStage shaderReadStage
) noexcept {
    if (cmd == VK_NULL_HANDLE || image == VK_NULL_HANDLE || width == 0 || height == 0 || mipLevels == 0) {
        return;
    }

    uint32_t mipWidth  = width;
    uint32_t mipHeight = height;
    for (uint32_t mip = 1; mip < mipLevels; ++mip) {
        const VkImageSubresourceRange srcMipRange {
            .aspectMask     = ToVk(Vk::ImageAspect::Color),
            .baseMipLevel   = mip - 1,
            .levelCount     = 1,
            .baseArrayLayer = 0,
            .layerCount     = 1,
        };
        ImageBarrier::Transition(
            image,
            srcMipRange,
            Vk::ImageLayout::TransferDstOptimal,
            Vk::ImageLayout::TransferSrcOptimal,
            Vk::PipelineStage::Transfer,
            Vk::Access::TransferWrite,
            Vk::PipelineStage::Transfer,
            Vk::Access::TransferRead
        )
            .Record(cmd);

        const int32_t     nextWidth  = static_cast<int32_t>(std::max(mipWidth / 2, 1U));
        const int32_t     nextHeight = static_cast<int32_t>(std::max(mipHeight / 2, 1U));
        const VkImageBlit blit {
            .srcSubresource = {.aspectMask = ToVk(Vk::ImageAspect::Color), .mipLevel = mip - 1, .layerCount = 1},
            .srcOffsets     = {{0, 0, 0}, {static_cast<int32_t>(mipWidth), static_cast<int32_t>(mipHeight), 1}},
            .dstSubresource = {.aspectMask = ToVk(Vk::ImageAspect::Color), .mipLevel = mip, .layerCount = 1},
            .dstOffsets     = {{0, 0, 0}, {nextWidth, nextHeight, 1}},
        };
        vkCmdBlitImage(cmd, image, ToVk(Vk::ImageLayout::TransferSrcOptimal), image, ToVk(Vk::ImageLayout::TransferDstOptimal), 1, &blit, ToVk(Vk::Filter::Linear));

        ImageBarrier::Transition(
            image,
            srcMipRange,
            Vk::ImageLayout::TransferSrcOptimal,
            Vk::ImageLayout::ShaderReadOnlyOptimal,
            Vk::PipelineStage::Transfer,
            Vk::Access::TransferRead,
            shaderReadStage,
            Vk::Access::ShaderRead
        )
            .Record(cmd);

        mipWidth  = std::max(mipWidth / 2, 1U);
        mipHeight = std::max(mipHeight / 2, 1U);
    }

    const VkImageSubresourceRange lastMipRange {
        .aspectMask     = ToVk(Vk::ImageAspect::Color),
        .baseMipLevel   = mipLevels - 1,
        .levelCount     = 1,
        .baseArrayLayer = 0,
        .layerCount     = 1,
    };
    ImageBarrier::Transition(
        image,
        lastMipRange,
        Vk::ImageLayout::TransferDstOptimal,
        Vk::ImageLayout::ShaderReadOnlyOptimal,
        Vk::PipelineStage::Transfer,
        Vk::Access::TransferWrite,
        shaderReadStage,
        Vk::Access::ShaderRead
    )
        .Record(cmd);
}

} // namespace ZHLN::Vk
