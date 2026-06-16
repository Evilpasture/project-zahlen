// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsStateSystem.hpp"

#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <ecs/ECS.hpp>
#include <physics/Physics.hpp>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN {

void PhysicsStateSystem::WriteBack(Engine& engine) noexcept {
	auto& reg = engine.GetRegistry();
	const auto& world = engine.GetPhysicsContext().GetWorld();

	auto entities = reg.GetEntitiesWith<PhysicsComponent>();
	auto physComps = reg.GetRawArray<PhysicsComponent>();

	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];
		auto& phys = physComps[i];
		auto* state = reg.Get<PhysicsStateComponent>(e);

		if (state != nullptr) {
			uint32_t dense = world.slotToDense[phys.physicsHandle.index];
			size_t base = static_cast<size_t>(dense) * 4;

			JPH::BodyID bodyID = world.bodyIDs[dense];
			bool isCharacter = (world.slotStates[phys.physicsHandle.index].load(
									std::memory_order_relaxed) == Physics::SLOT_CHARACTER);
			bool isActive =
				isCharacter || (!bodyID.IsInvalid() && world.bodyInterface->IsActive(bodyID));

			// Shift prev state
			state->prevPosition = state->currPosition;
			state->prevRotation = state->currRotation;

			// Fetch current physical state
			state->currPosition = JPH::Vec3(static_cast<float>(world.positions[base]),
											static_cast<float>(world.positions[base + 1]),
											static_cast<float>(world.positions[base + 2]));

			if (isCharacter) {
				auto* move = reg.Get<MovementComponent>(e);
				if (move != nullptr) {
					state->currRotation = JPH::Quat(move->orientation[0], move->orientation[1],
													move->orientation[2], move->orientation[3]);
				} else {
					state->currRotation = JPH::Quat::sIdentity();
				}
			} else {
				state->currRotation =
					JPH::Quat(world.rotations[base], world.rotations[base + 1],
							  world.rotations[base + 2], world.rotations[base + 3]);
			}

			if (!isActive) {
				// If inactive, snap prev to curr to completely halt interpolation
				state->prevPosition = state->currPosition;
				state->prevRotation = state->currRotation;
			}
		}
	}
}

void VisualInterpolationSystem::Update(Engine& engine, float alpha) noexcept {
	auto& reg = engine.GetRegistry();
	auto entities = reg.GetEntitiesWith<PhysicsStateComponent>();
	auto states = reg.GetRawArray<PhysicsStateComponent>();

	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];
		auto& state = states[i];
		auto* trans = reg.Get<TransformComponent>(e);

		if (trans != nullptr) {
			trans->position =
				state.prevPosition + alpha * (state.currPosition - state.prevPosition);
			trans->rotation = state.prevRotation.SLERP(state.currRotation, alpha);
		}
	}
}

} // namespace ZHLN
