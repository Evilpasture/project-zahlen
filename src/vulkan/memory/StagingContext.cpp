// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// clang-format off
#include "Rendering.hpp"
// clang-format on
#include "StagingContext.hpp"
#include "Allocator.hpp"
#include <Zahlen/Core/Defer.hpp>
#include <cstring>
#include <utility>

namespace ZHLN::Vk {

StagingContext::StagingContext(Allocator& allocator, const Context& ctx): _allocator(&allocator), _ctx(&ctx) {
}

StagingContext::~StagingContext() {
    Wait();
    _recorder.Abort();
    for (auto& buffer: _stagingBuffers) {
        _allocator->DestroyBuffer(buffer);
    }
    if (_fence != VK_NULL_HANDLE) {
        vkDestroyFence(_ctx->Device(), _fence, nullptr);
    }
}

StagingContext::StagingContext(StagingContext&& other) noexcept:
    _allocator(other._allocator), _ctx(other._ctx), _cmdPool(std::move(other._cmdPool)),
    _recorder(std::move(other._recorder)), _stagingBuffers(std::move(other._stagingBuffers)), _fence(std::exchange(other._fence, VK_NULL_HANDLE)) {
}

auto StagingContext::Begin() noexcept -> std::expected<void, ErrorCode> {
    if (_fence != VK_NULL_HANDLE) {
        Wait();
        vkDestroyFence(_ctx->Device(), _fence, nullptr);
        _fence = VK_NULL_HANDLE;
    }
    _recorder.Abort();
    for (auto& buffer: _stagingBuffers) _allocator->DestroyBuffer(buffer);
    _stagingBuffers.clear();
    _cmdPool       = CommandPool<QueueType::Graphics>(_ctx->Device(), _ctx->PhysicalInfo().graphics_family);
    auto alloc_res = _cmdPool.Allocate(1);
    if (!alloc_res) [[unlikely]] {
        return std::unexpected(alloc_res.error());
    }
    auto recording = CommandRecorder::Begin(_cmdPool[0]);
    if (!recording) {
        return std::unexpected(recording.error());
    }
    _recorder = std::move(*recording);
    return {};
}

auto StagingContext::UploadImage2D(VkImage dstImage, uint32_t w, uint32_t h, uint32_t mipLevels, const void* data, size_t bytes) noexcept
    -> std::expected<void, ErrorCode> {
    return Buffer::Create(_allocator->Get(), bytes, BufferUsage::TransferSrc, MemoryUsage::CPUOnly)
        .and_then([&, dstImage, w, h, mipLevels, data, bytes](auto&& staging) -> std::expected<void, ErrorCode> {
            defer _([&] { _allocator->DestroyBuffer(staging); });
            auto mapped = staging.Map(_allocator->Get());
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
        _recorder.Handle(), dstImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels
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
    ZHLN_CmdCopyBufferToImage(_recorder.Handle(), &copy_region);

    if (mipLevels > 1) {
        ZHLN_GenerateMipmaps(_recorder.Handle(), dstImage, w, h, mipLevels, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    } else {
        TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
            _recorder.Handle(), dstImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
        );
    }
}

void StagingContext::UploadPrefilteredCubeMap(VkImage dstImage, VkBuffer stagingBuf, uint32_t baseSize, uint32_t mipLevels) {
    TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(
        _recorder.Handle(), dstImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels
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
            vkCmdCopyBufferToImage2(_recorder.Handle(), &copy_info);
        }
        current_offset += (face_size * 6);
    }

    TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
        _recorder.Handle(), dstImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels
    );
}

void StagingContext::AddBuffer(Buffer&& buf) {
    _stagingBuffers.push_back(std::move(buf));
}

auto StagingContext::ExecuteAsync() -> std::expected<void, ErrorCode> {
    auto executable = std::move(_recorder).End();
    if (!executable) {
        return std::unexpected(executable.error());
    }

    if (_fence != VK_NULL_HANDLE) {
        Wait();
        vkDestroyFence(_ctx->Device(), _fence, nullptr);
        _fence = VK_NULL_HANDLE;
    }

    VkFenceCreateInfo fence_info = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = 0};
    if (vkCreateFence(_ctx->Device(), &fence_info, nullptr, &_fence) != VK_SUCCESS) {
        // Never submit untracked work whose staging buffers could be freed.
        return std::unexpected(VulkanCallError::VulkanCallFailed);
    }

    if (auto result = QueueSubmit(_ctx->GraphicsQueue(), std::move(*executable), {}, {}, _fence); !result) {
        vkQueueWaitIdle(_ctx->GraphicsQueue());
        vkDestroyFence(_ctx->Device(), _fence, nullptr);
        _fence = VK_NULL_HANDLE;
        return std::unexpected(result.error());
    }
    return {};
}

void StagingContext::Wait() noexcept {
    if (_fence != VK_NULL_HANDLE) {
        vkWaitForFences(_ctx->Device(), 1, &_fence, VK_TRUE, UINT64_MAX);
    }
}

}
