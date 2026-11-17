// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "CharacterComponents.hpp"
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Input/Input.hpp>
namespace ZHLN::COM {
struct IEngine;
struct IEntities;
struct IActionContext;
struct ISimulation;
} // namespace ZHLN::COM
namespace ZHLN::Character {
// Host policy only. The engine owns fixed-step gravity, integration, character
// velocity commits, grounded readback, and render interpolation. Do not combine
// this driver and NativeCharacterController on the same entity/world.
struct HostCharacterComponent {
    float gravity      = 32.0f;
    float turnSpeed    = 10.0f;
    float pushStrength = 50.0f;
    float pushRadius   = 0.75f;
    bool  enabled      = true;
};
struct InputSnapshot {
    InputState  keyboardMouse {};
    StickSample leftStick {}, rightStick {};
    bool        keyboardAssigned = true;
    bool        gamepadAssigned  = false;
    bool        gamepadJump      = false;
    bool        gamepadSprint    = false;
};
// Pure arithmetic shared with the retained native input adapter.
auto TranslateSeatIntent(const InputSnapshot&) noexcept -> SeatIntent;
auto ReadInput(const COM::IActionContext&, PlayerSeat seat = kPrimarySeat) -> SeatIntent;
// Attach requires an existing live entity. No physics body is created,
// overwritten or retired here. These calls explicitly own the host/motor data.
bool Attach(COM::IEntities&, Entity, const MovementComponent& = {}, const HostCharacterComponent& = {});
void Detach(COM::IEntities&, Entity);
// Feed an explicit snapshot (AI, replay, deterministic tests), or call Step to
// read each entity's routed seat and the previous resolved main-camera yaw.
void Apply(COM::IEntities&, Entity, const SeatIntent&, float cameraYawDegrees, float dt, bool freeze = false);
void PushProps(COM::IEntities&, COM::ISimulation&, float dt);
void Step(COM::IEngine&, float dt);
struct Layer {
    void Tick(COM::IEngine& engine, float dt) {
        Step(engine, dt);
    }
    void Stop(COM::IEngine& engine);
};
} // namespace ZHLN::Character
