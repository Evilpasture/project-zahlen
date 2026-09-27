// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>

namespace ZHLN {

class Engine;

namespace ECS {
class Registry;
}

void AccumulateImpulse(ECS::Registry& registry, Entity entity, float x, float y, float z);

class ZHLN_API PhysicsSystem {
  public:
    static constexpr float TargetDt = 1.0f / 60.0f;

    static void Update(Engine& engine, float dt, float& accumulator) noexcept;
};

}
