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
                            ECS::Query<const Components::CameraComponent> cameraQuery, ECS::ResMut<SceneData> scene) {
    auto& sceneData = *scene;

    // The view camera is world data now: lighting sorts and packs relative to
    // the main camera entity's pose, the same state the renderer projects.
    const auto cameraComp = cameraQuery.GetSingleton<Components::CameraComponent>();

    struct LightImportance {
        Entity entity;
        float  score;
    };
    ZHLN::Array<LightImportance> lightPriorities;

    const JPH::Vec3 viewPos = cameraComp ? cameraComp->camera.position : JPH::Vec3::sZero();

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

    ZHLN::Array<LightDesc> sceneLights;
    auto                   lightEntities = query.GetEntitiesWith<Components::LightComponent>();
    sceneLights.reserve(lightEntities.size());

    for (Entity e: lightEntities) {
        query.Patch<Components::LightComponent>(e, [&](const auto& light) {
            // The scene's own terms, world space: position comes from the entity's
            // transform, and the view-space copy the shader reads is the renderer's
            // business (it has the view matrix; this system does not need one).
            LightDesc desc {};
            desc.type        = light.type;
            desc.intensity   = light.intensity;
            desc.radius      = light.radius;
            desc.twoSided    = light.twoSided;
            desc.range       = (light.range > 0.0f) ? light.range : 1000.0f;
            desc.shadowLayer = light.shadowLayer;
            desc.direction   = light.direction;
            desc.color       = light.color;

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
                desc.position = pos;

                if (light.type == LightType::Directional || light.type == LightType::Spot || light.type == LightType::Sun) {
                    desc.direction = (light.direction.LengthSq() > 1e-4f) ? light.direction.Normalized() : -worldMat.GetColumn3(2).Normalized();
                }
            }

            if (desc.type == LightType::Area) {
                desc.points = light.points;
            }

            sceneLights.push_back(desc);
        });
    }

    sceneData.lights = std::move(sceneLights);
}

}
