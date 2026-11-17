// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TargetCamera.hpp"
#include <FreeCam/FreeCam.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <algorithm>
#include <cmath>

namespace ZHLN::CameraRig {
auto ReadInput(const COM::IActionContext& input) -> InputSnapshot {
    return {
        .look = input.MouseDelta(kPrimarySeat), .wheel = input.MouseWheel(kPrimarySeat), .lookButton = input.MouseButtonDown(MouseButton::Right, kPrimarySeat)
    };
}
void Apply(COM::IEntities& entities, const InputSnapshot& input, float dt) {
    if (!std::isfinite(dt) || dt < 0.0f)
        return;
    for (const auto entity: entities.EntitiesWith<TargetCameraComponent>()) {
        if (entities.Get<FreeCam::FreeCamTagComponent>(entity) || entities.Get<Components::PendingDestroy>(entity))
            continue;
        auto* rig    = entities.Get<TargetCameraComponent>(entity);
        auto* camera = entities.Get<Components::CameraComponent>(entity);
        if (!rig || !camera || !entities.IsAlive(rig->target) || entities.Get<Components::PendingDestroy>(rig->target))
            continue;
        FVec3 targetPos {};
        if (const auto* world = entities.Get<Components::WorldTransformComponent>(rig->target)) {
            targetPos = {world->world[0, 3], world->world[1, 3], world->world[2, 3]};
        } else if (const auto* transform = entities.Get<Components::TransformComponent>(rig->target)) {
            targetPos = transform->position;
        } else
            continue;
        if (std::abs(input.wheel) > 0.01f)
            rig->targetDistance = std::clamp(rig->targetDistance - input.wheel * 0.5f, 1.5f, 15.0f);
        if (rig->stiffness > 0.0f) {
            const float factor = std::clamp(rig->stiffness * dt, 0.0f, 1.0f);
            rig->distance += (rig->targetDistance - rig->distance) * factor;
            rig->fov += (rig->targetFov - rig->fov) * factor;
        } else {
            rig->distance = rig->targetDistance;
            rig->fov      = rig->targetFov;
        }
        if (input.lookButton && dt > 0.0f) {
            rig->yaw += input.look.x * 0.15f;
            rig->pitch = std::clamp(rig->pitch - input.look.y * 0.15f, -89.0f, 89.0f);
        }
        auto& cam         = camera->camera;
        cam.yaw           = DegF {rig->yaw};
        cam.pitch         = DegF {rig->pitch};
        cam.fov           = DegF {rig->fov};
        const float yaw   = cam.yaw.ToRadians().value;
        const float pitch = cam.pitch.ToRadians().value;
        const FVec3 direction {std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch)};
        if (!rig->hasInitSmoothTarget || (targetPos - rig->smoothTargetPos).LengthSquared() > 100.0f || rig->stiffness <= 0.0f) {
            rig->smoothTargetPos     = targetPos;
            rig->hasInitSmoothTarget = 1;
        } else {
            rig->smoothTargetPos += (targetPos - rig->smoothTargetPos) * (1.0f - std::exp(-rig->stiffness * dt));
        }
        cam.position = rig->smoothTargetPos - direction.Normalized() * rig->distance + rig->targetOffset;
    }
}
void Step(COM::IEngine& engine, float dt) {
    auto&      entities = *engine.Entities();
    const auto camera   = entities.SingletonEntity<Components::MainCameraTagComponent>();
    if (camera.HasValue() && !entities.Get<Components::PendingDestroy>(camera.value) && !entities.Get<TargetCameraComponent>(camera.value))
        (void) entities.Attach(camera.value, TargetCameraComponent {});
    Apply(entities, ReadInput(*engine.Actions()), dt);
}
} // namespace ZHLN::CameraRig
