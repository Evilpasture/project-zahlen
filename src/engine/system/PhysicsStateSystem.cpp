// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsStateSystem.hpp"
#include "TransformSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>

namespace ZHLN::Tests {
static void VerifyRealVisualInterpolation(Engine& engine, float alpha) noexcept {
    static bool testsRun = false;
    if (testsRun) {
        return;
    }

    auto& reg      = engine.GetRegistry();
    auto  entities = reg.GetEntitiesWith<Components::PhysicsStateComponent>();
    if (entities.empty()) {
        return; // Wait until we have at least one active physics state
    }
    testsRun = true;

    auto  states       = reg.GetRawArray<Components::PhysicsStateComponent>();
    float clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);

    for (size_t i = 0; i < entities.size(); ++i) {
        Entity      e     = entities[i];
        const auto& state = states[i];
        const auto* trans = reg.Get<Components::Components::TransformComponent>(e);
        if (trans != nullptr) {
            JPH::Vec3 expected = state.prevPosition + clampedAlpha * (state.currPosition - state.prevPosition);
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
} // namespace ZHLN::Tests

namespace ZHLN {

void PhysicsStateSystem::Reconcile(Engine& engine) noexcept {
    engine.GetPhysicsContext().ReconcileOrphanedBodies(engine.GetRegistry().AliveQuery());
}

void PhysicsStateSystem::WriteBack(Engine& engine) noexcept {
    auto& reg = engine.GetRegistry();
    auto& pc  = engine.GetPhysicsContext();

    auto entities  = reg.GetEntitiesWith<Components::PhysicsComponent>();
    auto physComps = reg.GetRawArray<Components::PhysicsComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        Entity e     = entities[i];
        auto&  phys  = physComps[i];
        auto*  state = reg.Get<Components::PhysicsStateComponent>(e);

        if (state != nullptr) {
            // DespawnEntity marks a slot pending immediately, while plain
            // Registry::Destroy is reconciled at the next physics phase. The
            // context validates that lifecycle state and returns one coherent
            // interpolation snapshot without exposing PhysicsWorld's dense SoA.
            Physics::BodyStateSnapshot bodyState {};
            if (!pc.TryGetBodyState(phys.physicsHandle, bodyState)) {
                continue;
            }

            state->lastPhysicsSyncFrame = engine.GetCurrentFrame();
            state->prevPosition         = bodyState.previousPosition;
            state->currPosition         = bodyState.currentPosition;

            if (bodyState.isCharacter) {
                auto* move = reg.Get<Components::MovementComponent>(e);
                if (move != nullptr) {
                    state->prevRotation = move->prevOrientation;
                    state->currRotation = move->orientation;
                } else {
                    state->prevRotation = JPH::Quat::sIdentity();
                    state->currRotation = JPH::Quat::sIdentity();
                }
            } else {
                state->prevRotation = bodyState.previousRotation;
                state->currRotation = bodyState.currentRotation;
            }
        }
    }
}

void VisualInterpolationSystem::Update(Engine& engine, float alpha) noexcept {
    auto& reg      = engine.GetRegistry();
    auto  entities = reg.GetEntitiesWith<Components::PhysicsStateComponent>();
    auto  states   = reg.GetRawArray<Components::PhysicsStateComponent>();

    // Strict clamp prevents floating-point accumulator noise from overshooting the bounds
    float clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);

    for (size_t i = 0; i < entities.size(); ++i) {
        Entity e     = entities[i];
        auto&  state = states[i];
        auto*  trans = reg.Get<Components::Components::TransformComponent>(e);

        if (trans != nullptr) {
            trans->position = state.prevPosition + clampedAlpha * (state.currPosition - state.prevPosition);
            trans->rotation = state.prevRotation.SLERP(state.currRotation, clampedAlpha);
        }
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyRealVisualInterpolation(engine, alpha);
    }
}

} // namespace ZHLN
