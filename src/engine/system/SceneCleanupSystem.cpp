// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Engine/Internal/EngineAccess.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include "SceneCleanupSystem.hpp"
#include "../Hierarchy.hpp"
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/SceneResources.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <span>
#include <utility>
#include <vector>

namespace ZHLN {
namespace {

template <typename T>
void ReleaseSelected(Engine& engine, bool all) {
    auto& registry = ZHLN::EngineAccess::RegistryOf(engine);
    const auto entities = registry.GetEntitiesWith<T>();
    if (entities.empty()) {
        return;
    }
    auto components = registry.GetRawArray<T>();
    for (size_t i = 0; i < entities.size(); ++i) {
        if (all || registry.Get<Components::PendingDestroy>(entities[i]).has_value()) {
            SceneResources::Release(engine, components[i]);
        }
    }
}

void ReleasePhysics(Engine& engine, bool all) {
    auto& registry = ZHLN::EngineAccess::RegistryOf(engine);
    auto physics = engine.Physics();

    // Queue all body releases under one physics shadow lock while components
    // remain readable; ragdoll instances are released in a second bulk call.
    const auto bodyOwners = registry.GetEntitiesWith<Components::PhysicsComponent>();
    if (!bodyOwners.empty()) {
        std::vector<Physics::BodyID> bodies;
        bodies.reserve(bodyOwners.size());
        auto components = registry.GetRawArray<Components::PhysicsComponent>();
        for (size_t i = 0; i < bodyOwners.size(); ++i) {
            if (all || registry.Get<Components::PendingDestroy>(bodyOwners[i]).has_value()) {
                bodies.push_back(std::exchange(components[i].physicsID, Physics::BodyID::Null()));
            }
        }
        physics.DestroyBodies(std::span<const Physics::BodyID> {bodies});
    }

    const auto ragdollOwners = registry.GetEntitiesWith<Components::RagdollComponent>();
    if (!ragdollOwners.empty()) {
        std::vector<Physics::RagdollID> ragdolls;
        ragdolls.reserve(ragdollOwners.size());
        auto components = registry.GetRawArray<Components::RagdollComponent>();
        for (size_t i = 0; i < ragdollOwners.size(); ++i) {
            if (all || registry.Get<Components::PendingDestroy>(ragdollOwners[i]).has_value()) {
                ragdolls.push_back(std::exchange(components[i].ragdollID, Physics::RagdollID::Invalid));
                components[i].isAddedToPhysics = false;
            }
        }
        physics.DestroyRagdolls(ragdolls);
    }
}

void ReleaseOwnedResources(Engine& engine, bool all) {
    ReleasePhysics(engine, all);
    ReleaseSelected<Components::AudioSourceComponent>(engine, all);
    ReleaseSelected<Components::OwnedMeshComponent>(engine, all);
    ReleaseSelected<Components::SkeletalMeshComponent>(engine, all);
    engine.RunSceneCleanupPasses(all);
}

} // namespace

void SceneCleanupSystem::ProcessPending(Engine& engine) {
    auto& registry = ZHLN::EngineAccess::RegistryOf(engine);
    const auto marked = registry.GetEntitiesWith<Components::PendingDestroy>();
    if (marked.empty()) {
        return;
    }

    // Expand all marked roots in one hierarchy pass (also catches children
    // attached after a parent was marked), then query the intact owners.
    ExpandPendingDestroy(registry);
    ReleaseOwnedResources(engine, false);

    // Destroy compacts the PendingDestroy sparse set: snapshot it first.
    const auto pending = registry.GetEntitiesWith<Components::PendingDestroy>();
    const std::vector<Entity> toDestroy(pending.begin(), pending.end());
    for (Entity entity: toDestroy) {
        registry.Destroy(entity);
    }
}

void SceneCleanupSystem::ClearAll(Engine& engine) {
    ReleaseOwnedResources(engine, true);
    engine.Commands().Reset();
    ZHLN::EngineAccess::RegistryOf(engine).Clear();
}

} // namespace ZHLN
