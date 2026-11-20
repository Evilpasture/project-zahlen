// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArticulationContext.hpp"
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Vec3.hpp>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <Zahlen/ECS/Components.hpp>
#include <engine/SceneResources.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <engine/ModelPrefab.hpp>
#include <render/Render.hpp>
#include <Zahlen/Render/SkeletalAnimation.hpp>
#include <ecs/ECS.hpp>
#include <ecs/EntityCommandBuffer.hpp>
#include <physics/Physics.hpp>
#include <physics/MathConversions.hpp>
#include <algorithm>
#include <span>
#include <string>

namespace ZHLN {

void ArticulationContext::BindSkeleton(uint32_t jointOffset, const Skeleton& skeleton) noexcept {
    for (size_t i = 0; i < skeleton.joints.size(); ++i) {
        _jointStates.inverseBindMatrices[jointOffset + i] = skeleton.joints[i].inverseBindMatrix;
    }
}

uint32_t ArticulationContext::AllocateJoints(uint32_t count) noexcept {
    const auto capacity = static_cast<uint32_t>(_jointStates.jointBlendWeights.size());
    if (count == 0 || count > capacity) return ~uint32_t {0};

    ZHLN::MutexGuard guard(_jointPaletteMutex);

    // Reuse a retired range from the free list before bumping the cursor.
    for (auto it = _freeJointRanges.begin(); it != _freeJointRanges.end(); ++it) {
        if (it->count >= count) {
            const auto offset = it->offset;
            if (it->count == count) {
                _freeJointRanges.erase(it);
            } else {
                it->offset += count;
                it->count -= count;
            }
            return offset;
        }
    }

    // Fall back to monotonic bump allocation.
    const auto offset = _nextJointOffset.load(std::memory_order::relaxed);
    if (offset > capacity - count) return ~uint32_t {0};
    _nextJointOffset.store(offset + count, std::memory_order::relaxed);
    return offset;
}

void ArticulationContext::FreeJoints(uint32_t offset, uint32_t count) noexcept {
    if (count == 0 || offset + count > _jointStates.jointBlendWeights.size()) {
        return; // out-of-range or empty: nothing to retire
    }
    ZHLN::MutexGuard guard(_jointPaletteMutex);
    _freeJointRanges.push_back({offset, count});
    // Sort by offset and coalesce adjacent ranges so the free list stays
    // compact and first-fit reuse remains fast.
    std::sort(_freeJointRanges.begin(), _freeJointRanges.end(),
              [](const JointRange& a, const JointRange& b) { return a.offset < b.offset; });
    std::vector<JointRange> coalesced;
    coalesced.reserve(_freeJointRanges.size());
    for (const auto& range : _freeJointRanges) {
        if (!coalesced.empty() && coalesced.back().offset + coalesced.back().count == range.offset) {
            coalesced.back().count += range.count;
        } else {
            coalesced.push_back(range);
        }
    }
    _freeJointRanges = std::move(coalesced);
    // Clear the retired joint state so a reused range starts clean.
    _jointStates.ResetJoints(offset, count);
}

bool ArticulationContext::AttachRagdoll(
    Entity rootEntity, ECS::Registry& reg, PhysicsContext& pc, const Skeleton& skeleton, std::span<const Physics::RagdollPartParams> authoredParts,
    uint32_t jointOffset
) {
    // Validate once at the authoring boundary, not by scanning every ragdoll
    // every development frame. No skeleton or joint slice may index outside
    // the context's storage.
    const size_t capacity = _jointStates.inverseBindMatrices.size();
    if (skeleton.joints.empty() || authoredParts.empty() || jointOffset > capacity ||
        skeleton.joints.size() > capacity - jointOffset) {
        return false;
    }
    for (const auto& joint: skeleton.joints) {
        if (joint.parentIndex < -1 || joint.parentIndex >= static_cast<int32_t>(skeleton.joints.size())) {
            return false;
        }
    }
    for (const auto& part: authoredParts) {
        if (part.jointIndex >= skeleton.joints.size() || part.parentJointIndex < -1 ||
            part.parentJointIndex >= static_cast<int32_t>(skeleton.joints.size())) {
            return false;
        }
    }

    JPH::Ref<JPH::Skeleton> joltSkel = new JPH::Skeleton();
    for (const auto& joint: skeleton.joints) {
        std::string parentName = (joint.parentIndex >= 0) ? skeleton.joints[joint.parentIndex].name.c_str() : "";
        joltSkel->AddJoint(joint.name.c_str(), parentName);
    }
    joltSkel->CalculateParentJointIndices();

    const auto ragdollID = pc.CreateSkeletalRagdoll(joltSkel, authoredParts);
    if (ragdollID == Physics::RagdollID::Invalid) {
        return false;
    }

    BindSkeleton(jointOffset, skeleton);

    SceneResources::Attach(
        pc, reg, rootEntity, Components::RagdollComponent {
                        .ragdollID    = ragdollID,
                        .skeletonAsset    = InvalidAssetID,
                        .state            = RagdollState::Inactive,
                        .prevState        = RagdollState::Inactive,
                        .jointOffset      = jointOffset,
                        .jointCount       = static_cast<uint32_t>(skeleton.joints.size()),
                        .isAddedToPhysics = false
                    }
    );
    ZHLN::Log("[ArticulationContext] Skeletal ragdoll attached to entity {} ({} parts).", rootEntity.index, authoredParts.size());
    return true;
}


}
