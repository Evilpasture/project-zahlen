// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Render/Render.hpp>
#include <expected>

namespace ZHLN {
class Engine;

class ZHLN_API RenderSystem {
  public:
    static std::expected<void, ErrorCode> Update(Engine& engine, float dt);

  private:
    static FrameOutcome<FrameSkipped> RenderMain(Engine& engine, int& outPhysicsDrawMode, JPH::Mat44& outShadowProjView, float dt);

    static void RenderDebug(Engine& engine, int physicsDrawMode);
};
}
