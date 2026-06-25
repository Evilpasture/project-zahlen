#pragma once

#include "Zahlen/Engine.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Math/Vec3.h>
namespace ZHLN {

class LightingSystem {
  public:
	struct LightComponent {
		uint32_t type; // 0=Dir, 1=Point, 2=Spot, 3=Area (LTC Quad)
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
};
} // namespace ZHLN
