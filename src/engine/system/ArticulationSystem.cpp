// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArticulationSystem.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/SceneResources.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace ZHLN {

namespace Tests {
static void VerifyArticulationStateConsistency(const ECS::Registry& reg) noexcept {
    static bool testsRun = false;
    if (testsRun) {
        return;
    }
    testsRun = true;

    auto entities = reg.GetEntitiesWith<Components::RagdollComponent>();
    auto ragdolls = reg.GetRawArray<Components::RagdollComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        Entity      e       = entities[i];
        const auto& ragComp = ragdolls[i];

        if (ragComp.state != RagdollState::Inactive && ragComp.state != RagdollState::Dynamic && ragComp.state != RagdollState::Kinematic &&
            ragComp.state != RagdollState::PartialBlend) {
            ZHLN::Log("[Test Fail] Articulation State: Entity {} has invalid ragdoll state {}", e.index, static_cast<int>(ragComp.state));
        }
        if (ragComp.jointCount > 2000 || ragComp.jointCount == 0) {
            ZHLN::Log("[Test Fail] Articulation State: Entity {} has unreasonable joint count: {}", e.index, ragComp.jointCount);
        }
    }
}
}

void ArticulationSystem::BindSkeleton(uint32_t jointOffset, const Skeleton& skeleton) noexcept {
    for (size_t i = 0; i < skeleton.joints.size(); ++i) {
        _jointStates.inverseBindMatrices[jointOffset + i] = skeleton.joints[i].inverseBindMatrix;
    }
}

uint32_t ArticulationSystem::AllocateJoints(uint32_t count) noexcept {
    const uint32_t offset = _nextJointOffset.fetch_add(count, std::memory_order::relaxed);
    if (offset + count > _jointStates.jointBlendWeights.size()) [[unlikely]] {
        ZHLN::LogWarning("[ArticulationSystem] Exceeded maximum joint matrix capacity ({})!", _jointStates.jointBlendWeights.size());
    }
    return offset % _jointStates.jointBlendWeights.size();
}

bool ArticulationSystem::AttachRagdoll(
    Entity rootEntity, ECS::Registry& reg, PhysicsContext& pc, const Skeleton& skeleton, std::span<const Physics::RagdollPartParams> authoredParts,
    uint32_t jointOffset
) {
    if (authoredParts.empty()) {
        return false;
    }

    auto* joltSkel = new JPH::Skeleton();
    for (const auto& joint: skeleton.joints) {
        std::string parentName = (joint.parentIndex >= 0) ? skeleton.joints[joint.parentIndex].name.c_str() : "";
        joltSkel->AddJoint(joint.name.c_str(), parentName);
    }
    joltSkel->CalculateParentJointIndices();

    const std::vector<Physics::RagdollPartParams> parts(authoredParts.begin(), authoredParts.end());

    const auto ragdollHandle = pc.CreateSkeletalRagdoll(joltSkel, parts);
    if (ragdollHandle == Physics::RagdollHandle::Invalid) {
        return false;
    }

    BindSkeleton(jointOffset, skeleton);

    SceneResources::Attach(
        pc, reg, rootEntity, Components::RagdollComponent {
                        .ragdollHandle    = ragdollHandle,
                        .skeletonAsset    = InvalidAssetID,
                        .state            = RagdollState::Inactive,
                        .prevState        = RagdollState::Inactive,
                        .jointOffset      = jointOffset,
                        .jointCount       = static_cast<uint32_t>(skeleton.joints.size()),
                        .isAddedToPhysics = false
                    }
    );
    ZHLN::Log("[ArticulationSystem] Skeletal ragdoll attached to entity {} ({} parts).", rootEntity.index, parts.size());
    return true;
}

void ArticulationSystem::Update(ECS::Query<Components::RagdollComponent&, const Components::PhysicsComponent,
                                            const Components::KinematicPoseOverrideComponent, const Components::SkeletalMeshComponent,
                                            Components::TransformComponent&, const Components::RagdollHitReactionCommand,
                                            const Components::RagdollImpulseCommand> query,
                                 ECS::Registry& registry, ECS::ResMut<ArticulationSystem> articulation,
                                 ECS::ResMut<PhysicsContext> physics, ECS::ResMut<RenderContext> render, FrameDt frameDt) {
    auto& sys = *articulation;
    auto& pc  = *physics;
    auto& rc  = *render;
    const float dt = frameDt.value;

    auto entities = query.Entities<Components::RagdollComponent>();
    auto ragdolls = query.Raw<Components::RagdollComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        Entity                        e       = entities[i];
        Components::RagdollComponent& ragComp = ragdolls[i];
        auto                          phys    = query.Get<Components::PhysicsComponent>(e);

        if (ragComp.ragdollHandle == Physics::RagdollHandle::Invalid) {
            continue;
        }
        auto ragdoll = pc.GetRagdoll(ragComp.ragdollHandle);
        if (!ragdoll) {
            continue;
        }

        uint32_t offset = ragComp.jointOffset;
        uint32_t count  = ragComp.jointCount;

        if (auto hitCmd = query.Get<Components::RagdollHitReactionCommand>(e)) {
            if (hitCmd->jointIndex < count) {
                uint32_t globalIdx                         = offset + hitCmd->jointIndex;
                sys._jointStates.jointBlendWeights[globalIdx] = std::clamp(hitCmd->weight, 0.0f, 1.0f);
                sys._jointStates.jointStiffness[globalIdx]    = std::clamp(hitCmd->stiffness, 0.0f, 1.0f);
                sys._jointStates.jointBlendDecay[globalIdx]   = std::max(0.0f, hitCmd->decayRate);

                ragComp.state = RagdollState::PartialBlend;
            }
            registry.Remove<Components::RagdollHitReactionCommand>(e);
        }

        if (auto impulseCmd = query.Get<Components::RagdollImpulseCommand>(e)) {
            pc.AddRagdollImpulse(ragComp.ragdollHandle, impulseCmd->jointIndex, impulseCmd->impulse);
            registry.Remove<Components::RagdollImpulseCommand>(e);
        }

        bool hasActiveBlend = false;
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t globalIdx = offset + j;
            float    decay     = sys._jointStates.jointBlendDecay[globalIdx];

            if (decay > 0.0f) {
                sys._jointStates.jointBlendWeights[globalIdx] = std::max(0.0f, sys._jointStates.jointBlendWeights[globalIdx] - decay * dt);
                sys._jointStates.jointStiffness[globalIdx]    = std::min(1.0f, sys._jointStates.jointStiffness[globalIdx] + dt * 1.5f);

                if (sys._jointStates.jointBlendWeights[globalIdx] <= 0.0f) {
                    sys._jointStates.jointBlendDecay[globalIdx] = 0.0f;
                }
            }

            if (sys._jointStates.jointBlendWeights[globalIdx] > 0.001f) {
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

        JPH::RVec3 capsuleWorldPos = JPH::RVec3::sZero();
        if (phys && !pc.TryGetBodyPosition(phys->physicsHandle, capsuleWorldPos)) {
            capsuleWorldPos = JPH::RVec3::sZero();
        }

        JPH::SkeletonPose animPose;
        animPose.SetSkeleton(skel);
        animPose.SetRootOffset(capsuleWorldPos);

        JPH::Array<JPH::Mat44> localJoints(count, JPH::Mat44::sIdentity());
        for (uint32_t j = 0; j < count; ++j) {
            localJoints[j] = sys._jointStates.inverseBindMatrices[offset + j].Inversed();
        }

        JPH::Array<JPH::Mat44> modelJoints(count, JPH::Mat44::sIdentity());
        for (uint32_t j = 0; j < count; ++j) {
            int parentIdx = skel->GetJoint(j).mParentJointIndex;
            if (parentIdx >= 0) {
                modelJoints[j] = modelJoints[parentIdx] * localJoints[j];
            } else {
                modelJoints[j] = localJoints[j];
            }
        }

        if (const auto poseOverride = query.Get<Components::KinematicPoseOverrideComponent>(e); poseOverride && poseOverride->valid) {
            const uint32_t overrideCount = std::min<uint32_t>(count, poseOverride->jointCount);
            std::copy_n(poseOverride->modelTransforms.begin(), overrideCount, modelJoints.begin());
        }

        std::memcpy(animPose.GetJointMatrices().data(), modelJoints.data(), count * sizeof(JPH::Mat44));
        animPose.CalculateJointStates();

        if (ragComp.state != ragComp.prevState) {
            if (ragComp.state == RagdollState::Dynamic || ragComp.state == RagdollState::Kinematic || ragComp.state == RagdollState::PartialBlend) {
                if (!ragComp.isAddedToPhysics) {
                    const JPH::Vec3 initialVelocity = phys ? pc.GetCharacterVelocity(phys->physicsHandle) : JPH::Vec3::sZero();
                    pc.ActivateRagdoll(ragComp.ragdollHandle, animPose, initialVelocity);
                    ragComp.isAddedToPhysics = true;
                }
            } else if (ragComp.state == RagdollState::Inactive && ragComp.isAddedToPhysics) {
                pc.RemoveRagdoll(ragComp.ragdollHandle);
                ragComp.isAddedToPhysics = false;
            }
            ragComp.prevState = ragComp.state;
        }

        if (ragComp.state == RagdollState::Kinematic || ragComp.state == RagdollState::PartialBlend) {
            pc.DriveRagdollPose(ragComp.ragdollHandle, animPose);
        }

        if (ragComp.state != RagdollState::Inactive) {
            JPH::Array<JPH::Mat44> physicalWorldJoints(count, JPH::Mat44::sIdentity());
            JPH::RVec3             actualRootOffset = JPH::RVec3::sZero();

            if (!pc.GetRagdollPose(ragComp.ragdollHandle, actualRootOffset, physicalWorldJoints.data())) {
                continue;
            }

            auto allSkinnedEntities = query.Entities<Components::SkeletalMeshComponent>();
            for (Entity childEnt: allSkinnedEntities) {
                auto skelMesh = query.Get<Components::SkeletalMeshComponent>(childEnt);
                if (skelMesh && skelMesh->jointOffset == offset) {
                    if (auto trans = query.Get<Components::TransformComponent>(childEnt)) {
                        trans->position = JPH::Vec3(actualRootOffset);
                        trans->rotation = JPH::Quat::sIdentity();
                    }
                }
            }

            JPH::Array<JPH::Mat44> finalSkinningMatrices(count);
            JPH::Mat44             invRoot = JPH::Mat44::sTranslation(-JPH::Vec3(actualRootOffset));

            for (uint32_t j = 0; j < count; ++j) {
                JPH::Mat44 ibm       = sys._jointStates.inverseBindMatrices[offset + j];
                JPH::Mat44 physModel = invRoot * physicalWorldJoints[j];
                JPH::Mat44 animModel = modelJoints[j];

                float blendWeight = (ragComp.state == RagdollState::Dynamic) ? 1.0f : sys._jointStates.jointBlendWeights[offset + j];

                if (blendWeight <= 0.001f) {
                    finalSkinningMatrices[j] = animModel * ibm;
                } else if (blendWeight >= 0.999f) {
                    finalSkinningMatrices[j] = physModel * ibm;
                } else {
                    const Math::TransformTRS animTRS = Math::Decompose(animModel);
                    const Math::TransformTRS physTRS = Math::Decompose(physModel);
                    const JPH::Vec3&         tAnim   = animTRS.translation;
                    const JPH::Quat&         rAnim   = animTRS.rotation;
                    const JPH::Vec3&         sAnim   = animTRS.scale;
                    const JPH::Vec3&         tPhys   = physTRS.translation;
                    const JPH::Quat&         rPhys   = physTRS.rotation;
                    const JPH::Vec3&         sPhys   = physTRS.scale;

                    JPH::Vec3 tBlended = tAnim + blendWeight * (tPhys - tAnim);
                    JPH::Quat rBlended = rAnim.SLERP(rPhys, blendWeight).Normalized();
                    JPH::Vec3 sBlended = sAnim + blendWeight * (sPhys - sAnim);

                    JPH::Mat44 blendedModel  = JPH::Mat44::sRotationTranslation(rBlended, tBlended).PreScaled(sBlended);
                    finalSkinningMatrices[j] = blendedModel * ibm;
                }
            }

            rc.UpdateJointMatrices(offset, std::span {finalSkinningMatrices.data(), static_cast<size_t>(count)});
        }
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyArticulationStateConsistency(registry);
    }
}

}
