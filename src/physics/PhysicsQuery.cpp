#include "Physics.hpp"
#include "PhysicsWorld.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSystem.h>

namespace ZHLN::Physics {

namespace { // Internal helpers

class QueryFilter final : public JPH::BodyFilter {
	JPH::BodyID _ignoreID;

  public:
	explicit QueryFilter(JPH::BodyID ignore) : _ignoreID(ignore) {}
	bool ShouldCollide(const JPH::BodyID& inBodyID) const override { return inBodyID != _ignoreID; }
};

static bool TryGetValidHandle(const PhysicsWorld& world, JPH::BodyID bodyID,
							  ZHLN::Entity& outHandle) {
	if (bodyID.IsInvalid()) [[unlikely]] {
		return false;
	}

	const uint64_t rawData = world.bodyInterface->GetUserData(bodyID);
	const ZHLN::Entity handle = ZHLN::Entity::Unpack(rawData);

	if (handle.index >= world.slotCapacity) [[unlikely]] {
		return false;
	}

	const uint8_t state = world.slotStates[handle.index].load(std::memory_order_acquire);

	// Using the simplified C++ predicate
	const SlotPredicate pred = GetSlotPredicate(state);

	if (pred.isActive) {
		outHandle = handle;
		return true;
	}

	return false;
}

} // namespace

RaycastResult Raycast(const PhysicsContext& ctx, JPH::RVec3Arg origin, JPH::Vec3Arg direction,
					  float maxDistance, ZHLN::Entity ignore) {
	const auto& world = ctx.GetWorld();

	if (world.isStepping.load(std::memory_order_relaxed))
		return {};

	float lengthSq = direction.LengthSq();
	if (lengthSq < 1e-6f)
		return {};

	JPH::Vec3 scaledDir = direction.Normalized() * maxDistance;
	JPH::RRayCast ray{origin, scaledDir};
	JPH::RayCastResult hit;

	JPH::BodyID ignoreID = GetBodyID(world, ignore);
	QueryFilter filter(ignoreID);

	const auto* query = &world.system->GetNarrowPhaseQuery();
	bool hasHit = query->CastRay(ray, hit, {}, {}, filter);

	RaycastResult result;
	if (hasHit) {
		if (TryGetValidHandle(world, hit.mBodyID, result.handle)) {
			result.hasHit = true;
			result.fraction = hit.mFraction;
			// GetPointOnRay returns RVec3 if ray is RRayCast
			result.position = ray.GetPointOnRay(hit.mFraction);

			const auto* lockInterface = &world.system->GetBodyLockInterfaceNoLock();
			JPH::BodyLockRead lock(*lockInterface, hit.mBodyID);
			if (lock.Succeeded()) {
				// Surface normal is usually a Vec3 (float)
				result.normal =
					lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, result.position);
			} else {
				result.normal = JPH::Vec3::sAxisY();
			}
		}
	}
	return result;
}

ShapeCastResult Shapecast(const PhysicsContext& ctx, JPH::ShapeRefC shape, JPH::RVec3Arg pos,
						  JPH::QuatArg rot, JPH::Vec3Arg direction, float maxDistance,
						  ZHLN::Entity ignore) {
	const auto& world = ctx.GetWorld();
	if (world.isStepping.load(std::memory_order_relaxed))
		return {};

	float lengthSq = direction.LengthSq();
	if (lengthSq < 1e-6f)
		return {};

	JPH::Vec3 scaledDir = direction.Normalized() * maxDistance;

	// Use RMat44 for the double-precision start transform
	JPH::RShapeCast cast(shape, JPH::Vec3::sReplicate(1.0f),
						 JPH::RMat44::sRotationTranslation(rot, pos), scaledDir);

	// Use standard CastShapeCollector (Jolt manages precision inside)
	JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
	JPH::BodyID ignoreID = GetBodyID(world, ignore);
	QueryFilter filter(ignoreID);

	const auto* query = &world.system->GetNarrowPhaseQuery();
	JPH::ShapeCastSettings settings;
	settings.mBackFaceModeTriangles = JPH::EBackFaceMode::IgnoreBackFaces;
	settings.mBackFaceModeConvex = JPH::EBackFaceMode::IgnoreBackFaces;

	query->CastShape(cast, settings, JPH::RVec3::sZero(), collector, {}, {}, filter);

	ShapeCastResult result;
	if (collector.HadHit()) {
		if (TryGetValidHandle(world, collector.mHit.mBodyID2, result.handle)) {
			result.hasHit = true;
			result.fraction = collector.mHit.mFraction;

			// FIX: Explicitly cast float Vec3 to double RVec3
			result.contactPoint = JPH::RVec3(collector.mHit.mContactPointOn2);

			JPH::Vec3 axis = -collector.mHit.mPenetrationAxis;
			float lenSq = axis.LengthSq();
			result.contactNormal = (lenSq > 1e-6f) ? (axis / sqrt(lenSq)) : JPH::Vec3::sAxisY();
		}
	}
	return result;
}

void OverlapSphere(const PhysicsContext& ctx, JPH::RVec3Arg center, float radius,
				   std::vector<ZHLN::Entity>& outResults) {
	const auto& world = ctx.GetWorld();
	if (world.isStepping.load(std::memory_order_relaxed))
		return;

	JPH::SphereShapeSettings settings(radius);
	JPH::ShapeRefC shape = settings.Create().Get();

	// Use standard CollideShapeCollector
	JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
	const auto* query = &world.system->GetNarrowPhaseQuery();

	query->CollideShape(shape, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sTranslation(center), {},
						JPH::RVec3::sZero(), collector);

	ZHLN::Entity handle;
	for (const auto& hit : collector.mHits) {
		if (TryGetValidHandle(world, hit.mBodyID2, handle)) {
			outResults.push_back(handle);
		}
	}
}

void OverlapAABB(const PhysicsContext& ctx, JPH::RVec3Arg minBox, JPH::RVec3Arg maxBox,
				 std::vector<ZHLN::Entity>& outResults) {
	const auto& world = ctx.GetWorld();
	if (world.isStepping.load(std::memory_order_relaxed))
		return;

	JPH::AABox box((JPH::Vec3)minBox, (JPH::Vec3)maxBox);
	JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
	const auto* query = &world.system->GetBroadPhaseQuery();

	query->CollideAABox(box, collector);

	ZHLN::Entity handle;
	for (const auto& hitID : collector.mHits) {
		if (TryGetValidHandle(world, hitID, handle)) {
			outResults.push_back(handle);
		}
	}
}

} // namespace ZHLN::Physics