// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/COM/Types.hpp>
#include <Zahlen/COM/FrameTypes.hpp>
#include <Zahlen/Core/COM/IObject.hpp>

namespace ZHLN::COM {
// Optional queryable capability; IEngine's existing vtable stays unchanged.
// One synchronous host thread owns the frame. No callbacks are registered.
struct IFramePipeline: IObject {
    // Runs input evaluation, fixed physics, simulation/interpolation, transform
    // propagation, command playback and cleanup. Stops before camera projection.
    // dt must be finite and nonnegative. A second advance or Tick is refused
    // until this frame is finished/cancelled; no clock time is consumed on refusal.
    [[nodiscard]] virtual Result<void> AdvanceFrame(float dt) noexcept = 0;
    // Projects the host's final camera poses, culls, renders and commits history.
    // Exactly once per successful advance; CurrentFrame advances on completion.
    [[nodiscard]] virtual Result<void> FinishFrame() noexcept = 0;
    // No render, input re-evaluation or physics replay. Preserves presentation
    // history and counts the consumed simulation frame. Not a simulation rollback.
    [[nodiscard]] virtual Result<void> CancelFrame() noexcept        = 0;
    [[nodiscard]] virtual bool         FramePending() const noexcept = 0;

  protected:
    ~IFramePipeline() = default;
};
} // namespace ZHLN::COM
