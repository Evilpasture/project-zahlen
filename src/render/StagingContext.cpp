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
        vkDestroyFence(_ctx->Device(), _fence, nullptr);
    }
}

StagingContext::StagingContext(StagingContext&& other) noexcept:
    _allocator(other._allocator), _ctx(other._ctx), _cmdPool(std::move(other._cmdPool)), _cmd(std::exchange(other._cmd, VK_NULL_HANDLE)),
    _stagingBuffers(std::move(other._stagingBuffers)), _fence(std::exchange(other._fence, VK_NULL_HANDLE)) {
}

void StagingContext::Begin() {
    _cmdPool                 = CommandPool<QueueType::Graphics>(_ctx->Device(), _ctx->PhysicalInfo().graphics_family);
    [[maybe_unused]] bool ok = _cmdPool.Allocate(1);
    _cmd                     = _cmdPool[0];
    ZHLN_BeginCommandBuffer(_cmd);
}

auto StagingContext::UploadImage2D(VkImage dstImage, uint32_t w, uint32_t h, uint32_t mipLevels, const void* data, size_t bytes) noexcept
    -> std::expected<void, VkResult> {
    return Buffer::Create(_allocator->Get(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY)
        .and_then([&, dstImage, w, h, mipLevels, data, bytes](auto&& staging) -> std::expected<void, VkResult> {
            auto mapped = staging.Map();
            if (mapped.data != nullptr) {
                std::memcpy(mapped.data, data, bytes);
            } else {
                return std::unexpected(VK_ERROR_MEMORY_MAP_FAILED);
            }

            UploadImage2DBuffer(dstImage, w, h, mipLevels, staging.Handle(), 0);
            _stagingBuffers.push_back(std::forward<decltype(staging)>(staging));
            return {};
        });
}

void StagingContext::UploadImage2DBuffer(VkImage dstImage, uint32_t w, uint32_t h, uint32_t mipLevels, VkBuffer stagingBuf, VkDeviceSize offset) {
    ZHLN_ImageBarrierDesc initial_barrier = {
        .image      = dstImage,
        .src_access = 0,
        .dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .src_layout = VK_IMAGE_LAYOUT_UNDEFINED,
        .dst_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .src_stage  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .dst_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
        .base_mip   = 0,
        .mip_count  = mipLevels
    };
    ZHLN_CmdImageBarrier(_cmd, &initial_barrier);

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
        ZHLN_ImageBarrierDesc final_barrier = {
            .image      = dstImage,
            .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
            .src_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .src_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            .dst_stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
            .base_mip   = 0,
            .mip_count  = 1
        };
        ZHLN_CmdImageBarrier(_cmd, &final_barrier);
    }
}

void StagingContext::UploadPrefilteredCubeMap(VkImage dstImage, VkBuffer stagingBuf, uint32_t baseSize, uint32_t mipLevels) {
    ZHLN_ImageBarrierDesc initial_barrier = {
        .image      = dstImage,
        .src_access = 0,
        .dst_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .src_layout = VK_IMAGE_LAYOUT_UNDEFINED,
        .dst_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .src_stage  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .dst_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
        .base_mip   = 0,
        .mip_count  = mipLevels
    };
    ZHLN_CmdImageBarrier(_cmd, &initial_barrier);

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

    ZHLN_ImageBarrierDesc final_barrier = {
        .image      = dstImage,
        .src_access = VK_ACCESS_2_TRANSFER_WRITE_BIT,
        .dst_access = VK_ACCESS_2_SHADER_READ_BIT,
        .src_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .dst_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .src_stage  = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
        .dst_stage  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
        .base_mip   = 0,
        .mip_count  = mipLevels
    };
    ZHLN_CmdImageBarrier(_cmd, &final_barrier);
}

void StagingContext::AddBuffer(Buffer&& buf) {
    _stagingBuffers.push_back(std::move(buf));
}

void StagingContext::ExecuteAsync() {
    ZHLN_EndCommandBuffer(_cmd);

    // Destroy the previous fence if this context is being reused
    if (_fence != VK_NULL_HANDLE) {
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
