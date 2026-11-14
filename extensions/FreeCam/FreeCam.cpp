// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include "FreeCam.hpp"

#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/FramePlan.hpp>
#include <Zahlen/Engine/GameModule.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Engine/Capabilities/InputReader.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>


#include <algorithm>
#include <cmath>
#include <string_view>

namespace ZHLN::FreeCam {
namespace {

struct FreeCamOptOutComponent {};

void Drive(Components::CameraComponent& cc, const InputReader& input, const FreeCamComponent& knobs, float dt) {
    if (!knobs.enabled) {
        return;
    }
    Camera&     cam        = cc.camera;
    const float look       = knobs.lookSensitivity;
    const bool  lookButton = knobs.lookRequiresButton ? input.MouseButtonDown(MouseButton::Right) : true;
    if (lookButton) {
        const Axis2DValue delta = input.MouseDelta();
        cam.yaw += DegF{delta.x * look};
        cam.pitch = std::clamp(cam.pitch - DegF{delta.y * look}, DegF{-89.0f}, DegF{89.0f});
    }

    const float yawRad   = cam.yaw.ToRadians().value;
    const float pitchRad = cam.pitch.ToRadians().value;
    FVec3   forward{std::cos(yawRad) * std::cos(pitchRad), std::sin(pitchRad), std::sin(yawRad) * std::cos(pitchRad)};
    forward         = forward.Normalized();
    FVec3 right = Cross(forward, FVec3::UnitY());
    if (right.LengthSquared() > 1e-8f) {
        right = right.Normalized();
    }
    const FVec3 up    = FVec3::UnitY();
    const float speed = input.KeyDown(KeyCode::LShift) ? (knobs.speed * knobs.sprintMultiplier) : knobs.speed;

    FVec3 move = FVec3::Zero();
    if (input.KeyDown(KeyCode::W)) {
        move += forward;
    }
    if (input.KeyDown(KeyCode::S)) {
        move -= forward;
    }
    if (input.KeyDown(KeyCode::A)) {
        move -= right;
    }
    if (input.KeyDown(KeyCode::D)) {
        move += right;
    }
    if (input.KeyDown(KeyCode::E) || input.KeyDown(KeyCode::Space)) {
        move += up;
    }
    if (input.KeyDown(KeyCode::Q)) {
        move -= up;
    }
    if (move.LengthSquared() > 0.0f && dt > 0.0f) {
        cam.position += move.Normalized() * speed * dt;
    }
}

Entity MainCamera(const Entities& entities) {
    const auto cameras = entities.EntitiesWith<Components::MainCameraTagComponent>();
    return cameras.empty() ? Entity::Null() : cameras.front();
}

using SeedQuery = ECS::Query<const Components::MainCameraTagComponent, const FreeCamOptOutComponent,
                             const FreeCamTagComponent, const FreeCamComponent>;
void SeedBootCamera(SeedQuery query, ECS::EntityCommandBuffer& commands) {
    const auto cameras = query.Entities<Components::MainCameraTagComponent>();
    if (cameras.empty()) {
        return;
    }
    const Entity camera = cameras.front();
    if (query.Get<FreeCamOptOutComponent>(camera)) {
        return;
    }
    if (!query.Get<FreeCamTagComponent>(camera)) {
        commands.AddComponent(camera, FreeCamTagComponent {});
    }
    if (!query.Get<FreeCamComponent>(camera)) {
        commands.AddComponent(camera, FreeCamComponent {});
    }
}

using FreeCamQuery = ECS::Query<const FreeCamTagComponent, const FreeCamComponent, Components::CameraComponent&>;
void ApplyTyped(FreeCamQuery query, ECS::Res<InputReader> input, FrameDt frameDt) {
    const InputReader& kbm = *input;
    for (Entity e: query.Entities<FreeCamTagComponent>()) {
        const auto knobs = query.Get<FreeCamComponent>(e);
        const FreeCamComponent defaults {};
        const FreeCamComponent& cfg = knobs ? *knobs : defaults;
        query.Patch<Components::CameraComponent>(e, [&](auto& cc) { Drive(cc, kbm, cfg, frameDt.value); });
    }
}

} // namespace

void Apply(Engine& engine, float dt) {
    engine.GetRenderGraph().InvokeSequential<&ApplyTyped>(engine.MakeFrame(dt, FrameAlpha {}));
}

namespace {

void FreeCamStep(Engine& engine, float dt, FrameContext& ctx) {
    const Frame frame = engine.MakeFrame(dt, ctx.alpha);
    auto& graph = engine.GetRenderGraph();
    // The pose pass below must see the newly seeded components this frame.
    graph.InvokeSequentialAndPlayback<&SeedBootCamera>(frame);
    if (dt <= 0.0f) {
        return;
    }
    graph.InvokeSequential<&ApplyTyped>(frame);
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
    auto         reg    = engine.Entities();
    const Entity camera = MainCamera(reg);
    if (camera == Entity::Null()) {
        return;
    }
    reg.Detach<FreeCamOptOutComponent>(camera);
    if (!reg.Get<FreeCamTagComponent>(camera)) {
        reg.Attach(camera, FreeCamTagComponent {});
    }
    if (!reg.Patch<FreeCamComponent>(camera, [speed, lookRequiresButton](auto& fc) -> auto {
            fc.speed              = speed;
            fc.lookRequiresButton = lookRequiresButton;
        })) {
        reg.Attach(camera, FreeCamComponent {.speed = speed, .lookRequiresButton = lookRequiresButton});
    }
}

void Detach(Engine& engine) {
    auto         reg    = engine.Entities();
    const Entity camera = MainCamera(reg);
    if (camera == Entity::Null()) {
        return;
    }
    if (!reg.Get<FreeCamOptOutComponent>(camera)) {
        reg.Attach(camera, FreeCamOptOutComponent {});
    }
    reg.Detach<FreeCamTagComponent>(camera);
    reg.Detach<FreeCamComponent>(camera);
}

} // namespace ZHLN::FreeCam
