// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/CharacterController/CharacterMovement.hpp
#pragma once

#include "CharacterComponents.hpp"
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>
#include <Zahlen/Engine/Frame.hpp>

namespace ZHLN {
class PhysicsContext;

namespace Character {

using MovementQuery = ECS::Query<MovementComponent&, const Components::PhysicsComponent, const Components::RagdollComponent>;
using CharacterReadQuery = ECS::Query<const MovementComponent, const Components::PhysicsComponent>;
using CharacterGroundedQuery = ECS::Query<MovementComponent&, const Components::PhysicsComponent>;

// Fixed-substep passes. The frame owner invokes them sequentially around the
// physics step, so character steering and prop impulses are available before
// stepping and grounded state is written immediately afterward.
void MovementSystem(MovementQuery query, FrameDt dt);
void CommitCharacterSteering(CharacterReadQuery query, ECS::ResMut<PhysicsContext> physics);
void PushProps(CharacterReadQuery query, ECS::ResMut<PhysicsContext> physics);
void WriteCharacterGrounded(CharacterGroundedQuery query, ECS::Res<PhysicsContext> physics);

} // namespace Character
} // namespace ZHLN
