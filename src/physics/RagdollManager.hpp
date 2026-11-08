// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Core/Reference.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <Zahlen/Physics/PhysicsIDs.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace ZHLN::Physics {

struct RagdollSlot {
    JPH::Ref<JPH::Ragdoll> instance   = nullptr;
    uint32_t               generation = 1;
    bool                   active     = false;
};

// Isolated manager for articulated `JPH::Ragdoll` instances. Guarded by its own
// private mutex so driving ragdoll motors, reading poses, or applying joint
// impulses never blocks rigid-body queries or transform buffer readers.
struct alignas(64) RagdollManager {
    mutable ZHLN::Mutex      mutex {};
    std::vector<RagdollSlot> slots;
    std::vector<uint32_t>    freeSlots;

    void Shutdown() noexcept;

    [[nodiscard]] auto CreateSkeletalRagdoll(JPH::PhysicsSystem& joltSystem, JPH::Ref<JPH::Skeleton> skeleton, std::span<const RagdollPartParams> parts)
        -> RagdollID;

    void DestroyRagdolls(std::span<const RagdollID> handles) noexcept;

    [[nodiscard]] auto GetRagdoll(RagdollID handle) const noexcept -> ZHLN::Optional<JPH::Ragdoll&>;
    void               ActivateRagdoll(RagdollID handle, const JPH::SkeletonPose& pose, JPH::Vec3Arg initialVelocity) noexcept;
    void               RemoveRagdoll(RagdollID handle) noexcept;
    void               DriveRagdollPose(RagdollID handle, const JPH::SkeletonPose& pose) noexcept;
    void               AddRagdollImpulse(RagdollID handle, uint32_t jointIndex, JPH::Vec3Arg impulse, JPH::BodyInterface& bodyInterface) noexcept;
    [[nodiscard]] auto GetRagdollPose(RagdollID handle, JPH::RVec3& outRootOffset, JPH::Mat44* outWorldJoints) const noexcept -> bool;

  private:
    [[nodiscard]] auto FindSlotUnlocked(RagdollID handle) noexcept -> RagdollSlot*;
    [[nodiscard]] auto FindSlotUnlocked(RagdollID handle) const noexcept -> const RagdollSlot*;
};

} // namespace ZHLN::Physics
