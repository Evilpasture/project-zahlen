// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsSystem.hpp"
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#include <Zahlen/Physics/Physics.hpp>

namespace ZHLN {

void PhysicsSystem::CommitImpulses(ImpulseQuery query, ECS::EntityCommandBuffer& commands, ECS::ResMut<PhysicsContext> physics) {
    const auto entities = query.Entities<Components::ImpulseCommand>();
    // Match the old swap-removal traversal and consume even commands without a
    // physics component or with a zero impulse.
    for (size_t i = entities.size(); i > 0; --i) {
        const Entity e = entities[i - 1];
        const auto cmd = query.Get<Components::ImpulseCommand>(e);
        const auto phys = query.Get<Components::PhysicsComponent>(e);
        if (cmd && phys && cmd->linear.LengthSquared() > 0.0f) {
            physics->AddImpulse(phys->physicsID, cmd->linear);
        }
        commands.RemoveComponent<Components::ImpulseCommand>(e);
    }
}

}
