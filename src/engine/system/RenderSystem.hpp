// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Render.hpp>
#include <expected>

namespace ZHLN {
class Engine;

class ZHLN_API RenderSystem {
  public:
    // Top-level orchestrator managing frame lifecycles. Both markers a frame
    // verb can report are consumed here -- a skipped frame and a present that
    // did not go through as asked are neither failures nor this system's to act
    // on -- so what is left is the frame's errors.
    static std::expected<void, ErrorCode> Update(Engine& engine, float dt);

  private:
    // Phase 1: Resolves viewports, cascaded shadows, and draws standard mesh components.
    // FrameSkipped means BeginFrame reported a frame with nothing to draw into:
    // nothing was drawn and nothing is wrong.
    static FrameOutcome<FrameSkipped> RenderMain(Engine& engine, int& outPhysicsDrawMode, JPH::Mat44& outShadowProjView, float dt);

    // Phase 2: Resolves frustum outlines and Jolt physics debug visualizations
    static void RenderDebug(Engine& engine, int physicsDrawMode);
};
} // namespace ZHLN
