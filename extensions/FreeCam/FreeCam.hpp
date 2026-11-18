// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Input/ActionValues.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Vec3.hpp>
namespace ZHLN::COM {
struct IEngine;
struct IEntities;
struct IActionContext;
} // namespace ZHLN::COM
namespace ZHLN::FreeCam {
struct TagComponent {};

struct Component {
    float speed            = 12.0f;
    float lookSensitivity  = 0.12f;
    float sprintMultiplier = 3.0f;
    bool  enabled          = true;
    // When true (the default) look motion is read only while RMB is held, which
    // is what a desktop host with a visible cursor wants. A host that captures
    // the mouse -- Window::CaptureMouse(true), GLFW_CURSOR_DISABLED -- has no
    // cursor to drag with, and the deltas arrive whether or not a button is
    // down, so it sets this false and looks freely.
    bool  lookRequiresButton = true;
};

// Host-owned snapshot. Capturing input and stepping can be scheduled separately;
// no device injection, Engine module, graph, or Registry is involved.
struct InputSnapshot {
    FVec3       movement {}; // right, up, forward; normalized by Drive
    Axis2DValue look {};
    bool        lookButton = false;
    bool        sprint     = false;
};
auto ReadInput(const COM::IActionContext& input) -> InputSnapshot;
void Drive(Camera& camera, const Component& knobs, const InputSnapshot& input, float dt);
// Apply only drives already attached cameras; Step also seeds the boot camera.
void Apply(COM::IEntities&, const InputSnapshot&, float dt);
void Step(COM::IEngine&, float dt);
bool Attach(COM::IEntities&, float speed = 12.0f, bool lookRequiresButton = true);
void Detach(COM::IEntities&);
// Explicit Application::AddLayer composition. No automatic installation.
struct Layer {
    void BeforeRender(COM::IEngine& engine, float dt) {
        Step(engine, dt);
    }
};
} // namespace ZHLN::FreeCam
