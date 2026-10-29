// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Real.h>
#include <Jolt/Math/Vec3.h>
#include <Zahlen/Core/Atomic.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ZHLN::Physics {

struct BodyTable;

inline constexpr uint8_t kCharacterFlagOnGround = 1u << 0;

// Immutable-while-front SoA snapshot. Readers acquire `frontIndex` and read a
// completely self-consistent frame (`count`, `slotToDense`, `slotGenerations`,
// poses, velocities, and character flags) without locks while `PhysicsSync`
// mutates the inactive back frame.
struct TransformFrame {
    size_t     count             = 0;
    JPH::Real* positions         = nullptr;
    JPH::Real* prevPositions     = nullptr;
    float*     rotations         = nullptr;
    float*     prevRotations     = nullptr;
    float*     linearVelocities  = nullptr;
    float*     angularVelocities = nullptr;
    uint8_t*   characterFlags    = nullptr;
    uint32_t*  slotToDense       = nullptr;
    uint32_t*  denseToSlot       = nullptr;
    uint32_t*  slotGenerations   = nullptr;
    uint8_t*   slotStates        = nullptr;

    void WriteInitialPose(uint32_t dense, JPH::RVec3Arg pos, JPH::QuatArg rot, uint8_t charFlags = 0u) noexcept;
    void WriteTeleportPose(uint32_t dense, JPH::RVec3Arg pos, JPH::QuatArg rot) noexcept;
    void WriteTeleportPosition(uint32_t dense, JPH::RVec3Arg pos) noexcept;
    void SwapEraseDense(uint32_t dstDense, uint32_t srcDense) noexcept;
};

static_assert(std::is_standard_layout_v<TransformFrame>);

struct alignas(64) TransformBuffer {
    ZHLN::Atomic<uint32_t>         frontIndex {0};
    std::array<TransformFrame, 2>  frames {};
    size_t                         capacity = 0;

    void Init(uint32_t maxBodies);
    void Shutdown() noexcept;

    [[nodiscard]] auto GetReadSnapshot() const noexcept -> const TransformFrame& {
        return frames[frontIndex.load(std::memory_order::acquire)];
    }

    [[nodiscard]] auto GetWriteBuffer() noexcept -> TransformFrame& {
        return frames[1u - frontIndex.load(std::memory_order::relaxed)];
    }

    // Seeds the inactive back buffer from the current front snapshot, advancing
    // `prevPositions = positions` and `prevRotations = rotations` for all live
    // dense entries so sleeping and static bodies remain coherent across swaps.
    void BeginWriteFromFront() noexcept;

    // Publishes `BodyTable`'s authoritative dense count and slot mappings into
    // the back frame immediately prior to `Swap()`.
    void PublishTableMetadataToBack(const BodyTable& table) noexcept;

    void Swap() noexcept {
        const uint32_t nextFront = 1u - frontIndex.load(std::memory_order::relaxed);
        frontIndex.store(nextFront, std::memory_order::release);
    }
};

static_assert(std::is_standard_layout_v<TransformBuffer>);

} // namespace ZHLN::Physics
