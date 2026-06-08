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

	// Obtain the contiguous list of entity IDs that own a MovementComponent
	auto entities = reg.GetEntitiesWith<MovementComponent>();
	if (entities.empty()) {
		return;
	}

	// Get the direct contiguous memory span of the component array (extremely cache-friendly)
	auto movements = reg.GetRawArray<MovementComponent>();
	auto& pc = engine.GetPhysicsContext();

	// Use your fiber task system to partition the calculations into chunks of 128
	TaskSystem::ParallelFor(entities.size(), 128, [&](uint32_t start, uint32_t end, uint32_t) {
		for (uint32_t i = start; i < end; ++i) {
			MovementComponent& move = movements[i];
			Entity e = entities[i];

			// Cold path: Look up the secondary component (PhysicsComponent)
			// using the sparse set's constant-time index mapping.
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
			// 1. Calculate Vertical Velocity
			if (onGround) {
				if (move.jumpRequested) {
					move.currentYVel = move.jumpForce;
				} else {
					move.currentYVel = -1.0f; // Snap to slopes
				}
			} else {
				move.currentYVel -= 30.0f * dt; // Gravity
			}

			// 2. Clear Jump (Consumption)
			move.jumpRequested = false;

			// 3. Assemble and apply final vector directly to the distinct character virtual
			JPH::Vec3 velocity = {move.inputX * move.speed, move.currentYVel,
								  move.inputZ * move.speed};

			Physics::SetCharacterVelocity(pc, phys->physicsHandle, velocity);

			// 4. Character Orientation Interpolation
			JPH::Vec3 flatVel(velocity.GetX(), 0.0f, velocity.GetZ());
			if (flatVel.LengthSq() > 0.1f) {
				float targetAngleRad =
					std::atan2(-velocity.GetZ(), velocity.GetX()) + JPH::DegreesToRadians(90.0f);
				JPH::Quat targetRotation =
					JPH::Quat::sRotation(JPH::Vec3::sAxisY(), targetAngleRad);

				JPH::Quat currentRotation(move.orientation[0], move.orientation[1],
										  move.orientation[2], move.orientation[3]);

				float turnSpeed = 10.0f;
				JPH::Quat nextRotation =
					currentRotation.SLERP(targetRotation, JPH::Clamp(turnSpeed * dt, 0.0f, 1.0f));

				move.orientation[0] = nextRotation.GetX();
				move.orientation[1] = nextRotation.GetY();
				move.orientation[2] = nextRotation.GetZ();
				move.orientation[3] = nextRotation.GetW();
			}
		}
	});
}

} // namespace ZHLN
