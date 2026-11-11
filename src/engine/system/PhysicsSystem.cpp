// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Engine/Internal/EngineAccess.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include "PhysicsSystem.hpp"
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Core/Profiler.hpp>
#include <algorithm>
#include <Zahlen/ECS/ECS.hpp>

namespace ZHLN {

void AccumulateImpulse(ECS::Registry& registry, Entity entity, float x, float y, float z) {
    if (auto cmd = registry.Get<Components::ImpulseCommand>(entity)) {
        cmd->linear += {x, y, z};
        return;
    }
    if (!registry.IsAlive(entity)) {
        return;
    }
    registry.Add(entity, Components::ImpulseCommand {.linear = {x, y, z}});
}

namespace {

void CommitImpulses(Engine& engine) {
    auto&       reg      = ZHLN::EngineAccess::RegistryOf(engine);
    auto pc = engine.Physics();
    const auto  entities = reg.GetEntitiesWith<Components::ImpulseCommand>();
    for (int i = static_cast<int>(entities.size()) - 1; i >= 0; --i) {
        const Entity e    = entities[static_cast<size_t>(i)];
        const auto   cmd  = reg.Get<Components::ImpulseCommand>(e);
        const auto   phys = reg.Get<Components::PhysicsComponent>(e);
        if (cmd && phys) {
            const auto impulse = cmd->linear;
            if (impulse.LengthSquared() > 0.0f) {
                pc.AddImpulse(phys->physicsID, impulse);
            }
        }
        reg.Remove<Components::ImpulseCommand>(e);
    }
}

}

void PhysicsSystem::Update(Engine& engine, uint32_t fixedSteps) noexcept {
    const auto& character = engine.GetPhysicsSubstepHooks();

    {
        ZHLN::ScopedTimer profTimer("ECS System: Physics & Movement");
        for (uint32_t step = 0; step < fixedSteps; ++step) {
            if (character.preStep != nullptr) {
                character.preStep(engine, TargetDt);
            }
            CommitImpulses(engine);
            engine.Physics().Step(TargetDt);
            if (character.postStep != nullptr) {
                character.postStep(engine);
            }
        }
    }
}

}
