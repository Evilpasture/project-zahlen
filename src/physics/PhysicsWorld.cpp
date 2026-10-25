// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsWorld.hpp"
#include "Zahlen/Log.hpp"
#include <Zahlen/Threading/Mutex.hpp>
#include <cstring>
#include <new>
#include <utility>

namespace ZHLN::Physics {

namespace {

template <typename T>
[[nodiscard]] auto AllocateAligned(size_t elementCount, size_t alignment) -> T* {
    return static_cast<T*>(::operator new[](elementCount * sizeof(T), std::align_val_t {alignment}));
}

template <typename T>
void DeallocateAligned(T* ptr, size_t alignment) noexcept {
    if (ptr != nullptr) {
        ::operator delete[](ptr, std::align_val_t {alignment});
    }
}

void AllocateTransformFrame(TransformFrame& frame, size_t cap) {
    frame.count             = 0;
    frame.positions         = AllocateAligned<JPH::Real>(cap * 4, sizeof(JPH::Real) * 4);
    frame.prevPositions     = AllocateAligned<JPH::Real>(cap * 4, sizeof(JPH::Real) * 4);
    frame.rotations         = AllocateAligned<float>(cap * 4, sizeof(float) * 4);
    frame.prevRotations     = AllocateAligned<float>(cap * 4, sizeof(float) * 4);
    frame.linearVelocities  = AllocateAligned<float>(cap * 4, sizeof(float) * 4);
    frame.angularVelocities = AllocateAligned<float>(cap * 4, sizeof(float) * 4);
    frame.characterFlags    = AllocateAligned<uint8_t>(cap, CACHE_LINE);
    frame.slotToDense       = AllocateAligned<uint32_t>(cap, CACHE_LINE);
    frame.denseToSlot       = AllocateAligned<uint32_t>(cap, CACHE_LINE);
    frame.slotGenerations   = AllocateAligned<uint32_t>(cap, CACHE_LINE);
    frame.slotStates        = AllocateAligned<uint8_t>(cap, CACHE_LINE);

    std::memset(frame.positions, 0, cap * 4 * sizeof(JPH::Real));
    std::memset(frame.prevPositions, 0, cap * 4 * sizeof(JPH::Real));
    std::memset(frame.rotations, 0, cap * 4 * sizeof(float));
    std::memset(frame.prevRotations, 0, cap * 4 * sizeof(float));
    std::memset(frame.linearVelocities, 0, cap * 4 * sizeof(float));
    std::memset(frame.angularVelocities, 0, cap * 4 * sizeof(float));
    std::memset(frame.characterFlags, 0, cap * sizeof(uint8_t));
    std::memset(frame.slotToDense, 0, cap * sizeof(uint32_t));
    std::memset(frame.denseToSlot, 0, cap * sizeof(uint32_t));
    std::memset(frame.slotGenerations, 0, cap * sizeof(uint32_t));
    std::memset(frame.slotStates, 0, cap * sizeof(uint8_t));
}

void DeallocateTransformFrame(TransformFrame& frame) noexcept {
    frame.count = 0;
    DeallocateAligned(std::exchange(frame.positions, nullptr), sizeof(JPH::Real) * 4);
    DeallocateAligned(std::exchange(frame.prevPositions, nullptr), sizeof(JPH::Real) * 4);
    DeallocateAligned(std::exchange(frame.rotations, nullptr), sizeof(float) * 4);
    DeallocateAligned(std::exchange(frame.prevRotations, nullptr), sizeof(float) * 4);
    DeallocateAligned(std::exchange(frame.linearVelocities, nullptr), sizeof(float) * 4);
    DeallocateAligned(std::exchange(frame.angularVelocities, nullptr), sizeof(float) * 4);
    DeallocateAligned(std::exchange(frame.characterFlags, nullptr), CACHE_LINE);
    DeallocateAligned(std::exchange(frame.slotToDense, nullptr), CACHE_LINE);
    DeallocateAligned(std::exchange(frame.denseToSlot, nullptr), CACHE_LINE);
    DeallocateAligned(std::exchange(frame.slotGenerations, nullptr), CACHE_LINE);
    DeallocateAligned(std::exchange(frame.slotStates, nullptr), CACHE_LINE);
}

} // namespace

// ============================================================================
// TransformFrame & TransformBuffer
// ============================================================================

void TransformFrame::WriteInitialPose(uint32_t dense, JPH::RVec3Arg pos, JPH::QuatArg rot, uint8_t charFlags) noexcept {
    const size_t base = static_cast<size_t>(dense) * 4;

    positions[base + 0] = pos.GetX();
    positions[base + 1] = pos.GetY();
    positions[base + 2] = pos.GetZ();
    positions[base + 3] = static_cast<JPH::Real>(0);

    prevPositions[base + 0] = pos.GetX();
    prevPositions[base + 1] = pos.GetY();
    prevPositions[base + 2] = pos.GetZ();
    prevPositions[base + 3] = static_cast<JPH::Real>(0);

    rotations[base + 0] = rot.GetX();
    rotations[base + 1] = rot.GetY();
    rotations[base + 2] = rot.GetZ();
    rotations[base + 3] = rot.GetW();

    prevRotations[base + 0] = rot.GetX();
    prevRotations[base + 1] = rot.GetY();
    prevRotations[base + 2] = rot.GetZ();
    prevRotations[base + 3] = rot.GetW();

    linearVelocities[base + 0] = 0.0f;
    linearVelocities[base + 1] = 0.0f;
    linearVelocities[base + 2] = 0.0f;
    linearVelocities[base + 3] = 0.0f;

    angularVelocities[base + 0] = 0.0f;
    angularVelocities[base + 1] = 0.0f;
    angularVelocities[base + 2] = 0.0f;
    angularVelocities[base + 3] = 0.0f;

    characterFlags[dense] = charFlags;
}

void TransformFrame::WriteTeleportPose(uint32_t dense, JPH::RVec3Arg pos, JPH::QuatArg rot) noexcept {
    const size_t base = static_cast<size_t>(dense) * 4;

    positions[base + 0]     = pos.GetX();
    positions[base + 1]     = pos.GetY();
    positions[base + 2]     = pos.GetZ();
    positions[base + 3]     = static_cast<JPH::Real>(0);
    prevPositions[base + 0] = pos.GetX();
    prevPositions[base + 1] = pos.GetY();
    prevPositions[base + 2] = pos.GetZ();
    prevPositions[base + 3] = static_cast<JPH::Real>(0);

    rotations[base + 0]     = rot.GetX();
    rotations[base + 1]     = rot.GetY();
    rotations[base + 2]     = rot.GetZ();
    rotations[base + 3]     = rot.GetW();
    prevRotations[base + 0] = rot.GetX();
    prevRotations[base + 1] = rot.GetY();
    prevRotations[base + 2] = rot.GetZ();
    prevRotations[base + 3] = rot.GetW();
}

void TransformFrame::WriteTeleportPosition(uint32_t dense, JPH::RVec3Arg pos) noexcept {
    const size_t base       = static_cast<size_t>(dense) * 4;
    positions[base + 0]     = pos.GetX();
    positions[base + 1]     = pos.GetY();
    positions[base + 2]     = pos.GetZ();
    positions[base + 3]     = static_cast<JPH::Real>(0);
    prevPositions[base + 0] = pos.GetX();
    prevPositions[base + 1] = pos.GetY();
    prevPositions[base + 2] = pos.GetZ();
    prevPositions[base + 3] = static_cast<JPH::Real>(0);
}

void TransformFrame::SwapEraseDense(uint32_t dstDense, uint32_t srcDense) noexcept {
    if (dstDense == srcDense) {
        return;
    }
    const size_t dst = static_cast<size_t>(dstDense) * 4;
    const size_t src = static_cast<size_t>(srcDense) * 4;
    for (size_t i = 0; i < 4; ++i) {
        positions[dst + i]         = positions[src + i];
        prevPositions[dst + i]     = prevPositions[src + i];
        rotations[dst + i]         = rotations[src + i];
        prevRotations[dst + i]     = prevRotations[src + i];
        linearVelocities[dst + i]  = linearVelocities[src + i];
        angularVelocities[dst + i] = angularVelocities[src + i];
    }
    characterFlags[dstDense] = characterFlags[srcDense];
}

void TransformBuffer::Init(uint32_t maxBodies) {
    capacity = maxBodies;
    frontIndex.store(0, std::memory_order::relaxed);
    AllocateTransformFrame(frames[0], capacity);
    AllocateTransformFrame(frames[1], capacity);
}

void TransformBuffer::Shutdown() noexcept {
    DeallocateTransformFrame(frames[0]);
    DeallocateTransformFrame(frames[1]);
    capacity = 0;
    frontIndex.store(0, std::memory_order::relaxed);
}

void TransformBuffer::BeginWriteFromFront() noexcept {
    const uint32_t        fIdx  = frontIndex.load(std::memory_order::acquire);
    const TransformFrame& front = frames[fIdx];
    TransformFrame&       back  = frames[1u - fIdx];

    const size_t liveCount = front.count;
    back.count             = liveCount;
    if (liveCount == 0) {
        return;
    }

    const size_t posBytes = liveCount * 4 * sizeof(JPH::Real);
    const size_t auxBytes = liveCount * 4 * sizeof(float);

    std::memcpy(back.positions, front.positions, posBytes);
    std::memcpy(back.prevPositions, front.positions, posBytes);
    std::memcpy(back.rotations, front.rotations, auxBytes);
    std::memcpy(back.prevRotations, front.rotations, auxBytes);
    std::memcpy(back.linearVelocities, front.linearVelocities, auxBytes);
    std::memcpy(back.angularVelocities, front.angularVelocities, auxBytes);
    std::memcpy(back.characterFlags, front.characterFlags, liveCount * sizeof(uint8_t));
}

void TransformBuffer::PublishTableMetadataToBack(const BodyTable& table) noexcept {
    TransformFrame& back      = GetWriteBuffer();
    const size_t    slotCap   = table.slotCapacity;
    const size_t    liveCount = table.count.load(std::memory_order::acquire);

    back.count = liveCount;
    if (slotCap == 0) {
        return;
    }

    std::memcpy(back.slotToDense, table.slotToDense.data(), slotCap * sizeof(uint32_t));
    std::memcpy(back.denseToSlot, table.denseToSlot.data(), slotCap * sizeof(uint32_t));

    for (size_t i = 0; i < slotCap; ++i) {
        back.slotGenerations[i] = table.generations[i].load(std::memory_order::relaxed);
        back.slotStates[i]      = table.slotStates[i].load(std::memory_order::relaxed);
    }
}

// ============================================================================
// BodyTable (Fixed-Capacity — Never Reallocates)
// ============================================================================

void BodyTable::Init(uint32_t maxBodies) {
    slotCapacity = maxBodies;

    generations.resize(slotCapacity);
    slotStates.resize(slotCapacity);
    slotMotionTypes.resize(slotCapacity);
    slotBodyIDs.resize(slotCapacity);
    slotMaterialIDs.resize(slotCapacity);

    idToHandleMap.resize(slotCapacity + 1);
    joltBodyPtrs.resize(slotCapacity + 1, nullptr);

    slotToDense.resize(slotCapacity, 0u);
    denseToSlot.resize(slotCapacity, 0u);
    freeSlots.resize(slotCapacity, 0u);

    bodyIDs.resize(slotCapacity);
    materialIDs.resize(slotCapacity, 0u);
    userData.resize(slotCapacity, 0u);
    categories.resize(slotCapacity, 0xFFFFFFFFu);
    masks.resize(slotCapacity, 0xFFFFFFFFu);
    spawnPositions.resize(slotCapacity, JPH::RVec3::sZero());
    spawnRotations.resize(slotCapacity, JPH::Quat::sIdentity());

    for (size_t i = 0; i < idToHandleMap.size(); ++i) {
        idToHandleMap[i].store(0u, std::memory_order::relaxed);
        joltBodyPtrs[i] = nullptr;
    }

    for (uint32_t i = 0; i < maxBodies; ++i) {
        generations[i].store(1u, std::memory_order::relaxed);
        StoreSlotState(i, SlotState::Empty);
        slotMotionTypes[i].store(static_cast<uint8_t>(JPH::EMotionType::Static), std::memory_order::relaxed);
        slotBodyIDs[i].store(JPH::BodyID::cInvalidBodyID, std::memory_order::relaxed);
        slotMaterialIDs[i].store(0u, std::memory_order::relaxed);
        freeSlots[i] = (maxBodies - 1u) - i;
    }

    count.store(0u, std::memory_order::relaxed);
    freeCount.store(slotCapacity, std::memory_order::relaxed);
}

void BodyTable::Shutdown() noexcept {
    generations.clear();
    slotStates.clear();
    slotMotionTypes.clear();
    slotBodyIDs.clear();
    slotMaterialIDs.clear();
    idToHandleMap.clear();
    joltBodyPtrs.clear();
    slotToDense.clear();
    denseToSlot.clear();
    freeSlots.clear();
    bodyIDs.clear();
    materialIDs.clear();
    userData.clear();
    categories.clear();
    masks.clear();
    spawnPositions.clear();
    spawnRotations.clear();
    count.store(0u, std::memory_order::relaxed);
    freeCount.store(0u, std::memory_order::relaxed);
    slotCapacity = 0;
}

auto BodyTable::ReserveSlot(SlotState pendingState, JPH::EMotionType motion, JPH::RVec3Arg spawnPos, JPH::QuatArg spawnRot) -> BodyHandle {
    ZHLN::MutexGuard guard(allocMutex);
    size_t           available = freeCount.load(std::memory_order::relaxed);
    if (available == 0) {
        ZHLN::Panic("BodyTable: Exhausted fixed body capacity ({})! Increase PhysicsConfig::maxBodies.", slotCapacity);
    }

    const uint32_t slot = freeSlots[--available];
    freeCount.store(available, std::memory_order::release);

    const uint32_t gen  = generations[slot].load(std::memory_order::acquire);
    spawnPositions[slot] = spawnPos;
    spawnRotations[slot] = spawnRot;
    slotMotionTypes[slot].store(static_cast<uint8_t>(motion), std::memory_order::relaxed);
    slotBodyIDs[slot].store(JPH::BodyID::cInvalidBodyID, std::memory_order::relaxed);
    slotMaterialIDs[slot].store(0u, std::memory_order::relaxed);
    StoreSlotState(slot, pendingState);

    return BodyHandle {.index = slot, .generation = gen};
}

auto BodyTable::TryMarkPendingDestroy(BodyHandle handle) noexcept -> bool {
    const uint32_t slot = handle.index;
    if (slot >= slotCapacity || generations[slot].load(std::memory_order::acquire) != handle.generation) {
        return false;
    }

    uint8_t expected = slotStates[slot].load(std::memory_order::acquire);
    while (GetSlotPredicate(static_cast<SlotState>(expected)).isDestructible) {
        if (slotStates[slot].compare_exchange_weak(
                expected,
                static_cast<uint8_t>(SlotState::PendingDestroy),
                std::memory_order::acq_rel,
                std::memory_order::acquire
            )) {
            return true;
        }
    }
    return false;
}

auto BodyTable::IsSlotActive(BodyHandle handle) const noexcept -> bool {
    return handle.index < slotCapacity && generations[handle.index].load(std::memory_order::acquire) == handle.generation &&
           GetSlotPredicate(LoadSlotState(handle.index)).isActive;
}

void BodyTable::RemoveDenseAndReleaseSlot(uint32_t slot, TransformFrame& backFrame) noexcept {
    const uint32_t denseIdx  = slotToDense[slot];
    const uint32_t lastDense = static_cast<uint32_t>(count.load(std::memory_order::acquire)) - 1u;

    if (denseIdx != lastDense) {
        backFrame.SwapEraseDense(denseIdx, lastDense);

        bodyIDs[denseIdx]     = bodyIDs[lastDense];
        userData[denseIdx]    = userData[lastDense];
        categories[denseIdx]  = categories[lastDense];
        masks[denseIdx]       = masks[lastDense];
        materialIDs[denseIdx] = materialIDs[lastDense];

        const uint32_t moverSlot = denseToSlot[lastDense];
        slotToDense[moverSlot]   = denseIdx;
        denseToSlot[denseIdx]    = moverSlot;
    }

    slotBodyIDs[slot].store(JPH::BodyID::cInvalidBodyID, std::memory_order::release);
    StoreSlotState(slot, SlotState::Empty);
    generations[slot].fetch_add(1u, std::memory_order::acq_rel);
    count.fetch_sub(1u, std::memory_order::release);

    ZHLN::MutexGuard guard(allocMutex);
    const size_t     fIdx = freeCount.load(std::memory_order::relaxed);
    freeSlots[fIdx]       = slot;
    freeCount.store(fIdx + 1u, std::memory_order::release);
}

// ============================================================================
// PhysicsWorld
// ============================================================================

void PhysicsWorld::Init(uint32_t inMaxBodies, JPH::PhysicsSystem* inSystem, JPH::JobSystem* inJobSystem, JPH::TempAllocator* inTempAlloc) {
    ZHLN::Assert(inMaxBodies > 0 && inMaxBodies < 10000000, "PhysicsWorld::Init: inMaxBodies ({}) is out of bounds!", inMaxBodies);

    system        = inSystem;
    bodyInterface = &inSystem->GetBodyInterface();
    jobSystem     = inJobSystem;
    tempAllocator = inTempAlloc;
    maxJoltBodies = inMaxBodies;
    time          = 0.0;
    isStepping.store(false, std::memory_order::relaxed);

    bodyTable.Init(inMaxBodies);
    transforms.Init(inMaxBodies);
    transforms.PublishTableMetadataToBack(bodyTable);
    transforms.Swap();
    transforms.PublishTableMetadataToBack(bodyTable);

    mailbox.Init(256);

    contactCapacity = 4096;
    contactBuffer.resize(contactCapacity);
    contactCount.store(0u, std::memory_order::relaxed);

    materialCapacity = 16;
    materialCount    = 0;
    materials.resize(materialCapacity);

    constraintCapacity = inMaxBodies;
    constraints.resize(constraintCapacity, nullptr);
    constraintStates.resize(constraintCapacity, SlotState::Empty);
    constraintGenerations.resize(constraintCapacity);
    freeConstraintSlots.resize(constraintCapacity);

    for (uint32_t i = 0; i < constraintCapacity; ++i) {
        constraintGenerations[i].store(1u, std::memory_order::relaxed);
        freeConstraintSlots[i] = (inMaxBodies - 1u) - i;
    }
    freeConstraintCount = constraintCapacity;
}

void PhysicsWorld::Shutdown() {
    mailbox.Shutdown();
    transforms.Shutdown();
    bodyTable.Shutdown();

    contactBuffer.clear();
    materials.clear();
    materialCount    = 0;
    materialCapacity = 0;

    constraints.clear();
    constraintStates.clear();
    constraintGenerations.clear();
    freeConstraintSlots.clear();
    constraintCapacity  = 0;
    freeConstraintCount = 0;
}

void PhysicsWorld::RegisterMaterial(uint32_t id, float friction, float restitution) {
    ZHLN::MutexGuard guard(materialMutex);
    for (size_t i = 0; i < materialCount; ++i) {
        if (materials[i].id == id) {
            materials[i].friction    = friction;
            materials[i].restitution = restitution;
            return;
        }
    }
    if (materialCount >= materials.size()) {
        const size_t newCap = materials.empty() ? 16u : materials.size() * 2u;
        materials.resize(newCap);
        materialCapacity = newCap;
    }
    materials[materialCount++] = MaterialData {.id = id, .friction = friction, .restitution = restitution};
}

auto PhysicsWorld::ResolveMaterial(uint32_t id) const -> MaterialData {
    constexpr MaterialData materialDefault {.id = 0u, .friction = 0.2f, .restitution = 0.0f};
    if (id == 0u) {
        return materialDefault;
    }
    ZHLN::MutexGuard guard(materialMutex);
    for (size_t i = 0; i < materialCount; ++i) {
        if (materials[i].id == id) {
            return materials[i];
        }
    }
    return materialDefault;
}

auto PhysicsWorld::AllocateConstraintHandle() -> ConstraintHandle {
    ZHLN::MutexGuard guard(constraintMutex);
    if (freeConstraintCount == 0) {
        ZHLN::Panic("PhysicsWorld: Exhausted fixed constraint capacity ({})!", constraintCapacity);
    }

    const uint32_t slot = freeConstraintSlots[--freeConstraintCount];
    const uint32_t gen  = constraintGenerations[slot].load(std::memory_order::relaxed);
    return ConstraintHandle {.index = slot, .generation = gen};
}

void PhysicsWorld::RemoveConstraintSlot(uint32_t slot) noexcept {
    ZHLN::MutexGuard guard(constraintMutex);
    constraintGenerations[slot].fetch_add(1u, std::memory_order::relaxed);
    constraintStates[slot]                     = SlotState::Empty;
    freeConstraintSlots[freeConstraintCount++] = slot;
}

auto PhysicsWorld::SaveState() const -> JPH::Array<std::byte> {
    const TransformFrame& snap         = transforms.GetReadSnapshot();
    const size_t          currentCount = snap.count;
    const size_t          slotCap      = bodyTable.slotCapacity;

    const size_t posSize     = currentCount * sizeof(JPH::Real) * 4;
    const size_t rotSize     = currentCount * sizeof(float) * 4;
    const size_t velSize     = currentCount * sizeof(float) * 4;
    const size_t mappingSize = slotCap * (sizeof(uint32_t) * 3 + sizeof(uint8_t));
    const size_t totalSize   = sizeof(WorldStateHeader) + posSize + rotSize + (velSize * 2) + mappingSize;

    JPH::Array<std::byte> buffer;
    buffer.resize(totalSize);
    auto* ptr = buffer.data();

    WorldStateHeader header;
    header.bodyCount    = static_cast<uint32_t>(currentCount);
    header.slotCapacity = static_cast<uint32_t>(slotCap);
    header.worldTime    = time;
    std::memcpy(ptr, &header, sizeof(WorldStateHeader));
    ptr += sizeof(WorldStateHeader);

    if (currentCount > 0) {
        std::memcpy(ptr, snap.positions, posSize);
        ptr += posSize;
        std::memcpy(ptr, snap.rotations, rotSize);
        ptr += rotSize;
        std::memcpy(ptr, snap.linearVelocities, velSize);
        ptr += velSize;
        std::memcpy(ptr, snap.angularVelocities, velSize);
        ptr += velSize;
    }

    if (slotCap > 0) {
        std::memcpy(ptr, snap.slotGenerations, slotCap * sizeof(uint32_t));
        ptr += (slotCap * sizeof(uint32_t));
        std::memcpy(ptr, snap.slotToDense, slotCap * sizeof(uint32_t));
        ptr += (slotCap * sizeof(uint32_t));
        std::memcpy(ptr, snap.denseToSlot, slotCap * sizeof(uint32_t));
        ptr += (slotCap * sizeof(uint32_t));
        std::memcpy(ptr, snap.slotStates, slotCap * sizeof(uint8_t));
    }

    return buffer;
}

auto PhysicsWorld::LoadState(const uint8_t* data, size_t size) -> bool {
    if (isStepping.load(std::memory_order::acquire) || size < sizeof(WorldStateHeader)) {
        return false;
    }
    const auto* header = reinterpret_cast<const WorldStateHeader*>(data);
    if (header->magic != WorldStateHeader::ZHLN || header->version != WorldStateHeader::Version) {
        return false;
    }

    const size_t slotCap    = bodyTable.slotCapacity;
    const size_t savedCount = header->bodyCount;
    if (header->slotCapacity != slotCap) {
        return false;
    }
    const size_t expectedSize =
        sizeof(WorldStateHeader) + savedCount * (sizeof(JPH::Real) + sizeof(float) * 3) * 4 + slotCap * (sizeof(uint32_t) * 3 + sizeof(uint8_t));
    if (savedCount > slotCap || size < expectedSize) {
        return false;
    }

    ZHLN::MutexGuard abiGuard(sync.shadowLock);
    ZHLN::MutexGuard allocGuard(bodyTable.allocMutex);

    const uint8_t* ptr     = data + sizeof(WorldStateHeader);
    const size_t   posSize = savedCount * sizeof(JPH::Real) * 4;
    const size_t   rotSize = savedCount * sizeof(float) * 4;
    const size_t   velSize = savedCount * sizeof(float) * 4;

    TransformFrame& back = transforms.GetWriteBuffer();
    back.count           = savedCount;
    if (savedCount > 0) {
        std::memcpy(back.positions, ptr, posSize);
        std::memcpy(back.prevPositions, ptr, posSize);
        ptr += posSize;
        std::memcpy(back.rotations, ptr, rotSize);
        std::memcpy(back.prevRotations, ptr, rotSize);
        ptr += rotSize;
        std::memcpy(back.linearVelocities, ptr, velSize);
        ptr += velSize;
        std::memcpy(back.angularVelocities, ptr, velSize);
        ptr += velSize;
    } else {
        ptr += posSize + rotSize + velSize * 2;
    }

    for (size_t i = 0; i < slotCap; ++i) {
        uint32_t g = 0;
        std::memcpy(&g, ptr, sizeof(uint32_t));
        bodyTable.generations[i].store(g, std::memory_order::relaxed);
        ptr += sizeof(uint32_t);
    }
    std::memcpy(bodyTable.slotToDense.data(), ptr, slotCap * sizeof(uint32_t));
    ptr += (slotCap * sizeof(uint32_t));
    std::memcpy(bodyTable.denseToSlot.data(), ptr, slotCap * sizeof(uint32_t));
    ptr += (slotCap * sizeof(uint32_t));

    for (size_t i = 0; i < slotCap; ++i) {
        bodyTable.slotStates[i].store(*ptr, std::memory_order::relaxed);
        ptr += 1;
    }

    size_t newFreeCount = 0;
    for (uint32_t i = 0; i < slotCap; ++i) {
        if (bodyTable.slotStates[i].load(std::memory_order::relaxed) == static_cast<uint8_t>(SlotState::Empty)) {
            bodyTable.freeSlots[newFreeCount++] = i;
        }
    }
    bodyTable.freeCount.store(newFreeCount, std::memory_order::release);
    bodyTable.count.store(savedCount, std::memory_order::release);
    time = header->worldTime;

    transforms.PublishTableMetadataToBack(bodyTable);
    transforms.Swap();

    const TransformFrame& snap = transforms.GetReadSnapshot();
    for (size_t i = 0; i < savedCount; ++i) {
        const JPH::BodyID bid = bodyTable.bodyIDs[i];
        if (bid.IsInvalid()) {
            continue;
        }

        const JPH::RVec3 p(snap.positions[i * 4 + 0], snap.positions[i * 4 + 1], snap.positions[i * 4 + 2]);
        const JPH::Quat  q(snap.rotations[i * 4 + 0], snap.rotations[i * 4 + 1], snap.rotations[i * 4 + 2], snap.rotations[i * 4 + 3]);
        const JPH::Vec3  lv(snap.linearVelocities[i * 4 + 0], snap.linearVelocities[i * 4 + 1], snap.linearVelocities[i * 4 + 2]);
        const JPH::Vec3  av(snap.angularVelocities[i * 4 + 0], snap.angularVelocities[i * 4 + 1], snap.angularVelocities[i * 4 + 2]);

        bodyInterface->SetPositionAndRotation(bid, p, q, JPH::EActivation::Activate);
        bodyInterface->SetLinearVelocity(bid, lv);
        bodyInterface->SetAngularVelocity(bid, av);
    }

    return true;
}

} // namespace ZHLN::Physics
