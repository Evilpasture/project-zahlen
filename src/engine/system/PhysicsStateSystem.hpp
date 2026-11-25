// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/ECS/Components.hpp>
#include <ecs/SystemParameters.hpp>
#include <physics/Physics.hpp>
#include <vector>

namespace ZHLN {
class PhysicsContext;

// The system's own working set. The dev-only interpolation invariant runs once
// per node, and the vectors the pass fills each frame belong to the pass: both
// were hidden state (a function-local static and a thread_local) that this node
// now owns out loud.
struct InterpolationScratch {
    bool                                    invariantVerified = false;
    std::vector<Physics::BodyID>            handles;
    std::vector<size_t>                     sourceIndex;
    std::vector<Physics::BodyStateSnapshot> snapshots;
};

class VisualInterpolationSystem {
  public:
    static void Update(
        ECS::Query<
            const Components::PhysicsComponent,
            Components::TransformComponent&,
            const Components::CharacterMotorComponent,
            const Components::CharacterMotorStateComponent,
            const Components::RagdollComponent> query,
        ECS::Res<PhysicsContext>                physics,
        FrameAlpha                              alpha,
        ECS::Local<InterpolationScratch>        scratch
    ) noexcept;
};

}
