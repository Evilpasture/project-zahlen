// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EnvironmentSunSystem.hpp"
#include <Zahlen/Engine/AssetManager.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#include <numbers>
#include <string_view>

namespace ZHLN {

void EnvironmentSunSystem::Update(SunEnvQuery query, ECS::EntityCommandBuffer& ecb, ECS::Res<AssetManager> assets) {
    using EnvironmentSunTagComponent = Components::EnvironmentSunTagComponent;
    using EnvironmentMapComponent = Components::EnvironmentMapComponent;
    using LightComponent = Components::LightComponent;
    using SunTagComponent = Components::SunTagComponent;
    using PendingDestroy = Components::PendingDestroy;
    const auto ownedSpan = query.Entities<EnvironmentSunTagComponent>();
    const Entity owned = ownedSpan.empty() ? Entity::Null() : ownedSpan.front();

    bool authoredSun = false;
    for (const Entity entity: query.Entities<LightComponent>()) {
        const auto light = query.Get<LightComponent>(entity);
        if (!query.Get<EnvironmentSunTagComponent>(entity) &&
            !query.Get<PendingDestroy>(entity) && light && light->type == LightType::Sun) {
            authoredSun = true;
            break;
        }
    }
    if (!authoredSun) {
        for (const Entity entity: query.Entities<SunTagComponent>()) {
            if (!query.Get<EnvironmentSunTagComponent>(entity) && !query.Get<PendingDestroy>(entity)) {
                authoredSun = true;
                break;
            }
        }
    }

    const auto envEntities = query.Entities<EnvironmentMapComponent>();
    const Entity envEntity = envEntities.empty() ? Entity::Null() : envEntities.front();
    const auto   env       = envEntity != Entity::Null() ? query.Get<EnvironmentMapComponent>(envEntity) : Optional<const EnvironmentMapComponent&> {};
    const auto   pixels    = env && !env->source.empty() && !query.Get<PendingDestroy>(envEntity)
        ? assets->FindEnvironmentImage(std::string_view(env->source)) : std::nullopt;
    const EnvironmentSun* sun = pixels && pixels->sun ? &*pixels->sun : nullptr;

    if (authoredSun || sun == nullptr) {
        if (ownedSpan.empty()) return;
        for (const Entity entity: ownedSpan) {
            if (!query.Get<PendingDestroy>(entity)) {
                ecb.DestroyEntity(entity);
            }
        }
        if (authoredSun && sun != nullptr) {
            Log("[IBL] Authored sun replaces cooked HDR sun; conditioned IBL and original visible sky remain in use.");
        }
        return;
    }

    const auto& dir = sun->direction;
    const auto& energy = sun->irradiance;
    const Components::LightComponent baked {
        .type = LightType::Sun,
        .color = JPH::Vec3(energy[0], energy[1], energy[2]) * std::numbers::pi_v<float>,
        .intensity = 1.0f,
        .direction = JPH::Vec3(dir[0], dir[1], dir[2]).Normalized(),
    };
    if (owned == Entity::Null()) {
        ecb.CreateEntity(EnvironmentSunTagComponent {}, baked);
    } else if (auto existingLight = query.Get<LightComponent>(owned)) {
        *existingLight = baked;
    } else {
        ecb.AddComponent(owned, baked);
    }
    // Guard against stale duplicates after a scene import or data-only edit.
    if (ownedSpan.size() > 1) {
        for (const Entity entity: ownedSpan.subspan(1)) {
            if (!query.Get<PendingDestroy>(entity)) {
                ecb.DestroyEntity(entity);
            }
        }
    }
}

} // namespace ZHLN
