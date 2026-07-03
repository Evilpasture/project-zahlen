// src/engine/system/PhysicsSystem.cpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsSystem.hpp"

#include "engine/system/PhysicsStateSystem.hpp"

#include <Zahlen/Engine.hpp>
#include <Zahlen/Profiler.hpp>
#include <algorithm>
#include <physics/Physics.hpp>

namespace ZHLN {

// Freestanding system hook declared in MovementSystem.cpp
void MovementSystem(Engine& engine, float dt);

void PhysicsSystem::Update(Engine& engine, float dt) noexcept {
	float cappedDt = std::min(dt, 0.1f);
	_accumulator += cappedDt;

	// Avoid spiral-of-death situations by capping the accumulator to a maximum of 4 steps
	_accumulator = std::min(_accumulator, _targetDt * 4.0f);

	{
		ZHLN_PROFILE_SCOPE("ECS System: Physics & Movement");
		while (_accumulator >= _targetDt) {
			// 1. Process movement inputs and steer physical character bodies
			MovementSystem(engine, _targetDt);

			// 2. Step the Jolt physics simulation
			engine.GetPhysicsContext().Step(_targetDt);

			// 3. Write newly calculated positions back to state history
			ZHLN::PhysicsStateSystem::WriteBack(engine);

			_accumulator -= _targetDt;
		}
	}

	// 4. Calculate the remaining remainder ratio for visual interpolation
	engine.GetCurrentAlpha() = _accumulator / _targetDt;
}

} // namespace ZHLN
