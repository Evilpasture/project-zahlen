// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DecalSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN {

void DecalSystem::Update(ECS::Query<const Components::DecalComponent, const Components::WorldTransformComponent> decals,
                         ECS::ResMut<RenderContext> render) {
    decals.ForEach([&](Entity, const auto& decal, const auto& worldTrans) {
        JPH::Mat44 worldMat = worldTrans.world;
        render->DrawDecal(
            {.transform    = worldMat,
             .invTransform = worldMat.Inversed(),
             .albedoMap    = decal.albedoMap,
             .normalMap    = decal.normalMap,
             .roughness    = decal.roughness,
             .metallic     = decal.metallic}
        );
    });
}

}
