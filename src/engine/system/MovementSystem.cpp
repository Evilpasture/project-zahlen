// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/system/MovementSystem.cpp
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Log.hpp"
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <cmath>

namespace ZHLN::Tests {
static void VerifyMovementStateConsistency(const ECS::Registry& reg) noexcept {
    static bool testsRun = false;
    if (testsRun) {
        return;
    }

    auto entities = reg.GetEntitiesWith<Components::MovementComponent>();
    if (entities.empty()) {
        return;
    }
    testsRun = true;

    auto movements = reg.GetRawArray<Components::MovementComponent>();
    for (size_t i = 0; i < entities.size(); ++i) {
        Entity      e    = entities[i];
        const auto& move = movements[i];

        if (!move.wasGrounded && move.isGrounded && move.landingTimer <= 0.0f) {
            ZHLN::Log(
                "[Test Fail] Movement State: Entity {} transitioned from airborne to grounded but "
                "landing timer not set properly (landingTimer={})",
                e.index, move.landingTimer
            );
        }

        if (move.jumpDelayTimer < 0.0f) {
            ZHLN::Log("[Test Fail] Movement State: Entity {} has negative jump delay timer: {}", e.index, move.jumpDelayTimer);
        }

        float orientationMagSq = move.orientation.GetX() * move.orientation.GetX() + move.orientation.GetY() * move.orientation.GetY() +
                                 move.orientation.GetZ() * move.orientation.GetZ() + move.orientation.GetW() * move.orientation.GetW();
        if (std::abs(orientationMagSq - 1.0f) > 0.01f) {
            ZHLN::Log("[Test Fail] Movement State: Entity {} orientation not normalized (mag={:.4f})", e.index, std::sqrt(orientationMagSq));
        }

        float velMag = std::sqrt(move.inputX * move.inputX + move.inputZ * move.inputZ);
        if (velMag > 2.0f) {
            ZHLN::Log("[Test Fail] Movement State: Entity {} input velocity unusually high (mag={:.2f})", e.index, velMag);
        }
    }
}
} // namespace ZHLN::Tests

namespace ZHLN {

void MovementSystem(Engine& engine, float dt) {
    auto& reg = engine.GetRegistry();

    auto entities = reg.GetEntitiesWith<Components::MovementComponent>();
    if (entities.empty()) {
        return;
    }

    auto  movements = reg.GetRawArray<Components::MovementComponent>();
    auto& pc        = engine.GetPhysicsContext();

    TaskSystem::ParallelFor(entities.size(), 128, [&](uint32_t start, uint32_t end, uint32_t) {
        for (uint32_t i = start; i < end; ++i) {
            Components::MovementComponent& move = movements[i];
            Entity                         e    = entities[i];
            move.prevOrientation                = move.orientation;

            auto* phys = reg.Get<Components::PhysicsComponent>(e);
            if (!phys) {
                continue;
            }

            ECS::Patch<Components::RagdollComponent>(reg, e, [&](auto& ragComp) {
                if (ragComp.state == RagdollState::Dynamic || ragComp.state == RagdollState::Kinematic) {
                    return;
                }
            });

            bool onGround = pc.IsCharacterOnGround(phys->physicsHandle);

            move.wasGrounded = move.isGrounded;
            move.isGrounded  = onGround;

            if (move.isGrounded && !move.wasGrounded) {
                move.landingTimer = 0.25f;
            }
            if (move.landingTimer > 0.0f) {
                move.landingTimer -= dt;
            }

            // 1. Accumulate gravity or handle jumping
            if (onGround) {
                if (move.jumpRequested) {
                    move.currentYVel   = move.jumpForce;
                    move.isGrounded    = false;
                    move.jumpRequested = false;
                } else {
                    move.currentYVel = 0.0f;
                }
            } else {
                move.currentYVel -= 32.0f * dt; // <-- Integrates gravity when in air
            }

            // 2. Feed velocity into Jolt CharacterVirtual
            const float     speedMultiplier = (move.jumpDelayTimer > 0.0f) ? 0.25f : 1.0f;
            const JPH::Vec3 velocity        = {move.inputX * move.speed * speedMultiplier, move.currentYVel, move.inputZ * move.speed * speedMultiplier};

            pc.SetCharacterVelocity(phys->physicsHandle, velocity);

            JPH::Vec3 flatVel(velocity.GetX(), 0.0f, velocity.GetZ());
            if (flatVel.LengthSq() > 0.1f) {
                float     targetAngleRad = std::atan2(-velocity.GetZ(), velocity.GetX()) + JPH::DegreesToRadians(90.0f);
                JPH::Quat targetRotation = JPH::Quat::sRotation(JPH::Vec3::sAxisY(), targetAngleRad);

                JPH::Quat currentRotation = move.orientation;

                float     turnSpeed    = 10.0f;
                JPH::Quat nextRotation = currentRotation.SLERP(targetRotation, JPH::Clamp(turnSpeed * dt, 0.0f, 1.0f));

                move.orientation = nextRotation;
            }
        }
    });

    if constexpr (isDev) {
        ZHLN::Tests::VerifyMovementStateConsistency(reg);
    }
}
} // namespace ZHLN
