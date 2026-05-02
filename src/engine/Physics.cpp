// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include "engine/Physics.hpp"
#include "engine/Log.hpp"
// clang-format on

namespace ZHLN {

// --- Jolt Boilerplate: Layers & Filters (Using Enums) ---
namespace Layers {
enum ID : JPH::ObjectLayer { NON_MOVING = 0, MOVING = 1, NUM_LAYERS = 2 };
}

namespace BroadPhaseLayers {
enum ID : uint8_t { NON_MOVING = 0, MOVING = 1, NUM_LAYERS = 2 };
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
  public:
	BPLayerInterfaceImpl() {
		mObjectToBroadPhase[Layers::NON_MOVING] =
			JPH::BroadPhaseLayer(BroadPhaseLayers::NON_MOVING);
		mObjectToBroadPhase[Layers::MOVING] = JPH::BroadPhaseLayer(BroadPhaseLayers::MOVING);
	}
	uint32_t GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }
	JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
		JPH_ASSERT(inLayer < Layers::NUM_LAYERS);
		return mObjectToBroadPhase[inLayer];
	}
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
	const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override {
		switch (static_cast<BroadPhaseLayers::ID>(static_cast<uint8_t>(inLayer))) {
			case BroadPhaseLayers::NON_MOVING:
				return "NON_MOVING";
			case BroadPhaseLayers::MOVING:
				return "MOVING";
			default:
				JPH_ASSERT(false);
				return "INVALID";
		}
	}
#endif
  private:
	JPH::BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS];
};

class ObjectVsBroadPhaseLayerFilterImpl : public JPH::ObjectVsBroadPhaseLayerFilter {
  public:
	bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override {
		switch (inLayer1) {
			case Layers::NON_MOVING:
				return inLayer2 == JPH::BroadPhaseLayer(BroadPhaseLayers::MOVING);
			case Layers::MOVING:
				return true;
			default:
				JPH_ASSERT(false);
				return false;
		}
	}
};

class ObjectLayerPairFilterImpl : public JPH::ObjectLayerPairFilter {
  public:
	bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override {
		switch (inObject1) {
			case Layers::NON_MOVING:
				return inObject2 == Layers::MOVING;
			case Layers::MOVING:
				return true;
			default:
				JPH_ASSERT(false);
				return false;
		}
	}
};

// --- Context Implementation (Pimpl) ---
struct PhysicsContext::Impl {
	JPH::PhysicsSystem physicsSystem;
	JPH::TempAllocatorImpl tempAllocator{10 * 1024 * 1024};
	JPH::JobSystemThreadPool jobSystem{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 1};

	BPLayerInterfaceImpl bpLayerInterface;
	ObjectVsBroadPhaseLayerFilterImpl objVsBpFilter;
	ObjectLayerPairFilterImpl objVsObjFilter;
};

PhysicsContext::PhysicsContext() : _impl(std::make_unique<Impl>()) {
	_impl->physicsSystem.Init(1024, 0, 1024, 1024, _impl->bpLayerInterface, _impl->objVsBpFilter,
							  _impl->objVsObjFilter);
	ZHLN::Log("PhysicsContext initialized.\n");
}

PhysicsContext::~PhysicsContext() = default;

void PhysicsContext::Step(float deltaTime) {
	const int collisionSteps = 1;
	_impl->physicsSystem.Update(deltaTime, collisionSteps, &_impl->tempAllocator,
								&_impl->jobSystem);
}

// --- Procedural API ---
namespace Physics {

JPH::BodyID CreateStaticFloor(PhysicsContext& ctx, float extent, EntityHandle handle) {
	JPH::BodyInterface& bodyInterface = ctx.GetImpl()->physicsSystem.GetBodyInterface();

	// Shape extents are always float (Vec3)
	JPH::BoxShapeSettings shapeSettings(JPH::Vec3(extent, 1.0f, extent));
	JPH::ShapeRefC shape = shapeSettings.Create().Get();

	// FIX: Use RVec3 for position to match constructor signature
	JPH::BodyCreationSettings settings(shape.GetPtr(), JPH::RVec3(0.0, -1.0, 0.0), // Use RVec3
									   JPH::Quat::sIdentity(), JPH::EMotionType::Static,
									   Layers::NON_MOVING);

	settings.mUserData = handle.Pack();

	return bodyInterface.CreateAndAddBody(settings, JPH::EActivation::DontActivate);
}

JPH::BodyID CreateDynamicBox(PhysicsContext& ctx, JPH::RVec3Arg position, JPH::Vec3Arg halfExtents,
							 EntityHandle handle) {
	JPH::BodyInterface& bodyInterface = ctx.GetImpl()->physicsSystem.GetBodyInterface();

	// Shape extents are always float (Vec3)
	JPH::BoxShapeSettings shapeSettings(halfExtents);
	JPH::ShapeRefC shape = shapeSettings.Create().Get();

	// FIX: Settings constructor expects RVec3 for position
	JPH::BodyCreationSettings settings(shape.GetPtr(),
									   position, // Now correctly passed as RVec3Arg
									   JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic,
									   Layers::MOVING);

	settings.mUserData = handle.Pack();

	return bodyInterface.CreateAndAddBody(settings, JPH::EActivation::Activate);
}

JPH::RVec3 GetPosition(const PhysicsContext& ctx, JPH::BodyID bodyID) {
	return ctx.GetImpl()->physicsSystem.GetBodyInterface().GetPosition(bodyID);
}

JPH::Quat GetRotation(const PhysicsContext& ctx, JPH::BodyID bodyID) {
	return ctx.GetImpl()->physicsSystem.GetBodyInterface().GetRotation(bodyID);
}

EntityHandle GetEntityHandle(const PhysicsContext& ctx, JPH::BodyID bodyID) {
	uint64_t rawData = ctx.GetImpl()->physicsSystem.GetBodyInterface().GetUserData(bodyID);
	return EntityHandle::Unpack(rawData);
}

} // namespace Physics
} // namespace ZHLN