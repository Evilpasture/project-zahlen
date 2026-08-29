// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/render/StagingContext.cpp
// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "StagingContext.hpp"
#include "Allocator.hpp"
#include <cstring>
#include <utility>

namespace ZHLN::Vk {

StagingContext::StagingContext(Allocator& allocator, const Context& ctx): _allocator(&allocator), _ctx(&ctx) {
}

StagingContext::~StagingContext() {
    // Destructor automatically cleans up the fence
    if (_fence != VK_NULL_HANDLE) {
        Wait();
        vkDestroyFence(_ctx->Device(), _fence, nullptr);
    }
}

StagingContext::StagingContext(StagingContext&& other) noexcept:
    _allocator(other._allocator), _ctx(other._ctx), _cmdPool(std::move(other._cmdPool)), _cmd(std::exchange(other._cmd, VK_NULL_HANDLE)),
    _stagingBuffers(std::move(other._stagingBuffers)), _fence(std::exchange(other._fence, VK_NULL_HANDLE)) {
}

auto StagingContext::Begin() noexcept -> std::expected<void, Error> {
    _cmdPool       = CommandPool<QueueType::Graphics>(_ctx->Device(), _ctx->PhysicalInfo().graphics_family);
    auto alloc_res = _cmdPool.Allocate(1);
    if (!alloc_res) [[unlikely]] {
        return std::unexpected(alloc_res.error());
    }
    _cmd = _cmdPool[0];
    ZHLN_BeginCommandBuffer(_cmd);
    return {};
}

auto StagingContext::UploadImage2D(VkImage dstImage, uint32_t w, uint32_t h, uint32_t mipLevels, const void* data, size_t bytes) noexcept
    -> std::expected<void, Error> {
    return Buffer::Create(_allocator->Get(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
        .and_then([&, dstImage, w, h, mipLevels, data, bytes](auto&& staging) -> std::expected<void, Error> {
            auto mapped = staging.Map();
            if (mapped.data != nullptr) {
                std::memcpy(mapped.data, data, bytes);
            } else {
                return std::unexpected(StagingError::MemoryMappingFailed);
            }

            UploadImage2DBuffer(dstImage, w, h, mipLevels, staging.Handle(), 0);
            _stagingBuffers.push_back(std::forward<decltype(staging)>(staging));
            return {};
        });
}

void StagingContext::UploadImage2DBuffer(VkImage dstImage, uint32_t w, uint32_t h, uint32_t mipLevels, VkBuffer stagingBuf, VkDeviceSize offset) {
    TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
        _cmd, dstImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels
    );

    ZHLN_BufferImageCopyDesc copy_region = {
        .buffer           = stagingBuf,
        .image            = dstImage,
        .layout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .width            = w,
        .height           = h,
        .buffer_offset    = offset,
        .mip_level        = 0,
        .base_array_layer = 0
    };
    ZHLN_CmdCopyBufferToImage(_cmd, &copy_region);

    if (mipLevels > 1) {
        ZHLN_GenerateMipmaps(_cmd, dstImage, w, h, mipLevels);
    } else {
        TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
            _cmd, dstImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
        );
    }
}

void StagingContext::UploadPrefilteredCubeMap(VkImage dstImage, VkBuffer stagingBuf, uint32_t baseSize, uint32_t mipLevels) {
    TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
        _cmd, dstImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels
    );

    size_t current_offset = 0;
    for (uint32_t mip = 0; mip < mipLevels; ++mip) {
        uint32_t mip_size  = baseSize >> mip;
        auto     face_size = static_cast<size_t>(mip_size) * mip_size * 4;

        for (uint32_t face = 0; face < 6; ++face) {
            VkBufferImageCopy2 region = {
                .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
                .pNext             = {},
                .bufferOffset      = current_offset + (face * face_size),
                .bufferRowLength   = {},
                .bufferImageHeight = {},
                .imageSubresource  = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = mip, .baseArrayLayer = face, .layerCount = 1},
                .imageOffset       = {},
                .imageExtent       = {.width = mip_size, .height = mip_size, .depth = 1},
            };

            VkCopyBufferToImageInfo2 copy_info = {
                .sType          = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2,
                .pNext          = {},
                .srcBuffer      = stagingBuf,
                .dstImage       = dstImage,
                .dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .regionCount    = 1,
                .pRegions       = &region,
            };
            vkCmdCopyBufferToImage2(_cmd, &copy_info);
        }
        current_offset += (face_size * 6);
    }

    TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
        _cmd, dstImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels
    );
}

void StagingContext::AddBuffer(Buffer&& buf) {
    _stagingBuffers.push_back(std::move(buf));
}

void StagingContext::ExecuteAsync() {
    ZHLN_EndCommandBuffer(_cmd);

    // Destroy the previous fence if this context is being reused
    if (_fence != VK_NULL_HANDLE) {
        Wait();
        vkDestroyFence(_ctx->Device(), _fence, nullptr);
        _fence = VK_NULL_HANDLE;
    }

    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
    vkCreateFence(_ctx->Device(), &fence_info, nullptr, &_fence);

    VkCommandBufferSubmitInfo sub_info = {
        .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .pNext         = {},
        .commandBuffer = _cmd,
        .deviceMask    = {},
    };
    VkSubmitInfo2 submit = {
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
    vkQueueSubmit2(_ctx->GraphicsQueue(), 1, &submit, _fence);
}

void StagingContext::Wait() noexcept {
    if (_fence != VK_NULL_HANDLE) {
        vkWaitForFences(_ctx->Device(), 1, &_fence, VK_TRUE, UINT64_MAX);
    }
}

} // namespace ZHLN::Vk
