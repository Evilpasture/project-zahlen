// src/engine/system/PhysicsSystem.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsSystem.hpp"
#include "PhysicsStateSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Profiler.hpp>
#include <algorithm>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>

namespace ZHLN {

void MovementSystem(Engine& engine, float dt);

void AccumulateImpulse(ECS::Registry& registry, Entity entity, float x, float y, float z) {
    const JPH::Vec3 linear(x, y, z);
    if (auto* cmd = registry.Get<Components::ImpulseCommand>(entity)) {
        cmd->linear += linear;
        return;
    }
    if (!registry.IsAlive(entity)) {
        return;
    }
    registry.Add(entity, Components::ImpulseCommand {.linear = linear});
}

namespace {

void CommitCharacterSteering(Engine& engine) {
    auto& reg      = engine.GetRegistry();
    auto& pc       = engine.GetPhysicsContext();
    auto  entities = reg.GetEntitiesWith<Components::MovementComponent>();
    auto  moves    = reg.GetRawArray<Components::MovementComponent>();
    for (size_t i = 0; i < entities.size(); ++i) {
        const auto* phys = reg.Get<Components::PhysicsComponent>(entities[i]);
        if (phys == nullptr) {
            continue;
        }
        const auto& move = moves[i];
        pc.SetCharacterVelocity(phys->physicsHandle, JPH::Vec3(move.currentVelX, move.currentYVel, move.currentVelZ));
    }
}

void WriteCharacterGrounded(Engine& engine) {
    auto& reg      = engine.GetRegistry();
    auto& pc       = engine.GetPhysicsContext();
    auto  entities = reg.GetEntitiesWith<Components::MovementComponent>();
    auto  moves    = reg.GetRawArray<Components::MovementComponent>();
    for (size_t i = 0; i < entities.size(); ++i) {
        const auto* phys = reg.Get<Components::PhysicsComponent>(entities[i]);
        if (phys == nullptr) {
            continue;
        }
        moves[i].wasGrounded = moves[i].isGrounded;
        moves[i].isGrounded  = pc.IsCharacterOnGround(phys->physicsHandle);
    }
}

void CommitImpulses(Engine& engine) {
    auto&       reg      = engine.GetRegistry();
    auto&       pc       = engine.GetPhysicsContext();
    const auto  entities = reg.GetEntitiesWith<Components::ImpulseCommand>();
    for (int i = static_cast<int>(entities.size()) - 1; i >= 0; --i) {
        const Entity e    = entities[static_cast<size_t>(i)];
        const auto*  cmd  = reg.Get<Components::ImpulseCommand>(e);
        const auto*  phys = reg.Get<Components::PhysicsComponent>(e);
        if (cmd != nullptr && phys != nullptr && cmd->linear.LengthSq() > 0.0f) {
            pc.AddImpulse(phys->physicsHandle, cmd->linear);
        }
        reg.Remove<Components::ImpulseCommand>(e);
    }
}

} // namespace

void PhysicsSystem::Update(Engine& engine, float dt) noexcept {
    PhysicsStateSystem::Reconcile(engine);

    float cappedDt = std::min(dt, 0.1f);
    _accumulator += cappedDt;

    _accumulator = std::min(_accumulator, _targetDt * 4.0f);

    {
        ZHLN::ScopedTimer profTimer("ECS System: Physics & Movement");
        while (_accumulator >= _targetDt) {
            MovementSystem(engine, _targetDt);
            CommitCharacterSteering(engine);
            CommitImpulses(engine);
            engine.GetPhysicsContext().Step(_targetDt);
            WriteCharacterGrounded(engine);

            _accumulator -= _targetDt;
        }
    }

    engine.GetCurrentAlpha() = _accumulator / _targetDt;
}

} // namespace ZHLN
