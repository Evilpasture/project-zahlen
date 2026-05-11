#include "PhysicsWorld.hpp"

#include <cstring>
#include <new>

namespace ZHLN::Physics {

// --- Internal Memory Utilities ---
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

// --- Implementation ---

void PhysicsWorld::Init(uint32_t inMaxBodies, JPH::PhysicsSystem* inSystem,
						JPH::JobSystem* inJobSystem, JPH::TempAllocator* inTempAlloc) {
	std::memset(this, 0, sizeof(PhysicsWorld));

	system = inSystem;
	bodyInterface = &inSystem->GetBodyInterface();
	jobSystem = inJobSystem;
	tempAllocator = inTempAlloc;
	maxJoltBodies = inMaxBodies;

	capacity = inMaxBodies;
	slotCapacity = inMaxBodies;

	positions = AllocateAligned<JPH::Real>(capacity * 4, sizeof(JPH::Real) * 4);
	prevPositions = AllocateAligned<JPH::Real>(capacity * 4, sizeof(JPH::Real) * 4);
	rotations = AllocateAligned<float>(capacity * 4, sizeof(float) * 4);
	prevRotations = AllocateAligned<float>(capacity * 4, sizeof(float) * 4);
	linearVelocities = AllocateAligned<float>(capacity * 4, sizeof(float) * 4);
	angularVelocities = AllocateAligned<float>(capacity * 4, sizeof(float) * 4);

	bodyIDs = new JPH::BodyID[capacity]();
	materialIDs = new uint32_t[capacity]();
	userData = new uint64_t[capacity]();

	slotToDense = new uint32_t[capacity]();
	denseToSlot = new uint32_t[capacity]();
	freeSlots = new uint32_t[capacity]();

	categories = new uint32_t[capacity]();
	masks = new uint32_t[capacity]();

	slotStates = new ZHLN::Atomic<uint8_t>[capacity]();
	generations = new ZHLN::Atomic<uint32_t>[capacity]();

	idToHandleMap = new ZHLN::Atomic<uint64_t>[capacity + 1]();
	joltBodyPtrs = new const void*[capacity + 1]();

	for (uint32_t i = 0; i < capacity; ++i) {
		generations[i].store(1, std::memory_order_relaxed);
		slotStates[i].store(SLOT_EMPTY, std::memory_order_relaxed);
		freeSlots[i] = (capacity - 1) - i;
	}

	count.store(0, std::memory_order_relaxed);
	freeCount.store(capacity, std::memory_order_relaxed);

	commandCapacity = 64;
	commandCount = 0;
	commandQueue = new Command[commandCapacity]();
	commandQueueSpare = new Command[commandCapacity]();

	contactCapacity = 4096;
	contactBuffer = static_cast<ContactEvent*>(::operator new[](
		contactCapacity * sizeof(ContactEvent), std::align_val_t{sizeof(ContactEvent)}));

	// Initialize Material Registry
	materialCapacity = 16;
	materialCount = 0;
	materials = new MaterialData[materialCapacity]();
	constraintGenerations = new ZHLN::Atomic<uint32_t>[constraintCapacity]();
	for (uint32_t i = 0; i < constraintCapacity; ++i) {
		constraintGenerations[i].store(1, std::memory_order_relaxed);
		freeConstraintSlots[i] = (constraintCapacity - 1) - i;
	}
}

void PhysicsWorld::Shutdown() {
	DeallocateAligned(positions, sizeof(JPH::Real) * 4);
	DeallocateAligned(prevPositions, sizeof(JPH::Real) * 4);
	DeallocateAligned(rotations, sizeof(float) * 4);
	DeallocateAligned(prevRotations, sizeof(float) * 4);
	DeallocateAligned(linearVelocities, sizeof(float) * 4);
	DeallocateAligned(angularVelocities, sizeof(float) * 4);

	delete[] bodyIDs;
	delete[] materialIDs;
	delete[] userData;
	delete[] slotToDense;
	delete[] denseToSlot;
	delete[] freeSlots;
	delete[] categories;
	delete[] masks;

	delete[] slotStates;
	delete[] generations;
	delete[] idToHandleMap;
	delete[] joltBodyPtrs;

	delete[] commandQueue;
	delete[] commandQueueSpare;

	if (contactBuffer) {
		::operator delete[](contactBuffer, std::align_val_t{sizeof(ContactEvent)});
	}

	delete[] materials;
	delete[] constraintGenerations;
}

void PhysicsWorld::ResizeBuffers(size_t newCapacity) {
	if (newCapacity <= capacity)
		return;

	const size_t oldCap = capacity;
	capacity = newCapacity;
	slotCapacity = newCapacity;

	ReallocateAligned(positions, oldCap * 4, newCapacity * 4, sizeof(JPH::Real) * 4);
	ReallocateAligned(prevPositions, oldCap * 4, newCapacity * 4, sizeof(JPH::Real) * 4);
	ReallocateAligned(rotations, oldCap * 4, newCapacity * 4, sizeof(float) * 4);
	ReallocateAligned(prevRotations, oldCap * 4, newCapacity * 4, sizeof(float) * 4);
	ReallocateAligned(linearVelocities, oldCap * 4, newCapacity * 4, sizeof(float) * 4);
	ReallocateAligned(angularVelocities, oldCap * 4, newCapacity * 4, sizeof(float) * 4);

	ReallocateStandard(bodyIDs, oldCap, newCapacity);
	ReallocateStandard(materialIDs, oldCap, newCapacity);
	ReallocateStandard(userData, oldCap, newCapacity);
	ReallocateStandard(slotToDense, oldCap, newCapacity);
	ReallocateStandard(denseToSlot, oldCap, newCapacity);
	ReallocateStandard(freeSlots, oldCap, newCapacity);
	ReallocateStandard(categories, oldCap, newCapacity);
	ReallocateStandard(masks, oldCap, newCapacity);
	ReallocateStandard(generations, oldCap, newCapacity);
	ReallocateStandard(slotStates, oldCap, newCapacity);

	size_t freeIdx = freeCount.load(std::memory_order_relaxed);
	for (size_t i = oldCap; i < newCapacity; i++) {
		generations[i].store(1, std::memory_order_relaxed);
		slotStates[i].store(SLOT_EMPTY, std::memory_order_relaxed);
		freeSlots[freeIdx++] = static_cast<uint32_t>(i);
	}
	freeCount.store(freeIdx, std::memory_order_release);
}

EntityHandle PhysicsWorld::AllocateHandle() {
	size_t available = freeCount.load(std::memory_order_acquire);
	if (available == 0) {
		ResizeBuffers(capacity * 2);
		available = freeCount.load(std::memory_order_acquire);
	}

	uint32_t slot = freeSlots[--available];
	freeCount.store(available, std::memory_order_release);

	uint32_t gen = generations[slot].load(std::memory_order_relaxed);
	return EntityHandle{.index = slot, .generation = gen};
}

void PhysicsWorld::RemoveBodySlot(uint32_t slot) {
	const uint32_t denseIdx = slotToDense[slot];
	const uint32_t lastDense = static_cast<uint32_t>(count.load(std::memory_order_acquire)) - 1;

	if (denseIdx != lastDense) {
		for (int i = 0; i < 4; ++i) {
			positions[denseIdx * 4 + i] = positions[lastDense * 4 + i];
			prevPositions[denseIdx * 4 + i] = prevPositions[lastDense * 4 + i];
			rotations[denseIdx * 4 + i] = rotations[lastDense * 4 + i];
			prevRotations[denseIdx * 4 + i] = prevRotations[lastDense * 4 + i];
			linearVelocities[denseIdx * 4 + i] = linearVelocities[lastDense * 4 + i];
			angularVelocities[denseIdx * 4 + i] = angularVelocities[lastDense * 4 + i];
		}

		bodyIDs[denseIdx] = bodyIDs[lastDense];
		userData[denseIdx] = userData[lastDense];
		categories[denseIdx] = categories[lastDense];
		masks[denseIdx] = masks[lastDense];
		materialIDs[denseIdx] = materialIDs[lastDense];

		const uint32_t moverSlot = denseToSlot[lastDense];
		slotToDense[moverSlot] = denseIdx;
		denseToSlot[denseIdx] = moverSlot;
	}

	generations[slot].fetch_add(1, std::memory_order_relaxed);
	slotStates[slot].store(SLOT_EMPTY, std::memory_order_release);

	size_t fIdx = freeCount.fetch_add(1, std::memory_order_relaxed);
	freeSlots[fIdx] = slot;
	count.fetch_sub(1, std::memory_order_release);
}

} // namespace ZHLN::Physics