// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "BodyTable.hpp"
#include "CommandMailbox.hpp"
#include "TransformBuffer.hpp"
#include "Zahlen/Sync.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Array.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <Zahlen/physics/PhysicsHandles.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ZHLN::Physics {

struct CharacterManager;

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
static_assert(std::is_trivially_default_constructible_v<ContactEvent> && std::is_trivially_copyable_v<ContactEvent>);

struct MaterialData {
    uint32_t id;
    float    friction;
    float    restitution;
};

static_assert(std::is_trivially_default_constructible_v<MaterialData> && std::is_trivially_copyable_v<MaterialData>);

struct PhysicsWorld {
    // Must remain at offset 0 for Scripting's `ViewComposer::Build(&world, ...)` ABI.
    mutable BufferSync sync {};

    alignas(64) JPH::PhysicsSystem* system        = nullptr;
    JPH::BodyInterface*             bodyInterface = nullptr;
    JPH::JobSystem*                 jobSystem     = nullptr;
    JPH::TempAllocator*             tempAllocator = nullptr;
    uint32_t                        maxJoltBodies = 0;
    double                          time          = 0.0;

    alignas(64) ZHLN::Atomic<bool>     isStepping {false};
    ZHLN::Atomic<uint32_t>             stepGate {0};

    static constexpr uint32_t kStepGateSteppingBit = 0x80000000u;

    [[nodiscard]] auto TryEnterIdleJoltMutation() noexcept -> bool {
        uint32_t cur = stepGate.load(std::memory_order::relaxed);
        while ((cur & kStepGateSteppingBit) == 0u) {
            if (stepGate.compare_exchange_weak(cur, cur + 1u, std::memory_order::acquire, std::memory_order::relaxed)) {
                return true;
            }
        }
        return false;
    }

    void LeaveIdleJoltMutation() noexcept {
        stepGate.fetch_sub(1u, std::memory_order::release);
    }

    void BeginSteppingGate() noexcept {
        stepGate.fetch_or(kStepGateSteppingBit, std::memory_order::acq_rel);
        while ((stepGate.load(std::memory_order::acquire) & ~kStepGateSteppingBit) != 0u) {
        }
        isStepping.store(true, std::memory_order::release);
    }

    void EndSteppingGate() noexcept {
        isStepping.store(false, std::memory_order::release);
        stepGate.fetch_and(~kStepGateSteppingBit, std::memory_order::release);
    }

    BodyTable       bodyTable {};
    TransformBuffer transforms {};
    CommandMailbox  mailbox {};

    alignas(64) JPH::Array<ContactEvent> contactBuffer;
    ZHLN::Atomic<size_t>                 contactCount {0};
    size_t                               contactCapacity = 0;

    alignas(64) mutable ZHLN::Mutex materialMutex {};
    JPH::Array<MaterialData>        materials;
    size_t                          materialCount    = 0;
    size_t                          materialCapacity = 0;

    alignas(64) mutable ZHLN::Mutex    constraintMutex {};
    JPH::Array<JPH::Constraint*>       constraints;
    JPH::Array<ZHLN::Atomic<uint32_t>> constraintGenerations;
    JPH::Array<SlotState>              constraintStates;
    JPH::Array<uint32_t>               freeConstraintSlots;
    size_t                             constraintCapacity  = 0;
    size_t                             freeConstraintCount = 0;

    void Init(uint32_t inMaxBodies, JPH::PhysicsSystem* inSystem, JPH::JobSystem* inJobSystem, JPH::TempAllocator* inTempAlloc);
    void Shutdown();

    void               RegisterMaterial(uint32_t id, float friction, float restitution);
    [[nodiscard]] auto ResolveMaterial(uint32_t id) const -> MaterialData;

    [[nodiscard]] auto AllocateConstraintHandle() -> ConstraintHandle;
    void               RemoveConstraintSlot(uint32_t slot) noexcept;

    void ProcessCommands(std::span<Command> commands, CharacterManager& characters, TransformFrame& backFrame);
    void Synchronize(
        const JPH::PhysicsSystem*               inSystem,
        std::span<JPH::CharacterVirtual* const> activeCharacters,
        TransformFrame&                         backFrame
    ) noexcept;

    [[nodiscard]] auto SaveState() const -> JPH::Array<std::byte>;
    [[nodiscard]] auto LoadState(const uint8_t* data, size_t size) -> bool;
};

static_assert(std::is_standard_layout_v<PhysicsWorld>);
static_assert(offsetof(PhysicsWorld, sync) == 0);

auto CreateNativeConstraint(ConstraintType type, JPH::Body* b1, JPH::Body* b2, const ConstraintParams& p) -> JPH::Constraint*;

} // namespace ZHLN::Physics
