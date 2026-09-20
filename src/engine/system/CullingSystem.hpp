// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Zahlen/Common.h>
#include <Zahlen/Entity.hpp>
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

  private:
    std::array<JPH::Vec3, 8> m_frustumCorners {};
};

} // namespace ZHLN
