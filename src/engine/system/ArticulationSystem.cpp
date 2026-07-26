// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArticulationSystem.hpp"
#include <Jolt/Jolt.h>
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <cstring>
#include <detail/ControlFlow.hpp>
#include <ecs/ECS.hpp>
#include <physics/Physics.hpp>
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

        if (ragComp.state != RagdollState::Inactive && ragComp.state != RagdollState::Limp && ragComp.state != RagdollState::KeyframeMotor) {
            ZHLN::Log("[Test Fail] Articulation State: Entity {} has invalid ragdoll state {}", e.index, static_cast<int>(ragComp.state));
        }

        if (ragComp.prevState != RagdollState::Inactive && ragComp.prevState != RagdollState::Limp && ragComp.prevState != RagdollState::KeyframeMotor) {
            ZHLN::Log("[Test Fail] Articulation State: Entity {} has invalid prev state {}", e.index, static_cast<int>(ragComp.prevState));
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

void ArticulationSystem::Update(Engine& engine, float dt) {
    (void) dt;
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

        if (ragComp.state != ragComp.prevState) {
            if (ragComp.state == RagdollState::Limp || ragComp.state == RagdollState::KeyframeMotor) {
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

        if (ragComp.state == RagdollState::KeyframeMotor) {
            ZHLN_LOCK(world.sync.shadowLock) {
                ragdoll->Activate();
                ragdoll->DriveToPoseUsingMotors(animPose);
            }
        }

        if (ragComp.state == RagdollState::Limp || ragComp.state == RagdollState::KeyframeMotor) {
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
                JPH::Mat44 ibm           = (j < skeleton.joints.size()) ? skeleton.joints[j].inverseBindMatrix : JPH::Mat44::sIdentity();
                finalSkinningMatrices[j] = invRoot * physicalWorldJoints[j] * ibm;
            }

            rc.UpdateJointMatrices(jointOffset, finalSkinningMatrices.data(), ragComp.jointCount);
        }
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyArticulationStateConsistency(reg);
    }
}

} // namespace ZHLN
