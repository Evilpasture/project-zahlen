#pragma once

#include "Zahlen/Entity.hpp"
#include "Zahlen/Sync.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <cstdint>
#include <detail/Atomic.hpp>
#include <detail/Platform.hpp>
#include <threading/Mutex.hpp>
#include <type_traits>

namespace ZHLN {
class PhysicsContext;
}

namespace ZHLN::Physics {

struct WorldStateHeader {
	static constexpr uint64_t ZHLN = 0x5A484C4E;
	const uint64_t magic = ZHLN;
	const uint32_t version = 1;
	uint32_t bodyCount;
	uint32_t slotCapacity;
	double worldTime;
};

inline constexpr std::size_t CACHE_LINE = 64;

struct ConstraintHandle {
	uint32_t index;
	uint32_t generation;
	[[nodiscard]] constexpr uint64_t Pack() const noexcept {
		return (static_cast<uint64_t>(generation) << 32) | index;
	}
};

static_assert((std::is_trivially_default_constructible_v<ConstraintHandle> && std::is_trivially_copyable_v<ConstraintHandle>));

enum class ConstraintType : uint8_t { Fixed, Point, Hinge, Slider, Cone, Distance };

struct ConstraintParams {
	JPH::Vec3 pivot;
	JPH::Vec3 axis;
	float limitMin;
	float limitMax;
	// Motor/Spring
	bool hasMotor;
	float target; // Angle or Position
	float frequency;
	float damping;
	float maxForce;
	bool disableCollisions;
};

// Protects struct Command from being non-trivial
static_assert((std::is_trivially_default_constructible_v<ConstraintParams> && std::is_trivially_copyable_v<ConstraintParams>));

enum class CommandType : uint8_t {
	DestroyBody,
	CreateConstraint,
	DestroyConstraint,
	SetConstraintTarget,
	SetCollisionFilter
};
#if defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnested-anon-types"
#endif
struct Command {
	CommandType type;
	union {
		ZHLN::Entity handle;	  // For DestroyBody
		ConstraintHandle cHandle; // For DestroyConstraint
		struct {				  // For CreateConstraint
			ConstraintType cType;
			ZHLN::Entity b1;
			ZHLN::Entity b2;
			ConstraintParams params;
		} createC;
		struct { // For SetConstraintTarget
			ConstraintHandle targetCHandle;
			float targetValue;
		} setTarget;
		struct { // For SetCollisionFilter
			ZHLN::Entity handle;
			uint32_t category;
			uint32_t mask;
		} setFilter;
	};
};
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static_assert((std::is_trivially_default_constructible_v<Command> && std::is_trivially_copyable_v<Command>)); // Must be trivial

enum SlotState : uint8_t {
	SLOT_EMPTY = 0,
	SLOT_ALIVE = 1,
	SLOT_CHARACTER = 2,
	SLOT_PENDING_DESTROY = 3,
};

enum class ContactType : uint8_t { Added = 0, Persisted = 1, Removed = 2 };

struct alignas(128) ContactEvent {
	// --- Block 1: Identity & Spatial (Offset 0-63) ---
	ZHLN::Entity body1;	  // 8
	ZHLN::Entity body2;	  // 8
	JPH::Real px, py, pz; // 12 or 24 (Double-ready)
	float nx, ny, nz;	  // 12
	float impulse;		  // 4
	ContactType type;	  // 4
	uint32_t flags;		  // 4 (e.g., Sensor bits)

	// --- Block 2: Advanced Dynamics & Metadata (Offset 64-127) ---
	float slidingSpeed;	 // 4
	float rvx, rvy, rvz; // 12 (Relative Velocity at contact point)
	uint32_t mat1, mat2; // 8  (Material IDs for sound/FX)
	uint32_t sub1, sub2; // 8  (Sub-shape IDs for bone-specific hits)

	// We LET the compiler do the padding.
};

static_assert(sizeof(ContactEvent) == 128,
			  "ContactEvent must be exactly 128 bytes for L1/L2 cache isolation!");

static_assert((std::is_trivially_default_constructible_v<ContactEvent> && std::is_trivially_copyable_v<ContactEvent>));

// Simple container for materials
struct MaterialData {
	uint32_t id;
	float friction;
	float restitution;
};

static_assert((std::is_trivially_default_constructible_v<MaterialData> && std::is_trivially_copyable_v<MaterialData>));

/**
 * @brief Thread-Safe, Cache-Isolated Structure of Arrays (SoA) Physics World.
 * No default initializers allowed to maintain Standard Layout / Triviality rules.
 */
std::pair<const ContactEvent*, size_t> GetContactEvents(const PhysicsContext& ctx);

struct PhysicsWorld {
	mutable BufferSync sync;
	// ========================================================================
	// BUCKET 1: JOLT CORE (Cold)
	// ========================================================================
	alignas(CACHE_LINE) JPH::PhysicsSystem* system;
	JPH::BodyInterface* bodyInterface;
	JPH::JobSystem* jobSystem;
	JPH::BroadPhaseLayerInterface* bpInterface;
	JPH::ObjectLayerPairFilter* pairFilter;
	JPH::ObjectVsBroadPhaseLayerFilter* bpFilter;
	JPH::ContactListener* contactListener;
	JPH::TempAllocator* tempAllocator;

	uint32_t maxJoltBodies;

	// ========================================================================
	// BUCKET 2: HOT SIMULATION STATE
	// ========================================================================
	alignas(CACHE_LINE) double time;
	ZHLN::Atomic<size_t> count;
	size_t capacity;
	size_t slotCapacity;
	ZHLN::Atomic<size_t> freeCount;

	JPH::Real* positions;
	JPH::Real* prevPositions;
	float* rotations;
	float* prevRotations;
	float* linearVelocities;
	float* angularVelocities;

	JPH::BodyID* bodyIDs;
	uint32_t* materialIDs;
	uint64_t* userData;

	// ========================================================================
	// BUCKET 3: SYNCHRONIZATION
	// ========================================================================
	alignas(CACHE_LINE) ZHLN::Atomic<bool> isStepping;

	Command* commandQueue;
	Command* commandQueueSpare;
	size_t commandCount;
	size_t commandCapacity;

	// ========================================================================
	// BUCKET 4: MAPPINGS & FILTERS (The ECS Engine)
	// ========================================================================
	alignas(CACHE_LINE) const void** joltBodyPtrs;

	ZHLN::Atomic<uint64_t>* idToHandleMap;
	uint32_t* slotToDense;
	uint32_t* denseToSlot;
	uint32_t* freeSlots;

	uint32_t* categories;
	uint32_t* masks;

	ZHLN::Atomic<uint8_t>* slotStates;
	ZHLN::Atomic<uint32_t>* generations;

	// ========================================================================
	// BUCKET 5: CONTACTS & EVENTS
	// ========================================================================
	alignas(CACHE_LINE) ContactEvent* contactBuffer;
	ZHLN::Atomic<size_t> contactCount;
	size_t contactCapacity;

	// ========================================================================
	// BUCKET 6: REGISTRIES
	// ========================================================================
	alignas(CACHE_LINE) MaterialData* materials;
	size_t materialCount;
	size_t materialCapacity;

	// ========================================================================
	// BUCKET 7: CONSTRAINTS (SoA)
	// ========================================================================
	alignas(CACHE_LINE) JPH::Constraint** constraints;
	ZHLN::Atomic<uint32_t>* constraintGenerations;
	uint8_t* constraintStates;
	uint32_t* freeConstraintSlots;
	size_t constraintCount;
	size_t constraintCapacity;
	size_t freeConstraintCount;

	// ========================================================================
	// METHODS
	// ========================================================================

	// Explicit lifecycle methods (keeps struct Trivial)
	void Init(uint32_t inMaxBodies, JPH::PhysicsSystem* inSystem, JPH::JobSystem* inJobSystem,
			  JPH::TempAllocator* inTempAlloc);
	void Shutdown();

	// Data Management
	void ResizeBuffers(size_t newCapacity);
	ZHLN::Entity AllocateHandle();
	void RemoveBodySlot(uint32_t slot);
	void ResizeConstraintBuffers(size_t newCapacity);

	JPH::BodyID GetBodyID(ZHLN::Entity handle);

	// Constraints
	ConstraintHandle AllocateConstraintHandle();
	void RemoveConstraintSlot(uint32_t slot);

	// Flush command buffer
	void FlushCommands(Command* capturedQueue, size_t capturedCount);

	/**
	 * @brief Synchronizes all Jolt state to the SoA World.
	 * Handles Rigid Bodies, Characters, and executes optimized SIMD batch copies.
	 *
	 * @param system The active Jolt PhysicsSystem.
	 * @param activeCharacters The array of active CharacterVirtuals.
	 */
	void Synchronize(const JPH::PhysicsSystem* system,
					 const JPH::Array<JPH::CharacterVirtual*>& activeCharacters) noexcept;

	JPH::Array<std::byte> SaveState() const;
	bool LoadState(const uint8_t* data, size_t size);
};

// Guarantee predictable layout for raw memory mapping and SIMD logic
static_assert(std::is_standard_layout_v<PhysicsWorld>);
static_assert(std::is_trivially_copyable_v<PhysicsWorld>);
static_assert((std::is_trivially_default_constructible_v<PhysicsWorld> && std::is_trivially_copyable_v<PhysicsWorld>));

// --- Slot Predication Logic ---

// Simplified masks for C++
static constexpr uint32_t MASK_ACTIVE = (1U << SLOT_ALIVE) | (1U << SLOT_CHARACTER);
static constexpr uint32_t MASK_DESTRUCTIBLE = (1U << SLOT_ALIVE) | (1U << SLOT_CHARACTER);

struct SlotPredicate {
	bool isActive;		 // Alive in Jolt right now and safe to query
	bool isDestructible; // Can be queued for destruction
};

[[nodiscard]] inline SlotPredicate GetSlotPredicate(uint8_t state) noexcept {
	const uint32_t stateBit = 1U << (state & 0x7);
	bool active = (stateBit & MASK_ACTIVE) != 0;
	bool destructible = (stateBit & MASK_DESTRUCTIBLE) != 0;
	return {.isActive = active, .isDestructible = destructible};
}

// --- Constraints ---
JPH::Constraint* CreateNativeConstraint(const ConstraintType type, JPH::Body* b1, JPH::Body* b2,
										const ConstraintParams& p);

} // namespace ZHLN::Physics
