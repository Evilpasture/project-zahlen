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

SubmittedStagingWork::SubmittedStagingWork(
    Allocator& allocator, const Context& ctx, CommandPool<QueueType::Graphics>&& pool, std::vector<Buffer>&& buffers, VkFence fence
) noexcept:
    _allocator(&allocator), _ctx(&ctx), _cmdPool(std::move(pool)), _stagingBuffers(std::move(buffers)), _fence(fence) {}

SubmittedStagingWork::SubmittedStagingWork(SubmittedStagingWork&& other) noexcept:
    _allocator(other._allocator), _ctx(other._ctx), _cmdPool(std::move(other._cmdPool)),
    _stagingBuffers(std::move(other._stagingBuffers)), _fence(std::exchange(other._fence, VK_NULL_HANDLE)) {}

SubmittedStagingWork::~SubmittedStagingWork() noexcept {
    Wait();
    for (auto& buffer: _stagingBuffers) {
        _allocator->DestroyBuffer(buffer);
    }
    if (_fence != VK_NULL_HANDLE) {
        vkDestroyFence(_ctx->Device(), _fence, nullptr);
    }
    // _cmdPool is destroyed after this body, when its GPU work has completed.
}

void SubmittedStagingWork::Wait() const noexcept {
    if (_fence != VK_NULL_HANDLE) {
        vkWaitForFences(_ctx->Device(), 1, &_fence, VK_TRUE, UINT64_MAX);
    }
}

StagingContext::StagingContext(
    Allocator& allocator, const Context& ctx, CommandPool<QueueType::Graphics>&& pool, CommandRecorder&& recorder
) noexcept:
    _allocator(&allocator), _ctx(&ctx), _cmdPool(std::move(pool)), _recorder(std::move(recorder)) {}

StagingContext::StagingContext(StagingContext&& other) noexcept:
    _allocator(other._allocator), _ctx(other._ctx), _cmdPool(std::move(other._cmdPool)),
    _recorder(std::move(other._recorder)), _stagingBuffers(std::move(other._stagingBuffers)) {}

StagingContext::~StagingContext() noexcept {
    std::move(*this).Abort();
    for (auto& buffer: _stagingBuffers) {
        _allocator->DestroyBuffer(buffer);
    }
}

void StagingContext::Abort() && noexcept { std::move(_recorder).Abort(); }

auto StagingContext::Begin(Allocator& allocator, const Context& ctx) noexcept -> std::expected<StagingContext, ErrorCode> {
    CommandPool<QueueType::Graphics> pool(ctx.Device(), ctx.PhysicalInfo().graphics_family);
    if (auto allocated = pool.Allocate(1); !allocated) [[unlikely]] {
        return std::unexpected(allocated.error());
    }
    auto recording = CommandRecorder::Begin(pool[0]);
    if (!recording) {
        return std::unexpected(recording.error());
    }
    return StagingContext {allocator, ctx, std::move(pool), std::move(*recording)};
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

auto StagingContext::ExecuteAsync() && -> std::expected<SubmittedStagingWork, ErrorCode> {
    auto executable = std::move(_recorder).End();
    if (!executable) {
        return std::unexpected(executable.error());
    }

    VkFence fence = VK_NULL_HANDLE;
    const VkFenceCreateInfo fenceInfo {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(_ctx->Device(), &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        // Never submit untracked work whose staging buffers could be freed.
        return std::unexpected(VulkanCallError::VulkanCallFailed);
    }

    if (auto result = QueueSubmit(_ctx->GraphicsQueue(), std::move(*executable), {}, {}, fence); !result) {
        // If a submission failed partway through, keep the pool and buffers
        // alive until the queue is idle, before the recording batch is freed.
        vkQueueWaitIdle(_ctx->GraphicsQueue());
        vkDestroyFence(_ctx->Device(), fence, nullptr);
        return std::unexpected(result.error());
    }
    return SubmittedStagingWork {*_allocator, *_ctx, std::move(_cmdPool), std::move(_stagingBuffers), fence};
}

}
