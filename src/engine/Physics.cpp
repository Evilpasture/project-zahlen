// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>

#include <Zahlen/Physics.hpp>
#include <Zahlen/Buffer.h>
#include "PhysicsWorld.hpp"
#include <Zahlen/Log.hpp>

// ZHLN Detail Utilities
#include <detail/Prefetch.hpp>
#include <detail/Span.hpp>
#include <detail/Loop.hpp>
// clang-format on

#include <array>
#include <cstring>
#include <memory>
#include <new>

namespace ZHLN {

struct ShapeKey {
	uint32_t type;
	float p1, p2, p3, p4;
};

struct ShapeEntry {
	ShapeKey key;
	JPH::ShapeRefC shape;
};

enum SlotState : uint8_t { SLOT_EMPTY = 0, SLOT_ALIVE = 1, SLOT_CHARACTER = 2 };

// =================================================================================================
// MEMORY UTILITIES
// =================================================================================================

template <typename T> [[nodiscard]] static T* AllocateAligned(size_t count, size_t alignment) {
	return static_cast<T*>(::operator new[](count * sizeof(T), std::align_val_t{alignment}));
}

template <typename T> static void DeallocateAligned(T* ptr, size_t alignment) {
	if (ptr)
		::operator delete[](ptr, std::align_val_t{alignment});
}

template <typename T>
static void ReallocateAligned(T*& ptr, size_t old_count, size_t new_count, size_t alignment) {
	T* new_ptr = AllocateAligned<T>(new_count, alignment);
	if (ptr && old_count > 0) {
		std::memcpy(new_ptr, ptr, old_count * sizeof(T));
		DeallocateAligned(ptr, alignment);
	} else if (ptr) {
		DeallocateAligned(ptr, alignment);
	}
	ptr = new_ptr;
}

template <typename T> static void ReallocateStandard(T*& ptr, size_t old_count, size_t new_count) {
	T* new_ptr = new T[new_count]();
	if (ptr && old_count > 0) {
		std::memcpy(new_ptr, ptr, old_count * sizeof(T));
		delete[] ptr;
	} else if (ptr) {
		delete[] ptr;
	}
	ptr = new_ptr;
}

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
	AuxStride* const ZHLN_RESTRICT shadow_lvel;
	AuxStride* const ZHLN_RESTRICT shadow_avel;
};

struct MappingDataCreateInfo {
	const void* ZHLN_RESTRICT* const ZHLN_RESTRICT body_ptrs;
	const ZHLN::Atomic<uint32_t>* const ZHLN_RESTRICT generations;
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

	// 1. Snapshot previous state (Center of Mass & Rotation)
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

	// 4. Write Velocities (Rigid Body Only)
	if constexpr (TType == JPH::EBodyType::RigidBody) {
		const auto& linVel = b->GetLinearVelocity();
		const auto& angVel = b->GetAngularVelocity();

		[[clang::always_inline]] JPH::Vec4(linVel, 0.0f)
			.StoreFloat4(reinterpret_cast<AuxPointerType>(&world.shadow_lvel[D]));

		[[clang::always_inline]] JPH::Vec4(angVel, 0.0f)
			.StoreFloat4(reinterpret_cast<AuxPointerType>(&world.shadow_avel[D]));
	}
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

/**
 * @brief Optimized SIMD sync for CharacterVirtual objects.
 * Handles Position, Rotation, and Linear Velocity.
 */
[[gnu::always_inline]]
inline void SyncCharacters(const JPH::Array<JPH::CharacterVirtual*>& characters,
						   const MappingDataCreateInfo& map,
						   const WorldDataCreateInfo& world) noexcept {
	for (auto* character : characters) {
		const EntityHandle h = EntityHandle::Unpack(character->GetUserData());

		// In-loop validation (similar to rigid body pass)
		const uint32_t slot = h.index;
		if (slot >= map.slot_capacity) [[unlikely]]
			continue;

		const uint32_t D = map.slot_to_dense[slot];

		// 1. Snapshot previous state
		world.shadow_ppos[D] = world.shadow_pos[D];
		world.shadow_prot[D] = world.shadow_rot[D];

		// 2. Optimized Position Store
		const JPH::RVec3 pos = character->GetPosition();
		auto* const targetPos = &world.shadow_pos[D];
		if constexpr (IS_DOUBLE) {
			pos.StoreDouble3(reinterpret_cast<PosPointerType>(targetPos));
			targetPos->w = 0.0;
		} else {
			JPH::Vec4(JPH::Vec3(pos), 0.0f)
				.StoreFloat4(reinterpret_cast<AuxPointerType>(targetPos));
		}

		// 3. Optimized Rotation Store
		const JPH::Quat rot = character->GetRotation();
		rot.GetXYZW().StoreFloat4(reinterpret_cast<AuxPointerType>(&world.shadow_rot[D]));

		// 4. Optimized Velocity Store
		const JPH::Vec3 vel = character->GetLinearVelocity();
		JPH::Vec4(vel, 0.0f).StoreFloat4(reinterpret_cast<AuxPointerType>(&world.shadow_lvel[D]));
	}
}

} // namespace Physics::Sync

// =================================================================================================
// CULVERIN-STYLE LIFECYCLE (SOA Plumbing)
// =================================================================================================

namespace Physics {

static void ResizeBuffers(PhysicsWorld& world, size_t newCapacity) {
	if (newCapacity <= world.capacity)
		return;

	size_t oldCap = world.capacity;
	world.capacity = newCapacity;
	world.slotCapacity = newCapacity;

	// 1. SIMD Aligned Buffers
	ReallocateAligned(world.positions, oldCap * 4, newCapacity * 4, 32);
	ReallocateAligned(world.prevPositions, oldCap * 4, newCapacity * 4, 32);
	ReallocateAligned(world.rotations, oldCap * 4, newCapacity * 4, 16);
	ReallocateAligned(world.prevRotations, oldCap * 4, newCapacity * 4, 16);
	ReallocateAligned(world.linearVelocities, oldCap * 4, newCapacity * 4, 16);
	ReallocateAligned(world.angularVelocities, oldCap * 4, newCapacity * 4, 16);

	// 2. Standard Buffers
	ReallocateStandard(world.bodyIDs, oldCap, newCapacity);
	ReallocateStandard(world.materialIDs, oldCap, newCapacity);
	ReallocateStandard(world.userData, oldCap, newCapacity);
	ReallocateStandard(world.slotToDense, oldCap, newCapacity);
	ReallocateStandard(world.denseToSlot, oldCap, newCapacity);
	ReallocateStandard(world.freeSlots, oldCap, newCapacity);
	ReallocateStandard(world.categories, oldCap, newCapacity);
	ReallocateStandard(world.masks, oldCap, newCapacity);
	ReallocateStandard(world.generations, oldCap, newCapacity);
	ReallocateStandard(world.slotStates, oldCap, newCapacity);

	// 3. Populate new free slots
	size_t freeIdx = world.freeCount.load(std::memory_order_relaxed);
	for (size_t i = oldCap; i < newCapacity; i++) {
		world.generations[i].store(1, std::memory_order_relaxed);
		world.slotStates[i].store(SLOT_EMPTY, std::memory_order_relaxed);
		world.freeSlots[freeIdx++] = static_cast<uint32_t>(i);
	}
	world.freeCount.store(freeIdx, std::memory_order_release);
}

static EntityHandle AllocateHandle(PhysicsWorld& world) {
	size_t available = world.freeCount.load(std::memory_order_acquire);
	if (available == 0) {
		ResizeBuffers(world, world.capacity * 2);
		available = world.freeCount.load(std::memory_order_acquire);
	}

	uint32_t slot = world.freeSlots[--available];
	world.freeCount.store(available, std::memory_order_release);

	uint32_t gen = world.generations[slot].load(std::memory_order_relaxed);
	return EntityHandle{.index = slot, .generation = gen};
}

static void RemoveBodySlot(PhysicsWorld& world, uint32_t slot) {
	const uint32_t denseIdx = world.slotToDense[slot];
	const uint32_t lastDense =
		static_cast<uint32_t>(world.count.load(std::memory_order_acquire)) - 1;

	// THE SWAP-TO-DELETE (Dense Pack)
	if (denseIdx != lastDense) {
		// --- Move SIMD Data (Stride 4) ---
		for (int i = 0; i < 4; ++i) {
			world.positions[denseIdx * 4 + i] = world.positions[lastDense * 4 + i];
			world.prevPositions[denseIdx * 4 + i] = world.prevPositions[lastDense * 4 + i];
			world.rotations[denseIdx * 4 + i] = world.rotations[lastDense * 4 + i];
			world.prevRotations[denseIdx * 4 + i] = world.prevRotations[lastDense * 4 + i];
			world.linearVelocities[denseIdx * 4 + i] = world.linearVelocities[lastDense * 4 + i];
			world.angularVelocities[denseIdx * 4 + i] = world.angularVelocities[lastDense * 4 + i];
		}

		// --- Move Metadata ---
		world.bodyIDs[denseIdx] = world.bodyIDs[lastDense];
		world.userData[denseIdx] = world.userData[lastDense];
		world.categories[denseIdx] = world.categories[lastDense];
		world.masks[denseIdx] = world.masks[lastDense];
		world.materialIDs[denseIdx] = world.materialIDs[lastDense];

		// --- The Map Rewire ---
		const uint32_t moverSlot = world.denseToSlot[lastDense];
		world.slotToDense[moverSlot] = denseIdx;
		world.denseToSlot[denseIdx] = moverSlot;
	}

	// HOUSEKEEPING
	world.generations[slot].fetch_add(1, std::memory_order_relaxed);
	world.slotStates[slot].store(SLOT_EMPTY, std::memory_order_release);

	size_t fIdx = world.freeCount.fetch_add(1, std::memory_order_relaxed);
	world.freeSlots[fIdx] = slot;
	world.count.fetch_sub(1, std::memory_order_release);
}

} // namespace Physics

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

	// Map: Entity Index -> Character Object
	// We use JPH::Ref to keep the character alive
	JPH::Array<JPH::Ref<JPH::CharacterVirtual>> characterMap;

	// Fast list for the Step() function to iterate over
	JPH::Array<JPH::CharacterVirtual*> activeCharacters;

	// We need a listener for character-specific interactions
	class CharacterListener : public JPH::CharacterContactListener {
		// Implement if you need character-specific collision logic
	} characterListener;

	std::vector<ShapeEntry> shapeCache;
};

PhysicsContext::PhysicsContext() : _impl(std::make_unique<Impl>()) {
	const uint32_t maxBodies = 1024;

	_impl->physicsSystem.Init(maxBodies, 0, maxBodies, maxBodies, _impl->bpLayerInterface,
							  _impl->objVsBpFilter, _impl->objVsObjFilter);
	auto& world = _impl->world;
	std::memset(&world, 0, sizeof(Physics::PhysicsWorld));

	world.system = &_impl->physicsSystem;
	world.bodyInterface = &_impl->physicsSystem.GetBodyInterface();
	world.jobSystem = &_impl->jobSystem;
	world.tempAllocator = &_impl->tempAllocator;
	world.maxJoltBodies = maxBodies;

	world.capacity = maxBodies;
	world.slotCapacity = maxBodies;

	// --- 1. SIMD Aligned Buffers (Shadow State) ---
	world.positions = AllocateAligned<JPH::Real>(maxBodies * 4, 32);
	world.prevPositions = AllocateAligned<JPH::Real>(maxBodies * 4, 32);
	world.rotations = AllocateAligned<float>(maxBodies * 4, 16);
	world.prevRotations = AllocateAligned<float>(maxBodies * 4, 16);
	world.linearVelocities = AllocateAligned<float>(maxBodies * 4, 16);
	world.angularVelocities = AllocateAligned<float>(maxBodies * 4, 16);

	// --- 2. Standard ECS Mapping Tables ---
	world.bodyIDs = new JPH::BodyID[maxBodies]();
	world.materialIDs = new uint32_t[maxBodies]();
	world.userData = new uint64_t[maxBodies]();

	world.slotToDense = new uint32_t[maxBodies]();
	world.denseToSlot = new uint32_t[maxBodies]();
	world.freeSlots = new uint32_t[maxBodies]();

	world.categories = new uint32_t[maxBodies]();
	world.masks = new uint32_t[maxBodies]();

	// --- 3. Atomic State & Handle Tables ---
	world.slotStates = new ZHLN::Atomic<uint8_t>[maxBodies]();
	world.generations = new ZHLN::Atomic<uint32_t>[maxBodies]();

	// idToHandleMap is sized to Jolt's body limit + 1
	world.idToHandleMap = new ZHLN::Atomic<uint64_t>[maxBodies + 1]();
	world.joltBodyPtrs = new const void*[maxBodies + 1]();

	// --- 4. Initialize Free Stack (Culverin Style) ---
	for (uint32_t i = 0; i < maxBodies; ++i) {
		world.generations[i].store(1, std::memory_order_relaxed);
		world.slotStates[i].store(SLOT_EMPTY, std::memory_order_relaxed);
		// Push indices in reverse so we pop 0, 1, 2...
		world.freeSlots[i] = (maxBodies - 1) - i;
	}

	world.count.store(0, std::memory_order_relaxed);
	world.freeCount.store(maxBodies, std::memory_order_relaxed);
}

PhysicsContext::~PhysicsContext() {
	auto& world = _impl->world;

	// 1. Deallocate Aligned SIMD Buffers
	DeallocateAligned(world.positions, 32);
	DeallocateAligned(world.prevPositions, 32);
	DeallocateAligned(world.rotations, 16);
	DeallocateAligned(world.prevRotations, 16);
	DeallocateAligned(world.linearVelocities, 16);
	DeallocateAligned(world.angularVelocities, 16);

	// 2. Delete Standard ECS Tables
	delete[] world.bodyIDs;
	delete[] world.materialIDs;
	delete[] world.userData;
	delete[] world.slotToDense;
	delete[] world.denseToSlot;
	delete[] world.freeSlots;
	delete[] world.categories;
	delete[] world.masks;

	// 3. Delete Atomic State & Handle Tables
	delete[] world.slotStates;
	delete[] world.generations;
	delete[] world.idToHandleMap;
	delete[] world.joltBodyPtrs;
}

void PhysicsContext::Step(float deltaTime) {
	auto& world = _impl->world;
	world.isStepping.store(true, std::memory_order_release);

	// 1. Jolt Update Pass
	_impl->physicsSystem.Update(deltaTime, 1, &_impl->tempAllocator, &_impl->jobSystem);

	for (auto* character : _impl->activeCharacters) {
		character->Update(deltaTime, _impl->physicsSystem.GetGravity(),
						  _impl->physicsSystem.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
						  _impl->physicsSystem.GetDefaultLayerFilter(Layers::MOVING), {}, {},
						  _impl->tempAllocator);
	}

	// 2. Data Synchronization Pass
	{
		std::lock_guard<ZHLN::Mutex> lock(world.shadowLock);

		// Prepare Sync Descriptors (Pointers aligned to 16/32 bytes)
		Physics::Sync::WorldDataCreateInfo syncWorld = {
			.shadow_pos = std::assume_aligned<32>(
				reinterpret_cast<Physics::Sync::PosStride*>(world.positions)),
			.shadow_ppos = std::assume_aligned<32>(
				reinterpret_cast<Physics::Sync::PosStride*>(world.prevPositions)),
			.shadow_rot = std::assume_aligned<16>(
				reinterpret_cast<Physics::Sync::AuxStride*>(world.rotations)),
			.shadow_prot = std::assume_aligned<16>(
				reinterpret_cast<Physics::Sync::AuxStride*>(world.prevRotations)),
			.shadow_lvel = std::assume_aligned<16>(
				reinterpret_cast<Physics::Sync::AuxStride*>(world.linearVelocities)),
			.shadow_avel = std::assume_aligned<16>(
				reinterpret_cast<Physics::Sync::AuxStride*>(world.angularVelocities)),
		};

		Physics::Sync::MappingDataCreateInfo mapData = {
			.body_ptrs = world.joltBodyPtrs,
			.generations = world.generations,
			.slot_capacity = world.slotCapacity,
			.slot_to_dense = world.slotToDense,
		};

		// 3. Batch Sync Rigid Bodies
		const uint32_t activeRigids =
			_impl->physicsSystem.GetNumActiveBodies(JPH::EBodyType::RigidBody);
		Physics::Sync::ExecuteSyncPass<JPH::EBodyType::RigidBody>(
			activeRigids, &_impl->physicsSystem, mapData, syncWorld);

		// 4. SIMD Sync Characters
		Physics::Sync::SyncCharacters(_impl->activeCharacters, mapData, syncWorld);
	}

	world.isStepping.store(false, std::memory_order_release);
}

const Physics::PhysicsWorld& PhysicsContext::GetWorld() const {
	return _impl->world;
}

namespace Physics {

// =================================================================================================
// CULVERIN SHAPE CACHING
// =================================================================================================

JPH::ShapeRefC GetOrCreateShape(PhysicsContext& ctx, Physics::ShapeType type, float p1, float p2,
								float p3, float p4) {
	auto* impl = ctx.GetImpl();
	std::lock_guard<Mutex> lock(impl->world.shadowLock);

	// 1. Normalization (Matches Culverin logic for maximum cache hits)
	const float np1 = (p1 < 1e-3f && type != Physics::ShapeType::Plane) ? 1e-3f : p1;
	const float np2 = (p2 < 1e-3f && type != Physics::ShapeType::Plane) ? 1e-3f : p2;
	const float np3 = (p3 < 1e-3f && type != Physics::ShapeType::Plane) ? 1e-3f : p3;
	const float np4 = (type == Physics::ShapeType::Plane) ? p4 : 0.0f;

	// 2. Cache Lookup
	for (const auto& entry : impl->shapeCache) {
		if (entry.key.type == static_cast<uint32_t>(type) && entry.key.p1 == np1 &&
			entry.key.p2 == np2 && entry.key.p3 == np3 && entry.key.p4 == np4) {
			return entry.shape;
		}
	}

	// 3. Jolt Creation
	JPH::ShapeRefC shape;
	switch (type) {
		case Physics::ShapeType::Box: {
			JPH::BoxShapeSettings s(JPH::Vec3(np1, np2, np3), 0.05f);
			shape = s.Create().Get();
			break;
		}
		case Physics::ShapeType::Sphere: {
			JPH::SphereShapeSettings s(np1);
			shape = s.Create().Get();
			break;
		}
		case Physics::ShapeType::Capsule: {
			JPH::CapsuleShapeSettings s(np1, np2);
			shape = s.Create().Get();
			break;
		}
		case Physics::ShapeType::Cylinder: {
			JPH::CylinderShapeSettings s(np1, np2, 0.05f);
			shape = s.Create().Get();
			break;
		}
		case Physics::ShapeType::Plane: {
			JPH::Plane plane(JPH::Vec3(np1, np2, np3), np4);
			JPH::PlaneShapeSettings s(plane, nullptr, 1000.0f);
			shape = s.Create().Get();
			break;
		}
	}

	if (!shape) {
		ZHLN::Panic("Failed to create Jolt Shape! Degenerate parameters?");
	}

	// 4. Store in cache
	impl->shapeCache.push_back({ShapeKey{static_cast<uint32_t>(type), np1, np2, np3, np4}, shape});

	return shape;
}

// =================================================================================================
// PROCEDURAL API
// =================================================================================================

JPH::BodyID GetBodyID(const PhysicsWorld& world, EntityHandle handle) {
	if (handle.index >= world.slotCapacity)
		return JPH::BodyID();

	// Check generation: Source of Truth check
	if (world.generations[handle.index].load(std::memory_order_acquire) != handle.generation) {
		return JPH::BodyID(); // Stale handle
	}

	uint32_t dense = world.slotToDense[handle.index];
	return world.bodyIDs[dense];
}

void DestroyBody(PhysicsContext& ctx, EntityHandle handle) {
	auto& world = ctx.GetImpl()->world;

	// Lock the shadow buffer so the renderer doesn't read half-moved data
	std::lock_guard<Mutex> lock(world.shadowLock);

	const uint32_t slot = handle.index;
	if (slot >= world.slotCapacity)
		return;

	// Generational safety (The Culverin Way)
	if (world.generations[slot].load(std::memory_order_acquire) != handle.generation)
		return;

	// Resolve internal Jolt ID
	uint32_t dense = world.slotToDense[handle.index];
	JPH::BodyID bodyID = world.bodyIDs[dense];

	// 1. Unmap Jolt ID from Handle
	const uint32_t joltIdx = bodyID.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
	world.idToHandleMap[joltIdx].store(0, std::memory_order_release);
	world.joltBodyPtrs[joltIdx] = nullptr;

	// 2. Physical Removal
	world.bodyInterface->RemoveBody(bodyID);
	world.bodyInterface->DestroyBody(bodyID);

	// 3. Compact the SoA
	RemoveBodySlot(world, slot);
}

/**
 * @brief Generalized Rigid Body creation.
 */
EntityHandle CreateRigidBody(PhysicsContext& ctx, JPH::ShapeRefC shape, JPH::RVec3Arg pos,
							 JPH::QuatArg rot, JPH::EMotionType motion, JPH::ObjectLayer layer) {
	auto& world = ctx.GetImpl()->world;
	std::lock_guard<Mutex> lock(world.shadowLock);

	// Engine allocates the identity!
	EntityHandle handle = AllocateHandle(world);

	JPH::BodyCreationSettings settings(shape, pos, rot, motion, layer);
	settings.mUserData = handle.Pack();
	if (motion == JPH::EMotionType::Dynamic)
		settings.mAllowSleeping = true;

	JPH::BodyID id = world.bodyInterface->CreateAndAddBody(
		settings, (motion == JPH::EMotionType::Static) ? JPH::EActivation::DontActivate
													   : JPH::EActivation::Activate);

	// Map into the Dense SoA
	uint32_t dense = static_cast<uint32_t>(world.count.fetch_add(1, std::memory_order_relaxed));
	world.bodyIDs[dense] = id;
	world.slotToDense[handle.index] = dense;
	world.denseToSlot[dense] = handle.index;
	world.slotStates[handle.index].store(SLOT_ALIVE, std::memory_order_release);

	// Update Fast-Mapping for callbacks
	const uint32_t j_idx = id.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
	world.idToHandleMap[j_idx].store(handle.Pack(), std::memory_order_release);

	// Warm up shadow buffers so the very first render frame is correct
	world.positions[dense * 4 + 0] = pos.GetX();
	world.positions[dense * 4 + 1] = pos.GetY();
	world.positions[dense * 4 + 2] = pos.GetZ();
	world.positions[dense * 4 + 3] = 0.0;

	world.rotations[dense * 4 + 0] = rot.GetX();
	world.rotations[dense * 4 + 1] = rot.GetY();
	world.rotations[dense * 4 + 2] = rot.GetZ();
	world.rotations[dense * 4 + 3] = rot.GetW();

	return handle;
}

EntityHandle CreateCharacter(PhysicsContext& ctx, JPH::RVec3Arg position) {
	auto* impl = ctx.GetImpl();
	auto& world = impl->world;
	std::lock_guard<Mutex> lock(world.shadowLock);

	EntityHandle handle = AllocateHandle(world);

	// Using the cache for the character shape
	JPH::ShapeRefC charShape = GetOrCreateShape(ctx, ShapeType::Capsule, 0.5f, 0.3f);

	JPH::CharacterVirtualSettings settings;
	settings.mShape = charShape;
	auto character = new JPH::CharacterVirtual(&settings, position, JPH::Quat::sIdentity(),
											   &impl->physicsSystem);
	character->SetUserData(handle.Pack());

	// Character Management
	if (handle.index >= impl->characterMap.size())
		impl->characterMap.resize(handle.index + 1);
	impl->characterMap[handle.index] = character;
	impl->activeCharacters.push_back(character);

	// SoA Sync
	uint32_t dense = static_cast<uint32_t>(world.count.fetch_add(1, std::memory_order_relaxed));
	world.slotToDense[handle.index] = dense;
	world.denseToSlot[dense] = handle.index;
	world.bodyIDs[dense] = JPH::BodyID(); // Characters don't have Jolt BodyIDs
	world.slotStates[handle.index].store(SLOT_CHARACTER, std::memory_order_release);

	return handle;
}

void SetCharacterVelocity(PhysicsContext& ctx, EntityHandle handle, JPH::Vec3Arg velocity) {
	auto* impl = ctx.GetImpl();

	// Generational Safety Check
	if (handle.index < impl->characterMap.size()) {
		auto& character = impl->characterMap[handle.index];
		if (character &&
			EntityHandle::Unpack(character->GetUserData()).generation == handle.generation) {
			character->SetLinearVelocity(velocity);
		}
	}
}

JPH::Vec3 GetCharacterVelocity(const PhysicsContext& ctx, EntityHandle handle) {
	auto* impl = ctx.GetImpl();
	if (handle.index < impl->characterMap.size()) {
		auto& character = impl->characterMap[handle.index];
		if (character)
			return character->GetLinearVelocity();
	}
	return JPH::Vec3::sZero();
}

bool IsCharacterOnGround(const PhysicsContext& ctx, EntityHandle handle) {
	auto* impl = ctx.GetImpl();
	if (handle.index < impl->characterMap.size()) {
		auto& character = impl->characterMap[handle.index];
		if (character) {
			// Jolt returns "Supported" if we are standing on something
			return character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround;
		}
	}
	return false;
}

auto GetPositionBuffer(const PhysicsContext& ctx) -> ZHLN::BufferView {
	const auto& world = ctx.GetWorld();
	BufferView view;
	view.buf = world.positions;
	view.itemsize = sizeof(JPH::Real);
	// Stride is 4 * itemsize because our SoA is packed [x,y,z,w]
	view.strides[0] = sizeof(JPH::Real) * 4;
	view.shape[0] = world.count.load();
	view.ndim = 1;
	view.format[0] = (sizeof(JPH::Real) == 8) ? 'd' : 'f';
	return view;
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