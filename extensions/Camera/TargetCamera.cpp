// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Camera/TargetCamera.cpp
#include <Zahlen/Engine/Internal/EngineAccess.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Engine/Capabilities/EngineState.hpp>
#include "TargetCamera.hpp"

#include "Zahlen/Math/Camera.hpp"
#include "Zahlen/ECS/Components.hpp"
#include "Zahlen/Engine/Engine.hpp"
#include "Zahlen/Engine/FramePlan.hpp"
#include "Zahlen/Engine/GameModule.hpp"
#include "Zahlen/Input/Input.hpp"
#include "Zahlen/Log.hpp"
#include <FreeCam/FreeCam.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <algorithm>
#include <cmath>
#include <string_view>

namespace ZHLN::Tests {
static void VerifyCameraInterpolation(const Camera& cam, float alpha) noexcept {
    static bool testsRun = false;
    if (testsRun) {
        return;
    }
    testsRun = true;

    if (!std::isfinite(cam.position.x) || !std::isfinite(cam.position.y) || !std::isfinite(cam.position.z)) {
        ZHLN::Log(
            "[Test Fail] Camera Interpolation: Camera position contains NaN/Inf "
            "({:.3f}, {:.3f}, {:.3f})",
            cam.position.x, cam.position.y, cam.position.z
        );
    }
    if (cam.fov < 1.0f || cam.fov > 180.0f) {
        ZHLN::Log("[Test Fail] Camera Interpolation: FOV out of range: {:.2f}", cam.fov);
    }
    if (cam.pitch < -90.0f || cam.pitch > 90.0f) {
        ZHLN::Log("[Test Fail] Camera Interpolation: Pitch out of range: {:.2f}", cam.pitch);
    }
    if (alpha < 0.0f || alpha > 1.0f) {
        ZHLN::Log("[Test Fail] Camera Interpolation: Alpha out of bounds [0,1]: {:.4f}", alpha);
    }
}
} // namespace ZHLN::Tests

namespace ZHLN::CameraRig {

void TargetCameraSystem::Update(ECS::Registry& reg, Camera& cam, float dt, float alpha) noexcept {
    auto cameraEntities = reg.GetEntitiesWith<TargetCameraComponent>();
    if (cameraEntities.empty()) {
        return;
    }

    Entity camEnt = cameraEntities[0];

    reg.Patch<TargetCameraComponent>(camEnt, [&](auto& camComp) -> auto {
        // Fly-cam owns pose while the tag is present; skip the orbit.
        if (reg.Get<FreeCam::FreeCamTagComponent>(camEnt)) {
            return;
        }

        Entity targetEnt = camComp.target;
        if (!reg.IsAlive(targetEnt)) {
            return;
        }

        FVec3 targetPos = FVec3::Zero();

        // 2. TARGET POSITION RESOLUTION (Always smoothly interpolate using alpha)
        bool foundPos = reg.Patch<Components::WorldTransformComponent>(targetEnt, [&](const auto& worldTrans) -> auto {
            const auto& world = worldTrans.world;
            targetPos = {world[0, 3], world[1, 3], world[2, 3]};
        });

        if (!foundPos) {
            reg.Patch<Components::TransformComponent>(targetEnt, [&](const auto& trans) -> auto { targetPos = trans.position; });
        }

        // 3. ZOOM & FOV SMOOTHING
        auto  inputState = reg.GetSingleton<Components::InputStateComponent>();
        float wheelDelta = inputState ? inputState->GetMouseWheel() : 0.0f;
        if (std::abs(wheelDelta) > 0.01f) {
            camComp.targetDistance = std::clamp(camComp.targetDistance - wheelDelta * 0.5f, 1.5f, 15.0f);
        }

        if (camComp.stiffness > 0.0f) {
            float factor = std::clamp(camComp.stiffness * dt, 0.0f, 1.0f);
            camComp.distance += (camComp.targetDistance - camComp.distance) * factor;
            camComp.fov += (camComp.targetFov - camComp.fov) * factor;
        } else {
            camComp.distance = camComp.targetDistance;
            camComp.fov      = camComp.targetFov;
        }

        // 4. MOUSE LOOK ORBITING
        const float sensitivity = 0.15f;
        if (inputState && inputState->IsMouseButtonDown(static_cast<uint8_t>(KeyCode::RButton))) {
            camComp.yaw += inputState->GetMouseDeltaX() * sensitivity;
            camComp.pitch = std::clamp(camComp.pitch - (inputState->GetMouseDeltaY() * sensitivity), -89.0f, 89.0f);
        }

        cam.yaw   = camComp.yaw;
        cam.pitch = camComp.pitch;
        cam.fov   = camComp.fov;

        // 5. EXPONENTIAL SMOOTHING & FINAL POSITION
        float     yawRad   = DegF(camComp.yaw).ToRadians().value;
        float     pitchRad = DegF(camComp.pitch).ToRadians().value;
        FVec3 offsetDir(std::cos(yawRad) * std::cos(pitchRad), std::sin(pitchRad), std::sin(yawRad) * std::cos(pitchRad));

        if (camComp.hasInitSmoothTarget == 0) {
            camComp.smoothTargetPos     = targetPos;
            camComp.hasInitSmoothTarget = 1;
        }

        if ((targetPos - camComp.smoothTargetPos).LengthSquared() > 100.0f) {
            camComp.smoothTargetPos = targetPos; // Teleport instantly on large displacements
        } else if (camComp.stiffness > 0.0f) {
            float factor = 1.0f - std::exp(-camComp.stiffness * dt);
            camComp.smoothTargetPos += (targetPos - camComp.smoothTargetPos) * factor;
        } else {
            camComp.smoothTargetPos = targetPos;
        }

        cam.position = camComp.smoothTargetPos - (offsetDir.Normalized() * camComp.distance) + camComp.targetOffset;
    });

    if constexpr (isDev) {
        ZHLN::Tests::VerifyCameraInterpolation(cam, alpha);
    }
}

} // namespace ZHLN::CameraRig

namespace ZHLN {
namespace {

// The rig's frame step: keep the boot camera's component present (core no
// longer creates any camera rig -- that is gameplay policy), then resolve
// the orbit before the core CameraSystem projects the matrices.
void TargetCameraStep(Engine& engine, float dt, FrameContext& ctx) {
    auto& reg = ZHLN::EngineAccess::RegistryOf(engine);

    if (auto mainCams = reg.GetEntitiesWith<Components::MainCameraTagComponent>(); !mainCams.empty()) {
        const Entity camEnt = mainCams[0];
        if (!reg.Get<CameraRig::TargetCameraComponent>(camEnt)) {
            // Boot rig: the same defaults the core default scene used to
            // create, so the default view (a free-cam host keeps its
            // FreeCamTag) keeps its pre-decomposition framing.
            reg.Add(
                camEnt,
                CameraRig::TargetCameraComponent {
                    .distance          = 4.5f,
                    .targetDistance    = 4.5f,
                    .yaw               = -90.0f,
                    .pitch             = -10.0f,
                    .stiffness         = 15.0f,
                    .vignetteIntensity = 1.10f,
                    .vignettePower     = 1.50f,
                    .fov               = 45.0f,
                    .targetFov         = 45.0f
                }
            );
        }
    }

    static CameraRig::TargetCameraSystem sys;
    // The rig drives the world's camera; a world without one has nothing to drive.
    // The borrow is the component itself, so the system's updates land in the
    // registry without a copy to hand back.
    if (const auto camera = reg.GetSingleton<Components::CameraComponent>()) {
        sys.Update(reg, camera->camera, dt, ctx.alpha);
    }
}

// The rig's step, declared. It runs before the core "CameraSystems" step
// because the rig writes Camera position, yaw, pitch and fov, and the matrices
// CameraSystem projects have to come from that result.
//
// The step name is also the idempotency key: FramePlan::Compose rejects a
// duplicate name, so a second install of this module is a reported
// FramePlanError::DuplicateName instead of a second copy of the step. The scan
// this replaces -- walk the schedule, return early if the name is there --
// existed because an unused install had to be tolerated; a duplicate is now
// something the installer has to answer for.
constexpr ModuleStep kFrameSteps[] = {
    {
        .phase     = FramePhase::Camera,
        .name      = "TargetCameraSystem",
        .run       = &TargetCameraStep,
        .placement = StepPlacement::Before,
        .anchor    = "CameraSystems",
    },
};

void RegisterComponents(ECS::Registry& reg) {
    reg.RegisterComponent<CameraRig::TargetCameraComponent>();
}

} // namespace

auto CameraRig::Describe() -> Module {
    return Module{
        .name               = "CameraRig",
        .registerComponents = &RegisterComponents,
        .frameSteps         = kFrameSteps,
    };
}

void CameraRig::Install(Engine& engine) {
    if (auto installed = engine.InstallModule(Describe()); !installed) {
        LogError("[CameraRig] module install failed: {}", ToString(installed.error()));
    }
}

} // namespace ZHLN
