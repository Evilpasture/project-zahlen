// src/engine/system/PhysicsSystem.hpp
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

// Adds `linear` onto an existing ImpulseCommand, or inserts one. Never overwrites.
void AccumulateImpulse(ECS::Registry& registry, Entity entity, float x, float y, float z);

class ZHLN_API PhysicsSystem {
  public:
    static constexpr float TargetDt = 1.0f / 60.0f;

    /**
     * @brief Accumulates delta time and steps the Jolt physics simulation
     * using a semi-fixed timestep, running pre-step movement systems
     * and post-step write-backs as needed.
     *
     * The accumulator is injected, not owned: this class is stateless
     * algorithmic code, and the caller's storage gives the value its
     * lifetime. The frame scheduler passes Engine::GetPhysicsAccumulator(),
     * so the leftover time dies with that engine instance instead of a
     * static outliving every world.
     */
    static void Update(Engine& engine, float dt, float& accumulator) noexcept;
};

} // namespace ZHLN
