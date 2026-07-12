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
    // Top-level orchestrator managing frame lifecycles
    static std::expected<void, Error> Update(Engine& engine);

  private:
    // Phase 1: Resolves viewports, cascaded shadows, and draws standard mesh components
    static std::expected<void, Error> RenderMain(Engine& engine, int& outPhysicsDrawMode, JPH::Mat44& outShadowProjView);

    // Phase 2: Resolves frustum outlines and Jolt physics debug visualizations
    static void RenderDebug(Engine& engine, int physicsDrawMode);
};
} // namespace ZHLN
