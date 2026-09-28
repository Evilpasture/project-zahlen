// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#include "Rendering.hpp"
#include "SwapchainPresenter.hpp"

#include <array>
#include <cstdint>

namespace ZHLN::Vk {


auto SwapchainPresenter::Init(const Context& ctx, Allocator& alloc, uint32_t width, uint32_t height, uint32_t graphicsFamily, bool vsync)
    -> std::expected<void, ErrorCode> {
    _ctx   = &ctx;
    _alloc = &alloc;
    _vsync = vsync;

    _pacer.Resolve(ctx, surface.Get(), vsync);

    sync  = FrameSync<kFramesInFlight>::Create(ctx.Device());
    pools = CommandPools<kFramesInFlight, QueueType::Graphics>::Create(ctx.Device(), {.queueFamily = graphicsFamily, .buffersPerPool = 1});
    frameIndex = 0;
    if (!sync.Valid() || !pools.Valid()) {
        return std::unexpected(PresentationError::SyncCreationFailed);
    }

    return Rebuild(width, height);
}

auto SwapchainPresenter::Rebuild(uint32_t width, uint32_t height) -> std::expected<void, ErrorCode> {
    if ((_ctx == nullptr) || (_alloc == nullptr)) {
        return std::unexpected(PresentationError::ContextInvalid);
    }

    auto idle_res = Vk::WaitIdle(_ctx->Device());
    if (!idle_res) {
        return std::unexpected(idle_res.error());
    }

    if (surface.Get() == VK_NULL_HANDLE) {
        const VkExtent2D renderExtent = {.width = width, .height = height};
        {
            auto dt_res = RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>::Create(
                *_alloc, *_ctx, renderExtent, {.usage = ImageUsage::DepthStencilAttachment | ImageUsage::Sampled}
            );
            if (!dt_res) {
                return std::unexpected(dt_res.error());
            }
            depthTarget = std::move(*dt_res);
        }

        {
            auto hct_res = RenderTarget<kHeadlessColorFormat>::Create(
                *_alloc, *_ctx, renderExtent, {.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled | ImageUsage::TransferSrc}
            );
            if (!hct_res) {
                return std::unexpected(hct_res.error());
            }
            headlessColorTarget = std::move(*hct_res);
        }

        ++resourceGeneration;
        _pacer.OnSwapchainRebuilt(VK_NULL_HANDLE, VK_NULL_HANDLE, 0, VK_PRESENT_MODE_MAX_ENUM_KHR);
        return {};
    }

    const ZHLN_Device raw_dev = {
        .handle         = _ctx->Device(),
        .graphics_queue = _ctx->GraphicsQueue(),
        .present_queue  = _ctx->PresentQueue(),
        .transfer_queue = _ctx->TransferQueue(),
        .compute_queue  = _ctx->ComputeQueue()
    };
    const ZHLN_PhysicalDeviceInfo raw_phys = _ctx->PhysicalInfo();
    ZHLN_SwapchainDesc            s_desc   = {
        .device                = &raw_dev,
        .physical              = &raw_phys,
        .surface               = surface.Get(),
        .width                 = width,
        .height                = height,
        .vsync                 = _vsync,
        .present_mode          = _pacer.RequestedPresentMode(),
        .enable_present_timing = _pacer.WantsPresentTiming(),
        .old_swapchain         = swapchain.Get().handle,
    };

    if (!swapchain.Rebuild(s_desc)) {
        return std::unexpected(PresentationError::SwapchainCreationFailed);
    }
    _pacer.OnSwapchainRebuilt(_ctx->Device(), swapchain.Get().handle, swapchain.Get().image_count, swapchain.Get().present_mode);
    presentSemaphores.Rebuild(_ctx->Device(), swapchain.Get().image_count);

    {
        auto dt_res = RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>::Create(
            *_alloc, *_ctx, swapchain.Get().extent, {.usage = ImageUsage::DepthStencilAttachment | ImageUsage::Sampled}
        );
        if (!dt_res) {
            return std::unexpected(dt_res.error());
        }
        depthTarget = std::move(*dt_res);
    }

    ++resourceGeneration;

    return {};
}


auto SwapchainPresenter::AcquireNext(VkExtent2D desiredExtent, bool allowRebuild) noexcept -> FrameOutcome<SwapchainTarget> {
    if (_ctx == nullptr) {
        return std::unexpected(PresentationError::ContextInvalid);
    }

    const uint32_t slot = frameIndex;

    if (allowRebuild && swapchain.Valid() && desiredExtent.width != 0 && desiredExtent.height != 0) {
        const VkExtent2D current = swapchain.Get().extent;
        if (desiredExtent.width != current.width || desiredExtent.height != current.height) {
            if (auto rebuilt = Rebuild(desiredExtent.width, desiredExtent.height); !rebuilt) {
                return std::unexpected(rebuilt.error());
            }
        }
    }

    if (const VkResult waited = sync.Wait(slot); waited != VK_SUCCESS) {
        return std::unexpected(ToFrameError(waited));
    }
    sync.ResetFence(slot);
    pools[slot].Reset();

    if (!swapchain.Valid()) {
        auto& target = headlessColorTarget;
        if (!target.Valid()) {
            return std::unexpected(PresentationError::OffscreenTargetUnavailable);
        }
        return SwapchainTarget {
            .image       = MakeSlice(target.image.Handle(), target.view.Get(), target.extent, kHeadlessColorFormat),
            .imageIndex  = 0,
            .slot        = slot,
            .generation  = resourceGeneration,
            .presentable = false,
        };
    }

    const auto& sc = swapchain.Get();
    uint32_t    imageIndex = 0;
    ZHLN_AcquireDesc acquire {
        .swapchain       = sc.handle,
        .image_available = sync[slot].image_available,
        .timeout_ns      = UINT64_MAX,
    };
    const VkResult res = ZHLN_AcquireImage(_ctx->Device(), &acquire, &imageIndex);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        if (res == VK_ERROR_OUT_OF_DATE_KHR && desiredExtent.width != 0 && desiredExtent.height != 0) {
            (void)Rebuild(desiredExtent.width, desiredExtent.height);
        }
        if (res == VK_ERROR_OUT_OF_DATE_KHR) {
            return std::nullopt;
        }
        return std::unexpected(ToFrameError(res));
    }

    if (_pacer.IsTimingActive()) {
        _pacer.Observe(_ctx->Device(), sc.handle);
    }

    return SwapchainTarget {
        .image       = MakeSlice(sc.images[imageIndex], sc.views[imageIndex], sc.extent, sc.format),
        .imageIndex  = imageIndex,
        .slot        = slot,
        .generation  = resourceGeneration,
        .presentable = true,
    };
}


auto SwapchainPresenter::Present(
    VkQueue graphicsQueue, VkQueue presentQueue, VkCommandBuffer cmd, uint32_t imageIndex, VkImageLayout currentLayout,
    std::span<const VkSemaphoreSubmitInfo> extraWaits
) noexcept -> FrameOutcome<PresentSuboptimal> {
    const bool     presents = swapchain.Valid();
    const uint32_t slot     = frameIndex;

    if (presents && cmd != VK_NULL_HANDLE) {
        const VkImageMemoryBarrier2 barrier = Vk::MakeImageBarrier({
            .image      = swapchain.Get().images[imageIndex],
            .src_access = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
            .dst_access = 0,
            .src_layout = currentLayout,
            .dst_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .src_stage  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dst_stage  = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
            .aspect     = VK_IMAGE_ASPECT_COLOR_BIT,
            .base_mip   = 0,
            .mip_count  = VK_REMAINING_MIP_LEVELS,
        });
        Vk::PipelineBarrier(cmd, std::span<const VkBufferMemoryBarrier2> {}, std::span<const VkImageMemoryBarrier2> {&barrier, 1});
    }

    if (cmd != VK_NULL_HANDLE) {
        ZHLN_EndCommandBuffer(cmd);
    }

    const ZHLN_FrameSync& frameSync = sync[slot];

    std::array<VkSemaphoreSubmitInfo, 4> waits {};
    uint32_t                             waitCount = 0;
    if (presents) {
        waits[waitCount++] = Vk::MakeSemaphoreSubmitInfo(frameSync.image_available, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    for (const VkSemaphoreSubmitInfo& extra: extraWaits) {
        if (waitCount == waits.size()) {
            break;
        }
        waits[waitCount++] = extra;
    }

    const VkSemaphore            presentSem = PresentSemaphore(imageIndex);
    const VkSemaphoreSubmitInfo  signal     = Vk::MakeSemaphoreSubmitInfo(presentSem, 0, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);
    const VkCommandBufferSubmitInfo cmdInfo = Vk::MakeCommandBufferSubmitInfo(cmd);

    auto submitRes = Vk::QueueSubmit(
        graphicsQueue, std::span<const VkCommandBufferSubmitInfo> {&cmdInfo, 1}, std::span<const VkSemaphoreSubmitInfo> {waits.data(), waitCount},
        std::span<const VkSemaphoreSubmitInfo> {&signal, presents ? 1u : 0u}, frameSync.in_flight
    );
    if (!submitRes) [[unlikely]] {
        return std::unexpected(submitRes.error());
    }

    if (!presents) {
        return {};
    }

    const VkPresentId2KHR*           presentId  = nullptr;
    std::optional<TimedPresentChain> timedChain;
    if (auto prediction = _pacer.Predict()) {
        timedChain.emplace(*prediction);
        presentId = &timedChain->presentId;
    }
    const ZHLN_PresentDesc present {
        .present_queue   = presentQueue,
        .swapchain       = swapchain.Get().handle,
        .render_finished = presentSem,
        .image_index     = imageIndex,
        .present_id      = presentId,
    };
    auto presented = Vk::PresentFrame(present);
    if (!presented && presentId != nullptr && presented.error().Is(VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT)) {
        if (_ctx != nullptr) {
            _pacer.Observe(_ctx->Device(), swapchain.Get().handle);
        }
        const ZHLN_PresentDesc retry {
            .present_queue   = presentQueue,
            .swapchain       = swapchain.Get().handle,
            .render_finished = presentSem,
            .image_index     = imageIndex,
            .present_id      = nullptr,
        };
        presented = Vk::PresentFrame(retry);
    }
    if (!presented) {
        return std::unexpected(presented.error());
    } else if (presented->has_value()) {
        return PresentSuboptimal {};
    }
    return {};
}

}
