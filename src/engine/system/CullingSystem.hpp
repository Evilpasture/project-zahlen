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
    // Graph entry: runs against the context's main camera.
    template <bool UsePhysicsTransforms = false>
    void Update(SystemContext& ctx, JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow);

    // Graph core: culls against an explicit camera (ctx.camera is still the
    // main-camera identity used for the freeze/jitter bookkeeping).
    template <bool UsePhysicsTransforms = false>
    void Update(SystemContext& ctx, Camera& cam, JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow);

    // Bridges for imperative callers outside the graphs (RenderSystem's
    // shadow/extra passes): they still hold an Engine, not a SystemContext.
    template <bool UsePhysicsTransforms = false>
    void Update(Engine& engine, JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow);

    template <bool UsePhysicsTransforms = false>
    void Update(Engine& engine, Camera& cam, JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow);

    [[nodiscard]] std::array<JPH::Vec3, 8> GetFrustumCorners() const {
        return m_frustumCorners;
    }

    // Draws the line segments of the frozen camera frustum
    void DrawDebugFrustum(Engine& engine);

    // This world's culling toggles and counters (see CullingStats in
    // Zahlen/Profiler.hpp). Owned here, not process-global: two engines must
    // not freeze one another's frustums, and a test's counters must not leak
    // into the next test.
    CullingStats&       Stats() noexcept { return m_stats; }
    const CullingStats& Stats() const noexcept { return m_stats; }

  private:
    std::array<JPH::Vec3, 8> m_frustumCorners {};
    CullingStats             m_stats {};
    // Edge of the freeze transition, so the frozen view is captured exactly
    // once. Used to be a function-local static, shared by every world.
    bool                    m_wasFrozen = false;
};

} // namespace ZHLN
