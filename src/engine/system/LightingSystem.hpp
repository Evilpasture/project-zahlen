// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Zahlen/Engine.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <utility>
namespace ZHLN {

class LightingSystem {
  public:
	void Update(Engine& engine, float dt);
	/**
	 * @brief Resolves the absolute direction pointing TO the sun, along with its intensity.
	 * Evaluates LightType::Sun and falls back to Components::SunTagComponent.
	 */
	static std::pair<JPH::Vec3, float>
	GetSunDirectionAndIntensity(const ECS::Registry& reg) noexcept;
};
} // namespace ZHLN
