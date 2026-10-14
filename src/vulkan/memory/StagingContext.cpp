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
    Allocator&                         allocator,
    const Context&                     ctx,
    CommandPool<QueueType::Graphics>&& pool,
    std::vector<Buffer>                buffers,
    VkSemaphore                        timeline
) noexcept: _allocator(&allocator), _ctx(&ctx), _cmdPool(std::move(pool)), _stagingBuffers(std::move(buffers)), _timeline(timeline) {
}

SubmittedStagingWork::SubmittedStagingWork(SubmittedStagingWork&& other) noexcept:
    _allocator(other._allocator), _ctx(other._ctx), _cmdPool(std::move(other._cmdPool)), _stagingBuffers(std::move(other._stagingBuffers)),
    _timeline(std::exchange(other._timeline, VK_NULL_HANDLE)) {
}

SubmittedStagingWork::~SubmittedStagingWork() noexcept {
    Wait();
    for (auto& buffer: _stagingBuffers) {
        _allocator->DestroyBuffer(buffer);
    }
    if (_timeline != VK_NULL_HANDLE) {
        vkDestroySemaphore(_ctx->Device(), _timeline, nullptr);
    }
}

void SubmittedStagingWork::Wait() const noexcept {
    if (_timeline != VK_NULL_HANDLE) {
        constexpr uint64_t value = 1;
        const VkSemaphoreWaitInfo wait {.sType=VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,.semaphoreCount=1,.pSemaphores=&_timeline,.pValues=&value};
        vkWaitSemaphores(_ctx->Device(), &wait, UINT64_MAX);
    }
}

StagingContext::StagingContext(Allocator& allocator, const Context& ctx, CommandPool<QueueType::Graphics>&& pool, CommandRecorder&& recorder) noexcept:
    _allocator(&allocator), _ctx(&ctx), _cmdPool(std::move(pool)), _recorder(std::move(recorder)) {
}

StagingContext::StagingContext(StagingContext&& other) noexcept:
    _allocator(other._allocator), _ctx(other._ctx), _cmdPool(std::move(other._cmdPool)), _recorder(std::move(other._recorder)),
    _stagingBuffers(std::move(other._stagingBuffers)) {
}

StagingContext::~StagingContext() noexcept {
    std::move(*this).Abort();
    for (auto& buffer: _stagingBuffers) {
        _allocator->DestroyBuffer(buffer);
    }
}

void StagingContext::Abort() && noexcept {
    std::move(_recorder).Abort();
}

auto StagingContext::Begin(Allocator& allocator, const Context& ctx) noexcept -> std::expected<StagingContext, ErrorCode> {
    CommandPool<QueueType::Graphics> pool(ctx.Device(), ctx.PhysicalInfo().graphicsFamily);
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
    return Buffer::Create(*_allocator, bytes, BufferUsage::TransferSrc, MemoryUsage::CPUOnly)
        .and_then([&, dstImage, w, h, mipLevels, data, bytes](auto&& staging) -> std::expected<void, ErrorCode> {
            defer _([&] { _allocator->DestroyBuffer(staging); });
            auto  mapped = staging.Map(*_allocator);
            if (!mapped) {
                return std::unexpected(mapped.error());
            }
            std::memcpy(mapped->Data(), data, bytes);

            UploadImage2DBuffer(dstImage, w, h, mipLevels, staging.Handle(), 0);
            _stagingBuffers.push_back(std::forward<decltype(staging)>(staging));
            return {};
        });
}

void StagingContext::UploadImage2DBuffer(VkImage dstImage, uint32_t w, uint32_t h, uint32_t mipLevels, VkBuffer stagingBuf, VkDeviceSize offset) {
    TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(_recorder.Handle(), dstImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels);

    const VkBufferImageCopy2 region {
        .sType             = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2,
        .bufferOffset      = offset,
        .bufferRowLength   = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .mipLevel       = 0,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        .imageOffset = {},
        .imageExtent = {w, h, 1},
    };
    CopyBufferToImage(_recorder.Handle(), stagingBuf, dstImage, region);

    if (mipLevels > 1) {
        GenerateMipmaps(_recorder.Handle(), dstImage, w, h, mipLevels, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    } else {
        TransitionLayout<VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL>(
            _recorder.Handle(), dstImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1
        );
    }
}

void StagingContext::UploadPrefilteredCubeMap(VkImage dstImage, VkBuffer stagingBuf, uint32_t baseSize, uint32_t mipLevels) {
    TransitionLayout<VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL>(_recorder.Handle(), dstImage, VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels);

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

            CopyBufferToImage(_recorder.Handle(), stagingBuf, dstImage, region);
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

    const VkSemaphoreTypeCreateInfo typeInfo {.sType=VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE,.initialValue=0};
    const VkSemaphoreCreateInfo semaphoreInfo {.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,.pNext=&typeInfo};
    VkSemaphore timeline = VK_NULL_HANDLE;
    if (vkCreateSemaphore(_ctx->Device(), &semaphoreInfo, nullptr, &timeline) != VK_SUCCESS) {
        return std::unexpected(VulkanCallError::VulkanCallFailed);
    }
    const auto signal = MakeSemaphoreSubmitInfo(timeline, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    if (auto result = QueueSubmit(_ctx->GraphicsQueue(), std::move(*executable), {}, std::span<const VkSemaphoreSubmitInfo>{&signal, 1}); !result) {
        vkDestroySemaphore(_ctx->Device(), timeline, nullptr);
        const auto idle = Vk::WaitIdle(_ctx->GraphicsQueue());
        if (!idle) {
            return std::unexpected(idle.error());
        }
        return std::unexpected(result.error());
    }
    return SubmittedStagingWork {*_allocator, *_ctx, std::move(_cmdPool), std::move(_stagingBuffers), timeline};
}

} // namespace ZHLN::Vk
