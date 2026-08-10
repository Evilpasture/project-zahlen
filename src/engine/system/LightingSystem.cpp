// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "LightingSystem.hpp"
#include "Zahlen/Camera.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Entity.hpp"
#include "Zahlen/Render.hpp"
#include "Zahlen/Types.hpp"
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cstring>

namespace ZHLN {

std::pair<JPH::Vec3, float> LightingSystem::GetSunDirectionAndIntensity(const ECS::Registry& reg) noexcept {
    JPH::Vec3 sunDirection = {0.5f, 1.0f, 0.2f};
    float     sunIntensity = 180.0f;
    bool      sunFound     = false;

    for (Entity e: reg.GetEntitiesWith<Components::LightComponent>()) {
        ECS::Patch<Components::LightComponent>(reg, e, [&](const auto& light) {
            if (light.type == LightType::Sun) {
                // Prioritize explicit direction vector if set by script
                if (light.direction.LengthSq() > 1e-4f) {
                    sunDirection = light.direction;
                } else if (!ECS::Patch<Components::WorldTransformComponent>(reg, e, [&](const auto& worldTrans) {
                               sunDirection = worldTrans.world.GetColumn3(2);
                           })) {
                    ECS::Patch<Components::TransformComponent>(reg, e, [&](const auto& trans) { sunDirection = trans.GetLocalMatrix().GetColumn3(2); });
                }
                sunIntensity = light.intensity;
                sunFound     = true;
            }
        });

        if (sunFound) {
            break;
        }
    }

    // Fallback to legacy tag search if no explicit Sun type was registered
    if (!sunFound) {
        auto sunEntities = reg.GetEntitiesWith<Components::SunTagComponent>();
        if (!sunEntities.empty()) {
            Entity sunEnt = sunEntities[0];
            if (!ECS::Patch<Components::WorldTransformComponent>(reg, sunEnt, [&](const auto& worldTrans) { sunDirection = worldTrans.world.GetColumn3(2); })) {
                ECS::Patch<Components::TransformComponent>(reg, sunEnt, [&](const auto& trans) { sunDirection = trans.GetLocalMatrix().GetColumn3(2); });
            }

            ECS::Patch<Components::LightComponent>(reg, sunEnt, [&](const auto& light) { sunIntensity = light.intensity; });
        }
    }

    return {sunDirection.Normalized(), sunIntensity};
}

void LightingSystem::Update(Engine& engine, [[maybe_unused]] float dt) {
    auto& reg = engine.GetRegistry();
    auto& rc  = engine.GetRenderContext();

    // 1. DYNAMIC SHADOW ALLOCATION FOR PUNCTUAL LIGHTS
    Entity playerEnt = NullEntity;
    for (Entity e: reg.GetEntitiesWith<Components::PlayerTagComponent>()) {
        playerEnt = e;
        break;
    }

    if (playerEnt != NullEntity) {
        struct LightDistance {
            Entity entity;
            float  distSq;
        };
        ZHLN::Array<LightDistance> lightDistances;

        JPH::Vec3 playerPos    = JPH::Vec3::sZero();
        bool      hasPlayerPos = ECS::Patch<Components::WorldTransformComponent>(reg, playerEnt, [&](const auto& playerWorldTrans) {
            playerPos = playerWorldTrans.world.GetTranslation();
        });

        if (!hasPlayerPos) {
            hasPlayerPos = ECS::Patch<Components::TransformComponent>(reg, playerEnt, [&](const auto& playerTrans) { playerPos = playerTrans.position; });
        }

        if (hasPlayerPos) {
            for (Entity e: reg.GetEntitiesWith<Components::LightComponent>()) {
                ECS::Patch<Components::LightComponent>(reg, e, [&](auto& light) {
                    light.shadowLayer = -1; // Reset to disabled initially

                    // Punctual shadows are only allocated to local point/spot lights
                    if (light.type == LightType::Point || light.type == LightType::Spot) {
                        JPH::Vec3 lightPos    = JPH::Vec3::sZero();
                        bool      hasLightPos = ECS::Patch<Components::WorldTransformComponent>(reg, e, [&](const auto& worldTrans) {
                            lightPos = worldTrans.world.GetTranslation();
                        });

                        if (!hasLightPos) {
                            hasLightPos = ECS::Patch<Components::TransformComponent>(reg, e, [&](const auto& trans) { lightPos = trans.position; });
                        }

                        if (hasLightPos) {
                            float dSq = (lightPos - playerPos).LengthSq();
                            lightDistances.push_back({.entity = e, .distSq = dSq});
                        }
                    }
                });
            }

            // Sort light sources nearest to player
            std::ranges::sort(lightDistances, [](const LightDistance& a, const LightDistance& b) { return a.distSq < b.distSq; });

            auto shadowEntities = reg.GetEntitiesWith<Components::ShadowSettingsComponent>();
            if (!shadowEntities.empty()) {
                ECS::Patch<Components::ShadowSettingsComponent>(reg, shadowEntities[0], [&](const auto& shadowSettings) {
                    uint32_t shadowCasters = std::min(static_cast<uint32_t>(shadowSettings.maxPunctualShadows), static_cast<uint32_t>(lightDistances.size()));
                    for (uint32_t i = 0; i < shadowCasters; ++i) {
                        ECS::Patch<Components::LightComponent>(reg, lightDistances[i].entity, [&](auto& light) {
                            light.shadowLayer = static_cast<int32_t>(i);
                        });
                    }
                });
            }
        }
    }

    // 2. COMPILE GPU LIGHTS
    ZHLN::Array<GPULight> sceneLights;
    JPH::Mat44            viewMatrix    = engine.GetCamera().GetViewMatrix();
    auto                  lightEntities = reg.GetEntitiesWith<Components::LightComponent>();
    sceneLights.reserve(lightEntities.size());

    for (Entity e: lightEntities) {
        ECS::Patch<Components::LightComponent>(reg, e, [&](const auto& light) {
            GPULight gpuLight {};
            gpuLight.type        = light.type;
            gpuLight.intensity   = light.intensity;
            gpuLight.radius      = light.radius;
            gpuLight.twoSided    = light.twoSided;
            gpuLight.range       = (light.range > 0.0f) ? light.range : 1000.0f;
            gpuLight.shadowLayer = light.shadowLayer;
            std::memcpy(gpuLight.direction, &light.direction, sizeof(float) * 3);
            std::memcpy(gpuLight.color, &light.color, sizeof(float) * 3);

            JPH::Vec3  pos          = JPH::Vec3::sZero();
            JPH::Mat44 worldMat     = JPH::Mat44::sIdentity();
            bool       hasTransform = ECS::Patch<Components::WorldTransformComponent>(reg, e, [&](const auto& worldTrans) {
                pos      = worldTrans.world.GetTranslation();
                worldMat = worldTrans.world;
            });

            if (!hasTransform) {
                hasTransform = ECS::Patch<Components::TransformComponent>(reg, e, [&](const auto& trans) {
                    pos      = trans.position;
                    worldMat = trans.GetLocalMatrix();
                });
            }

            if (hasTransform) {
                std::memcpy(gpuLight.position, &pos, sizeof(float) * 3);

                // Transform position to view-space for cluster culling
                JPH::Vec3 posView        = viewMatrix * pos;
                gpuLight.positionView[0] = posView.GetX();
                gpuLight.positionView[1] = posView.GetY();
                gpuLight.positionView[2] = posView.GetZ();

                if (light.type == LightType::Directional || light.type == LightType::Spot || light.type == LightType::Sun) {
                    JPH::Vec3 dir         = -worldMat.GetColumn3(2).Normalized();
                    gpuLight.direction[0] = dir.GetX();
                    gpuLight.direction[1] = dir.GetY();
                    gpuLight.direction[2] = dir.GetZ();
                }
            }

            if (gpuLight.type == LightType::Area) {
                std::memcpy(gpuLight.points, &light.points, sizeof(JPH::Mat44));
            }

            sceneLights.push_back(gpuLight);
        });
    }

    rc.SetLights(sceneLights.data(), sceneLights.size());
}

} // namespace ZHLN
