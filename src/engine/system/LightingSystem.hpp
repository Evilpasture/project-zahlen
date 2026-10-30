// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Render/SceneData.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>
#include <utility>

namespace ZHLN {

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

    // Read-only: the system ranks lights and publishes them, and the shadow
    // budget is no longer its business. Which lights get a shadow tile is
    // decided where the tiles are (the renderer's shadow atlas).
    static void Update(ECS::Query<const Components::LightComponent, const Components::WorldTransformComponent,
                                  const Components::TransformComponent> query,
                       ECS::Query<const Components::CameraComponent> cameraQuery, ECS::ResMut<SceneData> scene);

    static SunLight GetSun(SunQuery reg) noexcept;
    static std::pair<JPH::Vec3, float> GetSunDirectionAndIntensity(SunQuery reg) noexcept;
};
} // namespace ZHLN
