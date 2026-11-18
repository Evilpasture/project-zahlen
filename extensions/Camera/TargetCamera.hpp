// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Input/ActionValues.hpp>
#include <Zahlen/Math/Vec3.hpp>
namespace ZHLN::COM {
struct IEngine;
struct IEntities;
struct IActionContext;
} // namespace ZHLN::COM
namespace ZHLN::CameraRig {
struct TargetComponent {
    Entity    target         = Entity::Null();
    float     distance       = 4.5f;
    float     targetDistance = 4.5f;
    float     yaw            = -90.0f;
    float     pitch          = -10.0f;
    FVec3 targetOffset   = {0.0f, 1.3f, 0.0f};
    float     stiffness      = 15.0f;

    // The renderer's vignette reads PostProcessSettingsComponent; these two
    // fields exist for authored/scripted rig state and stay part of the
    // component's FFI-visible layout.
    float vignetteIntensity = 1.10f;
    float vignettePower     = 1.50f;
    float fov               = 45.0f;
    float targetFov         = 45.0f;

    FVec3 smoothTargetPos     = FVec3::Zero();
    uint32_t  hasInitSmoothTarget = 0;
};

struct InputSnapshot {
    Axis2DValue look {};
    float       wheel      = 0;
    bool        lookButton = false;
};
auto ReadInput(const COM::IActionContext&) -> InputSnapshot;
// Each rig writes its own CameraComponent; free-cam tagged cameras are skipped.
// Prefer WorldTransform, with a local Transform fallback. No simulation step,
// command playback or matrix projection is hidden here. Call between
// IFramePipeline::AdvanceFrame and FinishFrame (or use the Application layer).
void Apply(COM::IEntities&, const InputSnapshot&, float dt);
void Step(COM::IEngine&, float dt);
struct Layer {
    void BeforeRender(COM::IEngine& engine, float dt) {
        Step(engine, dt);
    }
};
} // namespace ZHLN::CameraRig
