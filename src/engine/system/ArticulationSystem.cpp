// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArticulationSystem.hpp"
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
#include <vector>

namespace ZHLN {

namespace {
// Jolt's skeleton/ragdoll interfaces own JPH matrices; the animation and
// skinning data on this side of the boundary is native and column-major.
[[nodiscard]] auto ToJolt(const Mat4x4& matrix) -> JPH::Mat44 {
    return {
        JPH::Vec4(matrix[0, 0], matrix[1, 0], matrix[2, 0], matrix[3, 0]),
        JPH::Vec4(matrix[0, 1], matrix[1, 1], matrix[2, 1], matrix[3, 1]),
        JPH::Vec4(matrix[0, 2], matrix[1, 2], matrix[2, 2], matrix[3, 2]),
        JPH::Vec4(matrix[0, 3], matrix[1, 3], matrix[2, 3], matrix[3, 3])
    };
}

[[nodiscard]] auto FromJolt(const JPH::Mat44& matrix) -> Mat4x4 {
    Mat4x4 result {};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            result[static_cast<size_t>(row), static_cast<size_t>(column)] = matrix(row, column);
        }
    }
    return result;
}
} // namespace

void ArticulationSystem::Update(ECS::Query<Components::RagdollComponent&, const Components::PhysicsComponent,
                                            const Components::SkeletalMeshComponent, Components::TransformComponent&,
                                            const Components::RagdollHitReactionCommand, const Components::RagdollImpulseCommand> query,
                                 ECS::EntityCommandBuffer& ecb, ECS::ResMut<ArticulationContext> articulation,
                                 ECS::ResMut<PhysicsContext> physics, ECS::ResMut<PoseUploadQueue> poseUploads, FrameDt frameDt) {
    auto& context = *articulation;
    auto& pc  = *physics;
    const float dt = frameDt.value;

    auto entities = query.Entities<Components::RagdollComponent>();
    auto ragdolls = query.Raw<Components::RagdollComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        Entity                        e       = entities[i];
        Components::RagdollComponent& ragComp = ragdolls[i];
        auto                          phys    = query.Get<Components::PhysicsComponent>(e);

        if (ragComp.ragdollID == Physics::RagdollID::Invalid) {
            continue;
        }
        auto ragdoll = pc.GetRagdoll(ragComp.ragdollID);
        if (!ragdoll) {
            continue;
        }

        uint32_t offset = ragComp.jointOffset;
        uint32_t count  = ragComp.jointCount;

        if (auto hitCmd = query.Get<Components::RagdollHitReactionCommand>(e)) {
            if (hitCmd->jointIndex < count) {
                uint32_t globalIdx                         = offset + hitCmd->jointIndex;
                context._jointStates.jointBlendWeights[globalIdx] = std::clamp(hitCmd->weight, 0.0f, 1.0f);
                context._jointStates.jointStiffness[globalIdx]    = std::clamp(hitCmd->stiffness, 0.0f, 1.0f);
                context._jointStates.jointBlendDecay[globalIdx]   = std::max(0.0f, hitCmd->decayRate);

                ragComp.state = RagdollState::PartialBlend;
            }
            ecb.RemoveComponent<Components::RagdollHitReactionCommand>(e);
        }

        if (auto impulseCmd = query.Get<Components::RagdollImpulseCommand>(e)) {
            pc.AddRagdollImpulse(ragComp.ragdollID, impulseCmd->jointIndex, impulseCmd->impulse);
            ecb.RemoveComponent<Components::RagdollImpulseCommand>(e);
        }

        bool hasActiveBlend = false;
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t globalIdx = offset + j;
            float    decay     = context._jointStates.jointBlendDecay[globalIdx];

            if (decay > 0.0f) {
                context._jointStates.jointBlendWeights[globalIdx] = std::max(0.0f, context._jointStates.jointBlendWeights[globalIdx] - decay * dt);
                context._jointStates.jointStiffness[globalIdx]    = std::min(1.0f, context._jointStates.jointStiffness[globalIdx] + dt * 1.5f);

                if (context._jointStates.jointBlendWeights[globalIdx] <= 0.0f) {
                    context._jointStates.jointBlendDecay[globalIdx] = 0.0f;
                }
            }

            if (context._jointStates.jointBlendWeights[globalIdx] > 0.001f) {
                hasActiveBlend = true;
            }
        }

        if (hasActiveBlend && ragComp.state == RagdollState::Inactive) {
            ragComp.state = RagdollState::PartialBlend;
        } else if (!hasActiveBlend && ragComp.state == RagdollState::PartialBlend) {
            ragComp.state = RagdollState::Inactive;
        }

        if (ragComp.state == RagdollState::Inactive && ragComp.prevState == RagdollState::Inactive) {
            continue;
        }

        const JPH::Skeleton* skel = ragdoll->GetRagdollSettings()->GetSkeleton();

        auto capsuleWorldPos = JPH::RVec3::sZero();
        if (phys) {
            if (const auto bodyPos = pc.BodyPosition(phys->physicsID)) {
                capsuleWorldPos = Physics::ToJoltPosition(*bodyPos);
            }
        }

        JPH::SkeletonPose animPose;
        animPose.SetSkeleton(skel);
        animPose.SetRootOffset(capsuleWorldPos);

        std::vector<Mat4x4> localJoints(count, Mat4x4::Identity());
        for (uint32_t j = 0; j < count; ++j) {
            localJoints[j] = Inverse(context._jointStates.inverseBindMatrices[offset + j]);
        }

        std::vector<Mat4x4> modelJoints(count, Mat4x4::Identity());
        for (uint32_t j = 0; j < count; ++j) {
            int parentIdx = skel->GetJoint(j).mParentJointIndex;
            if (parentIdx >= 0) {
                modelJoints[j] = modelJoints[parentIdx] * localJoints[j];
            } else {
                modelJoints[j] = localJoints[j];
            }
        }

        if (const auto kinematic = poseUploads->KinematicPose(offset); !kinematic.empty()) {
            const size_t overrideCount = std::min<size_t>(count, kinematic.size());
            std::copy_n(kinematic.data(), overrideCount, modelJoints.data());
        }

        for (uint32_t j = 0; j < count; ++j) {
            animPose.GetJointMatrices()[j] = ToJolt(modelJoints[j]);
        }
        animPose.CalculateJointStates();

        if (ragComp.state != ragComp.prevState) {
            if (ragComp.state == RagdollState::Dynamic || ragComp.state == RagdollState::Kinematic || ragComp.state == RagdollState::PartialBlend) {
                if (!ragComp.isAddedToPhysics) {
                    auto initialVelocity = FVec3::Zero();
                    if (phys) {
                        const auto velocity = pc.GetCharacterVelocity(phys->physicsID);
                        initialVelocity = {velocity.GetX(), velocity.GetY(), velocity.GetZ()};
                    }
                    pc.ActivateRagdoll(ragComp.ragdollID, animPose, initialVelocity);
                    ragComp.isAddedToPhysics = true;
                }
            } else if (ragComp.state == RagdollState::Inactive && ragComp.isAddedToPhysics) {
                pc.RemoveRagdoll(ragComp.ragdollID);
                ragComp.isAddedToPhysics = false;
            }
            ragComp.prevState = ragComp.state;
        }

        if (ragComp.state == RagdollState::Kinematic || ragComp.state == RagdollState::PartialBlend) {
            pc.DriveRagdollPose(ragComp.ragdollID, animPose);
        }

        if (ragComp.state != RagdollState::Inactive) {
            JPH::Array<JPH::Mat44> physicalWorldJoints(count, JPH::Mat44::sIdentity());
            auto actualRootOffset = JPH::RVec3::sZero();

            if (!pc.GetRagdollPose(ragComp.ragdollID, actualRootOffset, physicalWorldJoints.data())) {
                continue;
            }

            auto allSkinnedEntities = query.Entities<Components::SkeletalMeshComponent>();
            for (Entity childEnt: allSkinnedEntities) {
                auto skelMesh = query.Get<Components::SkeletalMeshComponent>(childEnt);
                if (skelMesh && skelMesh->jointOffset == offset) {
                    if (auto trans = query.Get<Components::TransformComponent>(childEnt)) {
                        trans->position = {static_cast<float>(actualRootOffset.GetX()), static_cast<float>(actualRootOffset.GetY()),
                                           static_cast<float>(actualRootOffset.GetZ())};
                        trans->rotation = {0.0f, 0.0f, 0.0f, 1.0f};
                    }
                }
            }

            std::vector<Mat4x4> finalSkinningMatrices(count);
            const FVec3 rootPosition {static_cast<float>(actualRootOffset.GetX()), static_cast<float>(actualRootOffset.GetY()),
                                      static_cast<float>(actualRootOffset.GetZ())};
            const auto invRoot = CreateTranslation(-rootPosition);

            for (uint32_t j = 0; j < count; ++j) {
                const auto& ibm = context._jointStates.inverseBindMatrices[offset + j];
                const auto physModel = invRoot * FromJolt(physicalWorldJoints[j]);
                const auto& animModel = modelJoints[j];

                float blendWeight = (ragComp.state == RagdollState::Dynamic) ? 1.0f : context._jointStates.jointBlendWeights[offset + j];

                if (blendWeight <= 0.001f) {
                    finalSkinningMatrices[j] = animModel * ibm;
                } else if (blendWeight >= 0.999f) {
                    finalSkinningMatrices[j] = physModel * ibm;
                } else {
                    const auto animTRS = DecomposeTransform(animModel);
                    const auto physTRS = DecomposeTransform(physModel);
                    const auto tBlended = Lerp(animTRS.translation, physTRS.translation, blendWeight);
                    const auto rBlended = Slerp(animTRS.rotation, physTRS.rotation, blendWeight);
                    const auto sBlended = Lerp(animTRS.scale, physTRS.scale, blendWeight);
                    const auto blendedModel = ComposeTransform(tBlended, rBlended, sBlended);
                    finalSkinningMatrices[j] = blendedModel * ibm;
                }
            }

            poseUploads->Push(offset, std::span {finalSkinningMatrices.data(), static_cast<size_t>(count)});
        }
    }

}

}
