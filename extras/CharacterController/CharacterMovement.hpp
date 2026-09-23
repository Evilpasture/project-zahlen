// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/CharacterController/CharacterMovement.hpp
#pragma once

namespace ZHLN {

class Engine;
class PhysicsContext;

namespace ECS {
class Registry;
} // namespace ECS

namespace Character {

// One fixed physics substep of character locomotion: gravity/jump
// integration, velocity steering toward the input direction, and yaw
// slewing toward travel direction. Moved out of core together with
// MovementComponent -- it IS the overgrowth-style controller.
void MovementSystem(Engine& engine, float dt);

// Pushes MovementComponent velocities into their CharacterVirtual bodies.
// Runs before PhysicsContext::Step inside the substep loop.
void CommitCharacterSteering(Engine& engine);

// Character-on-prop interaction policy: every fixed step, a character that
// is moving through dynamic rigid bodies applies an impulse along its travel
// direction, so walking into a box pushes it. This used to be a hardcoded
// impulse formula in the low-level character contact listener; the engine
// core exerts no forces on bodies from character contacts any more, and the
// policy lives in the controller that owns the character. Takes the physics
// context and registry directly so standalone (engine-free) physics tests
// can drive it the same way the substep hook does.
void PushProps(PhysicsContext& pc, ECS::Registry& reg);

// Reads the CharacterVirtual grounded flags back into MovementComponent
// after PhysicsContext::Step.
void WriteCharacterGrounded(Engine& engine);

} // namespace Character
} // namespace ZHLN
