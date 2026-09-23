// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Render/PresentTiming.hpp
//
// The presentation pacer's public vocabulary: which pacing strategy the
// presenter resolved at bring-up, and the display-timing metrics the
// closed loop feeds back. This is the whole of what the engine knows about
// presentation timing -- no Vulkan types cross here, so gameplay, physics
// and settings code pace themselves off these values without including a
// Vulkan header.
//
// The strategy behind the values lives in src/vulkan/presentation/PresentPacer.hpp:
// one immutable PacingPolicy per presenter, chosen once from device
// enablement plus surface capabilities, with the observer (past-timing
// feedback in AcquireNext) and the predictor (target timestamps in Present)
// both contained inside it.
#pragma once

#include <Zahlen/Core/Description.hpp>
#include <cstdint>

namespace ZHLN {

// How a presenter puts frames on screen, resolved once at bring-up from what
// the driver enabled plus what the surface advertises. A single immutable
// value per presenter: the rest of the engine only ever reads the metrics
// below, never the capability probes that produced this.
enum class PacingPolicy : uint8_t {
    // VK_PRESENT_MODE_FIFO_LATEST_READY_KHR plus VK_EXT_present_timing: every
    // present carries an absolute target timestamp for its V-blank, and past
    // presentation feedback calibrates the next one. Tear-free, no buffer
    // bloat, hardware-locked refresh interval.
    PacedClosedLoop ZHLN_ANNOTATION(ZHLN::Description<"Closed-loop paced presentation (FIFO latest-ready plus present timing)"> {}) = 0,
    // VK_PRESENT_MODE_FIFO_LATEST_READY_KHR without target timestamps: stale
    // queued frames are skipped at V-blank, but there is no timing feedback,
    // so no calibrated display interval.
    AdaptiveVBlank ZHLN_ANNOTATION(ZHLN::Description<"Latest-ready V-blank presentation without timing feedback"> {}),
    // V-sync off (VK_PRESENT_MODE_IMMEDIATE_KHR): uncapped benchmark mode,
    // tearing allowed, no pacing of any kind.
    Decoupled ZHLN_ANNOTATION(ZHLN::Description<"Uncapped immediate presentation (benchmark mode)"> {}),
    // Classic V-blank pacing (VK_PRESENT_MODE_MAILBOX_KHR, else FIFO): tear-free
    // but open-loop, with neither latest-ready skipping nor timing feedback.
    LegacyVBlank ZHLN_ANNOTATION(ZHLN::Description<"Legacy mailbox/FIFO V-blank presentation"> {}),
};

// One presenter's display-timing feedback, as of the last acquired frame.
// Every field has an explicit validity companion: a headless session, a
// legacy policy, or a driver whose timing properties are not available yet
// all report "unknown" rather than a plausible-looking zero.
struct PresentTimingMetrics {
    // The strategy this presenter runs. Sealed during bring-up; a rebuild
    // (resize, monitor switch) re-arms the timing state but never changes this.
    PacingPolicy policy = PacingPolicy::LegacyVBlank;
    // True once the display's refresh interval is known from hardware timing
    // properties (closed-loop policy on a fixed-refresh display). False covers
    // every other case: other policies, headless sessions, variable refresh,
    // and the bootstrap frames before the first timing properties arrive.
    bool hasPacedTiming = false;
    // Nanoseconds per refresh cycle (the image-present-duration quanta), valid
    // only when hasPacedTiming is true. Engine::Run paces fixed-step
    // simulation off this instead of the wall clock.
    uint64_t refreshIntervalNs = 0;
    // True once a queue-operations-end to request-dequeued slack measurement
    // has been observed: how early the finished frame waits for its V-blank.
    bool hasMargin = false;
    // That slack in nanoseconds, valid only when hasMargin is true. The
    // fidelity governor scales quality down while this stays under ~2 ms.
    uint64_t lastPresentMarginNs = 0;
    // True when the presentation engine reports variable refresh (the timing
    // properties' interval is unbounded). Paced simulation stays on the wall
    // clock: a minimum refresh duration is not a frame cadence.
    bool variableRefresh = false;
    // Present id of the most recent consumed timing result, 0 when none has
    // been observed yet. Correlates end-to-end traces with presents.
    uint64_t lastPresentId = 0;
};

} // namespace ZHLN
