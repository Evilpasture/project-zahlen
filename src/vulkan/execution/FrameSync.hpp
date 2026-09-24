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

    // Named accessors: the raw ZHLN_FrameSync stays inside this class. Frame
    // orchestration (presentation waits, compute signals) asks for the
    // semaphore it means by name instead of reading the C struct's fields.
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
    /**
     * @brief Blocks the CPU until the target frame's fence is signaled.
     */
    [[nodiscard]] auto Wait(uint32_t frameIndex) const noexcept -> VkResult {
        return vkWaitForFences(_device, 1, &_frames[frameIndex % N].in_flight, VK_TRUE, UINT64_MAX);
    }

    /**
     * @brief Resets the target frame's in-flight fence (reuse before recording).
     */
    void ResetFence(uint32_t frameIndex) const noexcept {
        if (_device != VK_NULL_HANDLE) {
            vkResetFences(_device, 1, &_frames[frameIndex % N].in_flight);
        }
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
};
} // namespace ZHLN::Vk

#include "FrameSync.inl"
