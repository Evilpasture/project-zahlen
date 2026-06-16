#pragma once

#include <Jolt/Jolt.h>

namespace ZHLN {
class Engine;
class CameraSystem {
  public:
	struct CameraComponent {
		JPH::Mat44 viewProj = JPH::Mat44::sIdentity();
		JPH::Mat44 unjitteredViewProj = JPH::Mat44::sIdentity();
		JPH::Mat44 prevUnjitteredViewProj = JPH::Mat44::sIdentity();
		JPH::Mat44 frozenViewProj = JPH::Mat44::sIdentity();
		uint32_t frameCounter = 0;
	};
	// TODO(Evilpasture): Fix implementation first.
	void Update(Engine& engine, float dt, float alpha);
};
} // namespace ZHLN
