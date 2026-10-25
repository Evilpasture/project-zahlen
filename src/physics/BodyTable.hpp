// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "TransformBuffer.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Array.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/physics/PhysicsHandles.hpp>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ZHLN::Physics {

enum class SlotState : uint8_t {
    Empty              = 0,
    PendingCreateRigid = 1,
    PendingCreateChar  = 2,
    Alive              = 3,
    Character          = 4,
    PendingDestroy     = 5,
};

struct SlotPredicate {
    bool isActive;
    bool isDestructible;
};

[[nodiscard]] constexpr auto GetSlotPredicate(SlotState state) noexcept -> SlotPredicate {
    switch (state) {
        case SlotState::Alive:
        case SlotState::Character:
            return {.isActive = true, .isDestructible = true};
        case SlotState::PendingCreateRigid:
        case SlotState::PendingCreateChar:
            return {.isActive = false, .isDestructible = true};
        case SlotState::Empty:
        case SlotState::PendingDestroy:
            break;
    }
    return {.isActive = false, .isDestructible = false};
}

// Fixed-capacity generational slot map bounded strictly by `cfg.maxBodies`.
// Because no buffer ever reallocates after `Init()`, `generations`, `slotStates`,
// `slotBodyIDs`, and `idToHandleMap` expose stable addresses for lock-free
// spatial query filtering and handle validation across worker threads.
struct alignas(64) BodyTable {
    mutable ZHLN::Mutex  allocMutex {};
    ZHLN::Atomic<size_t> count {0};
    ZHLN::Atomic<size_t> freeCount {0};
    size_t               slotCapacity = 0;

    JPH::Array<ZHLN::Atomic<uint32_t>> generations;
    JPH::Array<ZHLN::Atomic<uint8_t>>  slotStates;
    JPH::Array<ZHLN::Atomic<uint8_t>>  slotMotionTypes;
    JPH::Array<ZHLN::Atomic<uint32_t>> slotBodyIDs;
    JPH::Array<ZHLN::Atomic<uint32_t>> slotMaterialIDs;
    JPH::Array<ZHLN::Atomic<uint64_t>> idToHandleMap;
    JPH::Array<const void*>            joltBodyPtrs;

    JPH::Array<uint32_t>    slotToDense;
    JPH::Array<uint32_t>    denseToSlot;
    JPH::Array<uint32_t>    freeSlots;
    JPH::Array<JPH::BodyID> bodyIDs;
    JPH::Array<uint32_t>    materialIDs;
    JPH::Array<uint64_t>    userData;
    JPH::Array<uint32_t>    categories;
    JPH::Array<uint32_t>    masks;
    JPH::Array<JPH::RVec3>  spawnPositions;
    JPH::Array<JPH::Quat>   spawnRotations;

    void Init(uint32_t maxBodies);
    void Shutdown() noexcept;

    [[nodiscard]] auto LoadSlotState(uint32_t slot) const noexcept -> SlotState {
        return static_cast<SlotState>(slotStates[slot].load(std::memory_order::acquire));
    }

    void StoreSlotState(uint32_t slot, SlotState state) noexcept {
        slotStates[slot].store(static_cast<uint8_t>(state), std::memory_order::release);
    }

    [[nodiscard]] auto ReserveSlot(SlotState pendingState, JPH::EMotionType motion, JPH::RVec3Arg spawnPos, JPH::QuatArg spawnRot) -> BodyHandle;
    [[nodiscard]] auto TryMarkPendingDestroy(BodyHandle handle) noexcept -> bool;
    [[nodiscard]] auto IsSlotActive(BodyHandle handle) const noexcept -> bool;
    void               RemoveDenseAndReleaseSlot(uint32_t slot, TransformFrame& backFrame) noexcept;
};

static_assert(std::is_standard_layout_v<BodyTable>);

} // namespace ZHLN::Physics
