// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TransformSystem.hpp"
#include "Zahlen/Engine.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ecs/ECS.hpp>

namespace ZHLN {

namespace {

JPH::Mat44 GetLogicalWorldTransform(const ECS::Registry& reg, Entity e) noexcept {
    const auto* trans       = reg.Get<Components::TransformComponent>(e);
    JPH::Mat44  localMatrix = (trans != nullptr) ? trans->GetLocalMatrix() : JPH::Mat44::sIdentity();

    const auto* hierarchy = reg.Get<Components::HierarchyComponent>(e);
    if ((hierarchy != nullptr) && hierarchy->parent != Entity::Null() && reg.IsAlive(hierarchy->parent)) {
        static thread_local int recursionDepth = 0;
        if (recursionDepth > 16) {
            return localMatrix;
        }
        recursionDepth++;
        JPH::Mat44 parentLogical = GetLogicalWorldTransform(reg, hierarchy->parent);
        recursionDepth--;
        return parentLogical * localMatrix;
    }
    return localMatrix;
}

} // namespace

JPH::Mat44 TransformSystem::GetWorldTransform(const ECS::Registry& reg, Entity e) const noexcept {
    const auto* trans       = reg.Get<Components::TransformComponent>(e);
    JPH::Mat44  localMatrix = (trans != nullptr) ? trans->GetLocalMatrix() : JPH::Mat44::sIdentity();

    const auto* hierarchy = reg.Get<Components::HierarchyComponent>(e);
    if ((hierarchy != nullptr) && hierarchy->parent != Entity::Null() && reg.IsAlive(hierarchy->parent)) {
        JPH::Mat44 parentLogical = GetLogicalWorldTransform(reg, hierarchy->parent);
        return parentLogical * localMatrix;
    }

    return localMatrix;
}

void TransformSystem::ResolveTransforms(ECS::Registry& reg) const noexcept {
    auto entities = reg.GetEntitiesWith<Components::TransformComponent>();

    for (Entity e: entities) {
        JPH::Mat44 computedWorld = GetWorldTransform(reg, e);
        auto*      worldComp     = reg.Get<Components::WorldTransformComponent>(e);

        if (worldComp == nullptr) {
            worldComp           = &reg.Add<Components::WorldTransformComponent>(e);
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
