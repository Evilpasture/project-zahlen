// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <physics/Physics.hpp>
#include <algorithm>
#include <array>
#include <span>

namespace ZHLN {
class PhysicsContext;
struct Skeleton;
namespace ECS { class Registry; }

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

    uint32_t AllocateJoints(uint32_t count) noexcept;

  private:
    friend class ArticulationSystem;
    void BindSkeleton(uint32_t jointOffset, const Skeleton& skeleton) noexcept;

    JointStateBuffer            _jointStates;
    ZHLN::Atomic<uint32_t>      _nextJointOffset {0};
};
}
