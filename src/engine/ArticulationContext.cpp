// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
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
#include <format>

namespace ZHLN {

void ArticulationContext::BindSkeleton(Physics::RagdollID id, const Skeleton& skeleton) noexcept {
    RagdollJointState state;
    const size_t count = skeleton.joints.size();
    state.blendWeights.assign(count, 0.0f);
    state.stiffness.assign(count, 1.0f);
    state.blendDecay.assign(count, 0.0f);
    state.inverseBindMatrices.reserve(count);
    for (const auto& joint : skeleton.joints) state.inverseBindMatrices.push_back(joint.inverseBindMatrix);
    _ragdollStates[id] = std::move(state);
}

bool ArticulationContext::AttachRagdoll(
    Entity rootEntity, ECS::Registry& reg, PhysicsContext& pc, const Skeleton& skeleton, std::span<const Physics::RagdollPartParams> authoredParts,
    uint32_t jointOffset
) {
    // Validate once at the authoring boundary. Per-ragdoll CPU state is
    // sized from the skeleton itself; GPU palette budget is the renderer's.
    if (skeleton.joints.empty() || authoredParts.empty()) {
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

    BindSkeleton(ragdollID, skeleton);

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
    return true;
}

}
