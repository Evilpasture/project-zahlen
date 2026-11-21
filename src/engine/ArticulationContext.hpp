// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <physics/Physics.hpp>
#include <algorithm>
#include <array>
#include <span>
#include <utility>
#include <vector>

namespace ZHLN {
class PhysicsContext;
struct Skeleton;
namespace ECS { class Registry; }

// A retired joint range returned to the palette by FreeJoints. Kept sorted and
// coalesced so AllocateJoints can reuse it without growing the monotonic cursor.
struct JointRange {
    uint32_t offset;
    uint32_t count;

    [[nodiscard]] auto operator<=>(const JointRange&) const noexcept = default;
};

struct alignas(64) JointStateBuffer {
    std::array<float, 8192>      jointBlendWeights;
    std::array<float, 8192>      jointStiffness;
    std::array<float, 8192>      jointBlendDecay;
    std::array<Mat4x4, 8192> inverseBindMatrices;

    void ResetJoints(uint32_t offset, uint32_t count) noexcept {
        std::fill_n(jointBlendWeights.begin() + offset, count, 0.0f);
        std::fill_n(jointStiffness.begin() + offset, count, 1.0f);
        std::fill_n(jointBlendDecay.begin() + offset, count, 0.0f);
    }
};

// World-owned articulation state. Scheduled systems borrow it as a resource;
// no system instance owns simulation state.
class ArticulationContext {
  public:
    ArticulationContext() = default;
    ArticulationContext(const ArticulationContext&) = delete;
    ArticulationContext& operator=(const ArticulationContext&) = delete;

    [[nodiscard]] bool AttachRagdoll(
        Entity rootEntity, ECS::Registry& reg, PhysicsContext& pc, const Skeleton& skeleton, std::span<const Physics::RagdollPartParams> authoredParts,
        uint32_t jointOffset
    );

    // Allocates `count` contiguous joints from the palette. Reuses retired
    // ranges from FreeJoints before bumping the monotonic cursor, so spawning
    // and despawning skinned characters over a long session cannot exhaust the
    // 8192-joint palette. Returns ~uint32_t{0} when no range is available.
    [[nodiscard]] uint32_t AllocateJoints(uint32_t count) noexcept;

    // Retires a previously allocated joint range back to the palette. Safe to
    // call once per allocation; double-free or out-of-range offsets are ignored.
    void FreeJoints(uint32_t offset, uint32_t count) noexcept;

  private:
    friend class ArticulationSystem;
    void BindSkeleton(uint32_t jointOffset, const Skeleton& skeleton) noexcept;

    JointStateBuffer            _jointStates;
    ZHLN::Atomic<uint32_t>      _nextJointOffset {0};
    ZHLN::Mutex                 _jointPaletteMutex;
    std::vector<JointRange>     _freeJointRanges;
};

} // namespace ZHLN
