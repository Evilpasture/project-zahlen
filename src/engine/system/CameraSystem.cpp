// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "CameraSystem.hpp"
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Render/RenderContext.hpp>

namespace ZHLN {

void CameraSystem::Update(ECS::Query<Components::CameraComponent&> cameras, ECS::Res<RenderContext> render) {
    const auto res = render->GetViewport();
    if (res.width == 0 || res.height == 0) {
        return;
    }

    for (Entity e: cameras.Entities<Components::CameraComponent>()) {
        if (auto cComp = cameras.Get<Components::CameraComponent>(e)) {
            // Each camera entity projects its own pose: no shared camera.
            const Camera& cam = cComp->camera;
            if (cComp->frameCounter == 0) {
                cComp->prevUnjitteredViewProj = cam.GetProjectionMatrix(static_cast<float>(res.width) / res.height) * cam.GetViewMatrix();
                cComp->unjitteredViewProj     = cComp->prevUnjitteredViewProj;
                cComp->viewProj               = cComp->unjitteredViewProj;
            } else {
                cComp->prevUnjitteredViewProj = cComp->unjitteredViewProj;
            }

            auto unjitteredProj = cam.GetProjectionMatrix(static_cast<float>(res.width) / res.height);
            cComp->unjitteredViewProj = unjitteredProj * cam.GetViewMatrix();

            // Both remaining AA modes are spatial post-processes; the camera
            // projection itself is never jittered.
            cComp->viewProj = cComp->unjitteredViewProj;

            cComp->frameCounter++;
        }
    }
}

}
