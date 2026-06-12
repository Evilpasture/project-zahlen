// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include "Zahlen/Camera.hpp"

#include <Jolt/Physics/Ragdoll/Ragdoll.h>
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
#include <Zahlen/Types.hpp>

// clang-format on

#include <cstdint>
#include <memory>

namespace ZHLN {

namespace Physics {
struct PhysicsWorld;
struct DebugDrawData;
} // namespace Physics

static_assert(std::is_trivially_copyable_v<ZHLN::Entity>);
static_assert((std::is_trivially_default_constructible_v<ZHLN::Entity> && std::is_trivially_copyable_v<ZHLN::Entity>));

class ZHLN_API PhysicsContext {
  public:
	PhysicsContext();
	~PhysicsContext();

	PhysicsContext(const PhysicsContext&) = delete;
	PhysicsContext& operator=(const PhysicsContext&) = delete;

	PhysicsContext(const PhysicsConfig& cfg);

	void Step(float deltaTime);
	[[nodiscard]] uint32_t GetActiveBodyCount() const;
	[[nodiscard]] size_t GetMemoryUsage() const;

	struct Impl;
	[[nodiscard]] Impl* GetImpl() const { return _impl.get(); }
	[[nodiscard]] const Physics::PhysicsWorld& GetWorld() const;

	void OptimizeBroadphase();

  private:
	std::unique_ptr<Impl> _impl;
};

namespace Physics {

enum class ShapeType : uint8_t { Box = 0, Sphere = 1, Capsule = 2, Cylinder = 3, Plane = 4 };

// --- Shape Caching ---
JPH::ShapeRefC GetOrCreateShape(PhysicsContext& ctx, ShapeType type, float p1, float p2 = 0.0f,
								float p3 = 0.0f, float p4 = 0.0f);

// --- Creation (Engine allocates and returns Handle) ---
ZHLN::Entity CreateRigidBody(PhysicsContext& ctx, const JPH::ShapeRefC& shape, JPH::RVec3Arg pos,
							 JPH::QuatArg rot, JPH::EMotionType motion, JPH::ObjectLayer layer,
							 uint32_t materialID = 0, uint32_t category = 0xFFFFFFFF,
							 uint32_t mask = 0xFFFFFFFF);

JPH::ShapeRefC CreateMeshShape(const Vertex* vertices, uint32_t vertexCount,
							   const uint32_t* indices, uint32_t indexCount);

ZHLN::Entity CreateMeshBody(PhysicsContext& ctx, const Vertex* vertices, uint32_t vertexCount,
							const uint32_t* indices, uint32_t indexCount, JPH::RVec3Arg pos,
							JPH::QuatArg rot, uint32_t category = 0xFFFFFFFF,
							uint32_t mask = 0xFFFFFFFF);

ZHLN::Entity CreateCharacter(PhysicsContext& ctx, JPH::RVec3Arg position,
							 uint32_t category = 0xFFFFFFFF, uint32_t mask = 0xFFFFFFFF);

struct RagdollPartParams {
	uint32_t jointIndex;
	int parentJointIndex = -1;
	JPH::ShapeRefC shape = nullptr;
	float mass = 10.0f;

	JPH::RVec3 position = JPH::RVec3::sZero();
	JPH::Quat rotation = JPH::Quat::sIdentity();

	JPH::Vec3 twistAxis = JPH::Vec3::sAxisX();
	JPH::Vec3 planeNormal = JPH::Vec3::sAxisY();

	float coneAngle = 0.0f;
	float twistMin = -0.1f;
	float twistMax = 0.1f;

	bool enableMotors = true;
	float maxMotorForce = 100.0f;
};

JPH::Ref<JPH::Ragdoll> CreateSkeletalRagdoll(PhysicsContext& ctx, const JPH::Skeleton* skeleton,
											 const std::vector<RagdollPartParams>& parts);

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
void SetCharacterPosition(PhysicsContext& ctx, ZHLN::Entity handle, JPH::RVec3Arg position);

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

struct CullResult {
	ZHLN::Entity* results;
	uint32_t count;
};
/**
 * @brief Uses Jolt's Broadphase to find all entities within a frustum.
 */
void FrustumCull(const PhysicsContext& ctx, const JPH::Mat44& viewProj, const Frustum& frustum,
				 JPH::Array<ZHLN::Entity>& outEntities);

static_assert((std::is_trivially_default_constructible_v<RaycastResult> && std::is_trivially_copyable_v<RaycastResult>) && (std::is_trivially_default_constructible_v<ShapeCastResult> && std::is_trivially_copyable_v<ShapeCastResult>) &&
			  (std::is_trivially_default_constructible_v<CullResult> && std::is_trivially_copyable_v<CullResult>));

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

void QueryAABB(const PhysicsContext& ctx, JPH::Vec3Arg min, JPH::Vec3Arg max,
			   JPH::Array<ZHLN::Entity>& outEntities);

// --- Internal Mapping Helpers (Now visible to Query module) ---
JPH::BodyID GetBodyID(const PhysicsWorld& world, ZHLN::Entity handle);
ZHLN::Entity GetEntityHandle(const PhysicsContext& ctx, JPH::BodyID bodyID);

JPH::ShapeRefC CreateHeightFieldShape(const std::vector<float>& heights, int sampleCount,
									  float worldSize);

// Internal helpers to bridge PIMPL barriers for engine factories
[[nodiscard]] JPH::PhysicsSystem& GetInternalSystem(PhysicsContext& ctx) noexcept;
[[nodiscard]] PhysicsWorld& GetInternalWorld(PhysicsContext& ctx) noexcept;

} // namespace Physics
} // namespace ZHLN
