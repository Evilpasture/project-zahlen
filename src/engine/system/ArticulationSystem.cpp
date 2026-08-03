// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArticulationSystem.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/ControlFlow.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN::Tests {

static void VerifyArticulationStateConsistency(const ECS::Registry& reg) noexcept {
    static bool testsRun = false;
    if (testsRun)
        return;
    testsRun = true;

    auto entities = reg.GetEntitiesWith<Components::RagdollComponent>();
    auto ragdolls = reg.GetRawArray<Components::RagdollComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        Entity      e       = entities[i];
        const auto& ragComp = ragdolls[i];

        if (ragComp.state != RagdollState::Inactive && ragComp.state != RagdollState::Limp && ragComp.state != RagdollState::KeyframeMotor &&
            ragComp.state != RagdollState::PartialBlend) {
            ZHLN::Log("[Test Fail] Articulation State: Entity {} has invalid ragdoll state {}", e.index, static_cast<int>(ragComp.state));
        }

        if (ragComp.isAddedToPhysics != 0 && ragComp.isAddedToPhysics != 1) {
            ZHLN::Log("[Test Fail] Articulation State: Entity {} isAddedToPhysics invalid: {}", e.index, ragComp.isAddedToPhysics);
        }

        if (ragComp.jointCount > 2000 || ragComp.jointCount == 0) {
            ZHLN::Log("[Test Fail] Articulation State: Entity {} has unreasonable joint count: {}", e.index, ragComp.jointCount);
        }
    }
}

} // namespace ZHLN::Tests

namespace ZHLN {

namespace {

void DecomposeMatrix(const JPH::Mat44& mat, JPH::Vec3& outT, JPH::Quat& outR, JPH::Vec3& outS) noexcept {
    outT         = mat.GetTranslation();
    JPH::Vec3 c0 = mat.GetColumn3(0);
    JPH::Vec3 c1 = mat.GetColumn3(1);
    JPH::Vec3 c2 = mat.GetColumn3(2);
    outS         = JPH::Vec3(c0.Length(), c1.Length(), c2.Length());

    if (outS.GetX() > 1e-5f) {
        c0 /= outS.GetX();
    } else {
        c0 = JPH::Vec3::sAxisX();
    }
    if (outS.GetY() > 1e-5f) {
        c1 /= outS.GetY();
    } else {
        c1 = JPH::Vec3::sAxisY();
    }
    if (outS.GetZ() > 1e-5f) {
        c2 /= outS.GetZ();
    } else {
        c2 = JPH::Vec3::sAxisZ();
    }

    outR = JPH::Mat44(JPH::Vec4(c0, 0), JPH::Vec4(c1, 0), JPH::Vec4(c2, 0), JPH::Vec4(0, 0, 0, 1)).GetQuaternion().Normalized();
}

} // namespace

void ArticulationSystem::Update(Engine& engine, float dt) {
    auto&       reg   = engine.GetRegistry();
    const auto& world = engine.GetPhysicsContext().GetWorld();
    auto&       rc    = engine.GetRenderContext();

    auto entities = reg.GetEntitiesWith<Components::RagdollComponent>();
    auto ragdolls = reg.GetRawArray<Components::RagdollComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        Entity                        e       = entities[i];
        Components::RagdollComponent& ragComp = ragdolls[i];
        auto*                         phys    = reg.Get<Components::PhysicsComponent>(e);

        if (ragComp.ragdollInstance == nullptr || ragComp.skeleton == nullptr) {
            continue;
        }

        ragComp.EnsureJointCapacity(ragComp.jointCount);

        // --- 1. PER-JOINT BLEND & STIFFNESS DECAY ---
        bool hasActiveBlend = false;
        for (uint32_t j = 0; j < ragComp.jointCount; ++j) {
            if (ragComp.jointBlendDecay[j] > 0.0f) {
                ragComp.jointBlendWeights[j] = std::max(0.0f, ragComp.jointBlendWeights[j] - ragComp.jointBlendDecay[j] * dt);
                ragComp.jointStiffness[j]    = std::min(1.0f, ragComp.jointStiffness[j] + dt * 1.5f);

                if (ragComp.jointBlendWeights[j] <= 0.0f) {
                    ragComp.jointBlendDecay[j] = 0.0f;
                }
            }

            if (ragComp.jointBlendWeights[j] > 0.001f) {
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

        JPH::Ragdoll*        ragdoll  = ragComp.ragdollInstance;
        const JPH::Skeleton* skel     = ragdoll->GetRagdollSettings()->GetSkeleton();
        const Skeleton&      skeleton = *ragComp.skeleton;

        JPH::RVec3 capsuleWorldPos = JPH::RVec3::sZero();
        if (phys != nullptr) {
            uint32_t     dense = world.slotToDense[phys->physicsHandle.index];
            const size_t base  = static_cast<size_t>(dense) * 4;
            capsuleWorldPos    = JPH::RVec3(world.positions[base], world.positions[base + 1], world.positions[base + 2]);
        }

        JPH::SkeletonPose animPose;
        animPose.SetSkeleton(skel);
        animPose.SetRootOffset(capsuleWorldPos);

        JPH::Array<JPH::Mat44> localJoints(ragComp.jointCount, JPH::Mat44::sIdentity());
        for (uint32_t j = 0; j < ragComp.jointCount; ++j) {
            if (j < skeleton.joints.size()) {
                localJoints[j] = skeleton.joints[j].inverseBindMatrix.Inversed();
            }
        }

        JPH::Array<JPH::Mat44> modelJoints(ragComp.jointCount, JPH::Mat44::sIdentity());
        for (uint32_t j = 0; j < ragComp.jointCount; ++j) {
            int parentIdx = skel->GetJoint(j).mParentJointIndex;
            if (parentIdx >= 0) {
                modelJoints[j] = modelJoints[parentIdx] * localJoints[j];
            } else {
                modelJoints[j] = localJoints[j];
            }
        }

        std::memcpy(animPose.GetJointMatrices().data(), modelJoints.data(), ragComp.jointCount * sizeof(JPH::Mat44));
        animPose.CalculateJointStates();

        // --- 2. PHYSICS ACTIVATION / DEACTIVATION TRANSITIONS ---
        if (ragComp.state != ragComp.prevState) {
            if (ragComp.state == RagdollState::Limp || ragComp.state == RagdollState::KeyframeMotor || ragComp.state == RagdollState::PartialBlend) {
                if (ragComp.isAddedToPhysics == 0) {
                    ZHLN_LOCK(world.sync.shadowLock) {
                        ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);
                        if (phys != nullptr) {
                            JPH::Vec3 charVel = Physics::GetCharacterVelocity(engine.GetPhysicsContext(), phys->physicsHandle);
                            ragdoll->SetPose(animPose);
                            ragdoll->SetLinearAndAngularVelocity(charVel, JPH::Vec3::sZero());
                        }
                        ragComp.isAddedToPhysics = 1;
                    }
                }
            } else if (ragComp.state == RagdollState::Inactive && ragComp.isAddedToPhysics != 0) {
                ZHLN_LOCK(world.sync.shadowLock) {
                    ragdoll->RemoveFromPhysicsSystem();
                    ragComp.isAddedToPhysics = 0;
                }
            }
            ragComp.prevState = ragComp.state;
        }

        // --- 3. MOTOR DRIVE ---
        if (ragComp.state == RagdollState::KeyframeMotor || ragComp.state == RagdollState::PartialBlend) {
            ZHLN_LOCK(world.sync.shadowLock) {
                ragdoll->Activate();
                ragdoll->DriveToPoseUsingMotors(animPose);
            }
        }

        // --- 4. PER-JOINT POSE SLERP/LERP BLENDING ---
        if (ragComp.state != RagdollState::Inactive) {
            JPH::Array<JPH::Mat44> physicalWorldJoints(ragComp.jointCount, JPH::Mat44::sIdentity());
            JPH::RVec3             actualRootOffset = JPH::RVec3::sZero();

            ZHLN_LOCK(world.sync.shadowLock) {
                ragdoll->GetPose(actualRootOffset, physicalWorldJoints.data());
            }

            uint32_t jointOffset = ragComp.jointOffset;
            auto     allEntities = reg.GetEntitiesWith<Components::MeshComponent>();
            auto     allMeshes   = reg.GetRawArray<Components::MeshComponent>();
            for (size_t k = 0; k < allEntities.size(); ++k) {
                if (allMeshes[k].skeletonIndex >= 0) {
                    auto* anim = reg.Get<Components::AnimatorComponent>(allEntities[k]);
                    if (anim && anim->prefab && &anim->prefab->skeletons[allMeshes[k].skeletonIndex] == ragComp.skeleton) {
                        jointOffset = allMeshes[k].jointOffset;
                        if (auto* trans = reg.Get<Components::TransformComponent>(allEntities[k])) {
                            trans->position = JPH::Vec3(actualRootOffset);
                            trans->rotation = JPH::Quat::sIdentity();
                        }
                    }
                }
            }

            JPH::Array<JPH::Mat44> finalSkinningMatrices(ragComp.jointCount);
            JPH::Mat44             invRoot = JPH::Mat44::sTranslation(-JPH::Vec3(actualRootOffset));

            for (uint32_t j = 0; j < ragComp.jointCount; ++j) {
                JPH::Mat44 ibm       = (j < skeleton.joints.size()) ? skeleton.joints[j].inverseBindMatrix : JPH::Mat44::sIdentity();
                JPH::Mat44 physModel = invRoot * physicalWorldJoints[j];
                JPH::Mat44 animModel = modelJoints[j];

                float blendWeight = (ragComp.state == RagdollState::Limp) ? 1.0f : ragComp.jointBlendWeights[j];

                if (blendWeight <= 0.001f) {
                    finalSkinningMatrices[j] = animModel * ibm;
                } else if (blendWeight >= 0.999f) {
                    finalSkinningMatrices[j] = physModel * ibm;
                } else {
                    JPH::Vec3 tAnim, sAnim, tPhys, sPhys;
                    JPH::Quat rAnim, rPhys;

                    DecomposeMatrix(animModel, tAnim, rAnim, sAnim);
                    DecomposeMatrix(physModel, tPhys, rPhys, sPhys);

                    JPH::Vec3 tBlended = tAnim + blendWeight * (tPhys - tAnim);
                    JPH::Quat rBlended = rAnim.SLERP(rPhys, blendWeight).Normalized();
                    JPH::Vec3 sBlended = sAnim + blendWeight * (sPhys - sAnim);

                    JPH::Mat44 blendedModel  = JPH::Mat44::sRotationTranslation(rBlended, tBlended).PreScaled(sBlended);
                    finalSkinningMatrices[j] = blendedModel * ibm;
                }
            }

            rc.UpdateJointMatrices(jointOffset, finalSkinningMatrices.data(), ragComp.jointCount);
        }
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyArticulationStateConsistency(reg);
    }
}

void ArticulationSystem::ApplyHitImpulse(
    Engine&          engine,
    Entity           entity,
    uint32_t         jointIdx,
    const JPH::Vec3& impulse,
    float            weight,
    float            stiffness,
    float            decayRate
) noexcept {
    auto& reg     = engine.GetRegistry();
    auto* ragComp = reg.Get<Components::RagdollComponent>(entity);
    if (ragComp == nullptr || ragComp->ragdollInstance == nullptr) {
        return;
    }

    ragComp->ApplyHitReaction(jointIdx, weight, stiffness, decayRate);

    // Apply physical impulse directly to the corresponding Jolt body
    if (jointIdx < ragComp->ragdollInstance->GetBodyCount()) {
        JPH::BodyID bodyID = ragComp->ragdollInstance->GetBodyID(jointIdx);
        if (!bodyID.IsInvalid()) {
            engine.GetPhysicsContext().GetWorld().bodyInterface->AddImpulse(bodyID, impulse);
            engine.GetPhysicsContext().GetWorld().bodyInterface->ActivateBody(bodyID);
        }
    }
}

} // namespace ZHLN
