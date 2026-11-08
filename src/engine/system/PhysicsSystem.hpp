// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Engine/Frame.hpp>
#include <Zahlen/ECS/Entity.hpp>

namespace ZHLN {

class Engine;

namespace ECS {
class Registry;
}

void AccumulateImpulse(ECS::Registry& registry, Entity entity, float x, float y, float z);

class ZHLN_API PhysicsSystem {
  public:
    static constexpr float TargetDt = kFixedStepDt;

    // Engine::Tick has already drained the clock; the physics step just runs
    // the number of fixed steps it was handed.
    static void Update(Engine& engine, uint32_t fixedSteps) noexcept;
};

}
