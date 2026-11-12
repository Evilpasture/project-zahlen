// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <VFX/ExplosionUpdate.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace ZHLN {
void UpdateExplosions(ExplosionUpdateQuery query, ECS::ResMut<SceneData> scene, ECS::EntityCommandBuffer& ecb, FrameDt frameDt) {
    const float dt = frameDt.value;

    // --------------------------------------------------------------------
    // 1. UPDATE ACTIVE EXPLOSIONS (Fireball, Flash, Shockwaves)
    // --------------------------------------------------------------------
    auto expEntities = query.Entities<ExplosionComponent>();
    if (!expEntities.empty()) {
        auto explosions = query.Raw<ExplosionComponent>();

        for (size_t i = 0; i < expEntities.size(); ++i) {
            Entity              e   = expEntities[i];
            ExplosionComponent& exp = explosions[i];
            if (query.Get<Components::PendingDestroy>(e)) {
                continue;
            }
            exp.age += dt;

            // Flash Decay on Root Entity (Fades sharply to true 0 within ~0.6s)
            query.Patch<Components::LightComponent>(e, [&](auto& light) {
                float flashT = std::exp(-exp.age * 5.5f);
                if (flashT < 0.002f) {
                    light.intensity = 0.0f; // Drop to pitch black so zero light lingers in fog
                } else {
                    float flicker   = (0.92f + ExplosionSystem::EffectHash(exp.origin * exp.age) * 0.16f);
                    light.intensity = flashT * flicker * 2800.0f * exp.scale * exp.scale;
                    light.color     = FVec3{1.0f, 0.48f * flashT + 0.1f, 0.09f * flashT};
                }
            });

            // Spawn autonomous crater decal
            if (!exp.craterSpawned && exp.age >= 0.20f && exp.type == OrdnanceType::ArtilleryMortar) {
                exp.craterSpawned = true;
                Entity crater     = query.CreateEntity();

                float     randomYaw = ExplosionSystem::EffectHash(exp.origin) * 6.2831853f;
                const auto yawRot = Quat::FromAxisAngle(FVec3::UnitY(), RadF{randomYaw});
                const auto baseRot = Quat::FromAxisAngle(FVec3::UnitX(), RadF{-std::numbers::pi_v<float> * 0.5f});
                const auto decalRot = (yawRot * baseRot).Normalized();

                float     radius    = 3.4f * exp.scale;
                float     projDepth = 5.0f * exp.scale;
                FVec3 decalScale(radius * 2.0f, radius * 2.0f, projDepth);

                const auto localMat = ComposeTransform(exp.origin, decalRot, decalScale);

                // Autonomous entity with CraterDecalComponent
                const auto [craterTexture, craterNormal] = ExplosionSystem::CraterTextures();
                ecb.AddComponent(
                    crater, Components::NameComponent {.name = String64("ArtilleryCraterDecal")},
                    Components::TransformComponent {.position = exp.origin, .rotation = decalRot, .scale = decalScale},
                    Components::WorldTransformComponent {.world = localMat, .previous = localMat},
                    Components::DecalComponent {.albedoMap = craterTexture, .normalMap = craterNormal, .roughness = 0.96f, .metallic = 0.0f},
                    CraterDecalComponent {
                        .age          = 0.0f,
                        .lifetime     = 28.0f,
                        .fadeDuration = 6.0f,
                        .baseRadius   = radius,
                        .baseDepth    = projDepth,
                        .origin       = exp.origin,
                        .rotation     = decalRot
                    }
                );
            }

            // Update Particle Emitters
            ExplosionSystem::AdvanceAndRender(exp, *scene, dt);

            // Clean up explosion particle root and children when particles finish
            if (exp.age > exp.duration) {
                if (exp.debrisEntity != Entity::Null()) {
                    ecb.DestroyEntity(exp.debrisEntity);
                }
                ecb.DestroyEntity(e); // Atomically destroys root and its LightComponent
            }
        }
    }

    // --------------------------------------------------------------------
    // 2. UPDATE PERSISTENT CRATER DECALS (Long-lived & Smooth Fade)
    // --------------------------------------------------------------------
    auto craterEntities = query.Entities<CraterDecalComponent>();
    if (!craterEntities.empty()) {
        auto craters = query.Raw<CraterDecalComponent>();

        for (size_t i = 0; i < craterEntities.size(); ++i) {
            Entity                craterEnt = craterEntities[i];
            CraterDecalComponent& crater    = craters[i];
            if (query.Get<Components::PendingDestroy>(craterEnt)) {
                continue;
            }
            crater.age += dt;

            // Handle smooth dissolution over the final fadeDuration seconds
            if (crater.age >= (crater.lifetime - crater.fadeDuration)) {
                float remaining = std::max(0.0f, crater.lifetime - crater.age);
                float fadeRatio = remaining / crater.fadeDuration; // 1.0 -> 0.0

                float ease          = std::pow(fadeRatio, 0.75f);
                float currentRadius = crater.baseRadius * ease;
                float currentDepth  = crater.baseDepth * ease;

                FVec3 newScale(currentRadius * 2.0f, currentRadius * 2.0f, currentDepth);

                query.Patch<Components::TransformComponent>(craterEnt, [&](auto& trans) { trans.scale = newScale; });
                query.Patch<Components::WorldTransformComponent>(craterEnt, [&](auto& wt) {
                    wt.world = ComposeTransform(crater.origin, crater.rotation, newScale);
                });
            }

            if (crater.age >= crater.lifetime) {
                ecb.DestroyEntity(craterEnt);
            }
        }
    }
}

} // namespace ZHLN
