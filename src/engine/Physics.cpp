// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>

#include <Zahlen/Physics.hpp>
#include <Zahlen/PhysicsWorld.hpp>
#include <Zahlen/Log.hpp>

// ZHLN Detail Utilities
#include <Zahlen/detail/Prefetch.hpp>
#include <Zahlen/detail/Span.hpp>
#include <Zahlen/detail/Loop.hpp>
// clang-format on

#include <array>
#include <cstring>
#include <memory>

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

// =================================================================================================
// UNIFIED SYNC PASS IMPLEMENTATION
// =================================================================================================
namespace Physics::Sync {

struct alignas(32) PosStride {
	JPH::Real x, y, z, w;
};
struct alignas(16) AuxStride {
	float x, y, z, w;
};

static_assert(sizeof(PosStride) == sizeof(JPH::Real) * 4);
static_assert(sizeof(AuxStride) == sizeof(float) * 4);
static_assert(sizeof(PosStride) % 32 == 0);

constexpr uint32_t BATCH_SIZE = 128;

struct SyncWorkItem {
	const JPH::Body* body;
	uint32_t dense_idx;
};

struct WorldDataCreateInfo {
	PosStride* const ZHLN_RESTRICT shadow_pos;
	PosStride* const ZHLN_RESTRICT shadow_ppos;
	AuxStride* const ZHLN_RESTRICT shadow_rot;
	AuxStride* const ZHLN_RESTRICT shadow_prot;
};

struct MappingDataCreateInfo {
	const void* ZHLN_RESTRICT* const ZHLN_RESTRICT body_ptrs;
	const std::atomic<uint32_t>* const ZHLN_RESTRICT generations;
	const size_t slot_capacity;
	const uint32_t* const ZHLN_RESTRICT slot_to_dense;
};

#ifdef JPH_DOUBLE_PRECISION
inline constexpr bool IS_DOUBLE = true;
using PosPointerType = JPH::Double3* const ZHLN_RESTRICT;
#else
inline constexpr bool IS_DOUBLE = false;
#endif

using AuxPointerType = JPH::Float4* const ZHLN_RESTRICT;

template <JPH::EBodyType TType>
[[gnu::always_inline, gnu::nonnull(2)]]
inline void ProcessItem(const uint32_t D, const JPH::Body* const ZHLN_RESTRICT b,
						const WorldDataCreateInfo world) noexcept {

	// 1. Snapshot previous state
	world.shadow_ppos[D] = world.shadow_pos[D];
	world.shadow_prot[D] = world.shadow_rot[D];

	// 2. Write Current COM Position
	auto* const targetPos = &world.shadow_pos[D];
	const auto& translation = b->GetCenterOfMassPosition();

	if constexpr (IS_DOUBLE) {
		[[clang::always_inline]] translation.StoreDouble3(
			reinterpret_cast<PosPointerType>(targetPos));
		targetPos->w = 0.0;
	} else {
		[[clang::always_inline]] JPH::Vec4(JPH::Vec3(translation), 0.0f)
			.StoreFloat4(reinterpret_cast<JPH::Float4* const ZHLN_RESTRICT>(targetPos));
	}

	// 3. Write Current Rotation
	const auto& rotation = b->GetRotation();
	[[clang::always_inline]] rotation.GetXYZW().StoreFloat4(
		reinterpret_cast<AuxPointerType>(&world.shadow_rot[D]));

	// Note: Linear/Angular velocity updates omitted for brevity, add them following the same
	// pattern!
}

template <JPH::EBodyType TType>
[[gnu::always_inline, gnu::hot, gnu::flatten]]
inline void ProcessBatch(const WorldDataCreateInfo world,
						 RestrictSpan<const SyncWorkItem> items) noexcept {

	const size_t count = items.size();
	[[assume(count > 0)]];
	[[assume(count <= BATCH_SIZE)]];

	ZHLN::UnrollLoop<4>(count, [&](auto j) {
		if (j + 2 < count) {
			const uint32_t next_idx = items[j + 2].dense_idx;
			ZHLN::Prefetch<AccessType::Write>(&world.shadow_pos[next_idx]);
			ZHLN::Prefetch<AccessType::Write>(&world.shadow_rot[next_idx]);
		}
		ProcessItem<TType>(items[j].dense_idx, items[j].body, world);
	});
}

template <JPH::EBodyType TType>
[[gnu::always_inline, gnu::flatten, gnu::nonnull(2)]]
inline void ExecuteSyncPass(const uint32_t active_count,
							const JPH::PhysicsSystem* const ZHLN_RESTRICT system,
							MappingDataCreateInfo map, const WorldDataCreateInfo world) noexcept {
	if (active_count == 0)
		return;

	const JPH::BodyID* const ZHLN_RESTRICT active_ids = system->GetActiveBodiesUnsafe(TType);
	if (active_ids == nullptr) [[unlikely]]
		return;

	const auto* const ZHLN_RESTRICT lock_iface = &system->GetBodyLockInterfaceNoLock();

	alignas(64) std::array<SyncWorkItem, BATCH_SIZE> worklist;
	uint32_t work_ptr = 0;

	for (uint32_t i = 0; i < active_count; i++) {
		const uint32_t raw_jolt_id = active_ids[i].GetIndexAndSequenceNumber();
		const uint32_t j_idx = raw_jolt_id & JPH::BodyID::cMaxBodyIndex;

		const auto* ZHLN_RESTRICT b =
			static_cast<const JPH::Body * ZHLN_RESTRICT>(map.body_ptrs[j_idx]);

		[[clang::always_inline]]
		if (b == nullptr || b->GetID().GetIndexAndSequenceNumber() != raw_jolt_id) [[unlikely]] {
			b = lock_iface->TryGetBody(JPH::BodyID(raw_jolt_id));
			map.body_ptrs[j_idx] = b;
		}
		[[assume(b != nullptr)]];

		// ECS Handle decoding
		const uint64_t handle = b->GetUserData();
		const uint32_t slot = static_cast<uint32_t>(handle & 0xFFFFFFFF);
		const uint32_t gen = static_cast<uint32_t>(handle >> 32);

		const uint32_t safe_slot = (slot < map.slot_capacity) ? slot : 0;
		const uint32_t current_gen = map.generations[safe_slot].load(std::memory_order_relaxed);

		// Branchless Validation
		const uint32_t bad = static_cast<uint32_t>(slot >= map.slot_capacity) | (current_gen ^ gen);
		const uint32_t d_idx = map.slot_to_dense[safe_slot];
		const uint32_t is_valid = static_cast<uint32_t>(bad == 0);

		[[assume(work_ptr < BATCH_SIZE)]];
		worklist[work_ptr].body = b;
		worklist[work_ptr].dense_idx = d_idx;
		work_ptr += is_valid;

		if (work_ptr == BATCH_SIZE) {
			ProcessBatch<TType>(world, RestrictSpan(worklist));
			work_ptr = 0;
		}
	}

	if (work_ptr > 0) {
		ProcessBatch<TType>(world, RestrictSpan(worklist).first(work_ptr));
	}
}

} // namespace Physics::Sync

// =================================================================================================
// CONTEXT IMPLEMENTATION
// =================================================================================================

struct PhysicsContext::Impl {
	JPH::PhysicsSystem physicsSystem;
	JPH::TempAllocatorImpl tempAllocator{10 * 1024 * 1024};
	JPH::JobSystemThreadPool jobSystem{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 1};

	BPLayerInterfaceImpl bpLayerInterface;
	ObjectVsBroadPhaseLayerFilterImpl objVsBpFilter;
	ObjectLayerPairFilterImpl objVsObjFilter;

	Physics::PhysicsWorld world{};
};

PhysicsContext::PhysicsContext() : _impl(std::make_unique<Impl>()) {
	const uint32_t maxBodies = 1024;

	_impl->physicsSystem.Init(maxBodies, 0, maxBodies, maxBodies, _impl->bpLayerInterface,
							  _impl->objVsBpFilter, _impl->objVsObjFilter);

	std::memset(&_impl->world, 0, sizeof(Physics::PhysicsWorld));

	_impl->world.system = &_impl->physicsSystem;
	_impl->world.bodyInterface = &_impl->physicsSystem.GetBodyInterface();
	_impl->world.jobSystem = &_impl->jobSystem;
	_impl->world.tempAllocator = &_impl->tempAllocator;
	_impl->world.maxJoltBodies = maxBodies;

	_impl->world.shadowLock.clear();

	// Allocate Bucket 2 & ECS Mapping Arrays
	// CRITICAL: Multiply by 4 (not 3) to allow safe 128/256-bit SIMD stores
	_impl->world.capacity = maxBodies;
	_impl->world.slotCapacity = maxBodies;

	_impl->world.positions = new JPH::Real[maxBodies * 4]();
	_impl->world.prevPositions = new JPH::Real[maxBodies * 4]();
	_impl->world.rotations = new float[maxBodies * 4]();
	_impl->world.prevRotations = new float[maxBodies * 4]();
	_impl->world.bodyIDs = new JPH::BodyID[maxBodies]();

	_impl->world.joltBodyPtrs = new const void*[maxBodies]();
	_impl->world.slotToDense = new uint32_t[maxBodies]();
	_impl->world.generations = new std::atomic<uint32_t>[maxBodies]();

	_impl->world.count.store(0, std::memory_order_relaxed);
}

PhysicsContext::~PhysicsContext() {
	delete[] _impl->world.positions;
	delete[] _impl->world.prevPositions;
	delete[] _impl->world.rotations;
	delete[] _impl->world.prevRotations;
	delete[] _impl->world.bodyIDs;
	delete[] _impl->world.joltBodyPtrs;
	delete[] _impl->world.slotToDense;
	delete[] _impl->world.generations;
}

void PhysicsContext::Step(float deltaTime) {
	auto& world = _impl->world;

	world.isStepping.store(true, std::memory_order_release);

	// 1. Step Physics
	const int collisionSteps = 1;
	_impl->physicsSystem.Update(deltaTime, collisionSteps, &_impl->tempAllocator,
								&_impl->jobSystem);

	// 2. Lock Shadow Buffer
	while (world.shadowLock.test_and_set(std::memory_order_acquire)) {
	}

	// 3. Setup Structs for Sync Pass
	Physics::Sync::WorldDataCreateInfo syncWorld = {
		.shadow_pos =
			std::assume_aligned<32>(reinterpret_cast<Physics::Sync::PosStride*>(world.positions)),
		.shadow_ppos = std::assume_aligned<32>(
			reinterpret_cast<Physics::Sync::PosStride*>(world.prevPositions)),
		.shadow_rot =
			std::assume_aligned<16>(reinterpret_cast<Physics::Sync::AuxStride*>(world.rotations)),
		.shadow_prot = std::assume_aligned<16>(
			reinterpret_cast<Physics::Sync::AuxStride*>(world.prevRotations)),
	};

	Physics::Sync::MappingDataCreateInfo mapData = {
		.body_ptrs = world.joltBodyPtrs,
		.generations = world.generations,
		.slot_capacity = world.slotCapacity,
		.slot_to_dense = world.slotToDense,
	};

	// 4. Execute Hyper-Fast SIMD Sync
	const uint32_t activeRigids =
		_impl->physicsSystem.GetNumActiveBodies(JPH::EBodyType::RigidBody);
	Physics::Sync::ExecuteSyncPass<JPH::EBodyType::RigidBody>(activeRigids, &_impl->physicsSystem,
															  mapData, syncWorld);

	world.shadowLock.clear(std::memory_order_release);
	world.isStepping.store(false, std::memory_order_release);
}

// =================================================================================================
// PROCEDURAL API
// =================================================================================================

namespace Physics {

// Simple wrapper to wire up the Body -> SoA mapping correctly
static void RegisterBodyToWorld(PhysicsWorld& world, JPH::BodyID id, EntityHandle handle) {
	size_t dense_idx = world.count.fetch_add(1, std::memory_order_relaxed);

	if (dense_idx < world.capacity && handle.index < world.slotCapacity) {
		world.bodyIDs[dense_idx] = id;
		world.slotToDense[handle.index] = dense_idx;
		world.generations[handle.index].store(handle.generation, std::memory_order_relaxed);
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
	RegisterBodyToWorld(world, id, handle);
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
	RegisterBodyToWorld(world, id, handle);
	return id;
}

JPH::RVec3 GetPosition(const PhysicsContext& ctx, JPH::BodyID bodyID) {
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