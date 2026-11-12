// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>

namespace ZHLN {
class RenderContext;

// Frame-plan camera projection. The phase owner invokes it sequentially before
// the render graph, with only the camera data and render service it needs.
class CameraSystem {
  public:
    static void Update(ECS::Query<Components::CameraComponent&> cameras, ECS::Res<RenderContext> render);
};
} // namespace ZHLN
