// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace ZHLN::Vk {

template <uint32_t N>
    requires(N > 0 && N <= 8)
class FrameSync {
  public:
    FrameSync() noexcept = default;
    ~FrameSync() noexcept;

    FrameSync(const FrameSync&)                    = delete;
    auto operator=(const FrameSync&) -> FrameSync& = delete;

    FrameSync(FrameSync&& other) noexcept;
    auto operator=(FrameSync&& other) noexcept -> FrameSync&;

    [[nodiscard("Frame sync creation may fail; verify validity before use in frame loop")]]
    static auto Create(VkDevice device) noexcept -> FrameSync;

    [[nodiscard]] constexpr auto operator[](const uint32_t frame) const noexcept -> const ZHLN_FrameSync& {
        return _frames[frame % N];
    }

    [[nodiscard]] constexpr auto ComputeTimeline(const uint32_t frame) const noexcept -> VkSemaphore {
        return _frames[frame % N].compute_timeline;
    }
    [[nodiscard]] constexpr auto ImageAvailable(const uint32_t frame) const noexcept -> VkSemaphore {
        return _frames[frame % N].image_available;
    }
    [[nodiscard]] constexpr auto RenderFinished(const uint32_t frame) const noexcept -> VkSemaphore {
        return _frames[frame % N].render_finished;
    }
    [[nodiscard]] static constexpr auto Count() noexcept -> uint32_t {
        return N;
    }
    [[nodiscard("Check FrameSync validity before use in frame loop")]]
    constexpr auto Valid() const noexcept -> bool {
        return _device != VK_NULL_HANDLE;
    }
    [[nodiscard]] auto Wait(uint32_t frameIndex) const noexcept -> VkResult {
        if (!_submitted[frameIndex % N]) { return VK_SUCCESS; }
        return vkWaitForFences(_device, 1, &_frames[frameIndex % N].in_flight, VK_TRUE, UINT64_MAX);
    }

    // Acquiring an image or beginning a command buffer can fail; only reset the
    // fence when there actually is an executable buffer ready to submit.
    [[nodiscard]] auto ResetFence(uint32_t frameIndex) const noexcept -> VkResult {
        return vkResetFences(_device, 1, &_frames[frameIndex % N].in_flight);
    }
    void MarkUnsubmitted(uint32_t frameIndex) noexcept { _submitted[frameIndex % N] = false; }
    void MarkSubmitted(uint32_t frameIndex) noexcept { _submitted[frameIndex % N] = true; }

    // Compute can be submitted even when no window is acquired. Wait for its
    // timeline separately at frame-slot reuse, not inside AcquireNext (which
    // must NOT serialize the current frame's asynchronous compute).
    void MarkComputeSubmitted(uint32_t frameIndex) noexcept { _computeValuesSubmitted[frameIndex % N] = _timelineValues[frameIndex % N]; }
    [[nodiscard]] auto WaitCompute(uint32_t frameIndex) const noexcept -> VkResult {
        const uint32_t slot = frameIndex % N;
        const uint64_t value = _computeValuesSubmitted[slot];
        if (value == 0) { return VK_SUCCESS; }
        const VkSemaphore semaphore = _frames[slot].compute_timeline;
        const VkSemaphoreWaitInfo info {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &semaphore,
            .pValues = &value,
        };
        return vkWaitSemaphores(_device, &info, UINT64_MAX);
    }

    uint64_t StepTimeline(uint32_t frameIndex) noexcept {
        return ++_timelineValues[frameIndex % N];
    }

    [[nodiscard]] uint64_t GetTimelineValue(uint32_t frameIndex) const noexcept {
        return _timelineValues[frameIndex % N];
    }

  private:
    VkDevice                      _device = VK_NULL_HANDLE;
    std::array<ZHLN_FrameSync, N> _frames {};
    std::array<uint64_t, N>       _timelineValues {};
    std::array<uint64_t, N>       _computeValuesSubmitted {};
    std::array<bool, N>           _submitted {};
};
}

#include "FrameSync.inl"
