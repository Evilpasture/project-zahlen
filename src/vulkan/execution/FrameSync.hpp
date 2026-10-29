// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include "../VkError.hpp"
#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace ZHLN::Vk {

struct FrameSyncSlot {
    VkSemaphore imageAvailable   = VK_NULL_HANDLE;
    VkSemaphore computeTimeline  = VK_NULL_HANDLE;
    VkSemaphore graphicsTimeline = VK_NULL_HANDLE;
};

template <uint32_t N>
    requires(N > 0)
class FrameSync {
  public:
    /// Semaphore sync bundle for a graphics queue submission.
    /// 256 bytes: exactly four 64-byte cache lines, no dead Vulkan struct padding.
    struct GraphicsSubmitSync {
        static constexpr size_t kMaxWaits   = 8;
        static constexpr size_t kMaxSignals = 2;

        std::array<SemaphoreSubmit, kMaxWaits>   waitInfos {};   // 192 bytes
        std::array<SemaphoreSubmit, kMaxSignals> signalInfos {}; // 48 bytes
        uint32_t                                 waitCount       = 0;
        uint32_t                                 signalCount     = 0;
        uint64_t                                 submissionValue = 0;

        [[nodiscard]] auto Waits() const noexcept -> std::span<const SemaphoreSubmit> {
            return {waitInfos.data(), waitCount};
        }
        [[nodiscard]] auto Signals() const noexcept -> std::span<const SemaphoreSubmit> {
            return {signalInfos.data(), signalCount};
        }
    };

    static_assert(sizeof(GraphicsSubmitSync) == 256, "GraphicsSubmitSync must fit exactly four cache lines.");

    /// Semaphore sync bundle for an async compute queue submission (32 bytes).
    struct ComputeSubmitSync {
        SemaphoreSubmit signal {}; // 24 bytes
        uint64_t        timelineValue = 0;

        [[nodiscard]] auto Signal() const noexcept -> std::span<const SemaphoreSubmit> {
            return {&signal, 1};
        }
    };

    static_assert(sizeof(ComputeSubmitSync) == 32, "ComputeSubmitSync must be exactly 32 bytes.");

    FrameSync() noexcept = default;
    ~FrameSync() noexcept;

    FrameSync(const FrameSync&)                    = delete;
    auto operator=(const FrameSync&) -> FrameSync& = delete;

    FrameSync(FrameSync&& other) noexcept;
    auto operator=(FrameSync&& other) noexcept -> FrameSync&;

    [[nodiscard("Frame sync creation may fail; verify validity before use in frame loop")]]
    // TODO(Evilpasture): refactor to std::expected
    static auto Create(VkDevice device) noexcept -> FrameSync;

    // Do I even need this?
    [[nodiscard]] static constexpr auto Count() noexcept -> uint32_t {
        return N;
    }
    [[nodiscard]] constexpr auto Valid() const noexcept -> bool {
        return _device != VK_NULL_HANDLE;
    }
    explicit constexpr operator bool() const noexcept {
        return Valid();
    }

    // Waits for all GPU work (graphics and compute) on this slot to complete.
    [[nodiscard]] auto WaitSlotIdle(uint32_t frameIndex) const noexcept -> std::expected<void, ErrorCode> {
        const uint32_t s = frameIndex % N;
        // Wait graphics
        {
            const uint64_t value = _graphicsValuesSubmitted[s];
            if (value != 0) {
                const VkSemaphore         semaphore = _frames[s].graphicsTimeline;
                const VkSemaphoreWaitInfo info {
                    .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                    .semaphoreCount = 1,
                    .pSemaphores    = &semaphore,
                    .pValues        = &value,
                };
                if (const VkResult res = vkWaitSemaphores(_device, &info, UINT64_MAX); res != VK_SUCCESS) {
                    return std::unexpected(ToError(res));
                }
            }
        }
        // Wait compute
        {
            const uint64_t value = _computeValuesSubmitted[s];
            if (value != 0) {
                const VkSemaphore         semaphore = _frames[s].computeTimeline;
                const VkSemaphoreWaitInfo info {
                    .sType          = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                    .semaphoreCount = 1,
                    .pSemaphores    = &semaphore,
                    .pValues        = &value,
                };
                if (const VkResult res = vkWaitSemaphores(_device, &info, UINT64_MAX); res != VK_SUCCESS) {
                    return std::unexpected(ToError(res));
                }
            }
        }
        return {};
    }

    // Semaphore needed specifically by vkAcquireNextImageKHR.
    [[nodiscard]] auto ImageAvailableSemaphore(uint32_t frameIndex) const noexcept -> VkSemaphore {
        return _frames[frameIndex % N].imageAvailable;
    }

    // Prepares all wait and signal semaphores for a graphics queue submission.
    // @param presentWaitSemaphore If non-null, the swapchain present semaphore to signal and the
    //        imageAvailable semaphore to wait on; pass VK_NULL_HANDLE for headless (offscreen) submits.
    [[nodiscard]] auto
        PrepareGraphicsSubmit(uint32_t slot, VkSemaphore presentWaitSemaphore = VK_NULL_HANDLE, std::span<const SemaphoreSubmit> extraWaits = {}) noexcept
        -> GraphicsSubmitSync {
        const uint32_t     s = slot % N;
        GraphicsSubmitSync sync {};

        // 1. Wait on swapchain acquire (imageAvailable) — only when presenting to swapchain
        if (presentWaitSemaphore != VK_NULL_HANDLE && _frames[s].imageAvailable != VK_NULL_HANDLE) {
            sync.waitInfos[sync.waitCount++] = SemaphoreSubmit::Binary(_frames[s].imageAvailable, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
        }

        // 2. Add extra external waits (e.g. async compute, transfer)
        for (const auto& extra: extraWaits) {
            ZHLN::PanicIf(sync.waitCount >= GraphicsSubmitSync::kMaxWaits, "GraphicsSubmitSync wait capacity exceeded");
            if (sync.waitCount < GraphicsSubmitSync::kMaxWaits) {
                sync.waitInfos[sync.waitCount++] = extra;
            }
        }

        // 3. Step timeline and prepare signals
        const uint64_t next_value            = ++_graphicsTimelineValues[s];
        sync.submissionValue                 = next_value;
        sync.signalInfos[sync.signalCount++] = SemaphoreSubmit::Timeline(_frames[s].graphicsTimeline, next_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);

        // 4. Signal present semaphore if swapchain is active
        if (presentWaitSemaphore != VK_NULL_HANDLE) {
            ZHLN::PanicIf(sync.signalCount >= GraphicsSubmitSync::kMaxSignals, "GraphicsSubmitSync signal capacity exceeded");
            if (sync.signalCount < GraphicsSubmitSync::kMaxSignals) {
                sync.signalInfos[sync.signalCount++] = SemaphoreSubmit::Binary(presentWaitSemaphore, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);
            }
        }

        return sync;
    }

    void OnGraphicsSubmitted(uint32_t slot, uint64_t submissionValue) noexcept {
        const uint32_t s            = slot % N;
        _graphicsValuesSubmitted[s] = submissionValue;
    }

    /// Prepares timeline signaling for an async compute submission.
    [[nodiscard]] auto PrepareComputeSubmit(uint32_t slot) noexcept -> ComputeSubmitSync {
        const uint32_t s   = slot % N;
        const uint64_t val = ++_timelineValues[s];
        return ComputeSubmitSync {
            .signal        = SemaphoreSubmit::Timeline(_frames[s].computeTimeline, val, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
            .timelineValue = val,
        };
    }

    void OnComputeSubmitted(uint32_t slot, uint64_t value) noexcept {
        _computeValuesSubmitted[slot % N] = value;
    }

    /// Builds a dependency for a stage waiting on compute simulation.
    [[nodiscard]] auto GetComputeWaitDependency(uint32_t slot, VkPipelineStageFlags2 stage) const noexcept -> std::optional<SemaphoreSubmit> {
        const uint32_t s   = slot % N;
        const uint64_t val = _computeValuesSubmitted[s];
        if (val > 0 && _frames[s].computeTimeline != VK_NULL_HANDLE) {
            return SemaphoreSubmit::Timeline(_frames[s].computeTimeline, val, stage);
        }
        return std::nullopt;
    }

    /// Builds a dependency for a stage waiting on a prior graphics submission.
    [[nodiscard]] auto GetGraphicsWaitDependency(uint32_t slot, VkPipelineStageFlags2 stage) const noexcept -> std::optional<SemaphoreSubmit> {
        const uint32_t s   = slot % N;
        const uint64_t val = _graphicsValuesSubmitted[s];
        if (val > 0 && _frames[s].graphicsTimeline != VK_NULL_HANDLE) {
            return SemaphoreSubmit::Timeline(_frames[s].graphicsTimeline, val, stage);
        }
        return std::nullopt;
    }

  private:
    void Reset() noexcept;

    VkDevice                     _device = VK_NULL_HANDLE;
    std::array<FrameSyncSlot, N> _frames {};
    std::array<uint64_t, N>      _timelineValues {};
    std::array<uint64_t, N>      _computeValuesSubmitted {};
    std::array<uint64_t, N>      _graphicsTimelineValues {};
    std::array<uint64_t, N>      _graphicsValuesSubmitted {};
};

} // namespace ZHLN::Vk

#include "FrameSync.inl"
