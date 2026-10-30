// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CameraSystem.hpp"
#include "Zahlen/Math/Camera.hpp"
#include "Zahlen/ECS/Components.hpp"
#include "Zahlen/Engine/Engine.hpp"
#include "Zahlen/ECS/Entity.hpp"
#include "Zahlen/Render/Render.hpp"
#include "Zahlen/Window/Window.hpp"
#include <Zahlen/ECS/ECS.hpp>

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
                    aaComp->state.jitterX     = 0.0f;
                    aaComp->state.jitterY     = 0.0f;
                    aaComp->state.prevJitterX = 0.0f;
                    aaComp->state.prevJitterY = 0.0f;
                }
                cComp->viewProj = cComp->unjitteredViewProj;
            }

            cComp->frameCounter++;
        }
    }
}

}
