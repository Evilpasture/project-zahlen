// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Components.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>

namespace ZHLN {
class AssetManager;
namespace ECS {
class EntityCommandBuffer;
}

// Reconciles a cooked emitter as a scene-owned directional light in UpdateGraph
// before scene cleanup, culling, and lighting run.
class EnvironmentSunSystem {
  public:
    using SunEnvQuery = ECS::Query<
        const Components::EnvironmentSunTagComponent,
        const Components::EnvironmentMapComponent,
        Components::LightComponent&,
        const Components::SunTagComponent,
        const Components::PendingDestroy>;

    static void Update(SunEnvQuery query, ECS::EntityCommandBuffer& ecb, ECS::Res<AssetManager> assets);
};

} // namespace ZHLN
