// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsStateSystem.hpp"
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Config.hpp>
#include <engine/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <algorithm>
#include <ecs/ECS.hpp>
#include <physics/Physics.hpp>
#include <vector>

namespace ZHLN {
namespace {

template <typename T>
void AppendRoleBodies(
    const VisualInterpolationSystem::InterpolationQuery& query,
    std::vector<Physics::BodyID>&                        handles,
    std::vector<Entity>&                                 sourceEntities
) {
    const auto entities = query.template Entities<T>();
    const auto raw      = query.template Raw<T>();
    for (size_t i = 0; i < entities.size(); ++i) {
        handles.push_back(raw[i].bodyID);
        sourceEntities.push_back(entities[i]);
    }
}

} // namespace

namespace Tests {
static void VerifyRealVisualInterpolation(
    VisualInterpolationSystem::InterpolationQuery query,
    const PhysicsContext&                         physics,
    float                                         alpha,
    bool&                                         verified
) noexcept {
    if (verified) {
        return;
    }

    std::vector<Physics::BodyID> handles;
    std::vector<Entity>          sourceEntities;
    AppendRoleBodies<Components::DynamicBodyComponent>(query, handles, sourceEntities);
    AppendRoleBodies<Components::CharacterBodyComponent>(query, handles, sourceEntities);
    if (handles.empty()) {
        return;
    }
    verified = true;

    float clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);

    std::vector<Physics::BodyStateSnapshot> snapshots(handles.size());
    physics.FillBodyStates(handles, snapshots);

    for (size_t j = 0; j < snapshots.size(); ++j) {
        if (!snapshots[j].valid) {
            continue;
        }
        Entity     e     = sourceEntities[j];
        const auto trans = query.Get<Components::TransformComponent>(e);
        if (!trans) {
            continue;
        }
        const auto expected = Lerp(snapshots[j].previousPosition, snapshots[j].currentPosition, clampedAlpha);
        const auto actual   = trans->position;
        if ((actual - expected).LengthSquared() > 1e-3f) {
            kLog.Warn(
                "Visual interpolation drift on entity {}: expected ({}, {}, {}), got ({}, {}, {}) at alpha = {}",
                e.index, expected.x, expected.y, expected.z, actual.x, actual.y, actual.z, clampedAlpha
            );
        }
    }
}
} // namespace Tests

void VisualInterpolationSystem::Update(
    InterpolationQuery               query,
    ECS::Res<PhysicsContext>         physics,
    FrameAlpha                       alpha,
    ECS::Local<InterpolationScratch> scratch
) noexcept {
    float clampedAlpha = std::clamp(alpha.value, 0.0f, 1.0f);

    auto& handles        = scratch->handles;
    auto& sourceEntities = scratch->sourceEntities;
    auto& snapshots      = scratch->snapshots;
    handles.clear();
    sourceEntities.clear();

    const size_t total = query.Entities<Components::DynamicBodyComponent>().size() +
                         query.Entities<Components::CharacterBodyComponent>().size();
    handles.reserve(total);
    sourceEntities.reserve(total);
    AppendRoleBodies<Components::DynamicBodyComponent>(query, handles, sourceEntities);
    AppendRoleBodies<Components::CharacterBodyComponent>(query, handles, sourceEntities);

    snapshots.resize(handles.size());
    physics->FillBodyStates(handles, snapshots);

    for (size_t j = 0; j < snapshots.size(); ++j) {
        if (!snapshots[j].valid) {
            continue;
        }
        Entity e     = sourceEntities[j];
        auto   trans = query.Get<Components::TransformComponent>(e);
        if (!trans) {
            continue;
        }

        const auto& snap = snapshots[j];
        trans->position  = Lerp(snap.previousPosition, snap.currentPosition, clampedAlpha);
        trans->rotation  = Slerp(Quat {snap.previousRotation}, Quat {snap.currentRotation}, clampedAlpha);
        const auto motor      = query.Get<Components::CharacterMotorComponent>(e);
        const auto motorState = query.Get<Components::CharacterMotorStateComponent>(e);
        const auto ragdoll    = query.Get<Components::RagdollComponent>(e);
        if (motor && motor->driveFacing && motorState && (!ragdoll || ragdoll->state == RagdollState::Inactive))
            trans->rotation = Slerp(motorState->previousFacing, motorState->facing, clampedAlpha);
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyRealVisualInterpolation(query, *physics, alpha.value, scratch->invariantVerified);
    }
}

} // namespace ZHLN
