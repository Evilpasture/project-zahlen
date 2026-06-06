#include "PhysicsWorld.hpp"

#include "detail/ControlFlow.hpp"

#include <cstring>
#include <new>
#include <type_traits>

namespace ZHLN::Physics {

// --- Internal Memory Utilities ---
template <typename T> [[nodiscard]] static T* AllocateAligned(size_t count, size_t alignment) {
	return static_cast<T*>(::operator new[](count * sizeof(T), std::align_val_t{alignment}));
}

template <typename T> static void DeallocateAligned(T* ptr, size_t alignment) {
	if (ptr) {
		::operator delete[](ptr, std::align_val_t{alignment});
	}
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
	static_assert(std::is_trivially_copyable_v<T>);
	T* new_ptr = new T[new_count]();
	if (ptr && old_count > 0) {
		std::memcpy(static_cast<void*>(new_ptr), static_cast<const void*>(ptr),
					old_count * sizeof(T));
		delete[] ptr;
	} else {
		delete[] ptr;
	}

	ptr = new_ptr;
}

// --- Implementation ---

void PhysicsWorld::Init(uint32_t inMaxBodies, JPH::PhysicsSystem* inSystem,
						JPH::JobSystem* inJobSystem, JPH::TempAllocator* inTempAlloc) {
	// Sanity check provides a concrete upper bound, silencing GCC's VRP warning
	if (inMaxBodies == 0 || inMaxBodies > 10000000) {
		ZHLN::Panic("PhysicsWorld::Init: inMaxBodies ({}) is out of bounds!", inMaxBodies);
	}
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

	// Constraints
	constraintCapacity = inMaxBodies;

	constraints = new JPH::Constraint*[constraintCapacity]();
	constraintStates = new uint8_t[constraintCapacity]();
	constraintGenerations = new ZHLN::Atomic<uint32_t>[constraintCapacity]();
	freeConstraintSlots = new uint32_t[constraintCapacity]();

	for (uint32_t i = 0; i < constraintCapacity; ++i) {
		constraintGenerations[i].store(1, std::memory_order_relaxed);
		constraintStates[i] = SLOT_EMPTY;
		freeConstraintSlots[i] = (constraintCapacity - 1) - i;
	}

	constraintCount = 0;
	freeConstraintCount = constraintCapacity;
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

	if (contactBuffer != nullptr) {
		::operator delete[](contactBuffer, std::align_val_t{sizeof(ContactEvent)});
	}

	delete[] materials;

	delete[] constraints;
	delete[] constraintStates;
	delete[] constraintGenerations;
	delete[] freeConstraintSlots;
}

void PhysicsWorld::ResizeBuffers(size_t newCapacity) {
	if (newCapacity <= capacity) {
		return;
	}

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

void PhysicsWorld::ResizeConstraintBuffers(size_t newCapacity) {
	if (newCapacity <= constraintCapacity) {
		return;
	}

	const size_t oldCap = constraintCapacity;
	constraintCapacity = newCapacity;

	ReallocateStandard(constraints, oldCap, newCapacity);
	ReallocateStandard(constraintStates, oldCap, newCapacity);
	ReallocateStandard(constraintGenerations, oldCap, newCapacity);
	ReallocateStandard(freeConstraintSlots, oldCap, newCapacity);

	// Initialize the newly expanded slots
	for (size_t i = oldCap; i < newCapacity; i++) {
		constraintGenerations[i].store(1, std::memory_order_relaxed);
		constraintStates[i] = SLOT_EMPTY;
		freeConstraintSlots[freeConstraintCount++] = static_cast<uint32_t>(i);
	}
}

ZHLN::Entity PhysicsWorld::AllocateHandle() {
	size_t available = freeCount.load(std::memory_order_acquire);
	if (available == 0) {
		ResizeBuffers(capacity * 2);
		available = freeCount.load(std::memory_order_acquire);
	}

	uint32_t slot = freeSlots[--available];
	freeCount.store(available, std::memory_order_release);

	uint32_t gen = generations[slot].load(std::memory_order_relaxed);
	return ZHLN::Entity{.index = slot, .generation = gen};
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

ConstraintHandle PhysicsWorld::AllocateConstraintHandle() {
	// If we are out of slots, double the capacity
	if (freeConstraintCount == 0) {
		ResizeConstraintBuffers(constraintCapacity * 2);
	}

	uint32_t slot = freeConstraintSlots[--freeConstraintCount];
	uint32_t gen = constraintGenerations[slot].load(std::memory_order_relaxed);
	return ConstraintHandle{.index = slot, .generation = gen};
}

void PhysicsWorld::RemoveConstraintSlot(uint32_t slot) {
	// Increment generation so old handles become invalid
	constraintGenerations[slot].fetch_add(1, std::memory_order_relaxed);
	constraintStates[slot] = SLOT_EMPTY;

	// Return slot to free list
	freeConstraintSlots[freeConstraintCount++] = slot;
}

JPH::Array<std::byte> PhysicsWorld::SaveState() const {
	size_t currentCount = count.load(std::memory_order_acquire);
	size_t slotCap = slotCapacity;

	// 1. Calculate Sizes
	size_t posSize = currentCount * sizeof(JPH::Real) * 4;
	size_t rotSize = currentCount * sizeof(float) * 4;
	size_t velSize = currentCount * sizeof(float) * 4; // linear + angular

	// Mappings: gen (u32), s2d (u32), d2s (u32), states (u8)
	size_t mappingSize = slotCap * (sizeof(uint32_t) * 3 + sizeof(uint8_t));

	size_t totalSize = sizeof(WorldStateHeader) + posSize + rotSize + (velSize * 2) + mappingSize;

	JPH::Array<std::byte> buffer(totalSize);
	auto* ptr = buffer.data();

	ZHLN_LOCK(sync.shadowLock) {

		// 2. Write Header
		WorldStateHeader header;
		header.bodyCount = (uint32_t)currentCount;
		header.slotCapacity = (uint32_t)slotCap;
		header.worldTime = time;
		std::memcpy(ptr, &header, sizeof(WorldStateHeader));
		ptr += sizeof(WorldStateHeader);

		// 3. Write SoA Hot Buffers (Positions, Rotations, Velocities)
		std::memcpy(ptr, positions, posSize);
		ptr += posSize;
		std::memcpy(ptr, rotations, rotSize);
		ptr += rotSize;
		std::memcpy(ptr, linearVelocities, velSize);
		ptr += velSize;
		std::memcpy(ptr, angularVelocities, velSize);
		ptr += velSize;

		// 4. Write Mapping Tables (Atomics must be loaded)
		for (size_t i = 0; i < slotCap; ++i) {
			uint32_t g = generations[i].load(std::memory_order_relaxed);
			std::memcpy(ptr, &g, sizeof(uint32_t));
			ptr += sizeof(uint32_t);
		}
		std::memcpy(ptr, slotToDense, slotCap * sizeof(uint32_t));
		ptr += (slotCap * sizeof(uint32_t));
		std::memcpy(ptr, denseToSlot, slotCap * sizeof(uint32_t));
		ptr += (slotCap * sizeof(uint32_t));

		for (size_t i = 0; i < slotCap; ++i) {
			auto s = static_cast<std::byte>(slotStates[i].load(std::memory_order_relaxed));
			*ptr = s;
			ptr += 1;
		}
	}
	return buffer;
}

bool PhysicsWorld::LoadState(const uint8_t* data, size_t size) {
	if (size < sizeof(WorldStateHeader)) {
		return false;
	}
	const auto* header = reinterpret_cast<const WorldStateHeader*>(data);
	if (header->magic != WorldStateHeader::ZHLN) {
		return false;
	}
	if (header->slotCapacity != slotCapacity) {
		return false;
	} // Safety: must match current allocation

	const uint8_t* ptr = data + sizeof(WorldStateHeader);
	size_t savedCount = header->bodyCount;

	ZHLN_LOCK(sync.shadowLock) {
		// 1. Restore SoA Buffers
		size_t posSize = savedCount * sizeof(JPH::Real) * 4;
		size_t rotSize = savedCount * sizeof(float) * 4;
		size_t velSize = savedCount * sizeof(float) * 4;

		std::memcpy(positions, ptr, posSize);
		ptr += posSize;
		std::memcpy(rotations, ptr, rotSize);
		ptr += rotSize;
		std::memcpy(linearVelocities, ptr, velSize);
		ptr += velSize;
		std::memcpy(angularVelocities, ptr, velSize);
		ptr += velSize;

		// 2. Restore Mapping Tables
		for (size_t i = 0; i < slotCapacity; ++i) {
			uint32_t g;
			std::memcpy(&g, ptr, sizeof(uint32_t));
			generations[i].store(g, std::memory_order_relaxed);
			ptr += sizeof(uint32_t);
		}
		std::memcpy(slotToDense, ptr, slotCapacity * sizeof(uint32_t));
		ptr += (slotCapacity * sizeof(uint32_t));
		std::memcpy(denseToSlot, ptr, slotCapacity * sizeof(uint32_t));
		ptr += (slotCapacity * sizeof(uint32_t));

		for (size_t i = 0; i < slotCapacity; ++i) {
			slotStates[i].store(*ptr, std::memory_order_relaxed);
			ptr += 1;
		}

		// 3. Reset Free List Logic
		size_t newFreeCount = 0;
		for (uint32_t i = 0; i < slotCapacity; ++i) {
			if (slotStates[i].load() == SLOT_EMPTY) {
				freeSlots[newFreeCount++] = i;
			}
		}
		freeCount.store(newFreeCount, std::memory_order_release);
		count.store(savedCount, std::memory_order_release);
		time = header->worldTime;
	}

	// 4. SYNC TO JOLT: Push SoA data back to the actual Jolt bodies
	// This part happens outside the shadow lock but while isStepping is ideally false
	for (size_t i = 0; i < savedCount; i++) {
		JPH::BodyID bid = bodyIDs[i];
		if (bid.IsInvalid()) {
			continue;
		}

		// Extract from SoA
		JPH::RVec3 p(positions[i * 4 + 0], positions[i * 4 + 1], positions[i * 4 + 2]);
		JPH::Quat q(rotations[i * 4 + 0], rotations[i * 4 + 1], rotations[i * 4 + 2],
					rotations[i * 4 + 3]);
		JPH::Vec3 lv(linearVelocities[i * 4 + 0], linearVelocities[i * 4 + 1],
					 linearVelocities[i * 4 + 2]);
		JPH::Vec3 av(angularVelocities[i * 4 + 0], angularVelocities[i * 4 + 1],
					 angularVelocities[i * 4 + 2]);

		bodyInterface->SetPositionAndRotation(bid, p, q, JPH::EActivation::Activate);
		bodyInterface->SetLinearVelocity(bid, lv);
		bodyInterface->SetAngularVelocity(bid, av);
	}

	return true;
}

} // namespace ZHLN::Physics
