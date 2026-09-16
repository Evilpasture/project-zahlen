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

    // Character locomotion rides the substep through the engine's
    // CharacterStepHooks (installed by extras/CharacterController): preStep
    // integrates locomotion and commits CharacterVirtual velocities, postStep
    // reads the grounded flags back. With no controller installed both are
    // null and physics steps pure -- impulses and rigid/character bodies
    // only. The order around Step() is unchanged from the inline version.
    const auto& character = engine.GetCharacterStepHooks();

    {
        ZHLN::ScopedTimer profTimer("ECS System: Physics & Movement");
        while (_accumulator >= _targetDt) {
            if (character.preStep != nullptr) {
                character.preStep(engine, _targetDt);
            }
            CommitImpulses(engine);
            engine.GetPhysicsContext().Step(_targetDt);
            if (character.postStep != nullptr) {
                character.postStep(engine);
            }

            _accumulator -= _targetDt;
        }
    }

    engine.GetCurrentAlpha() = _accumulator / _targetDt;
}

} // namespace ZHLN
