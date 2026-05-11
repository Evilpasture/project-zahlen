#pragma once

#include "Physics.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <cstdint>
#include <detail/Atomic.hpp>
#include <detail/Platform.hpp>
#include <threading/Mutex.hpp>
#include <type_traits>

namespace ZHLN::Physics {

#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t CACHE_LINE = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t CACHE_LINE = 64;
#endif

enum class CommandType : uint8_t { DestroyBody };

struct Command {
	CommandType type;
	EntityHandle handle;
};

enum SlotState : uint8_t {
	SLOT_EMPTY = 0,
	SLOT_ALIVE = 1,
	SLOT_CHARACTER = 2,
	SLOT_PENDING_DESTROY = 3,
};

enum class ContactType : uint32_t { Added = 0, Persisted = 1, Removed = 2 };

struct alignas(128) ContactEvent {
	// --- Block 1: Identity & Spatial (Offset 0-63) ---
	EntityHandle body1;	  // 8
	EntityHandle body2;	  // 8
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

	// Remaining padding to hit exactly 128 bytes
	uint8_t _padding[128 - (16 + sizeof(JPH::Real) * 3 + 24 + 4 + 12 + 8 + 8)];
};

static_assert(sizeof(ContactEvent) == 128,
			  "ContactEvent must be exactly 128 bytes for L1/L2 cache isolation!");

// --- Queries ---
// Returns a read-only view of the collision events from the LAST frame.
std::pair<const ContactEvent*, size_t> GetContactEvents(const PhysicsContext& ctx);

/**
 * @brief Thread-Safe, Cache-Isolated Structure of Arrays (SoA) Physics World.
 * No default initializers allowed to maintain Standard Layout / Triviality rules.
 */
struct PhysicsWorld {
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

	// SIMD-Aligned SoA Buffers [Stride 4]
	JPH::Real* positions;
	JPH::Real* prevPositions;
	float* rotations;
	float* prevRotations;
	float* linearVelocities;
	float* angularVelocities;

	// Standard SoA Buffers
	JPH::BodyID* bodyIDs;
	uint32_t* materialIDs;
	uint64_t* userData;

	// ========================================================================
	// BUCKET 3: SYNCHRONIZATION
	// ========================================================================
	alignas(CACHE_LINE) mutable ZHLN::Mutex shadowLock;
	ZHLN::Atomic<bool> isStepping;
	mutable ZHLN::Atomic<int> viewExportCount;

	// --- Raw Double-Buffered Command Queue ---
	Physics::Command* commandQueue;
	Physics::Command* commandQueueSpare;
	size_t commandCount;
	size_t commandCapacity;

	// ========================================================================
	// BUCKET 4: MAPPINGS & FILTERS (The ECS Engine)
	// ========================================================================
	alignas(CACHE_LINE) const void** joltBodyPtrs;

	ZHLN::Atomic<uint64_t>* idToHandleMap; // Maps JPH::BodyID.GetIndex() -> EntityHandle
	uint32_t* slotToDense;
	uint32_t* denseToSlot;
	uint32_t* freeSlots;

	uint32_t* categories;
	uint32_t* masks;

	ZHLN::Atomic<uint8_t>* slotStates; // Uses SlotState enum
	ZHLN::Atomic<uint32_t>* generations;

	// ========================================================================
	// BUCKET 5: CONTACTS & EVENTS
	// ========================================================================
	alignas(CACHE_LINE) ContactEvent* contactBuffer;
	ZHLN::Atomic<size_t> contactCount;
	size_t contactCapacity;
};

// Guarantee predictable layout for raw memory mapping and SIMD logic
static_assert(std::is_standard_layout_v<PhysicsWorld>);
static_assert(std::is_trivially_copyable_v<PhysicsWorld>);
static_assert(std::is_trivial_v<PhysicsWorld>);

} // namespace ZHLN::Physics