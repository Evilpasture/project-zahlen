// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/ECS/Components.hpp>
#include <ecs/SystemParameters.hpp>

namespace ZHLN {
class PhysicsContext;

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
        FrameAlpha                              alpha
    ) noexcept;
};

}
