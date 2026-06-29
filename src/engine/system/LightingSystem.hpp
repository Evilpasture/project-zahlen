// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Zahlen/Engine.hpp"
#include "Zahlen/Types.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
#include <utility>
namespace ZHLN {

class LightingSystem {
  public:
	struct LightComponent {
		LightType type; // 0=Dir, 1=Point, 2=Spot, 3=Area (LTC Quad)
		JPH::Vec3 color;
		float intensity;
		float radius;
		JPH::Vec3 direction;
		float range;
		JPH::Mat44 points;
		uint32_t twoSided;
		int32_t shadowLayer = -1;
	};
	static_assert(sizeof(LightComponent) == 160);
	void Update(Engine& engine, float dt);
	/**
	 * @brief Resolves the absolute direction pointing TO the sun, along with its intensity.
	 * Evaluates LightType::Sun and falls back to SunTagComponent.
	 */
	static std::pair<JPH::Vec3, float>
	GetSunDirectionAndIntensity(const ECS::Registry& reg) noexcept;
};
} // namespace ZHLN
