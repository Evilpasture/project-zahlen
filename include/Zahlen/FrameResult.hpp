// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/FrameResult.hpp
//
// What the frame verbs report when a frame did not go through as asked:
// ZHLN::FrameResult, and nothing else. It is the frame loop's own vocabulary --
// the outcomes it *acts* on -- not a mirror of Vulkan's result codes.
//
// The mapping from a VkResult to one of these lives in exactly one place,
// Vk::ToFrameError (src/vulkan/core/RenderCore.hpp). Every result it does not
// name travels verbatim: ErrorCode carries any enum, so a driver's own code
// arrives as itself -- category "VkResult", message its own identifier, e.g.
// VK_ERROR_SURFACE_LOST_KHR -- instead of being folded into a catch-all here.
//
// Frame verbs return std::expected<void, ErrorCode>; no std::expected in the
// engine carries a raw VkResult. A caller asks the one question it has:
//
//     if (code.Is(FrameResult::Suboptimal)) { /* skip the frame; nothing is wrong */ }
//
// which is what the app layer and the engine's systems need, since neither of
// them includes vulkan.h.
#pragma once

#include <Zahlen/Core/Description.hpp>
#include <cstdint>

namespace ZHLN {

/// The frame loop's outcomes, as names a caller can ask about.
///
/// There is deliberately no `Success`: a frame that worked reports the absence
/// of an error, an engaged std::expected, exactly as the engine's other
/// fallible calls do. `Suboptimal = 1` is pinned because ErrorCode packs the
/// enumerator into its value word, whose 0 means "no error"
/// (ErrorCode::operator bool); an enumerator with the value 0 would make that
/// error indistinguishable from success in every `if (code)` test.
enum class FrameResult : uint8_t {
    /// Not a failure, and nothing for the caller to fix. Two situations reach
    /// here, and they are the same situation as far as the frame loop is
    /// concerned -- this frame was not presented, the next one tries again:
    ///   * Vulkan said the surface and the swapchain no longer agree
    ///     (VK_SUBOPTIMAL_KHR, or VK_ERROR_OUT_OF_DATE_KHR) and the renderer has
    ///     already rebuilt for it, including retiring the records built against
    ///     the old generation;
    ///   * the window had no drawable area (minimised, or resized to zero).
    /// Logging one of these is noise: there is nothing to do about it.
    Suboptimal ZHLN_ANNOTATION(
        ZHLN::Description<"The frame was skipped without a failure: the swapchain and the surface disagree, or the window had no drawable area"> {}
    ) = 1,

    /// The renderer could not recreate what the frame needs -- the swapchain and
    /// the render targets behind it -- or the frame had nothing to be drawn
    /// into. The frame fails.
    TargetRecreationFailed ZHLN_ANNOTATION(ZHLN::Description<"The renderer could not recreate the swapchain and its targets"> {}),

    /// VK_ERROR_DEVICE_LOST: the device is gone. The caller's move is the
    /// disruptive one -- Engine::HandleDeviceLost() / Kernel::HandleDeviceLost()
    /// tears the render context down and builds it again.
    DeviceLost ZHLN_ANNOTATION(ZHLN::Description<"The graphics device was lost (VK_ERROR_DEVICE_LOST)"> {}),
};

static_assert(static_cast<uint32_t>(FrameResult::Suboptimal) != 0, "ErrorCode's 0 value means 'no error'; no FrameResult may use it.");

} // namespace ZHLN
