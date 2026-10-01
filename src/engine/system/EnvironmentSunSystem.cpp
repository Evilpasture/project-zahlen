// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "EnvironmentSunSystem.hpp"
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <numbers>
#include <string_view>
#include <vector>

namespace ZHLN {

void EnvironmentSunSystem::Update(ECS::Registry& registry, ECS::Res<AssetManager> assets) {
    using EnvironmentSunTagComponent = Components::EnvironmentSunTagComponent;
    using EnvironmentMapComponent = Components::EnvironmentMapComponent;
    using LightComponent = Components::LightComponent;
    using SunTagComponent = Components::SunTagComponent;
    using PendingDestroy = Components::PendingDestroy;
    const auto ownedSpan = registry.GetEntitiesWith<EnvironmentSunTagComponent>();
    const Entity owned = ownedSpan.empty() ? Entity::Null() : ownedSpan.front();

    bool authoredSun = false;
    for (const Entity entity: registry.GetEntitiesWith<LightComponent>()) {
        const auto* light = registry.Get<LightComponent>(entity);
        if (registry.Get<EnvironmentSunTagComponent>(entity) == nullptr &&
            registry.Get<PendingDestroy>(entity) == nullptr && light != nullptr && light->type == LightType::Sun) {
            authoredSun = true;
            break;
        }
    }
    if (!authoredSun) {
        for (const Entity entity: registry.GetEntitiesWith<SunTagComponent>()) {
            if (registry.Get<EnvironmentSunTagComponent>(entity) == nullptr && registry.Get<PendingDestroy>(entity) == nullptr) {
                authoredSun = true;
                break;
            }
        }
    }

    const Entity envEntity = registry.SingletonEntity<EnvironmentMapComponent>();
    const auto* env = registry.Get<EnvironmentMapComponent>(envEntity);
    const auto pixels = env != nullptr && !env->source.empty() && registry.Get<PendingDestroy>(envEntity) == nullptr
        ? assets->FindEnvironmentImage(std::string_view(env->source)) : std::nullopt;
    const EnvironmentSun* sun = pixels && pixels->sun ? &*pixels->sun : nullptr;

    if (authoredSun || sun == nullptr) {
        if (ownedSpan.empty()) return;
        // These entities contain only ECS data, no GPU/physics/audio handles.
        // Structural writes are serialized by the graph's Registry& hazard.
        const std::vector<Entity> obsolete {ownedSpan.begin(), ownedSpan.end()};
        for (const Entity entity: obsolete) registry.Destroy(entity);
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
        registry.Create(EnvironmentSunTagComponent {}, baked);
    } else if (registry.Get<LightComponent>(owned) != nullptr) {
        registry.Patch<LightComponent>(owned, [&](auto& light) { light = baked; });
    } else {
        registry.Add(owned, baked);
    }
    // Guard against stale duplicates after a scene import or data-only edit.
    if (ownedSpan.size() > 1) {
        const std::vector<Entity> duplicates {ownedSpan.begin() + 1, ownedSpan.end()};
        for (const Entity entity: duplicates) registry.Destroy(entity);
    }
}

} // namespace ZHLN
