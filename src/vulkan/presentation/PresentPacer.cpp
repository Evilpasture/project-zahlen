// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PresentPacer.hpp"
#include "../core/Context.hpp"

namespace ZHLN::Vk {

namespace {

constexpr VkPresentStageFlagsEXT kWantedStages = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT | VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT |
                                                VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
constexpr VkPresentStageFlagsEXT kBaselineStages = VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT | VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;

[[nodiscard]] auto FifoFamily(VkPresentModeKHR mode) noexcept -> bool {
    return mode == VK_PRESENT_MODE_FIFO_KHR || mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR || mode == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
}

[[nodiscard]] auto GloballyComparable(VkTimeDomainKHR domain) noexcept -> bool {
    return domain != VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT;
}

[[nodiscard]] auto SelectSchedulingDomain(
    const VkTimeDomainKHR* domains, const uint64_t* ids, uint32_t count, VkTimeDomainKHR& domain, uint64_t& id
) noexcept -> bool {
    constexpr VkTimeDomainKHR kPreference[] = {
        VK_TIME_DOMAIN_SWAPCHAIN_LOCAL_EXT,
        VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR,
        VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT,
        VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR,
        VK_TIME_DOMAIN_DEVICE_KHR,
    };
    for (VkTimeDomainKHR wanted: kPreference) {
        for (uint32_t i = 0; i < count; ++i) {
            if (domains[i] == wanted) {
                domain = domains[i];
                id     = ids[i];
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] auto SelectStageLocalDomain(
    const VkTimeDomainKHR* domains, const uint64_t* ids, uint32_t count, VkPresentStageFlagsEXT stageMask, VkTimeDomainKHR& domain,
    uint64_t& id, VkPresentStageFlagsEXT& anchor
) noexcept -> bool {
    VkPresentStageFlagsEXT wanted = 0u;
    if ((stageMask & VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT) != 0) {
        wanted = VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT;
    } else if ((stageMask & VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT) != 0) {
        wanted = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
    } else {
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (domains[i] == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT) {
            domain = domains[i];
            id     = ids[i];
            anchor = wanted;
            return true;
        }
    }
    return false;
}

[[nodiscard]] auto StageTime(const VkPastPresentationTimingEXT& result, VkPresentStageFlagsEXT stage) noexcept -> uint64_t {
    for (uint32_t i = 0; i < result.presentStageCount; ++i) {
        if (result.pPresentStages[i].stage == stage) {
            return result.pPresentStages[i].time;
        }
    }
    return 0;
}

}

void PresentPacer::Resolve(const Context& ctx, VkSurfaceKHR surface, bool vsync) noexcept {
    _physical = ctx.Physical();
    _surface  = surface;

    if (!vsync) {
        _policy = PacingPolicy::Decoupled;
        _sealed = true;
        return;
    }
    if (_surface == VK_NULL_HANDLE || _physical == VK_NULL_HANDLE) {
        _policy = PacingPolicy::LegacyVBlank;
        _sealed = true;
        return;
    }

    const DevicePresentSupport& device = ctx.PresentSupport();
    bool                        fifo   = device.fifoLatestReady;
    if (fifo) {
        uint32_t modeCount = 0;
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(_physical, _surface, &modeCount, nullptr) != VK_SUCCESS) {
            fifo = false;
        } else {
            VkPresentModeKHR modes[8] = {};
            uint32_t         queried  = modeCount < 8 ? modeCount : 8;
            fifo                      = false;
            if (vkGetPhysicalDeviceSurfacePresentModesKHR(_physical, _surface, &queried, modes) == VK_SUCCESS) {
                for (uint32_t i = 0; i < queried; ++i) {
                    if (modes[i] == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR) {
                        fifo = true;
                        break;
                    }
                }
            }
        }
    }
    if (!fifo) {
        _policy = PacingPolicy::LegacyVBlank;
        _sealed = true;
        return;
    }

    bool closedLoop = device.presentTiming && device.presentAtAbsoluteTime && device.presentId2;
    if (closedLoop) {
        const VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, .pNext = nullptr, .surface = _surface
        };
        VkPresentTimingSurfaceCapabilitiesEXT timingCaps = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT,
        };
        VkSurfaceCapabilitiesPresentId2KHR id2Caps = {
            .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_PRESENT_ID_2_KHR,
            .pNext = &timingCaps,
        };
        VkSurfaceCapabilities2KHR caps = {
            .sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR,
            .pNext = &id2Caps,
        };
        if (vkGetPhysicalDeviceSurfaceCapabilities2KHR(_physical, &surfaceInfo, &caps) != VK_SUCCESS || timingCaps.presentTimingSupported == VK_FALSE ||
            timingCaps.presentAtAbsoluteTimeSupported == VK_FALSE || id2Caps.presentId2Supported == VK_FALSE) {
            closedLoop = false;
        } else {
            _stageMask = timingCaps.presentStageQueries & kWantedStages;
            if ((_stageMask & kBaselineStages) == 0) {
                closedLoop = false;
            }
        }
    }

    if (closedLoop) {
        _policy = PacingPolicy::PacedClosedLoop;
    } else {
        _policy = PacingPolicy::AdaptiveVBlank;
        _sealed = true;
    }
}

void PresentPacer::OnSwapchainRebuilt(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageCount, VkPresentModeKHR actualMode) noexcept {
    if (_policy != PacingPolicy::PacedClosedLoop) {
        _timingActive = false;
        _sealed       = true;
        return;
    }
    if (device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE) {
        DowngradeToAdaptive();
        return;
    }
    if (!FifoFamily(actualMode)) {
        DowngradeToAdaptive();
        return;
    }

    const uint32_t queueSize = imageCount * 2 < 4 ? 4 : (imageCount * 2 > kMaxTimings ? kMaxTimings : imageCount * 2);
    if (vkSetSwapchainPresentTimingQueueSizeEXT(device, swapchain, queueSize) != VK_SUCCESS) {
        DowngradeToAdaptive();
        return;
    }
    if (!ResolveTimeDomain(device, swapchain)) {
        DowngradeToAdaptive();
        return;
    }
    RefreshTimingProperties(device, swapchain);

    _nextPresentId = 0;
    _hasBaseline   = false;
    _baselineId    = 0;
    _baselineTime  = 0;
    _hasMargin     = false;
    _lastMarginNs  = 0;

    _timingActive = true;
    _sealed       = true;
}

void PresentPacer::Observe(VkDevice device, VkSwapchainKHR swapchain) noexcept {
    if (!_timingActive || device == VK_NULL_HANDLE || swapchain == VK_NULL_HANDLE) {
        return;
    }

    for (uint32_t pass = 0; pass < 2; ++pass) {
        for (uint32_t i = 0; i < kMaxTimings; ++i) {
            _results[i] = {
                .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_EXT,
                .pNext = nullptr,
                .presentId = 0,
                .targetTime = 0,
                .presentStageCount = kMaxStages,
                .pPresentStages = &_stages[i * kMaxStages],
            };
        }
        VkPastPresentationTimingInfoEXT info = {
            .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_INFO_EXT,
            .swapchain = swapchain,
        };
        VkPastPresentationTimingPropertiesEXT props = {
            .sType = VK_STRUCTURE_TYPE_PAST_PRESENTATION_TIMING_PROPERTIES_EXT,
            .presentationTimingCount = kMaxTimings,
            .pPresentationTimings = _results.data(),
        };
        const VkResult result         = vkGetPastPresentationTimingEXT(device, &info, &props);
        if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
            break;
        }
        if (props.timingPropertiesCounter != _timingPropsCounter) {
            RefreshTimingProperties(device, swapchain);
        }
        if (props.timeDomainsCounter != _timeDomainsCounter) {
            ResolveTimeDomain(device, swapchain);
        }
        for (uint32_t i = 0; i < props.presentationTimingCount; ++i) {
            ConsumeResult(_results[i]);
        }
        if (result == VK_SUCCESS) {
            break;
        }
    }

    if (!_propsKnown) {
        RefreshTimingProperties(device, swapchain);
    }
}

auto PresentPacer::Predict() noexcept -> std::expected<PresentPrediction, Vk::Error> {
    if (!_timingActive) {
        return std::unexpected(PresentPacerError::TimingInactive);
    }

    const uint64_t id     = ++_nextPresentId;
    uint64_t       target = 0;
    if (_hasBaseline && _propsKnown && id > _baselineId) {
        const uint64_t ipd = _refreshInterval != 0 ? _refreshInterval : _refreshDuration;
        target             = _baselineTime + (id - _baselineId) * ipd;
        if (target < _baselineTime) {
            target = 0;
        }
    }
    const VkPresentTimingInfoFlagsEXT flags =
        target != 0 ? static_cast<VkPresentTimingInfoFlagsEXT>(VK_PRESENT_TIMING_INFO_PRESENT_AT_NEAREST_REFRESH_CYCLE_BIT_EXT)
                    : static_cast<VkPresentTimingInfoFlagsEXT>(0);

    return PresentPrediction {
        .presentId    = id,
        .targetTime   = target,
        .flags        = flags,
        .timeDomainId = _timeDomainId,
        .stageMask    = _stageMask,
        .targetStage  = _stageLocal ? _anchorStage : 0u,
    };
}

auto PresentPacer::RequestedPresentMode() const noexcept -> VkPresentModeKHR {
    switch (_policy) {
        case PacingPolicy::PacedClosedLoop:
        case PacingPolicy::AdaptiveVBlank:  return VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
        case PacingPolicy::Decoupled:       return VK_PRESENT_MODE_IMMEDIATE_KHR;
        case PacingPolicy::LegacyVBlank:    return VK_PRESENT_MODE_MAX_ENUM_KHR;
    }
    return VK_PRESENT_MODE_MAX_ENUM_KHR;
}

auto PresentPacer::Metrics() const noexcept -> PresentTimingMetrics {
    PresentTimingMetrics metrics;
    metrics.policy       = _policy;
    metrics.lastPresentId = _lastPresentId;
    if (!_sealed || _policy != PacingPolicy::PacedClosedLoop || !_timingActive || !_propsKnown || _refreshDuration == 0) {
        return metrics;
    }
    if (_refreshInterval == UINT64_MAX) {
        metrics.variableRefresh = true;
        return metrics;
    }
    metrics.hasPacedTiming    = true;
    metrics.refreshIntervalNs = _refreshInterval != 0 ? _refreshInterval : _refreshDuration;
    metrics.hasMargin         = _hasMargin;
    metrics.lastPresentMarginNs = _lastMarginNs;
    return metrics;
}

auto PresentPacer::PacedDeltaSeconds() const noexcept -> std::optional<float> {
    const PresentTimingMetrics metrics = Metrics();
    if (!metrics.hasPacedTiming) {
        return std::nullopt;
    }
    return static_cast<float>(metrics.refreshIntervalNs) / 1000000000.0F;
}

void PresentPacer::RefreshTimingProperties(VkDevice device, VkSwapchainKHR swapchain) noexcept {
    VkSwapchainTimingPropertiesEXT props = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIMING_PROPERTIES_EXT,
    };
    uint64_t counter = _timingPropsCounter;
    if (vkGetSwapchainTimingPropertiesEXT(device, swapchain, &props, &counter) != VK_SUCCESS) {
        return;
    }
    _refreshDuration    = props.refreshDuration;
    _refreshInterval    = props.refreshInterval;
    _timingPropsCounter = counter;
    _propsKnown         = true;
}

auto PresentPacer::ResolveTimeDomain(VkDevice device, VkSwapchainKHR swapchain) noexcept -> bool {
    uint64_t                           counter = _timeDomainsCounter;
    VkSwapchainTimeDomainPropertiesEXT props = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT,
    };
    const VkResult countResult = vkGetSwapchainTimeDomainPropertiesEXT(device, swapchain, &props, &counter);
    if (countResult != VK_SUCCESS) {
        return false;
    }
    VkTimeDomainKHR domains[8] = {};
    uint64_t        ids[8]     = {};
    props.timeDomainCount      = props.timeDomainCount < 8 ? props.timeDomainCount : 8;
    props.pTimeDomains         = domains;
    props.pTimeDomainIds       = ids;
    const VkResult listResult = vkGetSwapchainTimeDomainPropertiesEXT(device, swapchain, &props, &counter);
    if (listResult != VK_SUCCESS) {
        return false;
    }
    VkTimeDomainKHR        domain     = VK_TIME_DOMAIN_DEVICE_KHR;
    uint64_t               id         = 0;
    bool                   stageLocal = false;
    VkPresentStageFlagsEXT anchor     = 0u;
    if (!SelectSchedulingDomain(domains, ids, props.timeDomainCount, domain, id) &&
        !SelectStageLocalDomain(domains, ids, props.timeDomainCount, _stageMask, domain, id, anchor)) {
        return false;
    }
    stageLocal = (domain == VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT);
    if (domain != _timeDomain || id != _timeDomainId || stageLocal != _stageLocal || anchor != _anchorStage) {
        _timeDomain   = domain;
        _timeDomainId = id;
        _stageLocal   = stageLocal;
        _anchorStage  = anchor;
        _hasBaseline  = false;
        _baselineId   = 0;
        _baselineTime = 0;
    }
    _timeDomainsCounter = counter;
    return true;
}

void PresentPacer::ConsumeResult(const VkPastPresentationTimingEXT& result) noexcept {
    if (result.reportComplete == VK_FALSE) {
        return;
    }
    if (result.presentId > _lastPresentId) {
        _lastPresentId = result.presentId;
    }
    if (result.timeDomain != _timeDomain || result.timeDomainId != _timeDomainId) {
        if (!_stageLocal && GloballyComparable(result.timeDomain)) {
            _timeDomain   = result.timeDomain;
            _timeDomainId = result.timeDomainId;
            _hasBaseline  = false;
            _baselineId   = 0;
            _baselineTime = 0;
        } else if (_stageLocal) {
            _hasBaseline  = false;
            _baselineId   = 0;
            _baselineTime = 0;
        }
        return;
    }

    const uint64_t firstPixelOut = StageTime(result, VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
    const uint64_t dequeued      = StageTime(result, VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT);
    const uint64_t queueEnd      = StageTime(result, VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT);

    if (result.presentId != 0) {
        uint64_t stamp = 0;
        if (_stageLocal) {
            stamp = StageTime(result, _anchorStage);
        } else if (firstPixelOut != 0) {
            stamp = firstPixelOut;
        } else {
            stamp = dequeued;
        }
        if (stamp != 0) {
            _baselineId   = result.presentId;
            _baselineTime = stamp;
            _hasBaseline  = true;
        }
    }
    if (!_stageLocal && queueEnd != 0 && dequeued != 0 && dequeued >= queueEnd) {
        _lastMarginNs = dequeued - queueEnd;
        _hasMargin    = true;
    }
}

void PresentPacer::DowngradeToAdaptive() noexcept {
    _timingActive = false;
    if (_sealed) {
        return;
    }
    _sealed = true;
    _policy = PacingPolicy::AdaptiveVBlank;
}

}
