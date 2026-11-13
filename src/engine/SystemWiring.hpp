// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Engine/FramePlan.hpp>

namespace ZHLN {

class Engine;
namespace ECS {
class Registry;
}

// Core's own frame steps. Runs once per engine; see the definition.
void BuildCoreFramePlan(FramePlan& plan, FrameStepFn presentStep);
void BuildSystemGraphs(Engine& engine);

// Ensures core's components are registered and the baseline empty-world
// entities (InputStateComponent, MainCameraTagComponent + CameraComponent,
// DebugSettingsComponent) exist.
void SeedBaselineScene(ECS::Registry& reg);

}
