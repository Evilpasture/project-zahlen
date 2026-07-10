// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/system/TransformSystem.cpp
#include "TransformSystem.hpp"
#include "Zahlen/Engine.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Log.hpp>
#include <ecs/ECS.hpp>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN::Tests {
static void VerifyRealTransforms(const ECS::Registry& reg) noexcept {
    static bool testsRun = false;
    if (testsRun) {
        return;
    }

    auto entities     = reg.GetEntitiesWith<Components::MeshComponent>();
    bool hasHierarchy = false;
    for (Entity e: entities) {
        const auto* hierarchy = reg.Get<Components::HierarchyComponent>(e);
        if ((hierarchy != nullptr) && hierarchy->parent != NullEntity && reg.IsAlive(hierarchy->parent)) {
            hasHierarchy = true;
            break;
        }
    }

    if (!hasHierarchy) {
        return; // Wait until a real hierarchical relationship is loaded
    }
    testsRun = true;

    auto* engine = ZHLN::GetEngineContext();
    if (engine == nullptr) {
        return;
    }
    uint64_t currentFrame = engine->GetCurrentFrame();

    auto meshes = reg.GetRawArray<Components::MeshComponent>();
    for (size_t i = 0; i < entities.size(); ++i) {
        Entity      e         = entities[i];
        const auto* hierarchy = reg.Get<Components::HierarchyComponent>(e);

        // 1. Check for Temporal Aliasing (Is the data stale?)
        const auto* physState = reg.Get<Components::PhysicsStateComponent>(e);
        if ((physState != nullptr) && currentFrame > 1 && (physState->lastPhysicsSyncFrame < currentFrame - 1)) {
            ZHLN::Log(
                "[Test Fail] Stutter Warning: Entity {} has stale physics data! (Last Sync "
                "Frame: {}, Current Frame: {})",
                e.index, physState->lastPhysicsSyncFrame, currentFrame
            );
        }

        // 2. Check for Jitter (Did the transform move more than reasonable in 1 frame?)
        const auto& mesh = meshes[i];
        if (mesh.prevTransform != JPH::Mat44::sIdentity()) {
            float deltaPos = (mesh.worldTransform.GetTranslation() - mesh.prevTransform.GetTranslation()).Length();
            if (deltaPos > 100.0f) { // Arbitrary "Speed of Light" threshold
                ZHLN::Log("[Test Fail] Jitter Detected: Entity {} moved {} units in one frame!", e.index, deltaPos);
            }
        }

        // 3. Check for hierarchical invariants
        if ((hierarchy != nullptr) && hierarchy->parent != NullEntity && reg.IsAlive(hierarchy->parent)) {
            const auto&     childMesh = meshes[i];
            TransformSystem ts;
            JPH::Mat44      computed    = ts.GetWorldTransform(reg, e);
            JPH::Vec3       actualPos   = childMesh.worldTransform.GetTranslation();
            JPH::Vec3       expectedPos = computed.GetTranslation();
            if ((actualPos - expectedPos).LengthSq() > 1e-3f) {
                ZHLN::Log(
                    "[Test Fail] Real Transform System Invariant Failure on Entity {}: "
                    "Expected translation ({}, {}, {}), got ({}, {}, {})",
                    e.index, expectedPos.GetX(), expectedPos.GetY(), expectedPos.GetZ(), actualPos.GetX(), actualPos.GetY(), actualPos.GetZ()
                );
            }
        }
    }
}
} // namespace ZHLN::Tests

namespace ZHLN {
namespace {
// Helper to calculate the logical world-space transform of an entity
JPH::Mat44 GetLogicalWorldTransform(const ECS::Registry& reg, Entity e) noexcept {
    const auto* trans       = reg.Get<Components::Components::TransformComponent>(e);
    JPH::Mat44  localMatrix = (trans != nullptr) ? trans->GetMatrix() : JPH::Mat44::sIdentity();

    const auto* hierarchy = reg.Get<Components::HierarchyComponent>(e);
    if ((hierarchy != nullptr) && hierarchy->parent != NullEntity && reg.IsAlive(hierarchy->parent)) {
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
    const auto* mesh      = reg.Get<Components::MeshComponent>(e);
    JPH::Mat44  meshLocal = (mesh != nullptr) ? mesh->localTransform : JPH::Mat44::sIdentity();

    const auto* trans       = reg.Get<Components::Components::TransformComponent>(e);
    JPH::Mat44  localMatrix = (trans != nullptr) ? trans->GetMatrix() : JPH::Mat44::sIdentity();

    const auto* hierarchy = reg.Get<Components::HierarchyComponent>(e);
    if ((hierarchy != nullptr) && hierarchy->parent != NullEntity && reg.IsAlive(hierarchy->parent)) {
        // Retrieve only the logical parent matrix (bypassing the parent's visual offset)
        JPH::Mat44 parentLogical = GetLogicalWorldTransform(reg, hierarchy->parent);

        // If the node is animated, meshLocal is already computed relative to the glTF root
        // by the AnimationSystem. We skip multiplying by the redundant static localMatrix.
        if ((mesh != nullptr) && mesh->gltfNode != nullptr && !mesh->isSkinned) {
            return parentLogical * meshLocal;
        }

        return parentLogical * localMatrix * meshLocal;
    }

    return localMatrix * meshLocal;
}

void TransformSystem::ResolveTransforms(ECS::Registry& reg) const noexcept {
    auto entities = reg.GetEntitiesWith<Components::MeshComponent>();
    auto meshes   = reg.GetRawArray<Components::MeshComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        Components::MeshComponent& mesh = meshes[i];
        Entity                     e    = entities[i];
        mesh.worldTransform             = GetWorldTransform(reg, e);
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyRealTransforms(reg);
    }
}

void TransformSystem::UpdateTransformHistory(ECS::Registry& reg) noexcept {
    auto entities = reg.GetEntitiesWith<Components::MeshComponent>();
    auto meshes   = reg.GetRawArray<Components::MeshComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        Components::MeshComponent& mesh = meshes[i];
        mesh.prevTransform              = mesh.worldTransform;
    }
}

} // namespace ZHLN
