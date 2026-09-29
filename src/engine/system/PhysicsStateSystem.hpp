// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>

namespace ZHLN {
class PhysicsContext;

class ZHLN_API VisualInterpolationSystem {
  public:
    static void Update(ECS::Query<const Components::PhysicsComponent, Components::TransformComponent&> query,
                       ECS::Res<PhysicsContext> physics, FrameAlpha alpha) noexcept;
};

}
