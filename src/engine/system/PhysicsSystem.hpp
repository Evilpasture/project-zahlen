// src/engine/system/PhysicsSystem.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>

namespace ZHLN {

class Engine;

class ZHLN_API PhysicsSystem {
  public:
    PhysicsSystem()  = default;
    ~PhysicsSystem() = default;

    // Non-copyable to prevent duplicating the accumulator state
    PhysicsSystem(const PhysicsSystem&)            = delete;
    PhysicsSystem& operator=(const PhysicsSystem&) = delete;

    /**
     * @brief Accumulates delta time and steps the Jolt physics simulation
     * using a semi-fixed timestep, running pre-step movement systems
     * and post-step write-backs as needed.
     */
    void Update(Engine& engine, float dt) noexcept;

  private:
    float                  _accumulator = 0.0f;
    static constexpr float _targetDt    = 1.0f / 60.0f;
};

} // namespace ZHLN
