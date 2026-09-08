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
#include <Zahlen/Entity.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace ZHLN {

class Engine;
struct Skeleton;

// Cache-aligned SoA buffer for maximum evaluation throughput
struct alignas(64) GlobalJointStateBuffer {
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

extern GlobalJointStateBuffer g_JointStates;

class ZHLN_API ArticulationSystem {
  public:
    ArticulationSystem()  = default;
    ~ArticulationSystem() = default;

    ArticulationSystem(const ArticulationSystem&)            = delete;
    ArticulationSystem& operator=(const ArticulationSystem&) = delete;

    void Update(Engine& engine, float dt);

    /// Releases a ragdoll's Jolt registration while its ECS component is still
    /// addressable. DespawnEntity uses this before Registry::Destroy.
    void Release(Engine& engine, Entity owner) noexcept;
    /// Drains retained registrations before the PhysicsContext is destroyed.
    void Shutdown(Engine& engine) noexcept;

    static void BindSkeleton(uint32_t jointOffset, const Skeleton& skeleton) noexcept;

  private:
    struct TrackedRagdoll {
        Entity                 owner = Entity::Null();
        JPH::Ref<JPH::Ragdoll> instance;
        bool                   isAddedToPhysics = false;
    };

    void Reconcile(Engine& engine) noexcept;
    void Track(Entity owner, const Components::RagdollComponent& component);
    void ReleaseTracked(Engine& engine, size_t index) noexcept;

    std::vector<TrackedRagdoll> _tracked;
};

} // namespace ZHLN
