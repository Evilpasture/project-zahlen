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
#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace ZHLN {

class Engine;
class PhysicsContext;
struct SystemContext;
struct Skeleton;

namespace ECS {
class Registry;
} // namespace ECS

// Cache-aligned SoA buffer for maximum evaluation throughput. One per
// ArticulationSystem (see below), not process-global: a destroyed world must
// take its joint state with it, and two coexisting worlds must not overwrite
// each other's matrices.
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

    // Runs inside the update graph, so it consumes a SystemContext rather
    // than an Engine.
    void Update(SystemContext& ctx, float dt);

    // Releases a ragdoll's Jolt registration while its ECS component is still
    // addressable. DespawnEntity uses this before Registry::Destroy.
    void Release(Engine& engine, Entity owner) noexcept;
    // Drains retained registrations before the PhysicsContext is destroyed.
    void Shutdown(Engine& engine) noexcept;

    // Writes a skeleton's inverse bind matrices into this world's joint state
    // at `jointOffset`. Instance method: the buffer it writes into is owned
    // here, so it cannot land in another world's region.
    void BindSkeleton(uint32_t jointOffset, const Skeleton& skeleton) noexcept;

    // Hands out `count` consecutive joint slots in _jointStates. The counter
    // is an instance member (it used to be JointAllocator's static, one
    // monotonic process-wide value with no lifecycle): each world starts at
    // zero, and destroying a world reclaims its whole allocation range.
    uint32_t AllocateJoints(uint32_t count) noexcept;

  private:
    struct TrackedRagdoll {
        Entity                 owner = Entity::Null();
        JPH::Ref<JPH::Ragdoll> instance;
        bool                   isAddedToPhysics = false;
    };

    void Reconcile(ECS::Registry& registry, PhysicsContext& physics) noexcept;
    void Track(Entity owner, const Components::RagdollComponent& component);
    void ReleaseTracked(ECS::Registry& registry, PhysicsContext& physics, size_t index) noexcept;

    std::vector<TrackedRagdoll> _tracked;
    JointStateBuffer            _jointStates;
    ZHLN::Atomic<uint32_t>      _nextJointOffset {0};
};

} // namespace ZHLN
