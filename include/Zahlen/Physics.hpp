#pragma once

#include <Zahlen/Buffer.h>
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
// clang-format on
#include <cstdint>
#include <memory>

namespace ZHLN {
namespace Physics {
struct PhysicsWorld;
}
// --- ECS Handle for UserData ---
struct EntityHandle {
	uint32_t index = 0;
	uint32_t generation = 0;

	[[nodiscard]] constexpr uint64_t Pack() const noexcept {
		return (static_cast<uint64_t>(generation) << 32) | index;
	}

	[[nodiscard]] static constexpr EntityHandle Unpack(uint64_t userData) noexcept {
		return {.index = static_cast<uint32_t>(userData & 0xFFFFFFFF),
				.generation = static_cast<uint32_t>(userData >> 32)};
	}
};

static_assert(std::is_trivially_copyable_v<EntityHandle>);

class PhysicsContext {
  public:
	PhysicsContext();
	~PhysicsContext();

	PhysicsContext(const PhysicsContext&) = delete;
	PhysicsContext& operator=(const PhysicsContext&) = delete;

	void Step(float deltaTime);

	struct Impl;
	Impl* GetImpl() const { return _impl.get(); }

	const Physics::PhysicsWorld& GetWorld() const;

  private:
	std::unique_ptr<Impl> _impl;
};

namespace Physics {

// position is RVec3Arg (Double if JPH_DOUBLE_PRECISION is on)
// halfExtents is Vec3Arg (Always float)
JPH::BodyID CreateStaticFloor(PhysicsContext& ctx, float extent, EntityHandle handle = {});
JPH::BodyID CreateDynamicBox(PhysicsContext& ctx, JPH::RVec3Arg position, JPH::Vec3Arg halfExtents,
							 EntityHandle handle = {});
void DestroyBody(PhysicsContext& ctx, JPH::BodyID bodyID, EntityHandle handle);
JPH::RVec3 GetPosition(const PhysicsContext& ctx, JPH::BodyID bodyID);
auto GetPositionBuffer(const PhysicsContext& ctx) -> BufferView;
JPH::Quat GetRotation(const PhysicsContext& ctx, JPH::BodyID bodyID);

// NEW: Read the handle back from a collision event or query
EntityHandle GetEntityHandle(const PhysicsContext& ctx, JPH::BodyID bodyID);

// Returns the same handle for convenience
EntityHandle CreateCharacter(PhysicsContext& ctx, JPH::RVec3Arg position, EntityHandle handle);

// Commands now take the Handle
void SetCharacterVelocity(PhysicsContext& ctx, EntityHandle handle, JPH::Vec3Arg linearVelocity);
bool IsCharacterOnGround(const PhysicsContext& ctx, EntityHandle handle);
} // namespace Physics

} // namespace ZHLN