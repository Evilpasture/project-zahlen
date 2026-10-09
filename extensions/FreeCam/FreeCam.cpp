// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FreeCam.hpp"

#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/FrameScheduler.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/ecs/ECS.hpp>

#include <Jolt/Math/Vec3.h>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace ZHLN::FreeCam {
namespace {

void Drive(Components::CameraComponent& cc, const Components::InputStateComponent& input, const FreeCamComponent& knobs, float dt) {
    Camera&           cam  = cc.camera;
    const float       look = knobs.lookSensitivity;
    if (input.IsMouseButtonDown(static_cast<uint8_t>(KeyCode::RButton))) {
        cam.yaw += input.GetMouseDeltaX() * look;
        cam.pitch = std::clamp(cam.pitch - (input.GetMouseDeltaY() * look), -89.0f, 89.0f);
    }

    const float yawRad   = JPH::DegreesToRadians(cam.yaw);
    const float pitchRad = JPH::DegreesToRadians(cam.pitch);
    JPH::Vec3   forward(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));
    forward = forward.Normalized();
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
    if (move.LengthSq() > 0.0f) {
        cam.position += move.Normalized() * speed * dt;
    }
}

void FreeCamStep(Engine& engine, float dt, FrameContext&) {
    auto& reg   = engine.GetRegistry();
    auto  input = reg.GetSingleton<Components::InputStateComponent>();
    if (!input) {
        return;
    }

    for (Entity e: reg.GetEntitiesWith<Components::FreeCamTagComponent>()) {
        auto knobs = reg.Get<FreeCamComponent>(e);
        const FreeCamComponent defaults {};
        const FreeCamComponent& cfg = knobs ? *knobs : defaults;
        reg.Patch<Components::CameraComponent>(e, [&](auto& cc) -> auto { Drive(cc, *input, cfg, dt); });
    }
}

void AddFrameStep(FrameScheduler& scheduler) {
    for (const auto& step: scheduler.GetSteps()) {
        if (std::string_view(step.name) == "FreeCamSystem") {
            return;
        }
    }
    if (!scheduler.InsertBefore("CameraSystems", FramePhase::Camera, "FreeCamSystem", &FreeCamStep)) {
        scheduler.Add(FramePhase::Camera, "FreeCamSystem", &FreeCamStep);
    }
}

} // namespace

void Install(Engine& engine) {
    engine.GetRegistry().RegisterComponent<FreeCamComponent>();
    engine.AddFrameSchedulerExtension(&AddFrameStep);
}

void Attach(Engine& engine, float speed) {
    auto&              reg    = engine.GetRegistry();
    const ZHLN::Entity camera = reg.SingletonEntity<Components::MainCameraTagComponent>();
    if (camera == Entity::Null()) {
        return;
    }
    if (!reg.Get<Components::FreeCamTagComponent>(camera)) {
        reg.Add(camera, Components::FreeCamTagComponent {});
    }
    if (!reg.Patch<FreeCamComponent>(camera, [speed](auto& fc) -> auto { fc.speed = speed; })) {
        reg.Add(camera, FreeCamComponent {.speed = speed});
    }
}

} // namespace ZHLN::FreeCam
