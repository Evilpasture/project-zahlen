// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FreeCam.hpp"

#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/FramePlan.hpp>
#include <Zahlen/Engine/GameModule.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/ECS/ECS.hpp>

#include <Jolt/Math/Vec3.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace ZHLN::FreeCam {
namespace {

struct FreeCamOptOutComponent {};

void Drive(Components::CameraComponent& cc, const Components::InputStateComponent& input, const FreeCamComponent& knobs, float dt) {
    if (!knobs.enabled) {
        return;
    }
    Camera&     cam  = cc.camera;
    const float look = knobs.lookSensitivity;
    const bool  lookButton = knobs.lookRequiresButton ? input.IsMouseButtonDown(static_cast<uint8_t>(KeyCode::RButton)) : true;
    if (lookButton) {
        cam.yaw += input.GetMouseDeltaX() * look;
        cam.pitch = std::clamp(cam.pitch - (input.GetMouseDeltaY() * look), -89.0f, 89.0f);
    }

    const float yawRad   = JPH::DegreesToRadians(cam.yaw);
    const float pitchRad = JPH::DegreesToRadians(cam.pitch);
    JPH::Vec3   forward(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));
    forward         = forward.Normalized();
    JPH::Vec3 right = forward.Cross(JPH::Vec3::sAxisY());
    if (right.LengthSq() > 1e-8f) {
        right = right.Normalized();
    }
    const JPH::Vec3 up    = JPH::Vec3::sAxisY();
    const float     speed = input.IsKeyDown(static_cast<uint8_t>(KeyCode::LShift)) ? (knobs.speed * knobs.sprintMultiplier) : knobs.speed;

    JPH::Vec3 move = JPH::Vec3::sZero();
    if (input.IsKeyDown(static_cast<uint8_t>(KeyCode::W))) {
        move += forward;
    }
    if (input.IsKeyDown(static_cast<uint8_t>(KeyCode::S))) {
        move -= forward;
    }
    if (input.IsKeyDown(static_cast<uint8_t>(KeyCode::A))) {
        move -= right;
    }
    if (input.IsKeyDown(static_cast<uint8_t>(KeyCode::D))) {
        move += right;
    }
    if (input.IsKeyDown(static_cast<uint8_t>(KeyCode::E)) || input.IsKeyDown(static_cast<uint8_t>(KeyCode::Space))) {
        move += up;
    }
    if (input.IsKeyDown(static_cast<uint8_t>(KeyCode::Q))) {
        move -= up;
    }
    if (move.LengthSq() > 0.0f && dt > 0.0f) {
        cam.position += move.Normalized() * speed * dt;
    }
}

Entity MainCamera(ECS::Registry& reg) {
    return reg.SingletonEntity<Components::MainCameraTagComponent>();
}

void SeedBootCamera(ECS::Registry& reg) {
    const Entity camera = MainCamera(reg);
    if (camera == Entity::Null()) {
        return;
    }
    if (reg.Get<FreeCamOptOutComponent>(camera)) {
        return;
    }
    if (!reg.Get<FreeCamTagComponent>(camera)) {
        reg.Add(camera, FreeCamTagComponent {});
    }
    if (!reg.Get<FreeCamComponent>(camera)) {
        reg.Add(camera, FreeCamComponent {});
    }
}

} // namespace

void Apply(Engine& engine, float dt) {
    auto& reg   = engine.GetRegistry();
    auto  input = reg.GetSingleton<Components::InputStateComponent>();
    if (!input) {
        return;
    }
    for (Entity e: reg.GetEntitiesWith<FreeCamTagComponent>()) {
        auto                   knobs = reg.Get<FreeCamComponent>(e);
        const FreeCamComponent defaults {};
        const FreeCamComponent& cfg = knobs ? *knobs : defaults;
        reg.Patch<Components::CameraComponent>(e, [&](auto& cc) -> auto { Drive(cc, *input, cfg, dt); });
    }
}

namespace {

void FreeCamStep(Engine& engine, float dt, FrameContext&) {
    SeedBootCamera(engine.GetRegistry());
    if (dt <= 0.0f) {
        return;
    }
    Apply(engine, dt);
}

// Declared, and placed before the core "CameraSystems" step so the matrices it
// projects come from the pose this step just wrote. The duplicate-name scan the
// callback form needed is gone: FramePlan::Compose reports a second
// "FreeCamSystem" as FramePlanError::DuplicateName.
constexpr ModuleStep kFrameSteps[] = {
    {
        .phase     = FramePhase::Camera,
        .name      = "FreeCamSystem",
        .run       = &FreeCamStep,
        .placement = StepPlacement::Before,
        .anchor    = "CameraSystems",
    },
};

void RegisterComponents(ECS::Registry& reg) {
    reg.RegisterComponent<FreeCamTagComponent>();
    reg.RegisterComponent<FreeCamComponent>();
    reg.RegisterComponent<FreeCamOptOutComponent>();
}

} // namespace

auto Describe() -> Module {
    return Module{
        .name               = "FreeCam",
        .registerComponents = &RegisterComponents,
        .frameSteps         = kFrameSteps,
    };
}

void Install(Engine& engine) {
    if (auto installed = engine.InstallModule(Describe()); !installed) {
        LogError("[FreeCam] module install failed: {}", ToString(installed.error()));
    }
}

void Attach(Engine& engine, float speed, bool lookRequiresButton) {
    auto&        reg    = engine.GetRegistry();
    const Entity camera = MainCamera(reg);
    if (camera == Entity::Null()) {
        return;
    }
    reg.Remove<FreeCamOptOutComponent>(camera);
    if (!reg.Get<FreeCamTagComponent>(camera)) {
        reg.Add(camera, FreeCamTagComponent {});
    }
    if (!reg.Patch<FreeCamComponent>(camera, [speed, lookRequiresButton](auto& fc) -> auto {
            fc.speed              = speed;
            fc.lookRequiresButton = lookRequiresButton;
        })) {
        reg.Add(camera, FreeCamComponent {.speed = speed, .lookRequiresButton = lookRequiresButton});
    }
}

void Detach(Engine& engine) {
    auto&        reg    = engine.GetRegistry();
    const Entity camera = MainCamera(reg);
    if (camera == Entity::Null()) {
        return;
    }
    if (!reg.Get<FreeCamOptOutComponent>(camera)) {
        reg.Add(camera, FreeCamOptOutComponent {});
    }
    reg.Remove<FreeCamTagComponent>(camera);
    reg.Remove<FreeCamComponent>(camera);
}

} // namespace ZHLN::FreeCam
