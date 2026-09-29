// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SceneCleanupSystem.hpp"
#include "../Hierarchy.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/SceneResources.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <span>
#include <utility>
#include <vector>

namespace ZHLN {
namespace {

template <typename T>
void ReleaseSelected(Engine& engine, bool all) {
    auto& registry = engine.GetRegistry();
    const auto entities = registry.GetEntitiesWith<T>();
    if (entities.empty()) {
        return;
    }
    auto components = registry.GetRawArray<T>();
    for (size_t i = 0; i < entities.size(); ++i) {
        if (all || registry.Get<Components::PendingDestroy>(entities[i]) != nullptr) {
            SceneResources::Release(engine, components[i]);
        }
    }
}

void ReleasePhysics(Engine& engine, bool all) {
    auto& registry = engine.GetRegistry();
    auto& physics  = engine.GetPhysicsContext();

    // Queue all body releases under one physics shadow lock while components
    // remain readable; ragdoll instances are released in a second bulk call.
    const auto bodyOwners = registry.GetEntitiesWith<Components::PhysicsComponent>();
    if (!bodyOwners.empty()) {
        std::vector<Physics::BodyHandle> bodies;
        bodies.reserve(bodyOwners.size());
        auto components = registry.GetRawArray<Components::PhysicsComponent>();
        for (size_t i = 0; i < bodyOwners.size(); ++i) {
            if (all || registry.Get<Components::PendingDestroy>(bodyOwners[i]) != nullptr) {
                bodies.push_back(std::exchange(components[i].physicsHandle, Physics::BodyHandle::Null()));
            }
        }
        physics.DestroyBodies(std::span<const Physics::BodyHandle> {bodies});
    }

    const auto ragdollOwners = registry.GetEntitiesWith<Components::RagdollComponent>();
    if (!ragdollOwners.empty()) {
        std::vector<Physics::RagdollHandle> ragdolls;
        ragdolls.reserve(ragdollOwners.size());
        auto components = registry.GetRawArray<Components::RagdollComponent>();
        for (size_t i = 0; i < ragdollOwners.size(); ++i) {
            if (all || registry.Get<Components::PendingDestroy>(ragdollOwners[i]) != nullptr) {
                ragdolls.push_back(std::exchange(components[i].ragdollHandle, Physics::RagdollHandle::Invalid));
                components[i].isAddedToPhysics = false;
            }
        }
        physics.DestroyRagdolls(ragdolls);
    }
}

void ReleaseOwnedResources(Engine& engine, bool all) {
    ReleasePhysics(engine, all);
    ReleaseSelected<Components::AudioSourceComponent>(engine, all);
    ReleaseSelected<Components::LoopSynthComponent>(engine, all);
    ReleaseSelected<Components::OwnedMeshComponent>(engine, all);
    ReleaseSelected<Components::ParticleEmitterComponent>(engine, all);
    ReleaseSelected<Components::MeshParticleEmitterComponent>(engine, all);
    ReleaseSelected<Components::SkeletalMeshComponent>(engine, all);
    engine.RunSceneCleanupPasses(all);
}

} // namespace

void SceneCleanupSystem::ProcessPending(Engine& engine) {
    auto& registry = engine.GetRegistry();
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
    engine.GetMainECB().Reset();
    engine.GetRegistry().Clear();
}

} // namespace ZHLN
