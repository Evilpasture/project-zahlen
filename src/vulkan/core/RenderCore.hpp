// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <vkd3d/Color.hpp>

namespace ZHLN {

/// The four-float color aggregate lives in the joint layer now
/// (<vkd3d/Color.hpp>); this alias keeps the historical spelling.
using VkD3D::Color4;

} // namespace ZHLN

namespace ZHLN::Vk {

/// Spelling used inside the Vulkan layer while `Color4` still lives in `ZHLN`;
/// both names denote the same four-float aggregate.
using Color4 = ZHLN::Color4;

enum class VulkanCallError : uint8_t {
    VulkanCallFailed ZHLN_ANNOTATION(ZHLN::Description<"Vulkan call failed"> {}) = 1,
};

[[nodiscard]] auto WaitIdle(VkDevice device) noexcept -> std::expected<void, ErrorCode>;

struct ScopedScissor {
    VkCommandBuffer commandRect;
    VkRect2D        resetScissor;

    struct ScissorDesc {
        VkRect2D target;
        VkRect2D fallback;
    };
    ScopedScissor(VkCommandBuffer cmd, const ScissorDesc& desc) noexcept;
    ~ScopedScissor() noexcept;

    ScopedScissor(const ScopedScissor&)                    = delete;
    auto operator=(const ScopedScissor&) -> ScopedScissor& = delete;
    ScopedScissor(ScopedScissor&&)                         = delete;
    auto operator=(ScopedScissor&&) -> ScopedScissor&      = delete;
};

void CopyImageToBuffer(
    VkCommandBuffer    cmd,
    VkImage            srcImage,
    VkBuffer           dstBuffer,
    VkExtent2D         extent,
    Vk::ImageLayout      layout = Vk::ImageLayout::TransferSrcOptimal,
    Vk::ImageAspect aspect = Vk::ImageAspect::Color
) noexcept;

void CopyImage(VkCommandBuffer cmd, VkImage srcImage, Vk::ImageLayout srcLayout, VkImage dstImage, Vk::ImageLayout dstLayout, VkExtent3D extent) noexcept;

template <size_t RegionCount>
[[nodiscard]] constexpr auto CreateCopyRegions(
    VkDeviceSize       baseOffset,
    VkDeviceSize       regionSize,
    VkExtent3D         extent,
    Vk::ImageAspect aspect         = Vk::ImageAspect::Color,
    uint32_t           mipLevel       = 0,
    uint32_t           baseArrayLayer = 0
) noexcept -> std::array<VkBufferImageCopy2, RegionCount> {
    std::array<VkBufferImageCopy2, RegionCount> regions {};
    for (uint32_t i = 0; i < RegionCount; ++i) {
        regions[i] = VkBufferImageCopy2 {
            .sType        = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .bufferOffset = baseOffset + (i * regionSize),
            .imageSubresource =
                {
                    .aspectMask     = ToVk(aspect),
                    .mipLevel       = mipLevel,
                    .baseArrayLayer = baseArrayLayer + i,
                    .layerCount     = 1,
                },
            .imageExtent = extent,
        };
    }
    return regions;
}

template <size_t RegionCount>
    requires(RegionCount != std::dynamic_extent)
inline void CopyBufferToImage(
    VkCommandBuffer                                  cmd,
    VkBuffer                                         srcBuffer,
    VkImage                                          dstImage,
    std::span<const VkBufferImageCopy2, RegionCount> regions,
    Vk::ImageLayout                                    layout = Vk::ImageLayout::TransferDstOptimal
) noexcept {
    const VkCopyBufferToImageInfo2 info {
        .sType          = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer      = srcBuffer,
        .dstImage       = dstImage,
        .dstImageLayout = ToVk(layout),
        .regionCount    = static_cast<uint32_t>(RegionCount),
        .pRegions       = regions.data(),
    };
    vkCmdCopyBufferToImage2(cmd, &info);
}

inline void CopyBufferToImage(
    VkCommandBuffer           cmd,
    VkBuffer                  srcBuffer,
    VkImage                   dstImage,
    const VkBufferImageCopy2& region,
    Vk::ImageLayout             layout = Vk::ImageLayout::TransferDstOptimal
) noexcept {
    const std::span<const VkBufferImageCopy2, 1> regions {&region, 1};
    CopyBufferToImage<1>(cmd, srcBuffer, dstImage, regions, layout);
}

void ExecuteCommands(VkCommandBuffer primary, std::span<const VkCommandBuffer> secondaries) noexcept;

[[nodiscard]] constexpr auto GetMipLevels(uint32_t width, uint32_t height) noexcept -> uint32_t;
template <uint32_t Width, uint32_t Height>
consteval auto GetMipLevels() noexcept -> uint32_t;

void GenerateMipmaps(
    VkCommandBuffer       cmd,
    VkImage               image,
    uint32_t              width,
    uint32_t              height,
    Vk::PipelineStage shaderReadStage = Vk::PipelineStage::Fragment
) noexcept;
void GenerateMipmaps(VkCommandBuffer cmd, VkImage image, uint32_t width, uint32_t height, uint32_t mipLevels, Vk::PipelineStage shaderReadStage) noexcept;

} // namespace ZHLN::Vk

#include "RenderCore.inl"
