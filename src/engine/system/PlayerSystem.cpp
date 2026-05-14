// PlayerSystem.cpp
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
namespace ZHLN {
void UpdatePlayerControllers(Engine& engine, float dt) {
	auto& reg = engine.GetRegistry();
	auto entities = reg.GetEntitiesWith<PlayerControllerComponent>();

	for (auto e : entities) {
		auto* ctrl = reg.Get<PlayerControllerComponent>(e);
		auto* phys = reg.Get<PhysicsComponent>(e);
		if (!ctrl || !phys)
			continue;

		bool onGround =
			Physics::IsCharacterOnGround(engine.GetPhysicsContext(), phys->physicsHandle);

		// 1. Handle Vertical Logic (Gravity/Jumping)
		if (onGround) {
			if (ctrl->jumpRequested) {
				ctrl->currentYVel = ctrl->jumpForce;
			} else {
				ctrl->currentYVel = -1.0f; // Ground snap
			}
		} else {
			ctrl->currentYVel -= 30.0f * dt; // Gravity
		}

		// 2. Clear jump trigger
		ctrl->jumpRequested = false;

		// 3. Apply to Physics Character
		Physics::SetCharacterVelocity(
			engine.GetPhysicsContext(), phys->physicsHandle,
			{ctrl->moveX * ctrl->speed, ctrl->currentYVel, ctrl->moveZ * ctrl->speed});
	}
}
} // namespace ZHLN