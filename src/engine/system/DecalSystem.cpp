// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DecalSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN {

void DecalSystem::Update(Engine& engine) {
    auto& rc  = engine.GetRenderContext();
    auto& reg = engine.GetRegistry();

    for (Entity e: reg.GetEntitiesWith<Components::DecalComponent>()) {
        auto* decalComp = reg.Get<Components::DecalComponent>(e);
        auto* trans     = reg.Get<Components::TransformComponent>(e);
        if ((decalComp != nullptr) && (trans != nullptr)) {
            JPH::Mat44 worldMat = trans->GetMatrix();
            JPH::Mat44 invWorld = worldMat.Inversed();

            Renderer::DrawDecal(
                rc, {.transform    = worldMat,
                     .invTransform = invWorld,
                     .albedoIndex  = decalComp->albedoIndex,
                     .normalIndex  = decalComp->normalIndex,
                     .roughness    = decalComp->roughness,
                     .metallic     = decalComp->metallic}
            );
        }
    }
}

} // namespace ZHLN
