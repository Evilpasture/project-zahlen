// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsStateSystem.hpp"

#include "engine/system/TransformSystem.hpp"

#include <Zahlen/Components.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <algorithm>
#include <ecs/ECS.hpp>
#include <physics/Physics.hpp>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN::Tests {
static void VerifyRealVisualInterpolation(Engine& engine, float alpha) noexcept {
	static bool testsRun = false;
	if (testsRun) {
		return;
	}

	auto& reg = engine.GetRegistry();
	auto entities = reg.GetEntitiesWith<PhysicsStateComponent>();
	if (entities.empty()) {
		return; // Wait until we have at least one active physics state
	}
	testsRun = true;

	auto states = reg.GetRawArray<PhysicsStateComponent>();
	float clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);

	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];
		const auto& state = states[i];
		const auto* trans = reg.Get<TransformComponent>(e);
		if (trans != nullptr) {
			JPH::Vec3 expected =
				state.prevPosition + clampedAlpha * (state.currPosition - state.prevPosition);
			JPH::Vec3 actual(trans->position[0], trans->position[1], trans->position[2]);
			if ((actual - expected).LengthSq() > 1e-3f) {
				ZHLN::Log("[Test Fail] Real Visual Interpolation Invariant Failure on Entity {}: "
						  "Expected position ({}, {}, {}), got ({}, {}, {}) at alpha = {}",
						  e.index, expected.GetX(), expected.GetY(), expected.GetZ(), actual.GetX(),
						  actual.GetY(), actual.GetZ(), clampedAlpha);
			}
		}
	}
}
} // namespace ZHLN::Tests

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

			bool isCharacter = (world.slotStates[phys.physicsHandle.index].load(
									std::memory_order::relaxed) == Physics::SLOT_CHARACTER);

			// Read directly from Jolt's robust double-buffered history
			state->lastPhysicsSyncFrame = engine.GetCurrentFrame();
			state->prevPosition = JPH::Vec3(static_cast<float>(world.prevPositions[base]),
											static_cast<float>(world.prevPositions[base + 1]),
											static_cast<float>(world.prevPositions[base + 2]));
			state->currPosition = JPH::Vec3(static_cast<float>(world.positions[base]),
											static_cast<float>(world.positions[base + 1]),
											static_cast<float>(world.positions[base + 2]));

			if (isCharacter) {
				auto* move = reg.Get<MovementComponent>(e);
				if (move != nullptr) {
					state->prevRotation = move->prevOrientation;
								state->currRotation = move->orientation;
				} else {
					state->prevRotation = JPH::Quat::sIdentity();
					state->currRotation = JPH::Quat::sIdentity();
				}
			} else {
				state->prevRotation =
					JPH::Quat(world.prevRotations[base], world.prevRotations[base + 1],
							  world.prevRotations[base + 2], world.prevRotations[base + 3]);
				state->currRotation =
					JPH::Quat(world.rotations[base], world.rotations[base + 1],
							  world.rotations[base + 2], world.rotations[base + 3]);
			}
		}
	}
}

void VisualInterpolationSystem::Update(Engine& engine, float alpha) noexcept {
	auto& reg = engine.GetRegistry();
	auto entities = reg.GetEntitiesWith<PhysicsStateComponent>();
	auto states = reg.GetRawArray<PhysicsStateComponent>();

	// Strict clamp prevents floating-point accumulator noise from overshooting the bounds
	float clampedAlpha = std::clamp(alpha, 0.0f, 1.0f);

	for (size_t i = 0; i < entities.size(); ++i) {
		Entity e = entities[i];
		auto& state = states[i];
		auto* trans = reg.Get<TransformComponent>(e);

		if (trans != nullptr) {
			trans->position =
				state.prevPosition + clampedAlpha * (state.currPosition - state.prevPosition);
			trans->rotation = state.prevRotation.SLERP(state.currRotation, clampedAlpha);
		}
	}

	if constexpr (isDev) {
		ZHLN::Tests::VerifyRealVisualInterpolation(engine, alpha);
	}
}

} // namespace ZHLN
