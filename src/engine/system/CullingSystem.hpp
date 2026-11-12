// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Engine/EngineServices.hpp>
#include <Zahlen/Core/Profiler.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>
#include <array>

namespace ZHLN {
class RenderContext;
class DebugDraw;

// The culling pass's working set: the planes it derived this frame, the
// freeze-frame corners, and whether the last frame was frozen. This is the
// algorithm's scratch -- the pass writes it and reads it back, and nothing else
// has any business with it -- so it belongs to the node that owns the pass
// (ECS::Local<CullingScratch>) rather than to the world.
//
// The counters the pass publishes are a different thing: they are read by the
// overlay, the crash dump and the render tests, so they live in
// Components::CullingStatsComponent where world data belongs.
struct CullingScratch {
    Frustum                  mainFrustum {};
    Frustum                  shadowFrustum {};
    std::array<FVec3, 8> frustumCorners {};
    bool                     wasFrozen = false;
};

class ZHLN_API CullingSystem {
  public:
    using CullingQuery = ECS::Query<
        const Components::MeshComponent, const Components::WorldTransformComponent, Components::CameraComponent&,
        const Components::LightComponent, const Components::SunTagComponent,
        const Components::EnvironmentSunTagComponent, const Components::TransformComponent, Components::CullingStatsComponent&>;

    // Reflected graph entry point. The pass owns its scratch (ECS::Local), reads
    // the camera entity's pose from the registry, and writes the counters it
    // publishes into the stats singleton. There is no culler *object* left to
    // hand it: the class is a namespace for the pass.
    static void GraphUpdate(CullingQuery query, ECS::Res<RenderContext> render, VisibleEntities visible, VisibleShadowEntities shadow,
                            ECS::Local<CullingScratch> scratch);

    // The freeze-frame debug view. The corners are the pass's own state now, so
    // this derives them from the same view-projection the pass froze instead of
    // reaching into another node's storage.
    using DebugFrustumQuery = ECS::Query<const Components::CullingStatsComponent, const Components::CameraComponent>;
    static void DrawDebugFrustum(DebugFrustumQuery query, DebugDraw scene);

  private:
    static void UpdateCore(CullingQuery query, const RenderContext& rc, const Camera& cam, bool engineCam, CullingScratch& scratch,
                           CullingStats& stats, VisibilityList& outVisible, VisibilityList& outVisibleShadow);
};

// The eight world-space corners of a view-projection, in the order the debug
// edges expect. Shared by the freeze path (which stores them) and the debug draw
// (which draws them).
[[nodiscard]] ZHLN_API std::array<FVec3, 8> FrustumCornersFromViewProj(const Mat4x4& viewProj) noexcept;

} // namespace ZHLN
