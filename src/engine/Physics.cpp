// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>

#include <Zahlen/Physics.hpp>
#include <Zahlen/PhysicsWorld.hpp> // <-- New Include
#include <Zahlen/Log.hpp>
// clang-format on

#include <cstring>

namespace ZHLN {

// --- Jolt Boilerplate: Layers & Filters ---
namespace Layers {
enum ID : JPH::ObjectLayer { NON_MOVING = 0, MOVING = 1, NUM_LAYERS = 2 };
}
namespace BroadPhaseLayers {
enum ID : uint8_t { NON_MOVING = 0, MOVING = 1, NUM_LAYERS = 2 };
}

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
	JPH::BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS];

  public:
	BPLayerInterfaceImpl() {
		mObjectToBroadPhase[Layers::NON_MOVING] =
			JPH::BroadPhaseLayer(BroadPhaseLayers::NON_MOVING);
		mObjectToBroadPhase[Layers::MOVING] = JPH::BroadPhaseLayer(BroadPhaseLayers::MOVING);
	}
	uint32_t GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }

	JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override {
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

	// The flat, POD SoA World that we expose to Lua / Render / Logic
	Physics::PhysicsWorld world{};
};

PhysicsContext::PhysicsContext() : _impl(std::make_unique<Impl>()) {
	// 1. Initialize Jolt Systems
	const uint32_t maxBodies = 1024;
	_impl->physicsSystem.Init(maxBodies, 0, maxBodies, maxBodies, _impl->bpLayerInterface,
							  _impl->objVsBpFilter, _impl->objVsObjFilter);

	// 2. Zero-Initialize the POD SoA World memory explicitly
	// (Since we removed default initializers, this guarantees safety)
	std::memset(&_impl->world, 0, sizeof(Physics::PhysicsWorld));

	// 3. Link Bucket 1 (Cold Data Pointers)
	_impl->world.system = &_impl->physicsSystem;
	_impl->world.bodyInterface = &_impl->physicsSystem.GetBodyInterface();
	_impl->world.jobSystem = &_impl->jobSystem;
	_impl->world.tempAllocator = &_impl->tempAllocator;
	_impl->world.maxJoltBodies = maxBodies;

	// Clear the spinlock
	_impl->world.shadowLock.clear();

	// 4. Allocate Initial Capacity for Bucket 2 (Shadow State)
	_impl->world.capacity = maxBodies; // Simple 1:1 mapping for demonstration
	_impl->world.positions = new JPH::Real[maxBodies * 3]();
	_impl->world.rotations = new float[maxBodies * 4]();
	_impl->world.bodyIDs = new JPH::BodyID[maxBodies]();

	_impl->world.count.store(0, std::memory_order_relaxed);

	ZHLN::Log("PhysicsContext & SoA PhysicsWorld initialized.\n");
}

PhysicsContext::~PhysicsContext() {
	// Clean up Bucket 2 SoA arrays
	delete[] _impl->world.positions;
	delete[] _impl->world.rotations;
	delete[] _impl->world.bodyIDs;
}

void PhysicsContext::Step(float deltaTime) {
	auto& world = _impl->world;

	// Flag that we are stepping (useful if another thread polls)
	world.isStepping.store(true, std::memory_order_release);

	// 1. Execute Jolt Physics Step
	const int collisionSteps = 1;
	_impl->physicsSystem.Update(deltaTime, collisionSteps, &_impl->tempAllocator,
								&_impl->jobSystem);

	// 2. Synchronization Phase: Pull hot data from Jolt into our SoA buffers.
	// This is where the magic happens for your Python/Lua Data-Oriented consumers!
	size_t activeCount = world.count.load(std::memory_order_acquire);

	// Lock the shadow buffer to prevent reader collisions (e.g., Render Thread)
	while (world.shadowLock.test_and_set(std::memory_order_acquire)) {
		// Spin
	}

	for (size_t i = 0; i < activeCount; ++i) {
		JPH::BodyID id = world.bodyIDs[i];

		// If body is sleeping or invalid, skip (optimizations omitted here for brevity)
		if (!world.bodyInterface->IsActive(id))
			continue;

		JPH::RVec3 pos = world.bodyInterface->GetPosition(id);
		JPH::Quat rot = world.bodyInterface->GetRotation(id);

		// Write to SoA Array
		world.positions[i * 3 + 0] = pos.GetX();
		world.positions[i * 3 + 1] = pos.GetY();
		world.positions[i * 3 + 2] = pos.GetZ();

		world.rotations[i * 4 + 0] = rot.GetX();
		world.rotations[i * 4 + 1] = rot.GetY();
		world.rotations[i * 4 + 2] = rot.GetZ();
		world.rotations[i * 4 + 3] = rot.GetW();
	}

	world.shadowLock.clear(std::memory_order_release);
	world.isStepping.store(false, std::memory_order_release);
}

// --- Procedural API ---
namespace Physics {

// Quick helper to append to the SoA tracking
static void RegisterBodyToWorld(PhysicsWorld& world, JPH::BodyID id) {
	size_t index = world.count.fetch_add(1, std::memory_order_relaxed);
	if (index < world.capacity) {
		world.bodyIDs[index] = id;
	}
}

JPH::BodyID CreateStaticFloor(PhysicsContext& ctx, float extent, EntityHandle handle) {
	auto& world = ctx.GetImpl()->world;

	JPH::BoxShapeSettings shapeSettings(JPH::Vec3(extent, 1.0f, extent));
	JPH::ShapeRefC shape = shapeSettings.Create().Get();

	JPH::BodyCreationSettings settings(shape, JPH::RVec3(0.0, -1.0, 0.0), JPH::Quat::sIdentity(),
									   JPH::EMotionType::Static, Layers::NON_MOVING);
	settings.mUserData = handle.Pack();

	JPH::BodyID id =
		world.bodyInterface->CreateAndAddBody(settings, JPH::EActivation::DontActivate);
	RegisterBodyToWorld(world, id);
	return id;
}

JPH::BodyID CreateDynamicBox(PhysicsContext& ctx, JPH::RVec3Arg position, JPH::Vec3Arg halfExtents,
							 EntityHandle handle) {
	auto& world = ctx.GetImpl()->world;

	JPH::BoxShapeSettings shapeSettings(halfExtents);
	JPH::ShapeRefC shape = shapeSettings.Create().Get();

	JPH::BodyCreationSettings settings(shape, position, JPH::Quat::sIdentity(),
									   JPH::EMotionType::Dynamic, Layers::MOVING);
	settings.mUserData = handle.Pack();

	JPH::BodyID id = world.bodyInterface->CreateAndAddBody(settings, JPH::EActivation::Activate);
	RegisterBodyToWorld(world, id);
	return id;
}

JPH::RVec3 GetPosition(const PhysicsContext& ctx, JPH::BodyID bodyID) {
	// Now you can optionally grab this from Jolt directly OR from the SoA arrays
	// (ctx.GetImpl()->world.positions), depending on your thread context!
	return ctx.GetImpl()->world.bodyInterface->GetPosition(bodyID);
}

JPH::Quat GetRotation(const PhysicsContext& ctx, JPH::BodyID bodyID) {
	return ctx.GetImpl()->world.bodyInterface->GetRotation(bodyID);
}

EntityHandle GetEntityHandle(const PhysicsContext& ctx, JPH::BodyID bodyID) {
	uint64_t rawData = ctx.GetImpl()->world.bodyInterface->GetUserData(bodyID);
	return EntityHandle::Unpack(rawData);
}

} // namespace Physics
} // namespace ZHLN