// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/SystemContext.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>
#include <array>

namespace ZHLN {
class Engine;
class RenderContext;
struct Camera;

class ZHLN_API CullingSystem {
  public:
    using CullingQuery = ECS::Query<
        const Components::MeshComponent, const Components::WorldTransformComponent, Components::CameraComponent&,
        const Components::GlobalSettingsTagComponent, const Components::PostProcessSettingsComponent,
        const Components::ShadowSettingsComponent, const Components::LightComponent, const Components::SunTagComponent,
        const Components::EnvironmentSunTagComponent, const Components::TransformComponent>;

    // Reflected graph entry point. The stateful culler and both output lists
    // are injected by type, not extracted in SystemWiring.cpp.
    static void GraphUpdate(CullingQuery query, ECS::ResMut<CullingSystem> culling, ECS::Res<RenderContext> render,
                            ECS::ResMut<Camera> camera, VisibleEntities visible, VisibleShadowEntities shadow);

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
    template <bool UsePhysicsTransforms>
    void UpdateCore(CullingQuery query, const RenderContext& render, Camera& cam, bool engineCam,
                    JPH::Array<Entity>& outVisible, JPH::Array<Entity>& outVisibleShadow);

    std::array<JPH::Vec3, 8> m_frustumCorners {};
    CullingStats             m_stats {};
    bool                    m_wasFrozen = false;
};

}
