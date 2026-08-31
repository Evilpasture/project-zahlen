// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/system/MovementSystem.cpp
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Log.hpp"
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
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

            reg.Patch<Components::RagdollComponent>(e, [&](auto& ragComp) {
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
                move.currentYVel -= 32.0f * dt; // Integrate gravity when in air
                move.jumpRequested = false;     // Clear any unconsumed jump requests while airborne!
            }

            // 2. Calculate target velocity and smoothly interpolate toward it
            const float     jumpRecoveryMultiplier = (move.jumpDelayTimer > 0.0f) ? 0.25f : 1.0f;
            const float     sprintMultiplier       = move.isSprinting ? std::max(move.sprintMultiplier, 1.0f) : 1.0f;
            const float     movementSpeed          = move.speed * jumpRecoveryMultiplier * sprintMultiplier;
            const JPH::Vec3 targetVelocity         = {move.inputX * movementSpeed, 0.0f, move.inputZ * movementSpeed};

            // Apply acceleration/deceleration to smoothly transition velocity
            const float targetSpeedSq = targetVelocity.GetX() * targetVelocity.GetX() + targetVelocity.GetZ() * targetVelocity.GetZ();
            const float currentSpeedSq = move.currentVelX * move.currentVelX + move.currentVelZ * move.currentVelZ;
            const bool  hasInput       = targetSpeedSq > 0.01f;
            const float rate           = hasInput ? move.acceleration : move.deceleration;

            if (hasInput) {
                // Accelerate toward target velocity
                const float invTargetSpeed = (targetSpeedSq > 0.01f) ? 1.0f / std::sqrt(targetSpeedSq) : 0.0f;
                const float targetDirX     = targetVelocity.GetX() * invTargetSpeed;
                const float targetDirZ     = targetVelocity.GetZ() * invTargetSpeed;
                const float targetSpeed    = std::sqrt(targetSpeedSq);

                // Project current velocity onto target direction
                const float currentSpeedAlongTarget = move.currentVelX * targetDirX + move.currentVelZ * targetDirZ;
                const float newSpeed                = std::min(currentSpeedAlongTarget + rate * dt, targetSpeed);

                // Blend between current direction and target direction based on speed
                const float blendFactor = (targetSpeed > 0.01f) ? std::min(newSpeed / targetSpeed, 1.0f) : 0.0f;
                const float currentSpeed2D = std::sqrt(currentSpeedSq);
                const float invCurrentSpeed = (currentSpeed2D > 0.01f) ? 1.0f / currentSpeed2D : 0.0f;
                const float currentDirX    = move.currentVelX * invCurrentSpeed;
                const float currentDirZ    = move.currentVelZ * invCurrentSpeed;

                const float blendedDirX = currentDirX * (1.0f - blendFactor) + targetDirX * blendFactor;
                const float blendedDirZ = currentDirZ * (1.0f - blendFactor) + targetDirZ * blendFactor;
                const float blendedDirLen = std::sqrt(blendedDirX * blendedDirX + blendedDirZ * blendedDirZ);
                const float invBlendedDirLen = (blendedDirLen > 0.01f) ? 1.0f / blendedDirLen : 0.0f;

                move.currentVelX = blendedDirX * invBlendedDirLen * newSpeed;
                move.currentVelZ = blendedDirZ * invBlendedDirLen * newSpeed;
            } else {
                // Decelerate toward zero
                const float currentSpeed2D = std::sqrt(currentSpeedSq);
                if (currentSpeed2D > 0.01f) {
                    const float newSpeed = std::max(currentSpeed2D - rate * dt, 0.0f);
                    const float scale    = newSpeed / currentSpeed2D;
                    move.currentVelX *= scale;
                    move.currentVelZ *= scale;
                } else {
                    move.currentVelX = 0.0f;
                    move.currentVelZ = 0.0f;
                }
            }

            const JPH::Vec3 velocity = {move.currentVelX, move.currentYVel, move.currentVelZ};
            pc.SetCharacterVelocity(phys->physicsHandle, velocity);

            JPH::Vec3 flatVel(move.currentVelX, 0.0f, move.currentVelZ);
            if (flatVel.LengthSq() > 0.1f) {
                float     targetAngleRad = std::atan2(-move.currentVelZ, move.currentVelX) + JPH::DegreesToRadians(90.0f);
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
