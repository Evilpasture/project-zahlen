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
#include <cstring>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN {

GlobalJointStateBuffer g_JointStates;

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
} // namespace Tests

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

void ArticulationSystem::BindSkeleton(uint32_t jointOffset, const Skeleton& skeleton) noexcept {
    for (size_t i = 0; i < skeleton.joints.size(); ++i) {
        g_JointStates.inverseBindMatrices[jointOffset + i] = skeleton.joints[i].inverseBindMatrix;
    }
}

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

        if (ragComp.ragdollInstance == nullptr) {
            continue;
        }

        uint32_t offset = ragComp.jointOffset;
        uint32_t count  = ragComp.jointCount;

        if (auto* hitCmd = reg.Get<Components::RagdollHitReactionCommand>(e)) {
            if (hitCmd->jointIndex < count) {
                uint32_t globalIdx                         = offset + hitCmd->jointIndex;
                g_JointStates.jointBlendWeights[globalIdx] = std::clamp(hitCmd->weight, 0.0f, 1.0f);
                g_JointStates.jointStiffness[globalIdx]    = std::clamp(hitCmd->stiffness, 0.0f, 1.0f);
                g_JointStates.jointBlendDecay[globalIdx]   = std::max(0.0f, hitCmd->decayRate);

                ragComp.state = RagdollState::PartialBlend;
            }
            reg.Remove<Components::RagdollHitReactionCommand>(e);
        }

        if (auto* impulseCmd = reg.Get<Components::RagdollImpulseCommand>(e)) {
            if (impulseCmd->jointIndex < ragComp.ragdollInstance->GetBodyCount()) {
                JPH::BodyID bodyID = ragComp.ragdollInstance->GetBodyID(impulseCmd->jointIndex);
                if (!bodyID.IsInvalid()) {
                    ZHLN::Lock(world.sync.shadowLock, [&] {
                        world.bodyInterface->AddImpulse(bodyID, impulseCmd->impulse);
                        world.bodyInterface->ActivateBody(bodyID);
                    });
                }
            }
            reg.Remove<Components::RagdollImpulseCommand>(e);
        }

        bool hasActiveBlend = false;
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t globalIdx = offset + j;
            float    decay     = g_JointStates.jointBlendDecay[globalIdx];

            if (decay > 0.0f) {
                g_JointStates.jointBlendWeights[globalIdx] = std::max(0.0f, g_JointStates.jointBlendWeights[globalIdx] - decay * dt);
                g_JointStates.jointStiffness[globalIdx]    = std::min(1.0f, g_JointStates.jointStiffness[globalIdx] + dt * 1.5f);

                if (g_JointStates.jointBlendWeights[globalIdx] <= 0.0f) {
                    g_JointStates.jointBlendDecay[globalIdx] = 0.0f;
                }
            }

            if (g_JointStates.jointBlendWeights[globalIdx] > 0.001f) {
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

        JPH::Ragdoll*        ragdoll = ragComp.ragdollInstance;
        const JPH::Skeleton* skel    = ragdoll->GetRagdollSettings()->GetSkeleton();

        JPH::RVec3 capsuleWorldPos = JPH::RVec3::sZero();
        if (phys != nullptr) {
            uint32_t     dense = world.slotToDense[phys->physicsHandle.index];
            const size_t base  = static_cast<size_t>(dense) * 4;
            capsuleWorldPos    = JPH::RVec3(world.positions[base], world.positions[base + 1], world.positions[base + 2]);
        }

        JPH::SkeletonPose animPose;
        animPose.SetSkeleton(skel);
        animPose.SetRootOffset(capsuleWorldPos);

        JPH::Array<JPH::Mat44> localJoints(count, JPH::Mat44::sIdentity());
        for (uint32_t j = 0; j < count; ++j) {
            localJoints[j] = g_JointStates.inverseBindMatrices[offset + j].Inversed();
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

        std::memcpy(animPose.GetJointMatrices().data(), modelJoints.data(), count * sizeof(JPH::Mat44));
        animPose.CalculateJointStates();

        if (ragComp.state != ragComp.prevState) {
            if (ragComp.state == RagdollState::Dynamic || ragComp.state == RagdollState::Kinematic || ragComp.state == RagdollState::PartialBlend) {
                if (!ragComp.isAddedToPhysics) {
                    ZHLN::Lock(world.sync.shadowLock, [&] {
                        ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);
                        if (phys != nullptr) {
                            // FIXED: Use context instance method call correctly
                            JPH::Vec3 charVel = engine.GetPhysicsContext().GetCharacterVelocity(phys->physicsHandle);
                            ragdoll->SetPose(animPose);
                            ragdoll->SetLinearAndAngularVelocity(charVel, JPH::Vec3::sZero());
                        }
                        ragComp.isAddedToPhysics = true;
                    });
                }
            } else if (ragComp.state == RagdollState::Inactive && ragComp.isAddedToPhysics) {
                ZHLN::Lock(world.sync.shadowLock, [&] {
                    ragdoll->RemoveFromPhysicsSystem();
                    ragComp.isAddedToPhysics = false;
                });
            }
            ragComp.prevState = ragComp.state;
        }

        if (ragComp.state == RagdollState::Kinematic || ragComp.state == RagdollState::PartialBlend) {
            ZHLN::Lock(world.sync.shadowLock, [&] {
                ragdoll->Activate();
                ragdoll->DriveToPoseUsingMotors(animPose);
            });
        }

        if (ragComp.state != RagdollState::Inactive) {
            JPH::Array<JPH::Mat44> physicalWorldJoints(count, JPH::Mat44::sIdentity());
            JPH::RVec3             actualRootOffset = JPH::RVec3::sZero();

            ZHLN::Lock(world.sync.shadowLock, [&] { ragdoll->GetPose(actualRootOffset, physicalWorldJoints.data()); });

            auto allSkinnedEntities = reg.GetEntitiesWith<Components::SkeletalMeshComponent>();
            for (Entity childEnt: allSkinnedEntities) {
                auto* skelMesh = reg.Get<Components::SkeletalMeshComponent>(childEnt);
                if (skelMesh != nullptr && skelMesh->jointOffset == offset) {
                    if (auto* trans = reg.Get<Components::TransformComponent>(childEnt)) {
                        trans->position = JPH::Vec3(actualRootOffset);
                        trans->rotation = JPH::Quat::sIdentity();
                    }
                }
            }

            JPH::Array<JPH::Mat44> finalSkinningMatrices(count);
            JPH::Mat44             invRoot = JPH::Mat44::sTranslation(-JPH::Vec3(actualRootOffset));

            for (uint32_t j = 0; j < count; ++j) {
                JPH::Mat44 ibm       = g_JointStates.inverseBindMatrices[offset + j];
                JPH::Mat44 physModel = invRoot * physicalWorldJoints[j];
                JPH::Mat44 animModel = modelJoints[j];

                float blendWeight = (ragComp.state == RagdollState::Dynamic) ? 1.0f : g_JointStates.jointBlendWeights[offset + j];

                if (blendWeight <= 0.001f) {
                    finalSkinningMatrices[j] = animModel * ibm;
                } else if (blendWeight >= 0.999f) {
                    finalSkinningMatrices[j] = physModel * ibm;
                } else {
                    JPH::Vec3 tAnim {};
                    JPH::Vec3 sAnim {};
                    JPH::Vec3 tPhys {};
                    JPH::Vec3 sPhys {};
                    JPH::Quat rAnim {};
                    JPH::Quat rPhys {};

                    DecomposeMatrix(animModel, tAnim, rAnim, sAnim);
                    DecomposeMatrix(physModel, tPhys, rPhys, sPhys);

                    JPH::Vec3 tBlended = tAnim + blendWeight * (tPhys - tAnim);
                    JPH::Quat rBlended = rAnim.SLERP(rPhys, blendWeight).Normalized();
                    JPH::Vec3 sBlended = sAnim + blendWeight * (sPhys - sAnim);

                    JPH::Mat44 blendedModel  = JPH::Mat44::sRotationTranslation(rBlended, tBlended).PreScaled(sBlended);
                    finalSkinningMatrices[j] = blendedModel * ibm;
                }
            }

            rc.UpdateJointMatrices(offset, finalSkinningMatrices.data(), count);
        }
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyArticulationStateConsistency(reg);
    }
}

} // namespace ZHLN
