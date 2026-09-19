// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/FrameResult.hpp
//
// The frame path's vocabulary: what a frame verb reports when the frame went
// through, when it was skipped without being a failure, and when it failed.
//
// A frame verb returns FrameOutcome<T>, which has three outcomes and says which
// one it is in the type:
//
//   std::unexpected(ErrorCode)  the frame failed -- FrameResult (the failures
//                               only the renderer knows about) or the driver's
//                               own code, verbatim
//   std::nullopt                it succeeded, with nothing to report: the plain
//                               "void" case
//   T                           the verb's own non-failure, named per verb
//
// T is a name, not a bucket. Before this, one enum carried Suboptimal for every
// non-failure in the path, which made "the frame was skipped" and "the present
// was suboptimal" the same word -- and put it in the *error* channel, where a
// non-failure has no business being. Both are gone: a verb's non-failure is a
// value of its own type (FrameSkipped, PresentSuboptimal, or the image
// AcquireNext vended), and an error slot holds errors.
#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <cstdint>
#include <expected>
#include <optional>

namespace ZHLN {

/// std::expected<std::optional<T>, ErrorCode>, spelled where a signature wants
/// to say it. See this header's preamble for what each of the three states
/// means; T is std::nullopt's payload only when the verb has something to hand
/// back (AcquireNext's image), and a marker type when it does not.
template <typename T>
using FrameOutcome = std::expected<std::optional<T>, ErrorCode>;

/// BeginFrame's non-failure: no frame was begun, and nothing is wrong. The
/// cause is always the same shape -- there was nothing to draw into this frame
/// (a window with no drawable area: minimised, or resized to zero) -- so the
/// caller's move is to skip the frame and try again next one. Nothing to log:
/// there is no failure to report and nothing to fix.
struct FrameSkipped {};

/// The present path's non-failure: the frame was drawn, but the presentation of
/// it did not go through as asked, because the surface and the swapchain no
/// longer agree (VK_SUBOPTIMAL_KHR, or VK_ERROR_OUT_OF_DATE_KHR). The renderer
/// has already rebuilt the swapchain and retired the records built against the
/// old one, so the next frame draws against up-to-date images; this is
/// EndFrame's and the presenter's way of saying so rather than reporting a
/// failure. Nothing to log for the same reason as FrameSkipped.
struct PresentSuboptimal {};

/// The frame's failures: what a frame verb reports as an *error* when Vulkan is
/// not the one that has something to say. There is deliberately no `Success`
/// and no non-failure here -- successes are an engaged std::expected, and the
/// non-failures are FrameSkipped / PresentSuboptimal in the value slot.
///
/// `TargetRecreationFailed = 1` is pinned because ErrorCode packs the
/// enumerator into its value word, whose 0 means "no error"
/// (ErrorCode::operator bool); an enumerator with the value 0 would make that
/// error indistinguishable from success in every `if (code)` test.
enum class FrameResult : uint8_t {
    /// The renderer could not recreate what the frame needs: the swapchain and
    /// the render targets behind it, after a resize.
    TargetRecreationFailed ZHLN_ANNOTATION(ZHLN::Description<"The renderer could not recreate the swapchain and its targets"> {}) = 1,

    /// VK_ERROR_DEVICE_LOST: the device is gone. The caller's move is the
    /// disruptive one -- Engine::HandleDeviceLost() / Kernel::HandleDeviceLost()
    /// tears the render context down and builds it again.
    DeviceLost ZHLN_ANNOTATION(ZHLN::Description<"The graphics device was lost (VK_ERROR_DEVICE_LOST)"> {}),
};

static_assert(static_cast<uint32_t>(FrameResult::TargetRecreationFailed) != 0, "ErrorCode's 0 value means 'no error'; no FrameResult may use it.");

} // namespace ZHLN
