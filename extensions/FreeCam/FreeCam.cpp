// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "FreeCam.hpp"
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <algorithm>
#include <cmath>

namespace ZHLN::FreeCam {
struct HostFreeCamOptOutComponent {};
namespace {
auto MainCamera(COM::IEntities& entities) -> Entity {
    const auto camera = entities.SingletonEntity<Components::MainCameraTagComponent>();
    return camera.HasValue() ? camera.value : Entity::Null();
}
}
auto ReadInput(const COM::IActionContext& input) -> InputSnapshot {
    const auto down = [&](KeyCode key) { return input.KeyDown(key, kPrimarySeat); };
    return {
        .movement =
            {float(down(KeyCode::D)) - float(down(KeyCode::A)), float(down(KeyCode::E) || down(KeyCode::Space)) - float(down(KeyCode::Q)),
             float(down(KeyCode::W)) - float(down(KeyCode::S))},
        .look       = input.MouseDelta(kPrimarySeat),
        .lookButton = input.MouseButtonDown(MouseButton::Right, kPrimarySeat),
        .sprint     = down(KeyCode::LShift),
    };
}
void Drive(Camera& cam, const Component& knobs, const InputSnapshot& input, float dt) {
    if (!knobs.enabled || !std::isfinite(dt) || dt <= 0.0f)
        return;
    if (!knobs.lookRequiresButton || input.lookButton) {
        cam.yaw += DegF {input.look.x * knobs.lookSensitivity};
        cam.pitch = std::clamp(cam.pitch - DegF {input.look.y * knobs.lookSensitivity}, DegF {-89.0f}, DegF {89.0f});
    }
    const float yaw     = cam.yaw.ToRadians().value;
    const float pitch   = cam.pitch.ToRadians().value;
    const auto  forward = FVec3 {std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch)}.Normalized();
    auto        right   = Cross(forward, FVec3::UnitY());
    if (right.LengthSquared() > 1e-8f)
        right = right.Normalized();
    const auto move = right * input.movement.x + FVec3::UnitY() * input.movement.y + forward * input.movement.z;
    if (move.LengthSquared() > 0.0f)
        cam.position += move.Normalized() * knobs.speed * (input.sprint ? knobs.sprintMultiplier : 1.0f) * dt;
}
void Apply(COM::IEntities& entities, const InputSnapshot& input, float dt) {
    for (const auto entity: entities.EntitiesWith<TagComponent>()) {
        if (entities.Get<Components::PendingDestroy>(entity))
            continue;
        const auto* knobs  = entities.Get<Component>(entity);
        const auto  config = knobs ? *knobs : Component {};
        entities.Patch<Components::CameraComponent>(entity, [&](auto& cc) { Drive(cc.camera, config, input, dt); });
    }
}
bool Attach(COM::IEntities& entities, float speed, bool lookRequiresButton) {
    const auto camera = MainCamera(entities);
    if (camera == Entity::Null() || entities.Get<Components::PendingDestroy>(camera))
        return false;
    if (!entities.Get<TagComponent>(camera) && !entities.Attach(camera, TagComponent {}))
        return false;
    if (!entities.Patch<Component>(
            camera,
            [&](auto& fc) {
                fc.speed              = speed;
                fc.lookRequiresButton = lookRequiresButton;
            }
        ) &&
        !entities.Attach(camera, Component {.speed = speed, .lookRequiresButton = lookRequiresButton})) {
        entities.Detach<TagComponent>(camera);
        return false;
    }
    entities.Detach<HostFreeCamOptOutComponent>(camera);
    return true;
}
void Detach(COM::IEntities& entities) {
    const auto camera = MainCamera(entities);
    if (camera == Entity::Null())
        return;
    // If allocation fails, retain the old state rather than immediately reseed.
    if (!entities.Get<HostFreeCamOptOutComponent>(camera) && !entities.Attach(camera, HostFreeCamOptOutComponent {}))
        return;
    entities.Detach<TagComponent>(camera);
    entities.Detach<Component>(camera);
}
void Step(COM::IEngine& engine, float dt) {
    auto&      entities = *engine.Entities();
    const auto camera   = MainCamera(entities);
    if (camera != Entity::Null() && !entities.Get<HostFreeCamOptOutComponent>(camera) && !entities.Get<Components::PendingDestroy>(camera)) {
        // Seed independently: a caller may intentionally attach only the tag.
        if (!entities.Get<TagComponent>(camera))
            (void) entities.Attach(camera, TagComponent {});
        if (!entities.Get<Component>(camera))
            (void) entities.Attach(camera, Component {});
    }
    Apply(entities, ReadInput(*engine.Actions()), dt);
}
} // namespace ZHLN::FreeCam
