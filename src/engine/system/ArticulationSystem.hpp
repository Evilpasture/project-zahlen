// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Atomic.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace ZHLN {

class Engine;
class PhysicsContext;
struct SystemContext;
struct Skeleton;

namespace ECS {
class Registry;
}

struct alignas(64) JointStateBuffer {
    std::array<float, 8192>      jointBlendWeights;
    std::array<float, 8192>      jointStiffness;
    std::array<float, 8192>      jointBlendDecay;
    std::array<JPH::Mat44, 8192> inverseBindMatrices;

    void ResetJoints(uint32_t offset, uint32_t count) noexcept {
        std::fill_n(jointBlendWeights.begin() + offset, count, 0.0f);
        std::fill_n(jointStiffness.begin() + offset, count, 1.0f);
        std::fill_n(jointBlendDecay.begin() + offset, count, 0.0f);
    }
};

class ZHLN_API ArticulationSystem {
  public:
    ArticulationSystem()  = default;
    ~ArticulationSystem() = default;

    ArticulationSystem(const ArticulationSystem&)            = delete;
    ArticulationSystem& operator=(const ArticulationSystem&) = delete;

    void Update(SystemContext& ctx, float dt);

    void Release(Engine& engine, Entity owner) noexcept;
    void Shutdown(Engine& engine) noexcept;

    [[nodiscard]] bool AttachRagdoll(
        Entity rootEntity, ECS::Registry& reg, PhysicsContext& pc, const Skeleton& skeleton, std::span<const Physics::RagdollPartParams> authoredParts,
        uint32_t jointOffset
    );

    uint32_t AllocateJoints(uint32_t count) noexcept;

  private:
    struct TrackedRagdoll {
        Entity                 owner = Entity::Null();
        JPH::Ref<JPH::Ragdoll> instance;
        bool                   isAddedToPhysics = false;
    };

    void BindSkeleton(uint32_t jointOffset, const Skeleton& skeleton) noexcept;

    void Reconcile(ECS::Registry& registry, PhysicsContext& physics) noexcept;
    void Track(Entity owner, const Components::RagdollComponent& component);
    void ReleaseTracked(ECS::Registry& registry, PhysicsContext& physics, size_t index) noexcept;

    std::vector<TrackedRagdoll> _tracked;
    JointStateBuffer            _jointStates;
    ZHLN::Atomic<uint32_t>      _nextJointOffset {0};
};

}
