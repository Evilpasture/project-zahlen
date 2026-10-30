// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Camera/TargetCamera.hpp
//
// Third-person target camera: a spring-arm orbit rig that follows a tracked
// entity (zoom, pitch clamps, exponential smoothing), plus the free-cam
// branch its boot configuration selects. Camera rig behavior is gameplay
// policy, not render substrate: the core keeps the minimal Camera (optics,
// position, orientation, frustum), CameraComponent (view-projection
// matrices) and CameraSystem (matrix projection), and this module contributes
// the rig as a frame step through the engine's extension seams.
#pragma once

// clang-format off
#include <Zahlen/Physics/Jolt.hpp>
// clang-format on
#include <Jolt/Math/Vec3.h>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Engine/GameModule.hpp>
#include <optional>

namespace ZHLN {

class Engine;

namespace ECS {
class Registry;
} // namespace ECS

struct Camera;

namespace CameraRig {

// Spring-arm orbit camera state for one camera entity.
struct TargetCameraComponent {
    Entity    target         = Entity::Null();
    float     distance       = 4.5f;
    float     targetDistance = 4.5f;
    float     yaw            = -90.0f;
    float     pitch          = -10.0f;
    JPH::Vec3 targetOffset   = JPH::Vec3(0.0f, 1.3f, 0.0f);
    float     stiffness      = 15.0f;

    // The renderer's vignette reads PostProcessSettingsComponent; these two
    // fields exist for authored/scripted rig state and stay part of the
    // component's FFI-visible layout.
    float vignetteIntensity = 1.10f;
    float vignettePower     = 1.50f;
    float fov               = 45.0f;
    float targetFov         = 45.0f;

    JPH::Vec3 smoothTargetPos     = JPH::Vec3::sZero();
    uint32_t  hasInitSmoothTarget = 0;
};

// Resolves the rig's orbit around its tracked entity and writes the result
// into the engine Camera. Runs as a frame step in the Camera phase, before
// the core CameraSystem projects view-projection matrices, so the rig's
// position and orientation are what the frame is actually rasterized with.
class TargetCameraSystem {
  public:
    TargetCameraSystem()                                     = default;
    ~TargetCameraSystem()                                    = default;
    TargetCameraSystem(const TargetCameraSystem&)            = delete;
    TargetCameraSystem(TargetCameraSystem&&)                 = default;
    TargetCameraSystem& operator=(const TargetCameraSystem&) = delete;
    TargetCameraSystem& operator=(TargetCameraSystem&&)      = default;

    // The rig's orbit speed is this component's own state (see
    // TargetCameraComponent::stiffness and the entity's follow speed). It used to
    // take a `speedQuery` parameter — a function pointer the engine held and
    // handed out — and the body opened with `(void)speedQuery;`: the hook fed a
    // parameter nothing read. Deleted rather than redesigned, because the
    // behaviour it was meant to provide (share the tracked entity's speed) has a
    // component-shaped answer if it is ever wanted: the field belongs on the
    // target entity, where a camera rig can read it, not on Engine.
    void Update(ECS::Registry& reg, Camera& cam, float dt, float alpha) noexcept;
};

// Composition-root entry point: registers TargetCameraComponent and installs
// the module Describe() returns, whose frame step is placed before the core
// "CameraSystems" step so it runs first. The step also re-seeds the boot
// camera's component when a scene reset replaced the default camera (core no
// longer creates any camera rig). Call once after Engine::Create. The plan is
// composed once and is not rebuilt with the scene, so a second install reports
// a duplicate step instead of registering a second copy of it.
// The module as a value: registers TargetCameraComponent and declares the rig's
// frame step, placed before core's "CameraSystems". See TargetCamera.cpp.
auto Describe() -> Module;

void Install(Engine& engine);

} // namespace CameraRig
} // namespace ZHLN
