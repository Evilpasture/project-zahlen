// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PhysicsWorld.hpp"
#include "Zahlen/Log.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Zahlen/Core/Loop.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Prefetch.hpp>
#include <Zahlen/Core/Span.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <array>
#include <memory>

namespace ZHLN::Physics {

namespace {

struct alignas(32) PosStride {
    JPH::Real x, y, z, w;
};
struct alignas(16) AuxStride {
    float x, y, z, w;
};

static_assert(sizeof(PosStride) == sizeof(JPH::Real) * 4);
static_assert(sizeof(AuxStride) == sizeof(float) * 4);
static_assert(alignof(PosStride) == 32);
static_assert(alignof(AuxStride) == 16);
static_assert(sizeof(PosStride) % 32 == 0);

constexpr uint32_t BATCH_SIZE = 128;

struct SyncWorkItem {
    const JPH::Body* body;
    uint32_t         dense_idx;
};

// NOLINTBEGIN(cppcoreguidelines-avoid-const-or-ref-data-members)
struct WorldDataCreateInfo {
    PosStride* const ZHLN_RESTRICT shadow_pos;
    AuxStride* const ZHLN_RESTRICT shadow_rot;
    AuxStride* const ZHLN_RESTRICT shadow_lvel;
    AuxStride* const ZHLN_RESTRICT shadow_avel;
    uint8_t* const ZHLN_RESTRICT   char_flags;
};

struct MappingDataCreateInfo {
    const void* ZHLN_RESTRICT* const ZHLN_RESTRICT    body_ptrs;
    const ZHLN::Atomic<uint32_t>* const ZHLN_RESTRICT generations;
    const size_t                                      slot_capacity;
    const uint32_t* const ZHLN_RESTRICT               slot_to_dense;
};
// NOLINTEND(cppcoreguidelines-avoid-const-or-ref-data-members)

#ifdef JPH_DOUBLE_PRECISION
inline constexpr bool IS_DOUBLE = true;
using PosPointerType            = JPH::Double3*;
#else
inline constexpr bool IS_DOUBLE = false;
using PosPointerType            = JPH::Float3*;
#endif

using AuxPointerType = JPH::Float4*;

template <JPH::EBodyType TType>
[[gnu::always_inline, gnu::nonnull(2)]]
inline void ProcessItem(const uint32_t D, const JPH::Body* const ZHLN_RESTRICT b, const WorldDataCreateInfo world) noexcept {
    // `BeginWriteFromFront()` already seeded `prevPositions` / `prevRotations`
    // from the prior front buffer, so only the newly integrated state is written.
    auto* const targetPos   = &world.shadow_pos[D];
    const auto& translation = b->GetCenterOfMassPosition();

    if constexpr (IS_DOUBLE) {
        translation.StoreDouble3(reinterpret_cast<PosPointerType>(targetPos));
        targetPos->w = 0.0;
    } else {
        JPH::Vec4(JPH::Vec3(translation), 0.0f).StoreFloat4(reinterpret_cast<AuxPointerType>(targetPos));
    }

    const auto& rotation = b->GetRotation();
    rotation.GetXYZW().StoreFloat4(reinterpret_cast<AuxPointerType>(&world.shadow_rot[D]));

    if constexpr (TType == JPH::EBodyType::RigidBody) {
        JPH::Vec4(b->GetLinearVelocity(), 0.0f).StoreFloat4(reinterpret_cast<AuxPointerType>(&world.shadow_lvel[D]));
        JPH::Vec4(b->GetAngularVelocity(), 0.0f).StoreFloat4(reinterpret_cast<AuxPointerType>(&world.shadow_avel[D]));
    }
}

template <JPH::EBodyType TType>
[[gnu::always_inline,
  gnu::hot,
#if defined(__clang__)
  gnu::flatten
#endif
]]
inline void ProcessBatch(const WorldDataCreateInfo world, RestrictSpan<const SyncWorkItem> items) noexcept {
    const size_t count = items.size();
    [[assume(count > 0)]];
    [[assume(count <= BATCH_SIZE)]];

    ZHLN::UnrollLoop<4>(count, [&](auto j) -> auto {
        if (j + 2 < count) {
            const uint32_t next_idx = items[j + 2].dense_idx;
            ZHLN::Prefetch<AccessType::Write>(&world.shadow_pos[next_idx]);
            ZHLN::Prefetch<AccessType::Write>(&world.shadow_rot[next_idx]);
        }
        ProcessItem<TType>(items[j].dense_idx, items[j].body, world);
    });
}

template <JPH::EBodyType TType>
[[gnu::always_inline,
#if defined(__clang__)
  gnu::flatten,
#endif
  gnu::nonnull(3, 4)]]
inline void ExecuteSyncRange(
    const uint32_t                                    begin,
    const uint32_t                                    end,
    const JPH::BodyID* const ZHLN_RESTRICT            active_ids,
    const JPH::BodyLockInterface* const ZHLN_RESTRICT lock_iface,
    MappingDataCreateInfo                             map,
    const WorldDataCreateInfo                         world
) noexcept {
    alignas(64) std::array<SyncWorkItem, BATCH_SIZE> worklist {};
    uint32_t                                         work_ptr = 0;

    for (uint32_t i = begin; i < end; ++i) {
        const uint32_t raw_jolt_id = active_ids[i].GetIndexAndSequenceNumber();
        const uint32_t j_idx       = raw_jolt_id & JPH::BodyID::cMaxBodyIndex;

        ZHLN::Assert(
            j_idx < map.slot_capacity + 1,
            "PhysicsSync: Out of bounds j_idx ({}) detected! Slot capacity is {}. Active ID: {:#x}",
            j_idx,
            map.slot_capacity,
            raw_jolt_id
        );

        const auto* ZHLN_RESTRICT b = static_cast<const JPH::Body* ZHLN_RESTRICT>(map.body_ptrs[j_idx]);

        if (b == nullptr || b->GetID().GetIndexAndSequenceNumber() != raw_jolt_id) [[unlikely]] {
            b                    = lock_iface->TryGetBody(JPH::BodyID(raw_jolt_id));
            map.body_ptrs[j_idx] = b;
        }

        ZHLN::Assert(b != nullptr, "PhysicsSync: Failed to resolve Jolt Body for active ID: {:#x}", raw_jolt_id);

        const uint64_t handle = b->GetUserData();
        if (handle == 0u) [[unlikely]] {
            // Ragdoll bodies do not carry a `BodyHandle` user-data payload.
            continue;
        }
        const auto slot = static_cast<uint32_t>(handle & 0xFFFFFFFFu);
        const auto gen  = static_cast<uint32_t>(handle >> 32);

        const uint32_t safe_slot   = (slot < map.slot_capacity) ? slot : 0u;
        const uint32_t current_gen = map.generations[safe_slot].load(std::memory_order::relaxed);

        ZHLN::Assert(
            slot < map.slot_capacity, "PhysicsSync: Decoded slot index ({}) exceeds slot capacity ({}). Handle: {:#x}", slot, map.slot_capacity, handle
        );

        const uint32_t bad      = static_cast<uint32_t>(slot >= map.slot_capacity) | (current_gen ^ gen);
        const uint32_t d_idx    = map.slot_to_dense[safe_slot];
        const auto     is_valid = static_cast<uint32_t>(bad == 0u);

        [[assume(work_ptr < BATCH_SIZE)]];
        worklist[work_ptr].body      = b;
        worklist[work_ptr].dense_idx = d_idx;
        work_ptr += is_valid;

        if (work_ptr == BATCH_SIZE) {
            ProcessBatch<TType>(world, RestrictSpan(worklist));
            work_ptr = 0;
        }
    }

    if (work_ptr > 0) {
        ProcessBatch<TType>(world, RestrictSpan(worklist).first(work_ptr));
    }
}

template <JPH::EBodyType TType>
[[gnu::always_inline,
#if defined(__clang__)
  gnu::flatten,
#endif
  gnu::nonnull(2)]]
inline void ExecuteSyncPass(
    const uint32_t                                active_count,
    const JPH::PhysicsSystem* const ZHLN_RESTRICT inSystem,
    MappingDataCreateInfo                         map,
    const WorldDataCreateInfo                     world
) noexcept {
    if (active_count == 0) {
        return;
    }

    const JPH::BodyID* const ZHLN_RESTRICT active_ids = inSystem->GetActiveBodiesUnsafe(TType);
    if (active_ids == nullptr) [[unlikely]] {
        return;
    }

    const auto* const ZHLN_RESTRICT lock_iface = &inSystem->GetBodyLockInterfaceNoLock();

    constexpr uint32_t kSyncChunkSize = 256;
    TaskSystem::ParallelFor(active_count, kSyncChunkSize, [&](uint32_t start, uint32_t end, uint32_t) {
        ExecuteSyncRange<TType>(start, end, active_ids, lock_iface, map, world);
    });
}

[[gnu::always_inline]]
inline void SyncCharacters(std::span<JPH::CharacterVirtual* const> characters, const MappingDataCreateInfo& map, const WorldDataCreateInfo& world) noexcept {
    for (auto* character: characters) {
        const BodyHandle h    = BodyHandle::Unpack(character->GetUserData());
        const uint32_t   slot = h.index;
        if (slot >= map.slot_capacity) [[unlikely]] {
            continue;
        }

        const uint32_t D         = map.slot_to_dense[slot];
        const JPH::RVec3 pos     = character->GetPosition();
        auto* const      targetP = &world.shadow_pos[D];
        if constexpr (IS_DOUBLE) {
            pos.StoreDouble3(reinterpret_cast<PosPointerType>(targetP));
            targetP->w = 0.0;
        } else {
            JPH::Vec4(JPH::Vec3(pos), 0.0f).StoreFloat4(reinterpret_cast<AuxPointerType>(targetP));
        }

        character->GetRotation().GetXYZW().StoreFloat4(reinterpret_cast<AuxPointerType>(&world.shadow_rot[D]));
        JPH::Vec4(character->GetLinearVelocity(), 0.0f).StoreFloat4(reinterpret_cast<AuxPointerType>(&world.shadow_lvel[D]));

        world.char_flags[D] = (character->GetGroundState() == JPH::CharacterVirtual::EGroundState::OnGround) ? kCharacterFlagOnGround : 0u;
    }
}

} // namespace

[[
#if defined(__clang__)
    gnu::flatten,
#endif
    gnu::hot,
    gnu::nonnull(2)]]
void PhysicsWorld::Synchronize(
    const JPH::PhysicsSystem* const         inSystem,
    std::span<JPH::CharacterVirtual* const> activeCharacters,
    TransformFrame&                         backFrame
) noexcept {
    const uint32_t activeRigids = inSystem->GetNumActiveBodies(JPH::EBodyType::RigidBody);

    if (activeRigids > 0 || !activeCharacters.empty()) {
        const WorldDataCreateInfo worldInfo = {
            .shadow_pos  = std::assume_aligned<32>(reinterpret_cast<PosStride*>(backFrame.positions)),
            .shadow_rot  = std::assume_aligned<16>(reinterpret_cast<AuxStride*>(backFrame.rotations)),
            .shadow_lvel = std::assume_aligned<16>(reinterpret_cast<AuxStride*>(backFrame.linearVelocities)),
            .shadow_avel = std::assume_aligned<16>(reinterpret_cast<AuxStride*>(backFrame.angularVelocities)),
            .char_flags  = std::assume_aligned<64>(backFrame.characterFlags),
        };

        const MappingDataCreateInfo mapInfo = {
            .body_ptrs     = bodyTable.joltBodyPtrs.data(),
            .generations   = bodyTable.generations.data(),
            .slot_capacity = bodyTable.slotCapacity,
            .slot_to_dense = bodyTable.slotToDense.data(),
        };

        ExecuteSyncPass<JPH::EBodyType::RigidBody>(activeRigids, inSystem, mapInfo, worldInfo);

        if (!activeCharacters.empty()) {
            SyncCharacters(activeCharacters, mapInfo, worldInfo);
        }
    }

    transforms.PublishTableMetadataToBack(bodyTable);
}

} // namespace ZHLN::Physics
