// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "CharacterMotorSystem.hpp"
#include <Zahlen/ECS/Components.hpp>
#include <ecs/ECS.hpp>
#include <engine/Engine.hpp>
#include <physics/Physics.hpp>
#include <algorithm>
#include <cmath>

namespace ZHLN::CharacterMotorSystem {
namespace {
auto NonNegative(float v) -> float {
    return std::isfinite(v) ? std::max(v, 0.0f) : 0.0f;
}
auto Finite(float v) -> float {
    return std::isfinite(v) ? v : 0.0f;
}
} // namespace
// These passes run exclusively around each engine-owned fixed physics substep,
// not on a host callback and not on the variable-rate simulation graph.
void PreStep(Engine& engine, float dt) {
    auto& registry = engine.Registry();
    auto& physics = engine.Physics();
    for (const Entity entity: registry.GetEntitiesWith<Components::CharacterMotorComponent>()) {
        auto motor = registry.Get<Components::CharacterMotorComponent>(entity);
        auto body  = registry.Get<Components::PhysicsComponent>(entity);
        if (!motor || !body || body->isStatic || registry.Get<Components::PendingDestroy>(entity))
            continue;
        auto ragdoll = registry.Get<Components::RagdollComponent>(entity);
        if (ragdoll && ragdoll->state != RagdollState::Inactive)
            continue;
        if (!registry.Get<Components::CharacterMotorStateComponent>(entity))
            registry.Add(entity, Components::CharacterMotorStateComponent {});
        const auto previous = registry.Get<Components::CharacterMotorStateComponent>(entity);
        if (!previous)
            continue;
        auto state           = *previous;
        state.previousFacing = state.facing;
        if (motor->driveFacing && std::isfinite(motor->desiredFacing.LengthSquared()) && motor->desiredFacing.LengthSquared() > 0.0001f)
            state.facing = motor->desiredFacing.Normalized();
        const auto character = physics.AsCharacter(body->physicsID);
        state.grounded       = character ? physics.IsCharacterOnGround(*character) : false;
        const float x      = Finite(motor->desiredVelocity.x);
        const float z      = Finite(motor->desiredVelocity.z);
        const float dx     = x - state.velocity.x;
        const float dz     = z - state.velocity.z;
        const float length = std::hypot(dx, dz);
        const float rate   = NonNegative(x * x + z * z > 0.0001f ? motor->acceleration : motor->deceleration);
        const float scale  = length > 0.0f ? std::min(rate * dt / length, 1.0f) : 0.0f;
        state.velocity.x += dx * scale;
        state.velocity.z += dz * scale;
        if (state.grounded)
            state.velocity.y = motor->jumpRequested ? NonNegative(motor->jumpSpeed) : 0.0f;
        else
            state.velocity.y -= NonNegative(motor->gravity) * dt;
        registry.Patch<Components::CharacterMotorComponent>(entity, [](auto& command) { command.jumpRequested = false; });
        registry.Patch<Components::CharacterMotorStateComponent>(entity, [&](auto& output) { output = state; });
        if (character) {
            physics.SetVelocity(*character, state.velocity);
        }
    }
}
void PostStep(Engine& engine) {
    auto& registry = engine.Registry();
    auto& physics = engine.Physics();
    for (const Entity entity: registry.GetEntitiesWith<Components::CharacterMotorComponent>()) {
        auto body  = registry.Get<Components::PhysicsComponent>(entity);
        auto state = registry.Get<Components::CharacterMotorStateComponent>(entity);
        if (!body || !state || registry.Get<Components::PendingDestroy>(entity))
            continue;
        registry.Patch<Components::CharacterMotorStateComponent>(entity, [&](auto& output) {
            if (const auto character = physics.AsCharacter(body->physicsID)) {
                output.grounded = physics.IsCharacterOnGround(*character);
                output.velocity = physics.CharacterVelocity(*character);
            }
        });
    }
}
} // namespace ZHLN::CharacterMotorSystem
