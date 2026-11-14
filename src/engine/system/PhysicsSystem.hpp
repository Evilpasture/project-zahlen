// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>

namespace ZHLN {
class PhysicsContext;
namespace ECS { class EntityCommandBuffer; }

class ZHLN_API PhysicsSystem {
  public:
    using ImpulseQuery = ECS::Query<const Components::ImpulseCommand, const Components::PhysicsComponent>;

    // The frame owner plays these removals before stepping the physics world.
    static void CommitImpulses(ImpulseQuery query, ECS::EntityCommandBuffer& commands, ECS::ResMut<PhysicsContext> physics);
};

}
