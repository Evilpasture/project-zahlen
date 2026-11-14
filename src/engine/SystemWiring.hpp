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
//
// `presentStep` and `uiInputStep` are handed in rather than written here: both
// reach the engine's private PIMPL -- the render context and pose queues for
// one, the input router for the other -- and that PIMPL is not visible in this
// translation unit. A FrameStepFn carries no userdata, so a step that needs
// engine-private state has to be a function defined where that state is visible
// (Engine.cpp) and placed here by pointer. Same rule for both.
void BuildCoreFramePlan(FramePlan& plan, FrameStepFn presentStep, FrameStepFn uiInputStep);

// Registers core's graph systems, then every installed module's, and compiles
// both graphs. Runs when the graphs are first built and again when a module
// marked them dirty.
void BuildSystemGraphs(Engine& engine);

// Ensures the baseline default camera entity (MainCameraTagComponent +
// CameraComponent) exists when the world is created or cleared.
void SeedBaselineScene(ECS::Registry& reg);

}
