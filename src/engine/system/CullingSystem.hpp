// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Zahlen/Camera.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/EngineServices.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>
#include <array>

namespace ZHLN {
class Engine;
class RenderContext;

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
    std::array<JPH::Vec3, 8> frustumCorners {};
    bool                     wasFrozen = false;
};

class ZHLN_API CullingSystem {
  public:
    using CullingQuery = ECS::Query<
        const Components::MeshComponent, const Components::WorldTransformComponent, Components::CameraComponent&,
        const Components::GlobalSettingsTagComponent, const Components::PostProcessSettingsComponent,
        const Components::ShadowSettingsComponent, const Components::LightComponent, const Components::SunTagComponent,
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
    static void DrawDebugFrustum(Engine& engine);

  private:
    static void UpdateCore(CullingQuery query, const RenderContext& rc, const Camera& cam, bool engineCam, CullingScratch& scratch,
                           CullingStats& stats, JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow);
};

// The eight world-space corners of a view-projection, in the order the debug
// edges expect. Shared by the freeze path (which stores them) and the debug draw
// (which draws them).
[[nodiscard]] ZHLN_API std::array<JPH::Vec3, 8> FrustumCornersFromViewProj(const JPH::Mat44& viewProj) noexcept;

} // namespace ZHLN
