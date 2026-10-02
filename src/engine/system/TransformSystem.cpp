// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TransformSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN {
namespace {

using TransformQuery = ECS::Query<const Components::HierarchyComponent, const Components::TransformComponent,
                                  Components::WorldTransformComponent&>;

JPH::Mat44 GetLogicalWorldTransform(TransformQuery query, Entity e) noexcept {
    const auto trans       = query.Get<Components::TransformComponent>(e);
    JPH::Mat44 localMatrix = trans ? trans->GetLocalMatrix() : JPH::Mat44::sIdentity();

    const auto hierarchy = query.Get<Components::HierarchyComponent>(e);
    if (hierarchy && hierarchy->parent != Entity::Null() && query.IsAlive(hierarchy->parent)) {
        static thread_local int recursionDepth = 0;
        if (recursionDepth > 16) {
            return localMatrix;
        }
        ++recursionDepth;
        JPH::Mat44 parentLogical = GetLogicalWorldTransform(query, hierarchy->parent);
        --recursionDepth;
        return parentLogical * localMatrix;
    }
    return localMatrix;
}

JPH::Mat44 GetWorldTransform(TransformQuery query, Entity e) noexcept {
    const auto trans       = query.Get<Components::TransformComponent>(e);
    JPH::Mat44 localMatrix = trans ? trans->GetLocalMatrix() : JPH::Mat44::sIdentity();

    const auto hierarchy = query.Get<Components::HierarchyComponent>(e);
    if (hierarchy && hierarchy->parent != Entity::Null() && query.IsAlive(hierarchy->parent)) {
        return GetLogicalWorldTransform(query, hierarchy->parent) * localMatrix;
    }
    return localMatrix;
}

} // namespace

void TransformSystem::Update(TransformQuery query, ECS::Registry& registry) noexcept {
    for (Entity e: query.Entities<Components::TransformComponent>()) {
        JPH::Mat44 computedWorld = GetWorldTransform(query, e);
        auto       worldComp     = query.Get<Components::WorldTransformComponent>(e);

        if (!worldComp) {
            worldComp           = registry.Add<Components::WorldTransformComponent>(e);
            worldComp->previous = computedWorld;
        }
        worldComp->world = computedWorld;
    }
}

void TransformSystem::UpdateTransformHistory(ECS::Registry& reg) noexcept {
    auto entities        = reg.GetEntitiesWith<Components::WorldTransformComponent>();
    auto worldTransforms = reg.GetRawArray<Components::WorldTransformComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        worldTransforms[i].previous = worldTransforms[i].world;
    }
}

} // namespace ZHLN
