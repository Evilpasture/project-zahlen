// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Profiler.hpp>
#include <algorithm>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>

namespace ZHLN {

void AccumulateImpulse(ECS::Registry& registry, Entity entity, float x, float y, float z) {
    const JPH::Vec3 linear(x, y, z);
    if (auto cmd = registry.Get<Components::ImpulseCommand>(entity)) {
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
        const auto   cmd  = reg.Get<Components::ImpulseCommand>(e);
        const auto   phys = reg.Get<Components::PhysicsComponent>(e);
        if (cmd && phys && cmd->linear.LengthSq() > 0.0f) {
            pc.AddImpulse(phys->physicsHandle, cmd->linear);
        }
        reg.Remove<Components::ImpulseCommand>(e);
    }
}

}

void PhysicsSystem::Update(Engine& engine, float dt, float& accumulator) noexcept {
    float cappedDt = std::min(dt, 0.1f);
    accumulator += cappedDt;

    accumulator = std::min(accumulator, TargetDt * 4.0f);

    const auto& character = engine.GetCharacterStepHooks();

    {
        ZHLN::ScopedTimer profTimer("ECS System: Physics & Movement");
        while (accumulator >= TargetDt) {
            if (character.preStep != nullptr) {
                character.preStep(engine, TargetDt);
            }
            CommitImpulses(engine);
            engine.GetPhysicsContext().Step(TargetDt);
            if (character.postStep != nullptr) {
                character.postStep(engine);
            }

            accumulator -= TargetDt;
        }
    }

    engine.GetCurrentAlpha() = accumulator / TargetDt;
}

}
