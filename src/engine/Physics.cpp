// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>

#include <Zahlen/Physics.hpp>
#include <Zahlen/Buffer.hpp>
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

	// Allocate Bucket 2 & ECS Mapping Arrays
	// CRITICAL: Multiply by 4 (not 3) to allow safe 128/256-bit SIMD stores
	_impl->world.capacity = maxBodies;
	_impl->world.slotCapacity = maxBodies;

	_impl->world.positions = new JPH::Real[maxBodies * 4]();
	_impl->world.prevPositions = new JPH::Real[maxBodies * 4]();
	_impl->world.rotations = new float[maxBodies * 4]();
	_impl->world.prevRotations = new float[maxBodies * 4]();
	_impl->world.linearVelocities = new float[maxBodies * 4]();
	_impl->world.angularVelocities = new float[maxBodies * 4]();
	_impl->world.bodyIDs = new JPH::BodyID[maxBodies]();

	_impl->world.joltBodyPtrs = new const void*[maxBodies]();
	_impl->world.slotToDense = new uint32_t[maxBodies]();
	_impl->world.denseToSlot = new uint32_t[maxBodies]();
	_impl->world.generations = new ZHLN::Atomic<uint32_t>[maxBodies]();

	_impl->world.count.store(0, std::memory_order_relaxed);
}

PhysicsContext::~PhysicsContext() {
	delete[] _impl->world.positions;
	delete[] _impl->world.prevPositions;
	delete[] _impl->world.rotations;
	delete[] _impl->world.prevRotations;
	delete[] _impl->world.linearVelocities;
	delete[] _impl->world.angularVelocities;
	delete[] _impl->world.bodyIDs;
	delete[] _impl->world.joltBodyPtrs;
	delete[] _impl->world.slotToDense;
	delete[] _impl->world.generations;
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

// =================================================================================================
// PROCEDURAL API
// =================================================================================================

namespace Physics {

// Simple wrapper to wire up the Body -> SoA mapping correctly
static void RegisterBodyToWorld(PhysicsWorld& world, JPH::BodyID id, EntityHandle handle) {
	size_t dense_idx = world.count.fetch_add(1, std::memory_order_relaxed);

	if (dense_idx < world.capacity && handle.index < world.slotCapacity) {
		world.bodyIDs[dense_idx] = id;
		world.slotToDense[handle.index] = static_cast<uint32_t>(dense_idx);
		world.denseToSlot[dense_idx] = handle.index;
		world.generations[handle.index].store(handle.generation, std::memory_order_relaxed);
	}
}

void DestroyBody(PhysicsContext& ctx, JPH::BodyID bodyID, EntityHandle handle) {
	auto& world = ctx.GetImpl()->world;

	// 1. Remove from Jolt Physics
	world.bodyInterface->RemoveBody(bodyID);
	world.bodyInterface->DestroyBody(bodyID);

	// 2. Clear from pointer tracking (Prevents Sync pass from reading it)
	const uint32_t joltIdx = bodyID.GetIndexAndSequenceNumber() & JPH::BodyID::cMaxBodyIndex;
	if (joltIdx < world.maxJoltBodies) {
		world.joltBodyPtrs[joltIdx] = nullptr;
	}

	// 3. Swap-and-Pop in the SoA Arrays
	// We MUST lock the shadow buffer because the Renderer might be reading it right now!
	std::lock_guard<Mutex> lock(world.shadowLock);

	const uint32_t slot = handle.index;
	if (slot >= world.slotCapacity)
		return;

	// Verify handle generation (ensure we aren't deleting a stale handle)
	const uint32_t expectedGen = world.generations[slot].load(std::memory_order_acquire);
	if (expectedGen != handle.generation)
		return;

	const uint32_t denseIdx = world.slotToDense[slot];
	const uint32_t lastDense =
		static_cast<uint32_t>(world.count.load(std::memory_order_acquire)) - 1;

	// Perform the Dense Pack if it's not already the last item
	if (denseIdx != lastDense) {
		// --- 3A: Move Physics Data ---
		// Positions (Stride of 4)
		world.positions[denseIdx * 4 + 0] = world.positions[lastDense * 4 + 0];
		world.positions[denseIdx * 4 + 1] = world.positions[lastDense * 4 + 1];
		world.positions[denseIdx * 4 + 2] = world.positions[lastDense * 4 + 2];
		world.positions[denseIdx * 4 + 3] = world.positions[lastDense * 4 + 3];

		world.prevPositions[denseIdx * 4 + 0] = world.prevPositions[lastDense * 4 + 0];
		world.prevPositions[denseIdx * 4 + 1] = world.prevPositions[lastDense * 4 + 1];
		world.prevPositions[denseIdx * 4 + 2] = world.prevPositions[lastDense * 4 + 2];
		world.prevPositions[denseIdx * 4 + 3] = world.prevPositions[lastDense * 4 + 3];

		// Rotations & Velocities (Stride of 4)
		for (int i = 0; i < 4; ++i) {
			world.rotations[denseIdx * 4 + i] = world.rotations[lastDense * 4 + i];
			world.prevRotations[denseIdx * 4 + i] = world.prevRotations[lastDense * 4 + i];
			world.linearVelocities[denseIdx * 4 + i] = world.linearVelocities[lastDense * 4 + i];
			world.angularVelocities[denseIdx * 4 + i] = world.angularVelocities[lastDense * 4 + i];
		}

		// --- 3B: Move Metadata ---
		world.bodyIDs[denseIdx] = world.bodyIDs[lastDense];

		// --- 3C: Rewire Indirection Map ---
		// We need to find which "Slot" the last item belonged to so we can update its map
		// Note: You don't currently have `denseToSlot` in ZHLN, so we must add it!
		const uint32_t moverSlot = world.denseToSlot[lastDense];
		world.slotToDense[moverSlot] = denseIdx;
		world.denseToSlot[denseIdx] = moverSlot;
	}

	// 4. Invalidate Handle Generation & Decrement Count
	world.generations[slot].fetch_add(1, std::memory_order_release);
	world.count.fetch_sub(1, std::memory_order_release);

	// Optional: Push `slot` to a `freeSlots` stack if you want to reuse ECS indices.
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

EntityHandle CreateCharacter(PhysicsContext& ctx, JPH::RVec3Arg position, EntityHandle handle) {
	auto* impl = ctx.GetImpl();

	// 1. Setup Settings
	JPH::CharacterVirtualSettings settings;
	settings.mShape = new JPH::CapsuleShape(0.5f, 0.3f);

	// 2. Create
	auto character = new JPH::CharacterVirtual(&settings, position, JPH::Quat::sIdentity(),
											   &impl->physicsSystem);
	character->SetUserData(handle.Pack());

	// 3. Store in the generational map
	if (handle.index >= impl->characterMap.size()) {
		impl->characterMap.resize(handle.index + 1);
	}
	impl->characterMap[handle.index] = character;
	impl->activeCharacters.push_back(character);

	// 4. Register in SoA (Passing Invalid BodyID to signify it's a Character)
	RegisterBodyToWorld(impl->world, JPH::BodyID(), handle);

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

JPH::RVec3 GetPosition(const PhysicsContext& ctx, JPH::BodyID bodyID) {
	return ctx.GetImpl()->world.bodyInterface->GetPosition(bodyID);
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