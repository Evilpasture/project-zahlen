// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/ECS/Components.hpp>
#include <ecs/SystemParameters.hpp>

namespace ZHLN {
class PhysicsContext;
namespace ECS { class EntityCommandBuffer; }

class PhysicsSystem {
  public:
    using ImpulseQuery = ECS::Query<const Components::ImpulseCommand, const Components::DynamicBodyComponent>;

    // The frame owner plays these removals before stepping the physics world.
    static void CommitImpulses(ImpulseQuery query, ECS::EntityCommandBuffer& commands, ECS::ResMut<PhysicsContext> physics);
};

}
