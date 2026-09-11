// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsStateSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <vector>

namespace ZHLN::Tests {
static void VerifyRealVisualInterpolation(Engine& engine, float alpha) noexcept {
    static bool testsRun = false;
    if (testsRun) {
        return;
    }

    auto& reg      = engine.GetRegistry();
    auto  entities = reg.GetEntitiesWith<Components::PhysicsComponent>();
    if (entities.empty()) {
        return;
    }
    testsRun = true;

    auto physComps     = reg.GetRawArray<Components::PhysicsComponent>();
    float clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);

    std::vector<Entity>                     handles;
    std::vector<size_t>                     sourceIndex;
    std::vector<Physics::BodyStateSnapshot> snapshots;
    handles.reserve(entities.size());
    sourceIndex.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++i) {
        if (physComps[i].isStatic) {
            continue;
        }
        handles.push_back(physComps[i].physicsHandle);
        sourceIndex.push_back(i);
    }
    snapshots.resize(handles.size());
    engine.GetPhysicsContext().FillBodyStates(handles, snapshots);

    for (size_t j = 0; j < snapshots.size(); ++j) {
        if (!snapshots[j].valid) {
            continue;
        }
        Entity      e     = entities[sourceIndex[j]];
        const auto* trans = reg.Get<Components::TransformComponent>(e);
        if (trans == nullptr) {
            continue;
        }
        JPH::Vec3 expected = snapshots[j].previousPosition + clampedAlpha * (snapshots[j].currentPosition - snapshots[j].previousPosition);
        JPH::Vec3 actual(trans->position[0], trans->position[1], trans->position[2]);
        if ((actual - expected).LengthSq() > 1e-3f) {
            ZHLN::Log(
                "[Test Fail] Real Visual Interpolation Invariant Failure on Entity {}: "
                "Expected position ({}, {}, {}), got ({}, {}, {}) at alpha = {}",
                e.index, expected.GetX(), expected.GetY(), expected.GetZ(), actual.GetX(), actual.GetY(), actual.GetZ(), clampedAlpha
            );
        }
    }
}
} // namespace ZHLN::Tests

namespace ZHLN {

void PhysicsStateSystem::Reconcile(Engine& engine) noexcept {
    engine.GetPhysicsContext().ReconcileOrphanedBodies(engine.GetRegistry().AliveQuery());
}

void VisualInterpolationSystem::Update(Engine& engine, float alpha) noexcept {
    auto& reg      = engine.GetRegistry();
    auto  entities = reg.GetEntitiesWith<Components::PhysicsComponent>();
    auto  phys     = reg.GetRawArray<Components::PhysicsComponent>();

    float clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);

    thread_local std::vector<Entity>                     handles;
    thread_local std::vector<size_t>                     sourceIndex;
    thread_local std::vector<Physics::BodyStateSnapshot> snapshots;
    handles.clear();
    sourceIndex.clear();
    handles.reserve(entities.size());
    sourceIndex.reserve(entities.size());

    for (size_t i = 0; i < entities.size(); ++i) {
        if (phys[i].isStatic) {
            continue;
        }
        handles.push_back(phys[i].physicsHandle);
        sourceIndex.push_back(i);
    }

    snapshots.resize(handles.size());
    engine.GetPhysicsContext().FillBodyStates(handles, snapshots);

    for (size_t j = 0; j < snapshots.size(); ++j) {
        if (!snapshots[j].valid) {
            continue;
        }
        Entity e     = entities[sourceIndex[j]];
        auto*  trans = reg.Get<Components::TransformComponent>(e);
        if (trans == nullptr) {
            continue;
        }

        const auto& snap = snapshots[j];
        trans->position  = snap.previousPosition + clampedAlpha * (snap.currentPosition - snap.previousPosition);

        if (auto* move = reg.Get<Components::MovementComponent>(e); move != nullptr) {
            trans->rotation = move->prevOrientation.SLERP(move->orientation, clampedAlpha);
        } else {
            trans->rotation = snap.previousRotation.SLERP(snap.currentRotation, clampedAlpha);
        }
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyRealVisualInterpolation(engine, alpha);
    }
}

} // namespace ZHLN
