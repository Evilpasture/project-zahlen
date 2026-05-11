#pragma once

#include "Mutex.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Zahlen/Physics.hpp>
#include <cstdint>
#include <detail/Atomic.hpp>
#include <detail/Platform.hpp>
#include <type_traits>

namespace ZHLN::Physics {

#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t CACHE_LINE = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t CACHE_LINE = 64;
#endif

enum SlotState : uint8_t { SLOT_EMPTY = 0, SLOT_ALIVE = 1, SLOT_CHARACTER = 2 };

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
};

// Guarantee predictable layout for raw memory mapping and SIMD logic
static_assert(std::is_standard_layout_v<PhysicsWorld>);
static_assert(std::is_trivially_copyable_v<PhysicsWorld>);
static_assert(std::is_trivial_v<PhysicsWorld>);

} // namespace ZHLN::Physics