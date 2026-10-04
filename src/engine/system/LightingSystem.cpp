// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "LightingSystem.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Entity.hpp"
#include "Zahlen/Log.hpp"
#include "Zahlen/Render/GpuEnums.hpp"
#include "Zahlen/Render/Render.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <span>

namespace ZHLN {

auto LightingSystem::GetSun(SunQuery reg) noexcept -> SunLight {
    SunLight sun {
        .direction = JPH::Vec3(0.5f, 1.0f, 0.2f),
        .color = JPH::Vec3::sReplicate(1.0f),
        .intensity = 180.0f,
        .fromEnvironment = false,
    };
    bool found = false;

    for (Entity e: reg.GetEntitiesWith<Components::LightComponent>()) {
        reg.Patch<Components::LightComponent>(e, [&](const auto& light) {
            if (light.type != LightType::Sun) return;
            if (light.direction.LengthSq() > 1e-4f) {
                sun.direction = light.direction;
            } else if (!reg.Patch<Components::WorldTransformComponent>(e, [&](const auto& worldTrans) {
                           sun.direction = worldTrans.world.GetColumn3(2);
                       })) {
                reg.Patch<Components::TransformComponent>(e, [&](const auto& trans) { sun.direction = trans.GetLocalMatrix().GetColumn3(2); });
            }
            sun.intensity = light.intensity;
            // Preserve the white legacy default for tag-only sun entities.
            sun.color = light.color.LengthSq() > 1e-8f ? light.color : JPH::Vec3::sReplicate(1.0f);
            sun.fromEnvironment = reg.Get<Components::EnvironmentSunTagComponent>(e).has_value();
            found = true;
        });
        if (found) break;
    }

    if (!found) {
        const auto sunEntities = reg.GetEntitiesWith<Components::SunTagComponent>();
        if (!sunEntities.empty()) {
            const Entity sunEnt = sunEntities[0];
            if (!reg.Patch<Components::WorldTransformComponent>(sunEnt, [&](const auto& worldTrans) { sun.direction = worldTrans.world.GetColumn3(2); })) {
                reg.Patch<Components::TransformComponent>(sunEnt, [&](const auto& trans) { sun.direction = trans.GetLocalMatrix().GetColumn3(2); });
            }
            reg.Patch<Components::LightComponent>(sunEnt, [&](const auto& light) {
                sun.intensity = light.intensity;
                if (light.color.LengthSq() > 1e-8f) sun.color = light.color;
            });
        }
    }

    sun.direction = sun.direction.LengthSq() > 1e-8f ? sun.direction.Normalized() : JPH::Vec3(0.5f, 1.0f, 0.2f).Normalized();
    return sun;
}

std::pair<JPH::Vec3, float> LightingSystem::GetSunDirectionAndIntensity(SunQuery reg) noexcept {
    const SunLight sun = GetSun(reg);
    return {sun.direction, sun.intensity};
}

void LightingSystem::Update(ECS::Query<Components::LightComponent&, const Components::WorldTransformComponent,
                                       const Components::TransformComponent, const Components::ShadowSettingsComponent> query,
                            ECS::ResMut<RenderContext> render, ECS::Res<Camera> camera) {
    auto& rc = *render;

    struct LightImportance {
        Entity entity;
        float  score;
    };
    ZHLN::Array<LightImportance> lightPriorities;

    const JPH::Vec3 viewPos = camera->position;

    for (Entity e: query.GetEntitiesWith<Components::LightComponent>()) {
        query.Patch<Components::LightComponent>(e, [&](auto& light) {
            light.shadowLayer = -1;

            if (light.type == LightType::Point || light.type == LightType::Spot) {
                JPH::Vec3 lightPos    = JPH::Vec3::sZero();
                bool      hasLightPos = query.Patch<Components::WorldTransformComponent>(e, [&](const auto& worldTrans) {
                    lightPos = worldTrans.world.GetTranslation();
                });

                if (!hasLightPos) {
                    hasLightPos = query.Patch<Components::TransformComponent>(e, [&](const auto& trans) { lightPos = trans.position; });
                }

                if (hasLightPos) {
                    float distSq = std::max((lightPos - viewPos).LengthSq(), 1.0f);
                    lightPriorities.push_back({.entity = e, .score = light.intensity / distSq});
                }
            }
        });
    }

    std::ranges::sort(lightPriorities, [](const LightImportance& a, const LightImportance& b) { return a.score > b.score; });

    auto shadowEntities = query.GetEntitiesWith<Components::ShadowSettingsComponent>();
    if (!shadowEntities.empty()) {
        query.Patch<Components::ShadowSettingsComponent>(shadowEntities[0], [&](const auto& shadowSettings) {
            uint32_t shadowCasters = std::min(static_cast<uint32_t>(shadowSettings.maxPunctualShadows), static_cast<uint32_t>(lightPriorities.size()));
            for (uint32_t i = 0; i < shadowCasters; ++i) {
                query.Patch<Components::LightComponent>(lightPriorities[i].entity, [&](auto& light) {
                    light.shadowLayer = static_cast<int32_t>(i);
                });
            }
        });
    }

    ZHLN::Array<Light> sceneLights;
    JPH::Mat44         viewMatrix    = camera->GetViewMatrix();
    auto               lightEntities = query.GetEntitiesWith<Components::LightComponent>();
    sceneLights.reserve(lightEntities.size());

    for (Entity e: lightEntities) {
        query.Patch<Components::LightComponent>(e, [&](const auto& light) {
            // The light *is* the shader's struct: elements land where the
            // shader reads them, and SIMD crosses to the pod lane types
            // through Jolt's own StoreFloat3/StoreFloat4 -- Vec3/Vec4 are the
            // compute types, never the storage ones.
            Light packed {};
            packed.type        = light.type;
            packed.intensity   = light.intensity;
            packed.radius      = light.radius;
            packed.twoSided    = light.twoSided;
            packed.range       = (light.range > 0.0f) ? light.range : 1000.0f;
            packed.shadowLayer = light.shadowLayer;
            light.direction.StoreFloat3(&packed.direction);
            light.color.StoreFloat3(&packed.color);

            JPH::Vec3  pos          = JPH::Vec3::sZero();
            JPH::Mat44 worldMat     = JPH::Mat44::sIdentity();
            bool       hasTransform = query.Patch<Components::WorldTransformComponent>(e, [&](const auto& worldTrans) {
                pos      = worldTrans.world.GetTranslation();
                worldMat = worldTrans.world;
            });

            if (!hasTransform) {
                hasTransform = query.Patch<Components::TransformComponent>(e, [&](const auto& trans) {
                    pos      = trans.position;
                    worldMat = trans.GetLocalMatrix();
                });
            }

            if (hasTransform) {
                pos.StoreFloat3(&packed.position);
                // View-space position, w = 1: the shader reads .xyz, and a
                // defined w costs nothing (Vec4(Vec3Arg) leaves it unset).
                JPH::Vec4(viewMatrix * pos, 1.0f).StoreFloat4(&packed.positionView);

                if (light.type == LightType::Directional || light.type == LightType::Spot || light.type == LightType::Sun) {
                    JPH::Vec3 dir = JPH::Vec3::sZero();
                    if (light.direction.LengthSq() > 1e-4f) {
                        dir = light.direction.Normalized();
                    } else {
                        dir = -worldMat.GetColumn3(2).Normalized();
                    }
                    dir.StoreFloat3(&packed.direction);
                }
            }

            if (packed.type == LightType::Area) {
                // An area light's quad is a matrix in the component and four
                // 16-byte lanes in the shader: one column, one lane.
                for (uint32_t c = 0; c < 4; ++c) {
                    light.points.GetColumn4(c).StoreFloat4(&packed.points[c]);
                }
            }

            sceneLights.push_back(packed);
        });
    }

    rc.SetLights(std::span {sceneLights});
}

}
