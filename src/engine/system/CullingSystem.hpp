// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Profiler.hpp>
#include <array>

namespace ZHLN {
class Engine;
struct Camera;
struct SystemContext;

class ZHLN_API CullingSystem {
  public:
    template <bool UsePhysicsTransforms = false>
    void Update(SystemContext& ctx, JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow);

    template <bool UsePhysicsTransforms = false>
    void Update(SystemContext& ctx, Camera& cam, JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow);

    template <bool UsePhysicsTransforms = false>
    void Update(Engine& engine, JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow);

    template <bool UsePhysicsTransforms = false>
    void Update(Engine& engine, Camera& cam, JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow);

    [[nodiscard]] std::array<JPH::Vec3, 8> GetFrustumCorners() const {
        return m_frustumCorners;
    }

    void DrawDebugFrustum(Engine& engine);

    CullingStats&       Stats() noexcept { return m_stats; }
    const CullingStats& Stats() const noexcept { return m_stats; }

  private:
    std::array<JPH::Vec3, 8> m_frustumCorners {};
    CullingStats             m_stats {};
    bool                    m_wasFrozen = false;
};

}
