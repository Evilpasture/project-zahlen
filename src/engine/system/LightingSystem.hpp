// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>
#include <utility>

namespace ZHLN {
class RenderContext;
struct Camera;

class LightingSystem {
  public:
    using SunQuery = ECS::Query<const Components::LightComponent, const Components::WorldTransformComponent,
                                const Components::TransformComponent, const Components::SunTagComponent,
                                const Components::EnvironmentSunTagComponent>;
    struct SunLight {
        JPH::Vec3 direction;
        JPH::Vec3 color;
        float intensity;
        bool fromEnvironment;
    };

    static void Update(ECS::Query<Components::LightComponent&, const Components::WorldTransformComponent,
                                  const Components::TransformComponent, const Components::ShadowSettingsComponent> query,
                       ECS::ResMut<RenderContext> render, ECS::Res<Camera> camera);

    static SunLight GetSun(SunQuery reg) noexcept;
    static std::pair<JPH::Vec3, float> GetSunDirectionAndIntensity(SunQuery reg) noexcept;
};
} // namespace ZHLN
