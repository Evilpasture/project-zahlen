// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SwapchainPresenter.hpp"
#include "Rendering.hpp"
#include <array>
#include <cstdint>

namespace ZHLN::Vk {

SwapchainPresenter::~SwapchainPresenter() noexcept {
    Cleanup();
}

void SwapchainPresenter::Cleanup() noexcept {
    if (_alloc != nullptr) {
        depthTarget.Destroy(*_alloc);
        headlessColorTarget.Destroy(*_alloc);
    }
}

auto SwapchainPresenter::operator=(SwapchainPresenter&& other) noexcept -> SwapchainPresenter& {
    if (this != &other) {
        Cleanup();
        surface             = std::move(other.surface);
        swapchain           = std::move(other.swapchain);
        presentSemaphores   = std::move(other.presentSemaphores);
        depthTarget         = std::move(other.depthTarget);
        headlessColorTarget = std::move(other.headlessColorTarget);
        sync                = std::move(other.sync);
        pools               = std::move(other.pools);
        frameIndex          = std::exchange(other.frameIndex, 0);
        resourceGeneration  = std::exchange(other.resourceGeneration, 1);
        _ctx                = std::exchange(other._ctx, nullptr);
        _alloc              = std::exchange(other._alloc, nullptr);
        _vsync              = other._vsync;
        _pacer              = other._pacer;
    }
    return *this;
}

auto SwapchainPresenter::Init(const Context& ctx, Allocator& alloc, uint32_t width, uint32_t height, uint32_t graphicsFamily, bool vsync)
    -> std::expected<void, Vk::Error> {
    _ctx   = &ctx;
    _alloc = &alloc;
    _vsync = vsync;

    _pacer.Resolve(ctx, surface.Get(), vsync);

    sync       = FrameSync<kFramesInFlight>::Create(ctx.Device());
    pools      = CommandPools<kFramesInFlight, QueueType::Graphics>::Create(ctx.Device(), {.queueFamily = graphicsFamily, .buffersPerPool = 1});
    frameIndex = 0;
    if (!sync.Valid() || !pools.Valid()) {
        return std::unexpected(PresentationError::SyncCreationFailed);
    }

    return Rebuild(width, height);
}

auto SwapchainPresenter::Rebuild(uint32_t width, uint32_t height) -> std::expected<void, Vk::Error> {
    if ((_ctx == nullptr) || (_alloc == nullptr)) {
        return std::unexpected(PresentationError::ContextInvalid);
    }

    auto idle_res = Vk::WaitIdle(_ctx->Device());
    if (!idle_res) {
        return std::unexpected(idle_res.error());
    }

    if (surface.Get() == VK_NULL_HANDLE) {
        const VkExtent2D render_extent = {.width = width, .height = height};
        auto             dt_res        = RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>::Create(
            *_alloc, *_ctx, render_extent, {.usage = ImageUsage::DepthStencilAttachment | ImageUsage::Sampled}
        );
        if (!dt_res) {
            return std::unexpected(dt_res.error());
        }
        auto hct_res = RenderTarget<kHeadlessColorFormat>::Create(
            *_alloc, *_ctx, render_extent, {.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled | ImageUsage::TransferSrc}
        );
        if (!hct_res) {
            dt_res->Destroy(*_alloc);
            return std::unexpected(hct_res.error());
        }
        depthTarget.Destroy(*_alloc);
        headlessColorTarget.Destroy(*_alloc);
        depthTarget         = std::move(*dt_res);
        headlessColorTarget = std::move(*hct_res);

        ++resourceGeneration;
        _pacer.OnSwapchainRebuilt(VK_NULL_HANDLE, VK_NULL_HANDLE, 0, VK_PRESENT_MODE_MAX_ENUM_KHR);
        return {};
    }

    const VkExtent2D requested_extent {.width = width, .height = height};
    auto             swapchain_result = swapchain.Rebuild(
        _ctx->Device(), _ctx->PhysicalInfo(), surface.Get(), requested_extent, _vsync, _pacer.RequestedPresentMode(), _pacer.WantsPresentTiming()
    );
    if (!swapchain_result) {
        return std::unexpected(swapchain_result.error());
    }
    _pacer.OnSwapchainRebuilt(_ctx->Device(), swapchain.Get().handle, swapchain.Get().imageCount, swapchain.Get().presentMode);
    presentSemaphores.Rebuild(_ctx->Device(), swapchain.Get().imageCount);

    {
        auto dt_res = RenderTarget<VK_FORMAT_D32_SFLOAT_S8_UINT>::Create(
            *_alloc, *_ctx, swapchain.Get().extent, {.usage = ImageUsage::DepthStencilAttachment | ImageUsage::Sampled}
        );
        if (!dt_res) {
            return std::unexpected(dt_res.error());
        }
        depthTarget.Destroy(*_alloc);
        depthTarget = std::move(*dt_res);
    }

    ++resourceGeneration;

    return {};
}

auto SwapchainPresenter::AcquireNext(VkExtent2D desiredExtent, bool allowRebuild) noexcept -> std::expected<std::optional<SwapchainTarget>, Vk::Error> {
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
        return std::unexpected(Vk::Error {waited});
    }
    sync.MarkUnsubmitted(slot); // The prior submission finished; a skipped acquire does not need a fence.
    pools[slot].Reset();

    if (!swapchain.Valid()) {
        auto& target = headlessColorTarget;
        if (!target.Valid()) {
            return std::unexpected(PresentationError::OffscreenTargetUnavailable);
        }
        return SwapchainTarget {
            .image       = target.AsSlice(),
            .imageIndex  = 0,
            .slot        = slot,
            .generation  = resourceGeneration,
            .presentable = false,
        };
    }

    const auto&    sc          = swapchain.Get();
    uint32_t       image_index = 0;
    const VkResult res         = vkAcquireNextImageKHR(_ctx->Device(), sc.handle, UINT64_MAX, sync.ImageAvailable(slot), VK_NULL_HANDLE, &image_index);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        if (res == VK_ERROR_OUT_OF_DATE_KHR && desiredExtent.width != 0 && desiredExtent.height != 0) {
            (void) Rebuild(desiredExtent.width, desiredExtent.height);
        }
        if (res == VK_ERROR_OUT_OF_DATE_KHR) {
            return std::nullopt;
        }
        return std::unexpected(Vk::Error {res});
    }

    if (_pacer.IsTimingActive()) {
        _pacer.Observe(_ctx->Device(), sc.handle);
    }

    return SwapchainTarget {
        .image       = ImageSlice {sc.images[image_index], sc.views[image_index], sc.extent, sc.format},
        .imageIndex  = image_index,
        .slot        = slot,
        .generation  = resourceGeneration,
        .presentable = true,
    };
}

void SwapchainPresenter::PreparePresent(CommandRecorder& recorder, uint32_t imageIndex, VkImageLayout currentLayout) const noexcept {
    if (swapchain.Valid() && recorder) {
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
        Vk::PipelineBarrier(recorder.Handle(), std::span<const VkBufferMemoryBarrier2> {}, std::span<const VkImageMemoryBarrier2> {&barrier, 1});
    }
}

auto SwapchainPresenter::Present(
    VkQueue                                graphicsQueue,
    VkQueue                                presentQueue,
    ExecutableCommands                     cmds,
    uint32_t                               imageIndex,
    std::span<const VkSemaphoreSubmitInfo> extraWaits
) noexcept -> std::expected<std::optional<PresentSuboptimal>, Vk::Error> {
    if (!cmds) {
        return std::unexpected(CommandRecordingError::NotExecutable);
    }
    const bool     presents = swapchain.Valid();
    const uint32_t slot     = frameIndex;

    std::array<VkSemaphoreSubmitInfo, 4> waits {};
    uint32_t                             wait_count = 0;
    if (presents) {
        waits[wait_count++] = Vk::MakeSemaphoreSubmitInfo(sync.ImageAvailable(slot), 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    }
    for (const VkSemaphoreSubmitInfo& extra: extraWaits) {
        if (wait_count == waits.size()) {
            break;
        }
        waits[wait_count++] = extra;
    }

    const VkSemaphore           present_sem = PresentSemaphore(imageIndex);
    const VkSemaphoreSubmitInfo signal      = Vk::MakeSemaphoreSubmitInfo(present_sem, 0, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);

    if (const VkResult reset = sync.ResetFence(slot); reset != VK_SUCCESS) {
        return std::unexpected(Vk::Error {reset});
    }
    auto submit_res = Vk::QueueSubmit(
        graphicsQueue, std::move(cmds), std::span<const VkSemaphoreSubmitInfo> {waits.data(), wait_count},
        std::span<const VkSemaphoreSubmitInfo> {&signal, presents ? 1U : 0U}, sync.InFlight(slot)
    );
    if (!submit_res) [[unlikely]] {
        return std::unexpected(submit_res.error());
    }
    sync.MarkSubmitted(slot);

    if (!presents) {
        return {};
    }

    const VkPresentId2KHR*           present_id = nullptr;
    std::optional<TimedPresentChain> timed_chain;
    if (auto prediction = _pacer.Predict()) {
        timed_chain.emplace(*prediction);
        present_id = &timed_chain->presentId;
    }
    const auto present_frame = [&](const VkPresentId2KHR* id) -> std::expected<std::optional<PresentSuboptimal>, Vk::Error> {
        const VkPresentInfoKHR info {
            .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext              = id,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores    = &present_sem,
            .swapchainCount     = 1,
            .pSwapchains        = &swapchain.Get().handle,
            .pImageIndices      = &imageIndex,
        };
        const VkResult result = vkQueuePresentKHR(presentQueue, &info);
        if (result == VK_SUCCESS) {
            return {};
        }
        if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
            return PresentSuboptimal {};
        }
        return std::unexpected(Vk::Error {result});
    };
    auto presented = present_frame(present_id);
    if (!presented && present_id != nullptr && presented.error().Is(VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT)) {
        if (_ctx != nullptr) {
            _pacer.Observe(_ctx->Device(), swapchain.Get().handle);
        }
        presented = present_frame(nullptr);
    }
    if (!presented) {
        return std::unexpected(presented.error());
    }
    if (presented->has_value()) {
        return PresentSuboptimal {};
    }
    return {};
}

} // namespace ZHLN::Vk
