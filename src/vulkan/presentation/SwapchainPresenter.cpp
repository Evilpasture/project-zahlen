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
        headlessColorTarget.Destroy(*_alloc);
    }
}

auto SwapchainPresenter::operator=(SwapchainPresenter&& other) noexcept -> SwapchainPresenter& {
    if (this != &other) {
        Cleanup();
        surface             = std::move(other.surface);
        swapchain           = std::move(other.swapchain);
        presentSemaphores   = std::move(other.presentSemaphores);
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
    -> std::expected<void, ErrorCode> {
    _ctx   = &ctx;
    _alloc = &alloc;
    _vsync = vsync;

    _pacer.Resolve(ctx, surface.Get(), vsync);

    sync = FrameSync<kFramesInFlight>::Create(ctx.Device());
    if (!sync.Valid()) {
        return std::unexpected(PresentationError::SyncCreationFailed);
    }
    auto createdPools =
        CommandPools<kFramesInFlight, QueueType::Graphics>::Create(ctx.Device(), {.queueFamily = graphicsFamily, .buffersPerPool = 1});
    if (!createdPools) [[unlikely]] {
        return std::unexpected(createdPools.error());
    }
    pools      = std::move(*createdPools);
    frameIndex = 0;

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
        const VkExtent2D render_extent = {.width = width, .height = height};
        auto hct_res = FormatRenderTarget<kHeadlessColorFormat>::Create(
            *_alloc, _ctx->Device(), render_extent, {.usage = ImageUsage::ColorAttachment | ImageUsage::Sampled | ImageUsage::TransferSrc}
        );
        if (!hct_res) {
            return std::unexpected(hct_res.error());
        }
        headlessColorTarget.Destroy(*_alloc);
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

    ++resourceGeneration;

    return {};
}

auto SwapchainPresenter::AcquireNext(VkExtent2D desiredExtent, bool allowRebuild) noexcept -> PresentationOutcome<SwapchainTarget> {
    if (_ctx == nullptr) {
        return std::unexpected(PresentationError::ContextInvalid);
    }

    const uint32_t slot = frameIndex;

    // 1. Pre-emptive resize if extent changed
    if (allowRebuild && swapchain.Valid() && desiredExtent.width != 0 && desiredExtent.height != 0) {
        const VkExtent2D current = swapchain.Get().extent;
        if (desiredExtent.width != current.width || desiredExtent.height != current.height) {
            if (auto rebuilt = Rebuild(desiredExtent.width, desiredExtent.height); !rebuilt) {
                return std::unexpected(rebuilt.error());
            }
        }
    }

    // 2. Wait for previous work on this frame slot
    if (auto res = sync.WaitSlotIdle(slot); !res) {
        return std::unexpected(res.error());
    }
    if (auto reset = pools[slot].Reset(); !reset) [[unlikely]] {
        return std::unexpected(reset.error());
    }

    // 3. Headless fallback path
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

    // 4. Acquire from swapchain abstraction — no raw Vulkan handles leaked
    auto acquired = swapchain.AcquireNext(sync.ImageAvailableSemaphore(slot));
    if (!acquired) {
        if (acquired.error().Is(SwapchainAcquireError::OutOfDate)) {
            if (desiredExtent.width != 0 && desiredExtent.height != 0) {
                // Properly propagate rebuild failure instead of casting to (void)
                if (auto rebuilt = Rebuild(desiredExtent.width, desiredExtent.height); !rebuilt) {
                    return std::unexpected(rebuilt.error());
                }
            }
            // Return nullopt: skip this frame cleanly so the next iteration starts fresh
            return std::nullopt;
        }
        return std::unexpected(acquired.error());
    }

    if (_pacer.IsTimingActive()) {
        _pacer.Observe(_ctx->Device(), swapchain.Get().handle);
    }

    return SwapchainTarget {
        .image       = acquired->image,
        .imageIndex  = acquired->imageIndex,
        .slot        = slot,
        .generation  = resourceGeneration,
        .presentable = true,
    };
}

void SwapchainPresenter::PreparePresent(CommandRecorder& recorder, uint32_t imageIndex, VkImageLayout currentLayout) const noexcept {
    if (swapchain.Valid() && recorder) {
        ImageBarrier::PreparePresent(swapchain.Get().images[imageIndex], currentLayout).Record(recorder.Handle());
    }
}

auto SwapchainPresenter::Present(
    VkQueue                          graphicsQueue,
    VkQueue                          presentQueue,
    ExecutableCommands               cmds,
    uint32_t                         imageIndex,
    std::span<const SemaphoreSubmit> extraWaits
) noexcept -> PresentationOutcome<PresentSuboptimal> {
    if (!cmds) {
        return std::unexpected(CommandRecordingError::NotExecutable);
    }

    const uint32_t    slot        = frameIndex;
    const VkSemaphore present_sem = swapchain.Valid() ? PresentSemaphore(imageIndex) : VK_NULL_HANDLE;

    // 1. Tell FrameSync to generate the entire submit sync bundle
    auto submitSync = sync.PrepareGraphicsSubmit(slot, present_sem, extraWaits);

    // 2. Submit commands
    auto submitRes = Vk::QueueSubmit(graphicsQueue, std::move(cmds), submitSync.Waits(), submitSync.Signals());
    if (!submitRes) [[unlikely]] {
        return std::unexpected(submitRes.error());
    }

    // 3. Inform FrameSync that submission succeeded
    sync.OnGraphicsSubmitted(slot, submitSync.submissionValue);

    if (!swapchain.Valid()) {
        return {};
    }

    const VkPresentId2KHR*           present_id = nullptr;
    std::optional<TimedPresentChain> timed_chain;
    if (auto prediction = _pacer.Predict()) {
        timed_chain.emplace(*prediction);
        present_id = &timed_chain->presentId;
    }
    const auto present_frame = [&](const VkPresentId2KHR* id) -> PresentationOutcome<PresentSuboptimal> {
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
        return std::unexpected(ToError(result));
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
