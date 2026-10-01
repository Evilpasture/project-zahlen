// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/ecs/SystemParameters.hpp>

namespace ZHLN {
class AssetManager;

// Reconciles a cooked emitter as a scene-owned directional light *before*
// culling and lighting run. Registry& explicitly declares structural ECS
// writes to the graph; the renderer never owns or liveness-polls entities.
class EnvironmentSunSystem {
  public:
    static void Update(ECS::Registry& registry, ECS::Res<AssetManager> assets);
};

} // namespace ZHLN
