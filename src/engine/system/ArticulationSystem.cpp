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
#include <cgltf.h>
#include <cstring>
#include <detail/ControlFlow.hpp>
#include <ecs/ECS.hpp>
#include <physics/Physics.hpp>
#include <physics/PhysicsWorld.hpp>

namespace ZHLN::Tests {
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

        // Test 1: Ragdoll state is valid enum value
        if (ragComp.state != RagdollState::Inactive && ragComp.state != RagdollState::Limp && ragComp.state != RagdollState::KeyframeMotor) {
            ZHLN::Log("[Test Fail] Articulation State: Entity {} has invalid ragdoll state {}", e.index, static_cast<int>(ragComp.state));
        }

        // Test 2: Previous state is valid
        if (ragComp.prevState != RagdollState::Inactive && ragComp.prevState != RagdollState::Limp && ragComp.prevState != RagdollState::KeyframeMotor) {
            ZHLN::Log("[Test Fail] Articulation State: Entity {} has invalid prev state {}", e.index, static_cast<int>(ragComp.prevState));
        }

        // Test 3: isAddedToPhysics is boolean
        if (ragComp.isAddedToPhysics != 0 && ragComp.isAddedToPhysics != 1) {
            ZHLN::Log("[Test Fail] Articulation State: Entity {} isAddedToPhysics invalid: {}", e.index, ragComp.isAddedToPhysics);
        }

        // Test 4: Joint count is reasonable
        if (ragComp.jointCount > 2000 || ragComp.jointCount == 0) {
            ZHLN::Log("[Test Fail] Articulation State: Entity {} has unreasonable joint count: {}", e.index, ragComp.jointCount);
        }
    }
}
} // namespace ZHLN::Tests

namespace ZHLN {

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

        if ((ragComp.ragdollInstance == nullptr) || (ragComp.gltfSkin == nullptr)) {
            continue;
        }

        if (ragComp.state == RagdollState::Inactive && ragComp.prevState == RagdollState::Inactive) {
            continue;
        }

        JPH::Ragdoll*        ragdoll = ragComp.ragdollInstance;
        const JPH::Skeleton* skel    = ragdoll->GetRagdollSettings()->GetSkeleton();
        const auto*          skin    = static_cast<const cgltf_skin*>(ragComp.gltfSkin);

        // ====================================================================
        // 1. QUERY CAPSULE WORLD POSITION & SET ROOT OFFSET
        // ====================================================================
        JPH::RVec3 capsuleWorldPos = JPH::RVec3::sZero();
        if (phys != nullptr) {
            uint32_t     dense = world.slotToDense[phys->physicsHandle.index];
            const size_t base  = static_cast<size_t>(dense) * 4;
            capsuleWorldPos    = JPH::RVec3(world.positions[base], world.positions[base + 1], world.positions[base + 2]);
        }

        JPH::SkeletonPose animPose;
        animPose.SetSkeleton(skel);
        animPose.SetRootOffset(capsuleWorldPos); // <-- Set root to player's actual position!

        JPH::Array<JPH::Mat44> localJoints(ragComp.jointCount, JPH::Mat44::sIdentity());
        for (uint32_t j = 0; j < ragComp.jointCount; ++j) {
            // High-performance O(1) index-based joint node mapping
            const cgltf_node* jointNode = (j < skin->joints_count) ? skin->joints[j] : nullptr;
            if (jointNode != nullptr) {
                float m[16];
                cgltf_node_transform_local(jointNode, m);
                localJoints[j] = JPH::Mat44(
                    JPH::Vec4(m[0], m[1], m[2], m[3]), JPH::Vec4(m[4], m[5], m[6], m[7]), JPH::Vec4(m[8], m[9], m[10], m[11]),
                    JPH::Vec4(m[12], m[13], m[14], m[15])
                );
            }
        }

        // Propagate local-space matrices into model-space hierarchical matrices
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
        // ====================================================================
        // 2. STATE TRANSITIONS & MOMENTUM INHERITANCE
        // ====================================================================
        if (ragComp.state != ragComp.prevState) {
            if (ragComp.state == RagdollState::Limp || ragComp.state == RagdollState::KeyframeMotor) {
                if (ragComp.isAddedToPhysics == 0) { // <-- UPDATED (Explicit comparison)
                    ZHLN_LOCK(world.sync.shadowLock) {
                        ragdoll->AddToPhysicsSystem(JPH::EActivation::Activate);

                        if (phys != nullptr) {
                            JPH::Vec3 charVel = Physics::GetCharacterVelocity(engine.GetPhysicsContext(), phys->physicsHandle);
                            ragdoll->SetPose(animPose); // Correctly aligns visual/physical poses on transition
                            ragdoll->SetLinearAndAngularVelocity(charVel, JPH::Vec3::sZero());
                        }
                        ragComp.isAddedToPhysics = 1; // <-- UPDATED (Clean cast)
                    }
                }
            } else if (ragComp.state == RagdollState::Inactive && ragComp.isAddedToPhysics != 0) { // <-- UPDATED (Explicit comparison)
                ZHLN_LOCK(world.sync.shadowLock) {
                    ragdoll->RemoveFromPhysicsSystem();
                    ragComp.isAddedToPhysics = 0; // <-- UPDATED (Clean cast)
                }
            }
            ragComp.prevState = ragComp.state;
        }

        // ====================================================================
        // 3. EXECUTE ACTIVE PHYSICS
        // ====================================================================
        if (ragComp.state == RagdollState::KeyframeMotor) {
            ZHLN_LOCK(world.sync.shadowLock) {
                ragdoll->Activate();
                ragdoll->DriveToPoseUsingMotors(animPose);
            }
        }

        // ====================================================================
        // 4. READ-BACK & GPU INVERSE-BIND SKINNING
        // ====================================================================
        if (ragComp.state == RagdollState::Limp || ragComp.state == RagdollState::KeyframeMotor) {
            JPH::Array<JPH::Mat44> physicalWorldJoints(ragComp.jointCount, JPH::Mat44::sIdentity());
            JPH::RVec3             actualRootOffset = JPH::RVec3::sZero();

            ZHLN_LOCK(world.sync.shadowLock) {
                ragdoll->GetPose(actualRootOffset, physicalWorldJoints.data());
            }

            // Resolve jointOffset and update the Components::TransformComponent of visual meshes
            uint32_t jointOffset = ragComp.jointOffset;
            auto     allEntities = reg.GetEntitiesWith<Components::MeshComponent>();
            auto     allMeshes   = reg.GetRawArray<Components::MeshComponent>();
            for (size_t k = 0; k < allEntities.size(); ++k) {
                if (allMeshes[k].gltfSkin == skin) {
                    jointOffset = allMeshes[k].jointOffset;
                    if (auto* trans = reg.Get<Components::Components::TransformComponent>(allEntities[k])) {
                        trans->position = JPH::Vec3(actualRootOffset);
                        trans->rotation = JPH::Quat::sIdentity();
                    }
                }
            }

            // Calculate final skinning matrices: Final = InvRoot * PhysicalWorld *
            // InverseBindMatrix This transforms from mesh-local to root-relative space, which is
            // then brought to world space by the mesh's Components::TransformComponent (positioned
            // at actualRootOffset).
            JPH::Array<JPH::Mat44> finalSkinningMatrices(ragComp.jointCount);
            JPH::Mat44             invRoot = JPH::Mat44::sTranslation(-JPH::Vec3(actualRootOffset));

            for (uint32_t j = 0; j < ragComp.jointCount; ++j) {
                JPH::Mat44 ibm = JPH::Mat44::sIdentity();
                if (skin->inverse_bind_matrices != nullptr) {
                    float ibmRaw[16];
                    cgltf_accessor_read_float(skin->inverse_bind_matrices, j, ibmRaw, 16);
                    ibm = JPH::Mat44(
                        JPH::Vec4(ibmRaw[0], ibmRaw[1], ibmRaw[2], ibmRaw[3]), JPH::Vec4(ibmRaw[4], ibmRaw[5], ibmRaw[6], ibmRaw[7]),
                        JPH::Vec4(ibmRaw[8], ibmRaw[9], ibmRaw[10], ibmRaw[11]), JPH::Vec4(ibmRaw[12], ibmRaw[13], ibmRaw[14], ibmRaw[15])
                    );
                }
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
