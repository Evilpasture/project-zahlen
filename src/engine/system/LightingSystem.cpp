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
#include <Zahlen/ECS/ECS.hpp>
#include <algorithm>
#include <limits>
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

void LightingSystem::Update(ECS::Query<const Components::LightComponent, const Components::WorldTransformComponent,
                                       const Components::TransformComponent> query,
                            ECS::Query<const Components::CameraComponent> cameraQuery, ECS::ResMut<SceneData> scene) {
    auto& sceneData = *scene;

    // The view camera is world data now: lighting sorts and packs relative to
    // the main camera entity's pose, the same state the renderer projects.
    const auto cameraComp = cameraQuery.GetSingleton<Components::CameraComponent>();

    // One ranked entry per light. The renderer admits a bounded number of scene
    // lights and truncates the head of the list it is handed, so the order this
    // system publishes *is* the order that decides which lights exist on the
    // GPU. It used to publish entity-lump order, which made the surviving set
    // arbitrary with respect to the camera -- walking up to a fixture that was
    // never in the first 128 did nothing, while one across the map lit a wall
    // beside you. Ranking here fixes that: pinned (geometry-derived) lights
    // first, then nearest-and-brightest.
    struct RankedLight {
        LightDesc desc;
        Entity    entity   = Entity::Null();
        float     score    = 0.0f;
        bool      pinned   = false;
        bool      isRanged = false;
    };

    ZHLN::Array<RankedLight> ranked;

    const JPH::Vec3 viewPos = cameraComp ? cameraComp->camera.position : JPH::Vec3::sZero();

    for (Entity e: query.GetEntitiesWith<Components::LightComponent>()) {
        query.Patch<Components::LightComponent>(e, [&](const auto& light) {
            LightDesc desc {};
            desc.type             = light.type;
            desc.intensity        = light.intensity;
            desc.radius           = light.radius;
            desc.twoSided         = light.twoSided;
            desc.range            = (light.range > 0.0f) ? light.range : 1000.0f;
            desc.castsShadow      = light.castsShadow;
            desc.direction        = light.direction;
            desc.color            = light.color;
            desc.points           = light.points;
            desc.innerConeRadians = light.innerConeRadians;
            desc.outerConeRadians = light.outerConeRadians;
            desc.baked            = light.baked;

            JPH::Vec3 lightPos    = JPH::Vec3::sZero();
            bool      hasLightPos = query.Patch<Components::WorldTransformComponent>(e, [&](const auto& worldTrans) {
                lightPos = worldTrans.world.GetTranslation();
            });
            if (!hasLightPos) {
                hasLightPos = query.Patch<Components::TransformComponent>(e, [&](const auto& trans) { lightPos = trans.position; });
            }
            if (!hasLightPos && light.type == LightType::Area) {
                lightPos    = 0.25f * (light.points.GetColumn3(0) + light.points.GetColumn3(1) + light.points.GetColumn3(2) + light.points.GetColumn3(3));
                hasLightPos = true;
            }
            desc.position = lightPos;

            if (light.type == LightType::Spot && desc.direction.LengthSq() <= 1e-8f) {
                if (!query.Patch<Components::WorldTransformComponent>(e, [&](const auto& worldTrans) {
                        desc.direction = worldTrans.world.GetColumn3(2);
                    })) {
                    query.Patch<Components::TransformComponent>(e, [&](const auto& trans) {
                        desc.direction = trans.GetLocalMatrix().GetColumn3(2);
                    });
                }
            }

            RankedLight entry {};
            entry.desc   = desc;
            entry.entity = e;
            entry.pinned = light.pinned;

            if (light.type == LightType::Point || light.type == LightType::Spot) {
                entry.isRanged = true;

                if (hasLightPos) {
                    const float distSq = std::max((lightPos - viewPos).LengthSq(), 1.0f);
                    entry.score        = light.intensity / distSq;
                }
            } else {
                // Sun / directional / area terms are single-term and cost one
                // draw. They are never worth culling, so they outrank every
                // point/spot light regardless of how far away they are.
                entry.score = std::numeric_limits<float>::max();
            }

            ranked.push_back(std::move(entry));
        });
    }

    // Stable, so equal-ranked lights keep their relative order: the scene
    // description publishes geometry-derived lights first, and a stable sort is
    // what lets that ordering survive to the renderer.
    std::ranges::stable_sort(ranked, [](const RankedLight& a, const RankedLight& b) {
        if (a.pinned != b.pinned) {
            return a.pinned;
        }
        return a.score > b.score;
    });

    // No shadow slots are handed out here any more. A light's `castsShadow`
    // reaches the renderer untouched and the shadow atlas answers it per
    // frame, so a light that loses the ranking contest loses tile *resolution*
    // rather than its shadows -- which is what the slot assignment and its
    // hysteresis patch were both working around.

    ZHLN::Array<LightDesc> sceneLights;
    sceneLights.reserve(ranked.size());
    for (RankedLight& entry: ranked) {
        sceneLights.push_back(std::move(entry.desc));
    }

    sceneData.lights = std::move(sceneLights);
}

}
