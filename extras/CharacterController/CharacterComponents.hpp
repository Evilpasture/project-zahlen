// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/CharacterController/CharacterComponents.hpp
#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Quat.h>

namespace ZHLN::Character {

// Character locomotion state: what the controller integrates every physics
// substep. The constants (speed 7, sprint 1.65x, jump force 12, gravity 32
// in the integrator) are this controller's tuning, not engine substrate --
// core keeps PhysicsContext::CreateCharacter / CharacterVirtual and the raw
// InputStateComponent; interpreting keys into this state lives here.
struct MovementComponent {
    JPH::Quat orientation     = JPH::Quat::sIdentity();
    JPH::Quat prevOrientation = JPH::Quat::sIdentity();

    float inputX           = 0.0f;
    float inputZ           = 0.0f;
    float currentYVel      = 0.0f;
    float currentVelX      = 0.0f;
    float currentVelZ      = 0.0f;
    float speed            = 7.0f;
    float sprintMultiplier = 1.65f;
    float jumpForce        = 12.0f;
    float landingTimer     = 0.0f;
    float jumpDelayTimer   = 0.0f;
    float acceleration     = 25.0f;
    float deceleration     = 30.0f;

    bool jumpRequested = false;
    bool isGrounded    = true;
    bool wasGrounded   = true;
    bool isSprinting   = false;
};

// Per-entity raw-intent buffer: the input phase writes device state into it,
// the player-intent phase consumes it relative to the camera. Look deltas
// and zoom are kept for scripted cameras that read them through Lua.
struct InputComponent {
    float localMoveX     = 0.0f;
    float localMoveZ     = 0.0f;
    float lookYawDelta   = 0.0f;
    float lookPitchDelta = 0.0f;
    float zoomDelta      = 0.0f;
    bool  wantsToJump    = false;
    bool  wantsToSprint  = false;
};

} // namespace ZHLN::Character
