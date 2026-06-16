// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/system/MovementSystem.cpp
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "ecs/ECS.hpp"
#include "physics/Physics.hpp"

#include <cmath> // std::atan2
#include <threading/TaskSystem.hpp>

namespace ZHLN {

void MovementSystem(Engine& engine, float dt) {
	auto& reg = engine.GetRegistry();

	auto entities = reg.GetEntitiesWith<MovementComponent>();
	if (entities.empty()) {
		return;
	}

	auto movements = reg.GetRawArray<MovementComponent>();
	auto& pc = engine.GetPhysicsContext();

	TaskSystem::ParallelFor(entities.size(), 128, [&](uint32_t start, uint32_t end, uint32_t) {
		for (uint32_t i = start; i < end; ++i) {
			MovementComponent& move = movements[i];
			Entity e = entities[i];
			// Record previous orientation before calculating the new one
            move.prevOrientation = move.orientation;

			auto* phys = reg.Get<PhysicsComponent>(e);
			if (!phys) {
				continue;
			}

			if (auto* ragComp = reg.Get<RagdollComponent>(e)) {
				if (ragComp->state == RagdollState::Limp ||
					ragComp->state == RagdollState::KeyframeMotor) {
					continue;
				}
			}

			// Query Jolt ground state (thread-safe, read-only on distinct indices)
			bool onGround = Physics::IsCharacterOnGround(pc, phys->physicsHandle);

			// Track ground states and set/decay timers
			move.wasGrounded = move.isGrounded;
			move.isGrounded = onGround;

			if (move.isGrounded && !move.wasGrounded) {
				move.landingTimer = 0.25f; // Play landing animation for 250ms
			}
			if (move.landingTimer > 0.0f) {
				move.landingTimer -= dt;
			}

			// --- NEW: Handle Jump Anticipation (Crouch Prep) ---
			if (onGround && move.jumpRequested && move.jumpDelayTimer <= 0.0f) {
				// 150ms delay is typical for stylized models like Pomni.
				// Adjust this value to match your model's push-off frame.
				move.jumpDelayTimer = 0.15f;
			}
			move.jumpRequested = false; // Consume intent

			// 1. Calculate Vertical Velocity
			if (onGround) {
				if (move.jumpDelayTimer > 0.0f) {
					move.jumpDelayTimer -= dt;
					if (move.jumpDelayTimer <= 0.0f) {
						// Anticipation finished! Launch physically
						move.currentYVel = move.jumpForce;
						move.isGrounded = false; // Force takeoff state
					} else {
						// Coiling legs: stay anchored to the ground
						move.currentYVel = -1.0f;
					}
				} else {
					move.currentYVel = -1.0f; // Snap to slopes
				}
			} else {
				move.jumpDelayTimer = 0.0f;
				move.currentYVel -= 30.0f * dt; // Gravity
			}

			// 2. Assemble and apply final vector directly to the distinct character virtual
			// Scale down horizontal speed during the coiling phase for natural weight feel
			float speedMultiplier = (move.jumpDelayTimer > 0.0f) ? 0.25f : 1.0f;
			JPH::Vec3 velocity = {move.inputX * move.speed * speedMultiplier, move.currentYVel,
								  move.inputZ * move.speed * speedMultiplier};

			Physics::SetCharacterVelocity(pc, phys->physicsHandle, velocity);

			// 3. Character Orientation Interpolation
			JPH::Vec3 flatVel(velocity.GetX(), 0.0f, velocity.GetZ());
			if (flatVel.LengthSq() > 0.1f) {
				float targetAngleRad =
					std::atan2(-velocity.GetZ(), velocity.GetX()) + JPH::DegreesToRadians(90.0f);
				JPH::Quat targetRotation =
					JPH::Quat::sRotation(JPH::Vec3::sAxisY(), targetAngleRad);

				JPH::Quat currentRotation = move.orientation;


				float turnSpeed = 10.0f;
				JPH::Quat nextRotation =
					currentRotation.SLERP(targetRotation, JPH::Clamp(turnSpeed * dt, 0.0f, 1.0f));

				move.orientation = nextRotation;
			}
		}
	});
}
} // namespace ZHLN
