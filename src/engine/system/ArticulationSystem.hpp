// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>

namespace ZHLN {

class Engine;

class ZHLN_API ArticulationSystem {
  public:
    ArticulationSystem()  = default;
    ~ArticulationSystem() = default;

    ArticulationSystem(const ArticulationSystem&)            = delete;
    ArticulationSystem& operator=(const ArticulationSystem&) = delete;

    /**
     * @brief Evaluates active motor forces, decays per-joint hit reaction weights,
     * and performs per-joint SLERP/LERP pose blending before uploading matrices to the GPU.
     */
    void Update(Engine& engine, float dt);

    /**
     * @brief High-level API to trigger an impulse-based hit reaction on a specific joint
     * or joint region (e.g. getting shot in the shoulder).
     */
    static void ApplyHitImpulse(
        Engine&          engine,
        Entity           entity,
        uint32_t         jointIdx,
        const JPH::Vec3& impulse,
        float            weight    = 0.8f,
        float            stiffness = 0.2f,
        float            decayRate = 2.0f
    ) noexcept;
};

} // namespace ZHLN
