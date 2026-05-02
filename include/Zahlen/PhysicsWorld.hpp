#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Zahlen/Mutex.hpp>
#include <Zahlen/Physics.hpp>
#include <Zahlen/detail/Platform.hpp>
#include <atomic>
#include <cstdint>
#include <type_traits>

namespace ZHLN::Physics {

#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t CACHE_LINE = std::hardware_destructive_interference_size;
#else
inline constexpr std::size_t CACHE_LINE = 64;
#endif

/**
 * @brief Thread-Safe, Cache-Isolated Structure of Arrays (SoA) Physics World.
 * No default initializers allowed to maintain Standard Layout / Triviality rules.
 */
struct PhysicsWorld {
	// ========================================================================
	// BUCKET 1: READ-ONLY / COLD DATA
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
	size_t contactMaxCapacity;

	// ========================================================================
	// BUCKET 2: HOT SIMULATION STATE (The Stepper's Workspace)
	// ========================================================================
	alignas(CACHE_LINE) double time;
	std::atomic<size_t> count;
	size_t capacity;
	size_t slotCapacity;
	std::atomic<size_t> freeCount;

	// SoA Buffers (Shadow State Arrays)
	JPH::Real* positions; // [x, y, z] layout or SIMD padded
	JPH::Real* prevPositions;
	float* rotations; // Quaternions [x, y, z, w]
	float* prevRotations;
	float* linearVelocities;
	float* angularVelocities;
	JPH::BodyID* bodyIDs;
	uint32_t* materialIDs;

	// ========================================================================
	// BUCKET 3: COMMAND QUEUE & MUTEXES (The War Zone)
	// ========================================================================
	alignas(CACHE_LINE) ZHLN::Mutex shadowLock;

	void* commandQueue;
	void* commandQueueSpare;
	size_t commandCount;
	size_t commandCapacity;
	size_t spareCapacity;

	// ========================================================================
	// BUCKET 4: VOLATILE ATOMIC FLAGS (Polling Targets)
	// ========================================================================
	alignas(CACHE_LINE) std::atomic<bool> isStepping;
	std::atomic<bool> stepRequested;
	std::atomic<int> waitingThreads;

	// ========================================================================
	// BUCKET 5: QUERIES & COUNTERS
	// ========================================================================
	alignas(CACHE_LINE) std::atomic<int> activeQueries;
	std::atomic<int> viewExportCount;
	bool needsOptimization;

	// ========================================================================
	// BUCKET 6: MAPPINGS & FILTERS (ECS Indirection Arrays)
	// ========================================================================
	alignas(CACHE_LINE) const void** joltBodyPtrs;

	EntityHandle* idToHandleMap;
	uint32_t* slotToDense;
	uint32_t* denseToSlot;
	uint32_t* freeSlots;

	uint32_t* categories;
	uint32_t* masks;

	std::atomic<uint8_t>* slotStates;
	std::atomic<uint32_t>* generations;
};

// ----------------------------------------------------------------------------
// Compile-Time Guarantees
// ----------------------------------------------------------------------------

// Guarantee predictable layout for offsetof() and raw memory mapping
static_assert(std::is_standard_layout_v<PhysicsWorld>,
			  "[CRITICAL] PhysicsWorld must maintain Standard Layout!");

} // namespace ZHLN::Physics