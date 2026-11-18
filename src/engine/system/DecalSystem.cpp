// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DecalSystem.hpp"
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <render/Render.hpp>
#include <ecs/ECS.hpp>

namespace ZHLN {

void DecalSystem::Update(ECS::Query<const Components::DecalComponent, const Components::WorldTransformComponent> decals,
                         ECS::ResMut<RenderContext> render) {
    decals.ForEach([&](Entity, const auto& decal, const auto& worldTrans) {
        const auto& worldMat = worldTrans.world;
        render->DrawDecal(
            {.transform    = worldMat,
             .invTransform = Inverse(worldMat),
             .albedoMap    = decal.albedoMap,
             .normalMap    = decal.normalMap,
             .roughness    = decal.roughness,
             .metallic     = decal.metallic}
        );
    });
}

}
