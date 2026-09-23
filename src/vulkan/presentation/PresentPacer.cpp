// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/presentation/PresentPacer.cpp
//
// The closed-loop presentation pacer; see PresentPacer.hpp for the policy
// ladder and the scheduling formula. Everything Vulkan about display timing
// lives in this file: capability probes, the past-timing drain, target
// computation. Callers only ever see PacingPolicy and PresentTimingMetrics.
#include "PresentPacer.hpp"
#include "../core/Context.hpp"

namespace ZHLN::Vk {

namespace {

// The stages the closed loop wants: queue-operations-end (the frame finished
// on the GPU), request-dequeued (the V-blank the request waited for), and
// first-pixel-out (pixels leaving for the display). Margin is dequeued minus
// queue-end; the scheduling baseline prefers first-pixel-out, else dequeued.
constexpr VkPresentStageFlagsEXT kWantedStages = VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT | VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT |
                                                VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
// Scheduling needs a cross-stage comparable stage as its baseline anchor: the
// pixel stage or the dequeue event. Queue-operations-end alone feeds the
// margin metric but cannot anchor a target.
constexpr VkPresentStageFlagsEXT kBaselineStages = VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT | VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;

[[nodiscard]] auto FifoFamily(VkPresentModeKHR mode) noexcept -> bool {
    return mode == VK_PRESENT_MODE_FIFO_KHR || mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR || mode == VK_PRESENT_MODE_FIFO_LATEST_READY_KHR;
}

// A global domain is comparable across stages and across presents, so results
// reported in one are always adoptable (the upgrade path in ConsumeResult).
// The stage-local domain is only self-consistent within its anchor stage --
// schedulable, but never a baseline donor for another timeline. (Domains are
// never mutually comparable, so adopting a new one always restarts the
// baseline -- see ConsumeResult.)
[[nodiscard]] auto GloballyComparable(VkTimeDomainKHR domain) noexcept -> bool {
    return domain != VK_TIME_DOMAIN_PRESENT_STAGE_LOCAL_EXT;
}

// Picks the scheduling domain from an advertised pair: swapchain-local first
// (shared across every queue and stage of this swapchain by construction),
// then the host/device globals. Stage-local is never eligible.
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

// Last-resort scheduling domain: the swapchain's stage-local clock, anchored
// to the dequeue event (else first-pixel-out). Resolve only admits the closed
// loop when the stage mask holds one of the two, so a false answer here means
// the swapchain did not advertise the stage-local domain at all.
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

} // namespace

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

    // Device-side enablement first: without the fifo-latest-ready feature
    // there is no paced policy at all, so the surface probes below are moot.
    const DevicePresentSupport& device = ctx.PresentSupport();
    bool                        fifo   = device.fifoLatestReady;
    if (fifo) {
        uint32_t modeCount = 0;
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(_physical, _surface, &modeCount, nullptr) != VK_SUCCESS) {
            fifo = false;
        } else {
            // Present modes are a handful; two calls bound the query.
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

    // Latest-ready is available; the closed loop additionally needs the
    // timing feature group and the surface-side caps.
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
        // Provisional: TIMING_BIT goes into the swapchain description, and the
        // first OnSwapchainRebuilt seals or downgrades. Not sealed yet.
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
    // Non-zero target timestamps are only legal on FIFO-family modes; a
    // fallback away from latest-ready (surface declined the request) ends the
    // closed loop before it schedules anything illegal.
    if (!FifoFamily(actualMode)) {
        DowngradeToAdaptive();
        return;
    }

    // Size the timing queue past the in-flight presents so feedback is never
    // dropped faster than Observe drains it; clamp to the drain capacity.
    const uint32_t queueSize = imageCount * 2 < 4 ? 4 : (imageCount * 2 > kMaxTimings ? kMaxTimings : imageCount * 2);
    if (vkSetSwapchainPresentTimingQueueSizeEXT(device, swapchain, queueSize) != VK_SUCCESS) {
        DowngradeToAdaptive();
        return;
    }
    if (!ResolveTimeDomain(device, swapchain)) {
        DowngradeToAdaptive();
        return;
    }
    // Timing properties may legitimately lag the first rebuilds (the
    // presentation engine reports them once presents flow); Observe retries
    // until they arrive, and presents go out untimed meanwhile.
    RefreshTimingProperties(device, swapchain);

    // A new swapchain is a new timing queue: ids restart at 1 and the old
    // baseline (another queue's timestamps, another display's cadence) is
    // dropped. Presents go out untimed until feedback re-anchors them.
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

    // Two drain passes bound the work per acquire while still freeing a queue
    // that overflowed the first pass (VK_INCOMPLETE re-reports until empty).
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
        // The counters the drain reports subsume re-querying them separately:
        // a moved timing-properties counter refreshes the cadence, a moved
        // time-domains counter re-resolves the scheduling domain.
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

auto PresentPacer::Predict() noexcept -> std::expected<PresentPrediction, ErrorCode> {
    if (!_timingActive) {
        return std::unexpected(PresentPacerError::TimingInactive);
    }

    const uint64_t id     = ++_nextPresentId;
    uint64_t       target = 0;
    if (_hasBaseline && _propsKnown && id > _baselineId) {
        const uint64_t ipd = _refreshInterval != 0 ? _refreshInterval : _refreshDuration;
        target             = _baselineTime + (id - _baselineId) * ipd;
        if (target < _baselineTime) {
            // Nanosecond wraparound (a few centuries out): present ASAP.
            target = 0;
        }
    }
    // Only the alignment flag needs a nonzero target: untimed presents still
    // reserve a timing slot and consume a present id.
    const VkPresentTimingInfoFlagsEXT flags = target != 0 ? VK_PRESENT_TIMING_INFO_PRESENT_AT_NEAREST_REFRESH_CYCLE_BIT_EXT : 0u;

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
    // Interval MAX means variable refresh: the duration is a minimum, not a
    // cadence, so paced simulation stays on the wall clock. Interval 0 means
    // unknown dynamics: pace off the duration. Otherwise the interval is the
    // image-present-duration quanta the targets are computed in.
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
    // A live counter on both calls: the parameter is optional, but a strict
    // driver is within its rights to want somewhere to put the value.
    uint64_t                           counter = _timeDomainsCounter;
    VkSwapchainTimeDomainPropertiesEXT props = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_TIME_DOMAIN_PROPERTIES_EXT,
    };
    const VkResult countResult = vkGetSwapchainTimeDomainPropertiesEXT(device, swapchain, &props, &counter);
    if (countResult != VK_SUCCESS) {
        return false;
    }
    // Time domains are a handful; 8 slots bound the query, extras ignored.
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
    // Globals first; the stage-local clock is the last resort. A domain
    // appearing here that was absent before (or vice versa) is the
    // upgrade/downgrade path, handled by the change detection below.
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
        // A new domain (or anchor) invalidates the old baseline: its
        // timestamps belong to another timeline. Presents go out untimed
        // until feedback re-anchors them.
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
    // A result in another domain than the scheduling one means the domain was
    // unavailable at present time, or the display moved on. A global timeline
    // change (scheduled global, reported global) is adopted with a restarted
    // baseline; anything else is ignored, and the reported timestamps stay out
    // of the predictor -- they belong to a different timeline. Cross-world
    // moves (stage-local to global or back) go through the advertised set in
    // ResolveTimeDomain, never through a single feedback result.
    if (result.timeDomain != _timeDomain || result.timeDomainId != _timeDomainId) {
        if (!_stageLocal && GloballyComparable(result.timeDomain)) {
            _timeDomain   = result.timeDomain;
            _timeDomainId = result.timeDomainId;
            _hasBaseline  = false;
            _baselineId   = 0;
            _baselineTime = 0;
        } else if (_stageLocal) {
            // Scheduled in a stage-local clock, whose only timeline is the
            // anchor's: a result from any other one means ours moved on (the
            // re-resolve already failed, or this straggler predates it). Drop
            // the baseline so presents go out untimed until feedback
            // re-anchors them; targets in a dead stage timeline aim at nothing.
            // (Scheduled global, a stage-local straggler says nothing about
            // the global clock, so the baseline stands.)
            _hasBaseline  = false;
            _baselineId   = 0;
            _baselineTime = 0;
        }
        return;
    }

    const uint64_t firstPixelOut = StageTime(result, VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT);
    const uint64_t dequeued      = StageTime(result, VK_PRESENT_STAGE_REQUEST_DEQUEUED_BIT_EXT);
    const uint64_t queueEnd      = StageTime(result, VK_PRESENT_STAGE_QUEUE_OPERATIONS_END_BIT_EXT);

    // Baseline: latest first-pixel-out, else latest dequeue -- or, in a
    // stage-local clock, the anchor stage alone (stages are different
    // timelines there, so mixing them would aim targets at nothing). Zero
    // stage times (dropped or replaced presents) carry no signal and are
    // skipped; a zero present id cannot anchor the id-indexed target formula.
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
    // Margin: how early the finished frame waited for its V-blank. Only
    // forward-moving pairs count; anything else is a driver shrug. Never in
    // a stage-local clock: its stages are different timelines, so a
    // cross-stage duration is meaningless -- the margin stays unknown and the
    // fidelity governor stays inert on this path.
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

} // namespace ZHLN::Vk
