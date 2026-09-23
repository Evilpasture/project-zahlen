// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/vulkan/presentation/PresentPacer.hpp
//
// The closed-loop presentation pacer: one per presenter, owning everything the
// engine knows about display timing. It resolves a single immutable
// PacingPolicy at bring-up from device enablement plus surface capabilities,
// then runs the observer (past-timing feedback drained in AcquireNext) and the
// predictor (absolute target timestamps attached in Present) behind two calls.
// The rest of the renderer only ever sees PacingPolicy and
// PresentTimingMetrics -- no Vulkan timing type escapes this file's friends.
//
// Policy ladder, first match wins:
//   PacedClosedLoop: VK_EXT_present_timing feedback plus absolute target
//     timestamps on VK_PRESENT_MODE_FIFO_LATEST_READY_KHR. Every present aims
//     at its V-blank; feedback calibrates the next aim.
//   AdaptiveVBlank: FIFO_LATEST_READY without target timestamps. Stale queued
//     frames are skipped at V-blank, but there is no interval to pace off.
//   Decoupled: V-sync off, IMMEDIATE, uncapped benchmark mode.
//   LegacyVBlank: MAILBOX, else FIFO. Tear-free but open-loop.
//
// The closed loop is provisional until the first swapchain confirms it: the
// TIMING_BIT must be set at creation time, but the actual present mode, the
// timing properties and the queue are only known afterwards. If confirmation
// fails the pacer seals as AdaptiveVBlank instead -- still latest-ready, just
// untimed -- and the already-set TIMING_BIT is harmless without queries.
// A confirmed policy never changes; rebuilds (resize, monitor switch) re-arm
// the timing state -- queue size, domains, properties, ids, baseline --
// against the new swapchain without re-resolving.
//
// Scheduling: the predictor aims present N at
//   T = lastBaseline + (N - lastBaselineId) * imagePresentDuration,
// where the baseline is the latest consumed FIRST_PIXEL_OUT timestamp (or
// REQUEST_DEQUEUED when the pixel stage is unavailable), in the swapchain's
// scheduling time domain. Until a baseline exists, or the timing properties
// are unknown, presents go out untimed (target 0: as soon as possible), and
// the engine paces off the wall clock.
//
// When the swapchain exposes no global domain -- a compositor path whose
// stages only have their own clocks -- the loop schedules in the stage-local
// domain instead, anchored to the dequeue event alone. That clock is only
// self-consistent within its anchor, so the baseline never mixes stages and
// the cross-stage margin stays unknown (the fidelity governor stays inert);
// the refresh interval is a duration, clock-agnostic, so paced simulation
// still engages. A global domain appearing later upgrades silently.
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

// One timed present's chain, as Predict returns it. The presenter parks the
// value in a member until the present lands: the chain heads at VkPresentId2KHR
// with VkPresentTimingsInfoEXT under it -- that order, because
// VkPresentTimingInfoEXT::pNext must be NULL -- and every pointer in it aliases
// this struct, so nothing dangles while the present is in flight.
struct PresentPrediction {
    VkPresentTimingInfoEXT  timing    = {};
    VkPresentTimingsInfoEXT timings   = {};
    VkPresentId2KHR         presentId = {};
    uint64_t                idValue   = 0;
};

// Predict's only failure: the closed loop is not active (anything but a
// confirmed PacedClosedLoop), so there is no chain to aim. The presenter
// answers it with an untimed present; it never propagates further.
enum class PresentPacerError : uint8_t {
    TimingInactive ZHLN_ANNOTATION(ZHLN::Description<"Present timing is not active">{}) = 1,
};

class PresentPacer {
  public:
    // Drain capacity per Observe call. Queue sizes are imageCount * 2, and
    // image counts top out at 8, so one 16-deep drain frees a full queue.
    static constexpr uint32_t kMaxTimings = 16;
    // Queue-operations-end, request-dequeued, first-pixel-out.
    static constexpr uint32_t kMaxStages = 3;

    PresentPacer() noexcept                    = default;
    PresentPacer(const PresentPacer&) noexcept = default;
    PresentPacer& operator=(const PresentPacer&) noexcept = default;
    PresentPacer(PresentPacer&&) noexcept                 = default;
    PresentPacer& operator=(PresentPacer&&) noexcept = default;
    ~PresentPacer()                                  = default;

    // Resolves the pacing policy from device enablement (the Context's
    // PresentSupport record) plus surface queries (present modes, timing and
    // present-id capabilities). Runs once per presenter, before its first
    // swapchain is created. Without V-sync the policy is Decoupled; a null
    // surface (headless) skips every Vulkan timing probe and lands on
    // LegacyVBlank.
    void Resolve(const Context& ctx, VkSurfaceKHR surface, bool vsync) noexcept;

    // Re-arms the closed loop against a (re)built swapchain: sizes the timing
    // queue, resolves the scheduling time domain, refreshes the timing
    // properties, and restarts present ids and the baseline. The first call
    // seals a provisional PacedClosedLoop -- or downgrades it to
    // AdaptiveVBlank when the actual present mode left the FIFO family or the
    // timing calls fail. Later calls never change the policy. A null
    // swapchain (headless presenter) seals without arming.
    void OnSwapchainRebuilt(VkDevice device, VkSwapchainKHR swapchain, uint32_t imageCount, VkPresentModeKHR actualMode) noexcept;

    // Observer: drains consumed past-presentation results (up to kMaxTimings,
    // two passes when the queue overflows the drain), advances the scheduling
    // baseline, measures the queue-operations-end to request-dequeued slack,
    // and re-resolves timing properties and domains when their change
    // counters move. Called after every acquire while timing is active; a
    // no-op otherwise. Never blocks: it only collects already-consumed work.
    void Observe(VkDevice device, VkSwapchainKHR swapchain) noexcept;

    // Predictor: the per-present chain for the next present id -- target
    // timestamp in the scheduling domain, NEAREST_REFRESH_CYCLE alignment,
    // the negotiated stage queries. TimingInactive while timing is inactive,
    // in which case the presenter issues an untimed present.
    [[nodiscard]] auto Predict() noexcept -> std::expected<PresentPrediction, ZHLN::ErrorCode>;

    // The resolved policy. Provisional PacedClosedLoop reads back as
    // PacedClosedLoop already: the swapchain description is built from it.
    [[nodiscard]] auto Policy() const noexcept -> PacingPolicy {
        return _policy;
    }
    // True once the closed loop confirmed against a live swapchain: Observe
    // drains and Predict aims. False covers every other state, including a
    // provisional policy awaiting its first swapchain.
    [[nodiscard]] auto IsTimingActive() const noexcept -> bool {
        return _timingActive;
    }
    // The present mode the swapchain description should request: explicit
    // FIFO_LATEST_READY for the paced policies, IMMEDIATE for decoupled,
    // MAX_ENUM ("choose from vsync") for the legacy auto path.
    [[nodiscard]] auto RequestedPresentMode() const noexcept -> VkPresentModeKHR;
    // Whether the swapchain description should set TIMING_BIT. True for a
    // provisional closed loop: the bit must be set at creation, before the
    // first confirmation can run.
    [[nodiscard]] auto WantsPresentTiming() const noexcept -> bool {
        return _policy == PacingPolicy::PacedClosedLoop;
    }
    // The pacing strategy plus the latest display-timing feedback; see
    // PresentTimingMetrics for what each field's validity companion means.
    [[nodiscard]] auto Metrics() const noexcept -> PresentTimingMetrics;
    // The display-locked frame interval in seconds, when the closed loop
    // knows it from hardware timing properties on a fixed-refresh display;
    // std::nullopt in every other case.
    [[nodiscard]] auto PacedDeltaSeconds() const noexcept -> std::optional<float>;

  private:
    // Re-reads the refresh duration and interval; answers false while the
    // presentation engine has none to report yet (bootstrap) or the call
    // fails, keeping the last-known values either way.
    auto RefreshTimingProperties(VkDevice device, VkSwapchainKHR swapchain) noexcept -> bool;
    // Re-resolves the scheduling time domain (swapchain-local preferred,
    // host/device globals next, stage-local never) and restarts the baseline
    // against it; answers false when no comparable domain is available.
    auto ResolveTimeDomain(VkDevice device, VkSwapchainKHR swapchain) noexcept -> bool;
    // Consumes one drained result into the baseline, the slack margin and the
    // change counters. Results reported in a domain other than the scheduling
    // one re-anchor the domain when comparable (baseline restarts untimed)
    // and are otherwise ignored: a target timestamp is only meaningful in
    // the domain it was computed in.
    void ConsumeResult(const VkPastPresentationTimingEXT& result) noexcept;
    // Downgrades a provisional closed loop to AdaptiveVBlank: untimed from
    // here on, still latest-ready. Sealed policies pass through untouched.
    void DowngradeToAdaptive() noexcept;

    PacingPolicy   _policy              = PacingPolicy::LegacyVBlank;
    bool           _sealed              = false;
    bool           _timingActive        = false;
    VkPhysicalDevice _physical          = VK_NULL_HANDLE;
    VkSurfaceKHR     _surface           = VK_NULL_HANDLE;
    VkPresentStageFlagsEXT _stageMask   = 0;
    VkTimeDomainKHR _timeDomain         = VK_TIME_DOMAIN_DEVICE_KHR;
    uint64_t        _timeDomainId       = 0;
    // Last-resort scheduling in the swapchain's stage-local clock (a compositor
    // path that exposes no global domain): targets anchor to _anchorStage
    // alone -- stages are different timelines there, so the baseline never
    // mixes them and the cross-stage margin stays unknown. A global domain
    // appearing later upgrades off this silently.
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

} // namespace ZHLN::Vk
