// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/FreeCam/FreeCam.hpp
//
// First-person fly camera: RMB look, WASD, Q/E/Space vertical, Shift sprint.
// This tag used to live in core Components as a marker CameraSystem ignored.
// The policy (speed / look / sprint) and the Camera-phase step that writes
// Camera pose live here. Core's default scene no longer attaches the tag;
// hosts that want a fly cam Install (before InitializeDefaultScene) and Attach
// (after the scene exists, or let the frame step seed the boot camera).
#pragma once

#include <Zahlen/Engine/GameModule.hpp>

namespace ZHLN {

class Engine;

namespace FreeCam {

struct FreeCamTagComponent {};

struct FreeCamComponent {
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

// The module as a value: what the host installs, and what a host that wants to
// report a placement failure installs itself instead of calling Install.
auto Describe() -> Module;

void Install(Engine& engine);

// FreeCamTag + FreeCamComponent on the main camera. Idempotent. Pass
// lookRequiresButton = false for a captured-mouse host (see the component).
void Attach(Engine& engine, float speed = 12.0f, bool lookRequiresButton = true);

// Strip the tag and knobs from the main camera so a follow-cam / orbit
// host can own pose. Safe if nothing was attached.
void Detach(Engine& engine);

// Drive every tagged camera. The scheduler step calls this; hosts that
// Tick(0) (editor paused) call it themselves with a real dt.
void Apply(Engine& engine, float dt);

} // namespace FreeCam
} // namespace ZHLN
