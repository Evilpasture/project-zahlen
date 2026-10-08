// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>

#include "../VkError.hpp"

namespace ZHLN {

struct Color4 {
    float r, g, b, a;
};

} // namespace ZHLN

namespace ZHLN::Vk {

enum class VulkanCallError : uint8_t {
    VulkanCallFailed ZHLN_ANNOTATION(ZHLN::Description<"Vulkan call failed">{}) = 1,
};

[[nodiscard]] auto WaitIdle(VkDevice device) noexcept -> std::expected<void, Vk::Error>;

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
    VkCommandBuffer cmd,
    VkImage srcImage,
    VkBuffer dstBuffer,
    VkExtent2D extent,
    VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT
) noexcept;

template <size_t RegionCount>
[[nodiscard]] constexpr auto CreateCopyRegions(
    VkDeviceSize baseOffset,
    VkDeviceSize regionSize,
    VkExtent3D extent,
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT,
    uint32_t mipLevel = 0,
    uint32_t baseArrayLayer = 0
) noexcept -> std::array<VkBufferImageCopy2, RegionCount> {
    std::array<VkBufferImageCopy2, RegionCount> regions {};
    for (uint32_t i = 0; i < RegionCount; ++i) {
        regions[i] = VkBufferImageCopy2 {
            .sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
            .bufferOffset = baseOffset + i * regionSize,
            .imageSubresource = {
                .aspectMask = aspect,
                .mipLevel = mipLevel,
                .baseArrayLayer = baseArrayLayer + i,
                .layerCount = 1,
            },
            .imageExtent = extent,
        };
    }
    return regions;
}

template <size_t RegionCount>
inline void CopyBufferToImage(
    VkCommandBuffer cmd,
    VkBuffer srcBuffer,
    VkImage dstImage,
    const std::array<VkBufferImageCopy2, RegionCount>& regions,
    VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
) noexcept {
    const VkCopyBufferToImageInfo2 info {
        .sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
        .srcBuffer = srcBuffer,
        .dstImage = dstImage,
        .dstImageLayout = layout,
        .regionCount = static_cast<uint32_t>(RegionCount),
        .pRegions = regions.data(),
    };
    vkCmdCopyBufferToImage2(cmd, &info);
}

// Single-region convenience overload: most uploads copy exactly one region, and
// spelling out <1> plus a one-element array for that is pure noise.
inline void CopyBufferToImage(
    VkCommandBuffer cmd,
    VkBuffer srcBuffer,
    VkImage dstImage,
    const VkBufferImageCopy2& region,
    VkImageLayout layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
) noexcept {
    CopyBufferToImage<1>(cmd, srcBuffer, dstImage, {region}, layout);
}

void ExecuteCommands(VkCommandBuffer primary, std::span<const VkCommandBuffer> secondaries) noexcept;

[[nodiscard]] constexpr auto GetMipLevels(uint32_t width, uint32_t height) noexcept -> uint32_t;
template <uint32_t Width, uint32_t Height>
consteval auto GetMipLevels() noexcept -> uint32_t;

void GenerateMipmaps(
    VkCommandBuffer cmd,
    VkImage image,
    uint32_t width,
    uint32_t height,
    VkPipelineStageFlags2 shaderReadStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
) noexcept;
void GenerateMipmaps(
    VkCommandBuffer cmd,
    VkImage image,
    uint32_t width,
    uint32_t height,
    uint32_t mipLevels,
    VkPipelineStageFlags2 shaderReadStage
) noexcept;

} // namespace ZHLN::Vk

#include "RenderCore.inl"
