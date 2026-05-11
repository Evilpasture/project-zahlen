#pragma once

#include <Zahlen/Buffer.h>
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/Shape.h> // For ShapeRefC
// clang-format on
#include <cstdint>
#include <memory>

namespace ZHLN {

namespace Physics {
struct PhysicsWorld;
}

// --- ECS Handle for UserData ---
struct EntityHandle {
	uint32_t index;
	uint32_t generation;

	[[nodiscard]] constexpr uint64_t Pack() const noexcept {
		return (static_cast<uint64_t>(generation) << 32) | index;
	}

	[[nodiscard]] static constexpr EntityHandle Unpack(uint64_t userData) noexcept {
		return {.index = static_cast<uint32_t>(userData & 0xFFFFFFFF),
				.generation = static_cast<uint32_t>(userData >> 32)};
	}
};

static_assert(std::is_trivially_copyable_v<EntityHandle>);
static_assert(std::is_trivial_v<EntityHandle>);

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

enum class ShapeType : uint32_t { Box = 0, Sphere = 1, Capsule = 2, Cylinder = 3, Plane = 4 };

// --- Shape Caching ---
JPH::ShapeRefC GetOrCreateShape(PhysicsContext& ctx, ShapeType type, float p1, float p2 = 0.0f,
								float p3 = 0.0f, float p4 = 0.0f);

// --- Creation (Engine allocates and returns Handle) ---
EntityHandle CreateRigidBody(PhysicsContext& ctx, JPH::ShapeRefC shape, JPH::RVec3Arg pos,
							 JPH::QuatArg rot, JPH::EMotionType motion, JPH::ObjectLayer layer);

EntityHandle CreateCharacter(PhysicsContext& ctx, JPH::RVec3Arg position);

// --- Actions ---
void DestroyBody(PhysicsContext& ctx, EntityHandle handle);
void SetLinearVelocity(PhysicsContext& ctx, EntityHandle handle, JPH::Vec3Arg velocity);
void SetCharacterVelocity(PhysicsContext& ctx, EntityHandle handle, JPH::Vec3Arg velocity);

// --- Queries ---
JPH::Vec3 GetCharacterVelocity(const PhysicsContext& ctx, EntityHandle handle);
bool IsCharacterOnGround(const PhysicsContext& ctx, EntityHandle handle);
auto GetPositionBuffer(const PhysicsContext& ctx) -> BufferView;
JPH::Quat GetRotation(const PhysicsContext& ctx, JPH::BodyID bodyID);

} // namespace Physics
} // namespace ZHLN