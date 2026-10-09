// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Camera/TargetCamera.cpp
#include "TargetCamera.hpp"

#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/FrameScheduler.hpp"
#include "Zahlen/Input.hpp"
#include "Zahlen/Log.hpp"
#include <FreeCam/FreeCam.hpp>
#include <Zahlen/ecs/ECS.hpp>
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

    if (!std::isfinite(cam.position.GetX()) || !std::isfinite(cam.position.GetY()) || !std::isfinite(cam.position.GetZ())) {
        ZHLN::Log(
            "[Test Fail] Camera Interpolation: Camera position contains NaN/Inf "
            "({:.3f}, {:.3f}, {:.3f})",
            cam.position.GetX(), cam.position.GetY(), cam.position.GetZ()
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

void TargetCameraSystem::Update(
    ECS::Registry& reg, Camera& cam, float dt, float alpha, std::optional<float> (*speedQuery)(ECS::Registry&, Entity)
) noexcept {
    (void)speedQuery;
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

        JPH::Vec3 targetPos = JPH::Vec3::sZero();

        // 2. TARGET POSITION RESOLUTION (Always smoothly interpolate using alpha)
        bool foundPos = reg.Patch<Components::WorldTransformComponent>(targetEnt, [&](const auto& worldTrans) -> auto {
            targetPos = worldTrans.world.GetTranslation();
        });

        if (!foundPos) {
            reg.Patch<Components::TransformComponent>(targetEnt, [&](const auto& trans) -> auto { targetPos = trans.position; });
        }

        // 3. ZOOM & FOV SMOOTHING
        auto  inputState = reg.GetSingleton<Components::InputStateComponent>();
        float wheelDelta = inputState ? inputState->GetMouseWheel() : 0.0f;
        if (std::abs(wheelDelta) > 0.01f) {
            camComp.targetDistance = JPH::Clamp(camComp.targetDistance - wheelDelta * 0.5f, 1.5f, 15.0f);
        }

        if (camComp.stiffness > 0.0f) {
            float factor = JPH::Clamp(camComp.stiffness * dt, 0.0f, 1.0f);
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
        float     yawRad   = JPH::DegreesToRadians(camComp.yaw);
        float     pitchRad = JPH::DegreesToRadians(camComp.pitch);
        JPH::Vec3 offsetDir(JPH::Cos(yawRad) * JPH::Cos(pitchRad), JPH::Sin(pitchRad), JPH::Sin(yawRad) * JPH::Cos(pitchRad));

        if (camComp.hasInitSmoothTarget == 0) {
            camComp.smoothTargetPos     = targetPos;
            camComp.hasInitSmoothTarget = 1;
        }

        if ((targetPos - camComp.smoothTargetPos).LengthSq() > 100.0f) {
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
void TargetCameraStep(Engine& engine, float dt, FrameContext& /*ctx*/) {
    auto& reg = engine.GetRegistry();

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
        sys.Update(reg, camera->camera, dt, engine.GetCurrentAlpha(), engine.GetFreeCamSpeedQuery());
    }
}

void AddFrameStep(FrameScheduler& scheduler) {
    // Install is idempotent at the seam: a double install (a host that
    // re-wires, a test that re-installs on a pooled engine) must not register
    // the step twice on the next rebuild.
    for (const auto& step: scheduler.GetSteps()) {
        if (std::string_view(step.name) == "TargetCameraSystem") {
            return;
        }
    }

    // Before the core "CameraSystems" step: the rig writes Camera position,
    // yaw, pitch and fov, and the matrices CameraSystem projects must come
    // from that result. Without the anchor (a host trimmed the core schedule)
    // appending still keeps the rig running, just late.
    if (!scheduler.InsertBefore("CameraSystems", FramePhase::Camera, "TargetCameraSystem", &TargetCameraStep)) {
        scheduler.Add(FramePhase::Camera, "TargetCameraSystem", &TargetCameraStep);
    }
}

} // namespace

void CameraRig::Install(Engine& engine) {
    auto& reg = engine.GetRegistry();
    reg.RegisterComponent<CameraRig::TargetCameraComponent>();
    engine.AddFrameSchedulerExtension(&AddFrameStep);
}

} // namespace ZHLN
