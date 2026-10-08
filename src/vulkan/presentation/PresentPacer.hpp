// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#ifndef ZHLN_RENDERING_HPP_INCLUDED
#error "Please include <src/vulkan/Rendering.hpp> before including any other Zahlen render headers."
#endif

#include <Zahlen/Render/PresentTiming.hpp>
#include <volk.h>
#include <array>
#include <cstdint>
#include <optional>

namespace ZHLN::Vk {

class Context;

struct PresentPrediction {
    uint64_t                    presentId    = 0;
    uint64_t                    targetTime   = 0;
    VkPresentTimingInfoFlagsEXT flags        = 0;
    uint64_t                    timeDomainId = 0;
    VkPresentStageFlagsEXT      stageMask    = 0;
    VkPresentStageFlagsEXT      targetStage  = 0;
};

struct TimedPresentChain {
    uint64_t                idValue   = 0;
    VkPresentTimingInfoEXT  timing    = {};
    VkPresentTimingsInfoEXT timings   = {};
    VkPresentId2KHR         presentId = {};

    explicit TimedPresentChain(const PresentPrediction& pred) noexcept {
        idValue = pred.presentId;
        timing  = {
            .sType                        = VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT,
            .pNext                        = nullptr,
            .flags                        = pred.flags,
            .targetTime                   = pred.targetTime,
            .timeDomainId                 = pred.timeDomainId,
            .presentStageQueries          = pred.stageMask,
            .targetTimeDomainPresentStage = pred.targetStage,
        };
        timings = {
            .sType          = VK_STRUCTURE_TYPE_PRESENT_TIMINGS_INFO_EXT,
            .pNext          = nullptr,
            .swapchainCount = 1,
            .pTimingInfos   = &timing,
        };
        presentId = {
            .sType          = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR,
            .pNext          = &timings,
            .swapchainCount = 1,
            .pPresentIds    = &idValue,
        };
    }

    TimedPresentChain(const TimedPresentChain&)            = delete;
    TimedPresentChain& operator=(const TimedPresentChain&) = delete;
    TimedPresentChain(TimedPresentChain&&)                 = delete;
    TimedPresentChain& operator=(TimedPresentChain&&)      = delete;
};

enum class PresentPacerError : uint8_t {
    TimingInactive ZHLN_ANNOTATION(ZHLN::Description<"Present timing is not active">{}) = 1,
};

class PresentPacer {
  public:
    static constexpr uint32_t kMaxTimings = 16;
    static constexpr uint32_t kMaxStages = 3;

    PresentPacer() noexcept                    = default;
    PresentPacer(const PresentPacer&) noexcept = default;
    PresentPacer& operator=(const PresentPacer&) noexcept = default;
    PresentPacer(PresentPacer&&) noexcept                 = default;
    PresentPacer& operator=(PresentPacer&&) noexcept = default;
    ~PresentPacer()                                  = default;

    void Resolve(const Context& ctx, VkSurfaceKHR surface, bool vsync) noexcept;

    void OnSwapchainRebuilt(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageCount, VkPresentModeKHR actualMode) noexcept;

    void Observe(VkDevice device, VkSwapchainKHR swapchain) noexcept;

    [[nodiscard]] auto Predict() noexcept -> std::expected<PresentPrediction, ErrorCode>;

    [[nodiscard]] auto Policy() const noexcept -> PacingPolicy {
        return _policy;
    }
    [[nodiscard]] auto IsTimingActive() const noexcept -> bool {
        return _timingActive;
    }
    [[nodiscard]] auto RequestedPresentMode() const noexcept -> VkPresentModeKHR;
    [[nodiscard]] auto WantsPresentTiming() const noexcept -> bool {
        return _policy == PacingPolicy::PacedClosedLoop;
    }
    [[nodiscard]] auto Metrics() const noexcept -> PresentTimingMetrics;
    [[nodiscard]] auto PacedDeltaSeconds() const noexcept -> std::optional<float>;

  private:
    void RefreshTimingProperties(VkDevice device, VkSwapchainKHR swapchain) noexcept;
    auto ResolveTimeDomain(VkDevice device, VkSwapchainKHR swapchain) noexcept -> bool;
    void ConsumeResult(const VkPastPresentationTimingEXT& result) noexcept;
    void DowngradeToAdaptive() noexcept;

    PacingPolicy   _policy              = PacingPolicy::LegacyVBlank;
    bool           _sealed              = false;
    bool           _timingActive        = false;
    VkPhysicalDevice _physical          = VK_NULL_HANDLE;
    VkSurfaceKHR     _surface           = VK_NULL_HANDLE;
    VkPresentStageFlagsEXT _stageMask   = 0;
    VkTimeDomainKHR _timeDomain         = VK_TIME_DOMAIN_DEVICE_KHR;
    uint64_t        _timeDomainId       = 0;
    bool                   _stageLocal  = false;
    VkPresentStageFlagsEXT _anchorStage = 0u;
    uint64_t        _timingPropsCounter = 0;
    uint64_t        _timeDomainsCounter = 0;
    uint64_t        _refreshDuration    = 0;
    uint64_t        _refreshInterval    = 0;
    bool            _propsKnown         = false;
    uint64_t        _nextPresentId      = 0;
    bool            _hasBaseline        = false;
    uint64_t        _baselineId         = 0;
    uint64_t        _baselineTime       = 0;
    bool            _hasMargin          = false;
    uint64_t        _lastMarginNs       = 0;
    uint64_t        _lastPresentId      = 0;

    std::array<VkPastPresentationTimingEXT, kMaxTimings>              _results = {};
    std::array<VkPresentStageTimeEXT, kMaxTimings * kMaxStages>       _stages  = {};
};

}
