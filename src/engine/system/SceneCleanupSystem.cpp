// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SceneCleanupSystem.hpp"
#include "../Hierarchy.hpp"
#include <Zahlen/ECS/Components.hpp>
#include <ecs/ECS.hpp>
#include <ecs/EntityCommandBuffer.hpp>
#include <ecs/SystemParameters.hpp>
#include <engine/ArticulationContext.hpp>
#include <engine/Engine.hpp>
#include <engine/World.hpp>
#include <engine/SceneResources.hpp>
#include <physics/Physics.hpp>
#include <span>
#include <utility>
#include <vector>

namespace ZHLN {
namespace {

template <typename T>
void ReleaseSelected(Engine& engine, ECS::Query<T&, const Components::PendingDestroy> query, bool all) {
    const auto entities = query.template Entities<T>();
    if (entities.empty()) {
        return;
    }
    auto components = query.template Raw<T>();
    for (size_t i = 0; i < entities.size(); ++i) {
        if (all || query.template Get<Components::PendingDestroy>(entities[i]).has_value()) {
            SceneResources::Release(engine, components[i]);
        }
    }
}

using PhysicsCleanupQuery = ECS::Query<
    Components::StaticBodyComponent&,
    Components::KinematicBodyComponent&,
    Components::DynamicBodyComponent&,
    Components::CharacterBodyComponent&,
    Components::RagdollComponent&,
    const Components::PendingDestroy>;

template <typename T>
void CollectSelectedBodies(PhysicsCleanupQuery& query, bool all, std::vector<Physics::BodyID>& bodies) {
    const auto owners = query.template Entities<T>();
    if (owners.empty()) {
        return;
    }
    auto components = query.template Raw<T>();
    for (size_t i = 0; i < owners.size(); ++i) {
        if (all || query.template Get<Components::PendingDestroy>(owners[i]).has_value()) {
            using Handle = decltype(T::bodyID);
            bodies.push_back(std::exchange(components[i].bodyID, Handle::Null()));
        }
    }
}

void ReleasePhysics(Engine& engine, PhysicsCleanupQuery query, bool all) {
    auto& physics = engine.Physics();

    // Queue all body releases under one physics shadow lock while components
    // remain readable; ragdoll instances are released in a second bulk call.
    std::vector<Physics::BodyID> bodies;
    CollectSelectedBodies<Components::StaticBodyComponent>(query, all, bodies);
    CollectSelectedBodies<Components::KinematicBodyComponent>(query, all, bodies);
    CollectSelectedBodies<Components::DynamicBodyComponent>(query, all, bodies);
    CollectSelectedBodies<Components::CharacterBodyComponent>(query, all, bodies);
    if (!bodies.empty()) {
        physics.DestroyBodies(std::span<const Physics::BodyID> {bodies});
    }

    const auto ragdollOwners = query.Entities<Components::RagdollComponent>();
    if (!ragdollOwners.empty()) {
        std::vector<Physics::RagdollID> ragdolls;
        ragdolls.reserve(ragdollOwners.size());
        auto components = query.Raw<Components::RagdollComponent>();
        for (size_t i = 0; i < ragdollOwners.size(); ++i) {
            if (all || query.Get<Components::PendingDestroy>(ragdollOwners[i]).has_value()) {
                engine.GetWorld().GetArticulationContext().RemoveJointState(components[i].ragdollID);
                ragdolls.push_back(std::exchange(components[i].ragdollID, Physics::RagdollID::Invalid));
                components[i].isAddedToPhysics = false;
            }
        }
        physics.DestroyRagdolls(ragdolls);
    }
}

void ReleaseOwnedResources(Engine& engine, ECS::Registry& registry, bool all) {
    // This lifecycle boundary must also work before graph initialization and
    // during teardown. Construct narrow queries here rather than scheduling
    // cleanup into a graph or playing an unrelated command buffer.
    ReleasePhysics(engine, PhysicsCleanupQuery {registry}, all);
    ReleaseSelected<Components::AudioSourceComponent>(engine, ECS::Query<Components::AudioSourceComponent&, const Components::PendingDestroy> {registry}, all);
    ReleaseSelected<Components::OwnedMeshComponent>(engine, ECS::Query<Components::OwnedMeshComponent&, const Components::PendingDestroy> {registry}, all);
    ReleaseSelected<Components::SkeletalMeshComponent>(engine, ECS::Query<Components::SkeletalMeshComponent&, const Components::PendingDestroy> {registry}, all);
    ReleaseSelected<Components::MorphTargetComponent>(engine, ECS::Query<Components::MorphTargetComponent&, const Components::PendingDestroy> {registry}, all);
    engine.RunSceneCleanupPasses(all);
}

} // namespace

void SceneCleanupSystem::ProcessPending(Engine& engine, ECS::Registry& registry) {
    const auto marked = registry.GetEntitiesWith<Components::PendingDestroy>();
    if (marked.empty()) {
        return;
    }

    // Expand all marked roots in one hierarchy pass (also catches children
    // attached after a parent was marked), then query the intact owners.
    ExpandPendingDestroy(registry);
    ReleaseOwnedResources(engine, registry, false);

    // Destroy compacts the PendingDestroy sparse set: snapshot it first.
    const auto pending = registry.GetEntitiesWith<Components::PendingDestroy>();
    const std::vector<Entity> toDestroy(pending.begin(), pending.end());
    for (Entity entity: toDestroy) {
        registry.Destroy(entity);
    }
}

void SceneCleanupSystem::ClearAll(Engine& engine, ECS::Registry& registry) {
    ReleaseOwnedResources(engine, registry, true);
    engine.Commands().Reset();
    registry.Clear();
}

} // namespace ZHLN
