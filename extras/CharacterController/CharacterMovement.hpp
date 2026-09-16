// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/CharacterController/CharacterMovement.hpp
#pragma once

namespace ZHLN {

class Engine;

namespace Character {

/// One fixed physics substep of character locomotion: gravity/jump
/// integration, velocity steering toward the input direction, and yaw
/// slewing toward travel direction. Moved out of core together with
/// MovementComponent -- it IS the overgrowth-style controller.
void MovementSystem(Engine& engine, float dt);

/// Pushes MovementComponent velocities into their CharacterVirtual bodies.
/// Runs before PhysicsContext::Step inside the substep loop.
void CommitCharacterSteering(Engine& engine);

/// Reads the CharacterVirtual grounded flags back into MovementComponent
/// after PhysicsContext::Step.
void WriteCharacterGrounded(Engine& engine);

} // namespace Character
} // namespace ZHLN
