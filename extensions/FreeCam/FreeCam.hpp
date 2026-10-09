// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/FreeCam/FreeCam.hpp
//
// First-person fly camera: RMB look, WASD, Q/E/Space vertical, Shift sprint.
// Core's FreeCamTagComponent is only a marker -- CameraSystem projects
// matrices and does not interpret keys. This module is the missing policy:
// a FreeCamComponent (speed / look / sprint) plus a Camera-phase frame step
// that writes Camera position/yaw/pitch before CameraSystem runs.
#pragma once

namespace ZHLN {

class Engine;

namespace ECS {
class Registry;
}

namespace FreeCam {

struct FreeCamComponent {
    float speed             = 12.0f;
    float lookSensitivity   = 0.12f;
    float sprintMultiplier  = 3.0f;
};

// Registers FreeCamComponent and inserts the "FreeCamSystem" frame step
// immediately before core "CameraSystems". Call once after Engine::Create
// and before InitializeDefaultScene so the first schedule includes it.
void Install(Engine& engine);

// Puts FreeCamTag + FreeCamComponent on @p camera (the main camera if Null).
// Safe to call after the scene exists; overwrites speed knobs if already attached.
void Attach(Engine& engine, float speed = 12.0f);

} // namespace FreeCam
} // namespace ZHLN
