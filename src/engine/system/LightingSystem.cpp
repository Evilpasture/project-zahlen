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
        reg.Patch<Components::LightComponent>(e, [&](const auto& light) {
            if (light.type == LightType::Sun) {
                // Prioritize explicit direction vector if set by script
                if (light.direction.LengthSq() > 1e-4f) {
                    sunDirection = light.direction;
                } else if (!reg.Patch<Components::WorldTransformComponent>(e, [&](const auto& worldTrans) {
                               sunDirection = worldTrans.world.GetColumn3(2);
                           })) {
                    reg.Patch<Components::TransformComponent>(e, [&](const auto& trans) { sunDirection = trans.GetLocalMatrix().GetColumn3(2); });
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
            if (!reg.Patch<Components::WorldTransformComponent>(sunEnt, [&](const auto& worldTrans) { sunDirection = worldTrans.world.GetColumn3(2); })) {
                reg.Patch<Components::TransformComponent>(sunEnt, [&](const auto& trans) { sunDirection = trans.GetLocalMatrix().GetColumn3(2); });
            }

            reg.Patch<Components::LightComponent>(sunEnt, [&](const auto& light) { sunIntensity = light.intensity; });
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
        bool      hasPlayerPos = reg.Patch<Components::WorldTransformComponent>(playerEnt, [&](const auto& playerWorldTrans) {
            playerPos = playerWorldTrans.world.GetTranslation();
        });

        if (!hasPlayerPos) {
            hasPlayerPos = reg.Patch<Components::TransformComponent>(playerEnt, [&](const auto& playerTrans) { playerPos = playerTrans.position; });
        }

        if (hasPlayerPos) {
            for (Entity e: reg.GetEntitiesWith<Components::LightComponent>()) {
                reg.Patch<Components::LightComponent>(e, [&](auto& light) {
                    light.shadowLayer = -1; // Reset to disabled initially

                    // Punctual shadows are only allocated to local point/spot lights
                    if (light.type == LightType::Point || light.type == LightType::Spot) {
                        JPH::Vec3 lightPos    = JPH::Vec3::sZero();
                        bool      hasLightPos = reg.Patch<Components::WorldTransformComponent>(e, [&](const auto& worldTrans) {
                            lightPos = worldTrans.world.GetTranslation();
                        });

                        if (!hasLightPos) {
                            hasLightPos = reg.Patch<Components::TransformComponent>(e, [&](const auto& trans) { lightPos = trans.position; });
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
                reg.Patch<Components::ShadowSettingsComponent>(shadowEntities[0], [&](const auto& shadowSettings) {
                    uint32_t shadowCasters = std::min(static_cast<uint32_t>(shadowSettings.maxPunctualShadows), static_cast<uint32_t>(lightDistances.size()));
                    for (uint32_t i = 0; i < shadowCasters; ++i) {
                        reg.Patch<Components::LightComponent>(lightDistances[i].entity, [&](auto& light) {
                            light.shadowLayer = static_cast<int32_t>(i);
                        });
                    }
                });
            }
        }
    }

    // 2. COMPILE GPU LIGHTS
    ZHLN::Array<Light> sceneLights;
    JPH::Mat44         viewMatrix    = engine.GetCamera().GetViewMatrix();
    auto               lightEntities = reg.GetEntitiesWith<Components::LightComponent>();
    sceneLights.reserve(lightEntities.size());

    for (Entity e: lightEntities) {
        reg.Patch<Components::LightComponent>(e, [&](const auto& light) {
            Light packed {};
            packed.type        = light.type;
            packed.intensity   = light.intensity;
            packed.radius      = light.radius;
            packed.twoSided    = light.twoSided;
            packed.range       = (light.range > 0.0f) ? light.range : 1000.0f;
            packed.shadowLayer = light.shadowLayer;
            std::memcpy(packed.direction, &light.direction, sizeof(float) * 3);
            std::memcpy(packed.color, &light.color, sizeof(float) * 3);

            JPH::Vec3  pos          = JPH::Vec3::sZero();
            JPH::Mat44 worldMat     = JPH::Mat44::sIdentity();
            bool       hasTransform = reg.Patch<Components::WorldTransformComponent>(e, [&](const auto& worldTrans) {
                pos      = worldTrans.world.GetTranslation();
                worldMat = worldTrans.world;
            });

            if (!hasTransform) {
                hasTransform = reg.Patch<Components::TransformComponent>(e, [&](const auto& trans) {
                    pos      = trans.position;
                    worldMat = trans.GetLocalMatrix();
                });
            }

            if (hasTransform) {
                std::memcpy(packed.position, &pos, sizeof(float) * 3);

                // Transform position to view-space for cluster culling
                JPH::Vec3 posView      = viewMatrix * pos;
                packed.positionView[0] = posView.GetX();
                packed.positionView[1] = posView.GetY();
                packed.positionView[2] = posView.GetZ();

                if (light.type == LightType::Directional || light.type == LightType::Spot || light.type == LightType::Sun) {
                    JPH::Vec3 dir       = -worldMat.GetColumn3(2).Normalized();
                    packed.direction[0] = dir.GetX();
                    packed.direction[1] = dir.GetY();
                    packed.direction[2] = dir.GetZ();
                }
            }

            if (packed.type == LightType::Area) {
                std::memcpy(packed.points, &light.points, sizeof(JPH::Mat44));
            }

            sceneLights.push_back(packed);
        });
    }

    rc.SetLights(sceneLights.data(), sceneLights.size());
}

} // namespace ZHLN
