// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Hierarchy.cpp
#include <Zahlen/Audio.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include "ArticulationSystem.hpp"
#include <unordered_set>
#include <vector>

namespace ZHLN {

namespace {

void CollectDespawnPostorder(ECS::Registry& registry, Entity entity, std::vector<Entity>& postorder, std::unordered_set<uint64_t>& seen) {
    if (!registry.IsAlive(entity) || !seen.insert(entity.Pack()).second) {
        return;
    }

    std::vector<Entity> children;
    for (const Entity candidate: registry.GetEntitiesWith<Components::HierarchyComponent>()) {
        if (const auto* hierarchy = registry.Get<Components::HierarchyComponent>(candidate); hierarchy != nullptr && hierarchy->parent == entity) {
            children.push_back(candidate);
        }
    }

    for (const Entity child: children) {
        CollectDespawnPostorder(registry, child, postorder, seen);
    }
    postorder.push_back(entity);
}


} // namespace

void DespawnEntity(Engine& engine, Entity entity) {
    auto& registry = engine.GetRegistry();
    std::vector<Entity> postorder;
    std::unordered_set<uint64_t> seen;
    CollectDespawnPostorder(registry, entity, postorder, seen);

    for (const Entity current: postorder) {
        if (!registry.IsAlive(current)) {
            continue;
        }

        // These systems keep external handles outside component storage, and
        // therefore receive the entity while its component data is still valid.
        engine.GetArticulationSystem().Release(engine, current);
        engine.GetAudioContext().ReleaseOwner(current);
        if (const auto* physics = registry.Get<Components::PhysicsComponent>(current); physics != nullptr) {
            engine.GetPhysicsContext().DestroyBody(physics->physicsHandle);
        }
        engine.GetRenderContext().ReleaseEntityBuffers(current);
        registry.Destroy(current);
    }
}

} // namespace ZHLN
