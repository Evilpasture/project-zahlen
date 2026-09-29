// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Zahlen/Sync.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <cstdint>
#include <type_traits>

namespace ZHLN {
class PhysicsContext;
}

namespace ZHLN::Physics {

struct WorldStateHeader {
    static constexpr uint64_t ZHLN    = 0x5A484C4E;
    static constexpr uint32_t Version = 3;
    const uint64_t            magic   = ZHLN;
    const uint32_t            version = Version;
    uint32_t                  bodyCount {};
    uint32_t                  slotCapacity {};
    double                    worldTime {};
};

inline constexpr std::size_t CACHE_LINE = 64;

enum class CommandType : uint8_t { DestroyBody, CreateConstraint, DestroyConstraint, SetConstraintTarget, SetCollisionFilter };
#if defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnested-anon-types"
#endif
struct Command {
    CommandType type;
    union {
        BodyHandle       handle;
        ConstraintHandle cHandle;
        struct {
            ConstraintType   cType;
            BodyHandle       b1;
            BodyHandle       b2;
            ConstraintParams params;
        } createC;
        struct {
            ConstraintHandle targetCHandle;
            float            targetValue;
        } setTarget;
        struct {
            BodyHandle handle;
            uint32_t   category;
            uint32_t   mask;
        } setFilter;
    };
};
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static_assert((std::is_trivially_default_constructible_v<Command> && std::is_trivially_copyable_v<Command>) );

enum class SlotState : uint8_t {
    Empty          = 0,
    Alive          = 1,
    Character      = 2,
    PendingDestroy = 3,
};

enum class ContactType : uint8_t { Added = 0, Persisted = 1, Removed = 2 };

struct alignas(128) ContactEvent {
    BodyHandle  body1;
    BodyHandle  body2;
    JPH::Real   px, py, pz;
    float       nx, ny, nz;
    float       impulse;
    ContactType type;
    uint32_t    flags;

    float    slidingSpeed;
    float    rvx, rvy, rvz;
    uint32_t mat1, mat2;
    uint32_t sub1, sub2;

};

static_assert(sizeof(ContactEvent) == 128, "ContactEvent must be exactly 128 bytes for L1/L2 cache isolation!");

static_assert((std::is_trivially_default_constructible_v<ContactEvent> && std::is_trivially_copyable_v<ContactEvent>) );

struct MaterialData {
    uint32_t id;
    float    friction;
    float    restitution;
};

static_assert((std::is_trivially_default_constructible_v<MaterialData> && std::is_trivially_copyable_v<MaterialData>) );

struct PhysicsWorld {
    mutable BufferSync sync {};

    alignas(64) JPH::PhysicsSystem*     system          = nullptr;
    JPH::BodyInterface*                 bodyInterface   = nullptr;
    JPH::JobSystem*                     jobSystem       = nullptr;
    JPH::BroadPhaseLayerInterface*      bpInterface     = nullptr;
    JPH::ObjectLayerPairFilter*         pairFilter      = nullptr;
    JPH::ObjectVsBroadPhaseLayerFilter* bpFilter        = nullptr;
    JPH::ContactListener*               contactListener = nullptr;
    JPH::TempAllocator*                 tempAllocator   = nullptr;

    uint32_t maxJoltBodies = 0;

    alignas(64) double time = 0.0;
    ZHLN::Atomic<size_t> count {0};
    size_t               capacity     = 0;
    size_t               slotCapacity = 0;
    ZHLN::Atomic<size_t> freeCount {0};

    JPH::Real* positions         = nullptr;
    JPH::Real* prevPositions     = nullptr;
    float*     rotations         = nullptr;
    float*     prevRotations     = nullptr;
    float*     linearVelocities  = nullptr;
    float*     angularVelocities = nullptr;

    JPH::Array<JPH::BodyID> bodyIDs;
    JPH::Array<uint32_t>    materialIDs;
    JPH::Array<uint64_t>    userData;

    alignas(64) ZHLN::Atomic<bool> isStepping {false};

    JPH::Array<Command> commandQueue;
    JPH::Array<Command> commandQueueSpare;
    size_t              commandCount    = 0;
    size_t              commandCapacity = 0;

    alignas(64) JPH::Array<const void*> joltBodyPtrs;

    JPH::Array<ZHLN::Atomic<uint64_t>> idToHandleMap;
    JPH::Array<uint32_t>               slotToDense;
    JPH::Array<uint32_t>               denseToSlot;
    JPH::Array<uint32_t>               freeSlots;

    JPH::Array<uint32_t> categories;
    JPH::Array<uint32_t> masks;

    JPH::Array<ZHLN::Atomic<uint8_t>>   slotStates;
    JPH::Array<ZHLN::Atomic<uint32_t>> generations;

    alignas(64) JPH::Array<ContactEvent> contactBuffer;
    ZHLN::Atomic<size_t> contactCount {0};
    size_t               contactCapacity = 0;

    alignas(64) JPH::Array<MaterialData> materials;
    size_t materialCount    = 0;
    size_t materialCapacity = 0;

    alignas(64) JPH::Array<JPH::Constraint*> constraints;
    JPH::Array<ZHLN::Atomic<uint32_t>> constraintGenerations;
    JPH::Array<SlotState>              constraintStates;
    JPH::Array<uint32_t>               freeConstraintSlots;
    size_t                             constraintCount     = 0;
    size_t                             constraintCapacity  = 0;
    size_t                             freeConstraintCount = 0;


    void Init(uint32_t inMaxBodies, JPH::PhysicsSystem* inSystem, JPH::JobSystem* inJobSystem, JPH::TempAllocator* inTempAlloc);
    void Shutdown();

    void ResizeBuffers(size_t newCapacity);
    auto AllocateHandle() -> BodyHandle;
    void RemoveBodySlot(uint32_t slot);
    void ResizeConstraintBuffers(size_t newCapacity);

    auto AllocateConstraintHandle() -> ConstraintHandle;
    void RemoveConstraintSlot(uint32_t slot);

    [[nodiscard]] auto LoadSlotState(uint32_t slot) const noexcept -> SlotState {
        return static_cast<SlotState>(slotStates[slot].load(std::memory_order::acquire));
    }
    void StoreSlotState(uint32_t slot, SlotState state) noexcept {
        slotStates[slot].store(static_cast<uint8_t>(state), std::memory_order::release);
    }

    void FlushCommands(
        Command* capturedQueue, size_t capturedCount, JPH::Array<JPH::Ref<JPH::CharacterVirtual>>& characterMap,
        JPH::Array<JPH::CharacterVirtual*>& activeCharacters
    );

    void Synchronize(const JPH::PhysicsSystem* inSystem, const JPH::Array<JPH::CharacterVirtual*>& activeCharacters) noexcept;

    auto SaveState() const -> JPH::Array<std::byte>;
    auto LoadState(const uint8_t* data, size_t size) -> bool;
};

static_assert(std::is_standard_layout_v<PhysicsWorld>);


struct SlotPredicate {
    bool isActive;
    bool isDestructible;
};

[[nodiscard]] constexpr auto GetSlotPredicate(SlotState state) noexcept -> SlotPredicate {
    switch (state) {
        case SlotState::Alive:
        case SlotState::Character:
            return {.isActive = true, .isDestructible = true};
        case SlotState::Empty:
        case SlotState::PendingDestroy:
            break;
    }
    return {.isActive = false, .isDestructible = false};
}

auto CreateNativeConstraint(ConstraintType type, JPH::Body* b1, JPH::Body* b2, const ConstraintParams& p) -> JPH::Constraint*;

}
