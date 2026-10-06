// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <cstdint>

namespace ZHLN::Vk {

struct FrameSyncSlot {
    VkSemaphore imageAvailable = VK_NULL_HANDLE;
    VkSemaphore renderFinished = VK_NULL_HANDLE;
    VkSemaphore computeTimeline = VK_NULL_HANDLE;
    VkFence     inFlight = VK_NULL_HANDLE;
};

template <uint32_t N>
    requires(N > 0)
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

    [[nodiscard]] constexpr auto operator[](uint32_t frame) const noexcept -> const FrameSyncSlot& {
        return _frames[frame % N];
    }
    [[nodiscard]] constexpr auto ComputeTimeline(uint32_t frame) const noexcept -> VkSemaphore {
        return _frames[frame % N].computeTimeline;
    }
    [[nodiscard]] constexpr auto ImageAvailable(uint32_t frame) const noexcept -> VkSemaphore {
        return _frames[frame % N].imageAvailable;
    }
    [[nodiscard]] constexpr auto RenderFinished(uint32_t frame) const noexcept -> VkSemaphore {
        return _frames[frame % N].renderFinished;
    }
    [[nodiscard]] constexpr auto InFlight(uint32_t frame) const noexcept -> VkFence {
        return _frames[frame % N].inFlight;
    }
    [[nodiscard]] static constexpr auto Count() noexcept -> uint32_t {
        return N;
    }
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _device != VK_NULL_HANDLE;
    }
    explicit constexpr operator bool() const noexcept {
        return Valid();
    }

    [[nodiscard]] auto Wait(uint32_t frameIndex) const noexcept -> VkResult {
        const uint32_t slot = frameIndex % N;
        if (!_submitted[slot]) {
            return VK_SUCCESS;
        }
        return vkWaitForFences(_device, 1, &_frames[slot].inFlight, VK_TRUE, UINT64_MAX);
    }
    [[nodiscard]] auto ResetFence(uint32_t frameIndex) const noexcept -> VkResult {
        return vkResetFences(_device, 1, &_frames[frameIndex % N].inFlight);
    }
    void MarkUnsubmitted(uint32_t frameIndex) noexcept {
        _submitted[frameIndex % N] = false;
    }
    void MarkSubmitted(uint32_t frameIndex) noexcept {
        _submitted[frameIndex % N] = true;
    }

    void MarkComputeSubmitted(uint32_t frameIndex) noexcept {
        _computeValuesSubmitted[frameIndex % N] = _timelineValues[frameIndex % N];
    }
    [[nodiscard]] auto WaitCompute(uint32_t frameIndex) const noexcept -> VkResult {
        const uint32_t slot = frameIndex % N;
        const uint64_t value = _computeValuesSubmitted[slot];
        if (value == 0) {
            return VK_SUCCESS;
        }
        const VkSemaphore semaphore = _frames[slot].computeTimeline;
        const VkSemaphoreWaitInfo info {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &semaphore,
            .pValues = &value,
        };
        return vkWaitSemaphores(_device, &info, UINT64_MAX);
    }
    [[nodiscard]] auto StepTimeline(uint32_t frameIndex) noexcept -> uint64_t {
        return ++_timelineValues[frameIndex % N];
    }
    [[nodiscard]] auto GetTimelineValue(uint32_t frameIndex) const noexcept -> uint64_t {
        return _timelineValues[frameIndex % N];
    }

  private:
    void Reset() noexcept;

    VkDevice                _device = VK_NULL_HANDLE;
    std::array<FrameSyncSlot, N> _frames {};
    std::array<uint64_t, N>      _timelineValues {};
    std::array<uint64_t, N>      _computeValuesSubmitted {};
    std::array<bool, N>          _submitted {};
};

} // namespace ZHLN::Vk

#include "FrameSync.inl"
