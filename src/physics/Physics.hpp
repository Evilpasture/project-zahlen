#pragma once

#include <Zahlen/Buffer.h>
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/Shape.h> // For ShapeRefC
#include <Zahlen/Config.hpp>
#include <Zahlen/Entity.hpp>
#include <physics/PhysicsWorld.hpp>

// clang-format on

#include <cstdint>
#include <memory>

namespace ZHLN {

namespace Physics {
struct PhysicsWorld;
struct DebugDrawData;
} // namespace Physics

static_assert(std::is_trivially_copyable_v<ZHLN::Entity>);
static_assert(std::is_trivial_v<ZHLN::Entity>);

class PhysicsContext {
  public:
	PhysicsContext();
	~PhysicsContext();

	PhysicsContext(const PhysicsContext&) = delete;
	PhysicsContext& operator=(const PhysicsContext&) = delete;

	PhysicsContext(const PhysicsConfig& cfg);

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
ZHLN::Entity CreateRigidBody(PhysicsContext& ctx, JPH::ShapeRefC shape, JPH::RVec3Arg pos,
							 JPH::QuatArg rot, JPH::EMotionType motion, JPH::ObjectLayer layer,
							 uint32_t materialID = 0, uint32_t category = 0xFFFFFFFF,
							 uint32_t mask = 0xFFFFFFFF);

ZHLN::Entity CreateCharacter(PhysicsContext& ctx, JPH::RVec3Arg position,
							 uint32_t category = 0xFFFFFFFF, uint32_t mask = 0xFFFFFFFF);

// --- Actions & Settings ---
void SetCollisionFilter(PhysicsContext& ctx, ZHLN::Entity handle, uint32_t category, uint32_t mask);
DebugDrawData GetDebugDrawData(PhysicsContext& ctx, bool drawShapes = true,
							   bool drawConstraints = true);

// --- Materials ---
void RegisterMaterial(PhysicsContext& ctx, uint32_t id, float friction, float restitution);

// --- Actions ---
void DestroyBody(PhysicsContext& ctx, ZHLN::Entity handle);
void SetLinearVelocity(PhysicsContext& ctx, ZHLN::Entity handle, JPH::Vec3Arg velocity);
void SetCharacterVelocity(PhysicsContext& ctx, ZHLN::Entity handle, JPH::Vec3Arg velocity);

JPH::Vec3 GetCharacterVelocity(const PhysicsContext& ctx, ZHLN::Entity handle);
bool IsCharacterOnGround(const PhysicsContext& ctx, ZHLN::Entity handle);
auto GetPositionBuffer(const PhysicsContext& ctx) -> BufferView;
JPH::Quat GetRotation(const PhysicsContext& ctx, JPH::BodyID bodyID);
void AddImpulse(PhysicsContext& ctx, ZHLN::Entity handle, JPH::Vec3Arg impulse);

// --- Data Structures ---
struct RaycastResult {
	ZHLN::Entity handle;
	JPH::Vec3 normal;
	JPH::RVec3 position;
	float fraction;
	bool hasHit;
};

struct ShapeCastResult {
	ZHLN::Entity handle;
	JPH::RVec3 contactPoint;
	JPH::Vec3 contactNormal;
	float fraction;
	bool hasHit;
};

static_assert(std::is_trivial_v<RaycastResult> && std::is_trivial_v<ShapeCastResult>);

// --- Queries ---
[[nodiscard]] RaycastResult Raycast(const PhysicsContext& ctx, JPH::RVec3Arg origin,
									JPH::Vec3Arg direction, float maxDistance = 1000.0f,
									ZHLN::Entity ignore = {});

[[nodiscard]] ShapeCastResult Shapecast(const PhysicsContext& ctx, JPH::ShapeRefC shape,
										JPH::RVec3Arg pos, JPH::QuatArg rot, JPH::Vec3Arg direction,
										float maxDistance = 1000.0f, ZHLN::Entity ignore = {});

void OverlapSphere(const PhysicsContext& ctx, JPH::RVec3Arg center, float radius,
				   JPH::Array<ZHLN::Entity>& outResults);

void OverlapAABB(const PhysicsContext& ctx, JPH::RVec3Arg minBox, JPH::RVec3Arg maxBox,
				 JPH::Array<ZHLN::Entity>& outResults);

// --- Internal Mapping Helpers (Now visible to Query module) ---
JPH::BodyID GetBodyID(const PhysicsWorld& world, ZHLN::Entity handle);
ZHLN::Entity GetEntityHandle(const PhysicsContext& ctx, JPH::BodyID bodyID);

} // namespace Physics
} // namespace ZHLN