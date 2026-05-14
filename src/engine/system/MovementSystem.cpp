// MovementSystem.cpp
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
namespace ZHLN {
void MovementSystem(Engine& engine, float dt) {
	auto& reg = engine.GetRegistry();
	// We only care about entities that have BOTH movement and physics
	auto entities = reg.GetEntitiesWith<MovementComponent>();

	for (auto e : entities) {
		auto* move = reg.Get<MovementComponent>(e);
		auto* phys = reg.Get<PhysicsComponent>(e);

		if (!move || !phys)
			continue;

		auto& pc = engine.GetPhysicsContext();
		bool onGround = Physics::IsCharacterOnGround(pc, phys->physicsHandle);

		// 1. Calculate Vertical Velocity
		if (onGround) {
			if (move->jumpRequested) {
				move->currentYVel = move->jumpForce;
			} else {
				move->currentYVel = -1.0f; // Snap to slopes
			}
		} else {
			move->currentYVel -= 30.0f * dt; // Gravity
		}

		// 2. Clear Jump (Consumption)
		move->jumpRequested = false;

		// 3. Apply final vector to the physics engine
		JPH::Vec3 velocity = {move->inputX * move->speed, move->currentYVel,
							  move->inputZ * move->speed};

		Physics::SetCharacterVelocity(pc, phys->physicsHandle, velocity);
	}
}
} // namespace ZHLN