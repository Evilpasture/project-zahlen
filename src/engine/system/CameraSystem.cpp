// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CameraSystem.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Entity.hpp"
#include "Zahlen/Render/Render.hpp"
#include "Zahlen/Window.hpp"
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN {

void CameraSystem::Update(Engine& engine, float dt, float alpha) {
    const auto vp = engine.GetRenderContext().GetViewport();
    Update(engine.GetRegistry(), Extent2D {vp.width, vp.height}, dt, alpha);
}

void CameraSystem::Update(ECS::Registry& reg, Extent2D res, float , float ) {
    if (res.width == 0 || res.height == 0) {
        return;
    }

    for (Entity e: reg.GetEntitiesWith<Components::CameraComponent>()) {
        if (auto cComp = reg.Get<Components::CameraComponent>(e)) {
            // Each camera entity projects its own pose: no shared camera.
            const Camera& cam = cComp->camera;
            if (cComp->frameCounter == 0) {
                cComp->prevUnjitteredViewProj = cam.GetProjectionMatrix(static_cast<float>(res.width) / res.height) * cam.GetViewMatrix();
                cComp->unjitteredViewProj     = cComp->prevUnjitteredViewProj;
                cComp->viewProj               = cComp->unjitteredViewProj;
            } else {
                cComp->prevUnjitteredViewProj = cComp->unjitteredViewProj;
            }

            JPH::Mat44 unjitteredProj = cam.GetProjectionMatrix(static_cast<float>(res.width) / res.height);
            cComp->unjitteredViewProj = unjitteredProj * cam.GetViewMatrix();

            auto aaComp = reg.Get<Components::AASettingsComponent>(e);
            if (aaComp && aaComp->state.mode == AAMode::TAA) {
                aaComp->state.frameIndex++;
                cComp->viewProj = cam.GetJitteredProjectionMatrix(static_cast<float>(res.width) / res.height, res.width, res.height, aaComp->state) *
                                  cam.GetViewMatrix();
            } else {
                if (aaComp) {
                    aaComp->state.frameIndex = 0;
                }
                cComp->viewProj = cComp->unjitteredViewProj;
            }

            cComp->frameCounter++;
        }
    }
}

}
