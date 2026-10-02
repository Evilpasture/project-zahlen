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
static void VerifyRealVisualInterpolation(ECS::Query<const Components::PhysicsComponent, Components::TransformComponent&> query,
                                          const PhysicsContext& physics, float alpha) noexcept {
    static bool testsRun = false;
    if (testsRun) {
        return;
    }

    auto  entities = query.Entities<Components::PhysicsComponent>();
    if (entities.empty()) {
        return;
    }
    testsRun = true;

    auto physComps     = query.Raw<Components::PhysicsComponent>();
    float clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);

    std::vector<Physics::BodyHandle>        handles;
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
    physics.FillBodyStates(handles, snapshots);

    for (size_t j = 0; j < snapshots.size(); ++j) {
        if (!snapshots[j].valid) {
            continue;
        }
        Entity     e     = entities[sourceIndex[j]];
        const auto trans = query.Get<Components::TransformComponent>(e);
        if (!trans) {
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
}

namespace ZHLN {

void VisualInterpolationSystem::Update(ECS::Query<const Components::PhysicsComponent, Components::TransformComponent&> query,
                                       ECS::Res<PhysicsContext> physics, FrameAlpha alpha) noexcept {
    auto entities = query.Entities<Components::PhysicsComponent>();
    auto phys     = query.Raw<Components::PhysicsComponent>();

    float clampedAlpha = std::clamp(alpha.value, 0.0f, 1.0f);

    thread_local std::vector<Physics::BodyHandle>        handles;
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
    physics->FillBodyStates(handles, snapshots);

    for (size_t j = 0; j < snapshots.size(); ++j) {
        if (!snapshots[j].valid) {
            continue;
        }
        Entity e     = entities[sourceIndex[j]];
        auto   trans = query.Get<Components::TransformComponent>(e);
        if (!trans) {
            continue;
        }

        const auto& snap = snapshots[j];
        trans->position  = snap.previousPosition + clampedAlpha * (snap.currentPosition - snap.previousPosition);

        trans->rotation = snap.previousRotation.SLERP(snap.currentRotation, clampedAlpha);
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyRealVisualInterpolation(query, *physics, alpha.value);
    }
}

}
