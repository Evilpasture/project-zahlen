// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArticulationSystem.hpp"
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Skeleton/Skeleton.h>
#include <Jolt/Skeleton/SkeletonPose.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/SystemContext.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cstring>
#include <string>

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
} // namespace Tests

void ArticulationSystem::ReleaseTracked(ECS::Registry& registry, PhysicsContext& physics, size_t index) noexcept {
    TrackedRagdoll& tracked = _tracked[index];
    if (tracked.instance != nullptr && tracked.isAddedToPhysics) {
        physics.RemoveRagdoll(*tracked.instance.GetPtr());
    }

    if (auto* component = registry.Get<Components::RagdollComponent>(tracked.owner);
        component != nullptr && component->ragdollInstance.GetPtr() == tracked.instance.GetPtr()) {
        component->isAddedToPhysics = false;
    }
    _tracked.erase(_tracked.begin() + static_cast<std::ptrdiff_t>(index));
}

void ArticulationSystem::Track(Entity owner, const Components::RagdollComponent& component) {
    if (component.ragdollInstance == nullptr) {
        return;
    }

    for (auto& tracked: _tracked) {
        if (tracked.owner == owner) {
            if (tracked.instance.GetPtr() == component.ragdollInstance.GetPtr()) {
                tracked.isAddedToPhysics = component.isAddedToPhysics;
                return;
            }
            // A replacement component can arrive without an ECS lifecycle
            // callback. The old reference remains tracked until Reconcile
            // removes its Jolt registration on the next update.
            return;
        }
    }

    _tracked.push_back(
        {.owner = owner, .instance = JPH::Ref<JPH::Ragdoll>(component.ragdollInstance.GetPtr()), .isAddedToPhysics = component.isAddedToPhysics}
    );
}

void ArticulationSystem::Reconcile(ECS::Registry& registry, PhysicsContext& physics) noexcept {
    for (size_t index = 0; index < _tracked.size();) {
        const TrackedRagdoll& tracked   = _tracked[index];
        const auto* current = registry.Get<Components::RagdollComponent>(tracked.owner);
        if (!registry.IsAlive(tracked.owner) || current == nullptr || current->ragdollInstance.GetPtr() != tracked.instance.GetPtr()) {
            ReleaseTracked(registry, physics, index);
        } else {
            _tracked[index].isAddedToPhysics = current->isAddedToPhysics;
            ++index;
        }
    }
}

void ArticulationSystem::Release(Engine& engine, Entity owner) noexcept {
    for (size_t index = 0; index < _tracked.size();) {
        if (_tracked[index].owner == owner) {
            ReleaseTracked(engine.GetRegistry(), engine.GetPhysicsContext(), index);
        } else {
            ++index;
        }
    }

    // A component can be explicitly despawned before its first system update.
    // It cannot have been activated by ArticulationSystem yet, but handle a
    // manually activated component defensively without relying on component lifecycle callbacks.
    if (auto* component = engine.GetRegistry().Get<Components::RagdollComponent>(owner);
        component != nullptr && component->ragdollInstance != nullptr && component->isAddedToPhysics) {
        engine.GetPhysicsContext().RemoveRagdoll(*component->ragdollInstance.GetPtr());
        component->isAddedToPhysics = false;
    }
}

void ArticulationSystem::Shutdown(Engine& engine) noexcept {
    // A component could have been replaced between frames. First discard stale
    // ledger entries, then capture every current component before releasing
    // registrations while the PhysicsContext is still available.
    Reconcile(engine.GetRegistry(), engine.GetPhysicsContext());
    const auto owners = engine.GetRegistry().GetEntitiesWith<Components::RagdollComponent>();
    for (const Entity owner: owners) {
        if (const auto* component = engine.GetRegistry().Get<Components::RagdollComponent>(owner); component != nullptr) {
            Track(owner, *component);
        }
    }
    while (!_tracked.empty()) {
        ReleaseTracked(engine.GetRegistry(), engine.GetPhysicsContext(), _tracked.size() - 1);
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
        ZHLN::Log("[ArticulationSystem] WARNING: Exceeded maximum joint matrix capacity ({})!", _jointStates.jointBlendWeights.size());
    }
    return offset % _jointStates.jointBlendWeights.size();
}

// TODO(Approach B -- split authoring from simulation): the joint-name
// heuristics below ("pelvis", "spine", "head", ...) and the capsule/sphere
// dimensions they pick are procedural ragdoll *authoring*, not articulation,
// and they are wrong for anything whose joints are not named like a human.
// They belong in a standalone builder -- e.g. RagdollGenerator::Build(const
// Skeleton&, PhysicsContext&) -> JPH::Ref<JPH::Ragdoll> -- which cooked
// collider data or a gameplay layer can replace without touching this system.
// ArticulationSystem would then only wire the result through an
// AttachRagdoll(Entity, JPH::Ref<JPH::Ragdoll>, const Skeleton&) that adds the
// component and binds the matrices. Moved here from
// PrefabFactory::SetupPlayerRagdoll unchanged so behaviour stays identical;
// this TODO is the follow-up that removes the string matching.
bool ArticulationSystem::BuildRagdoll(Entity rootEntity, ECS::Registry& reg, PhysicsContext& pc) {
    // The root owns the rig description: its AnimatorComponent holds the
    // prefab the skeletons live in.
    const auto* animComp = reg.Get<Components::AnimatorComponent>(rootEntity);
    if (animComp == nullptr || animComp->prefab == nullptr) {
        ZHLN::Log("[ArticulationSystem] WARNING: BuildRagdoll found no AnimatorComponent with a prefab on entity {}.", rootEntity.index);
        return false;
    }

    // One of the root's skinned children carries the rest: which skeleton of
    // that prefab, and the joint offset its matrices were allocated at. The
    // caller used to supply this list; the registry already knows it.
    const Skeleton* targetSkeleton = nullptr;
    uint32_t        jointOffset    = 0;
    for (const Entity child: reg.GetEntitiesWith<Components::SkeletalMeshComponent>()) {
        const auto* skelMesh = reg.Get<Components::SkeletalMeshComponent>(child);
        const auto* hier     = reg.Get<Components::HierarchyComponent>(child);
        if (skelMesh == nullptr || hier == nullptr || hier->parent != rootEntity) {
            continue;
        }
        if (skelMesh->skeletonIndex < 0 || static_cast<size_t>(skelMesh->skeletonIndex) >= animComp->prefab->skeletons.size()) {
            continue;
        }
        targetSkeleton = &animComp->prefab->skeletons[static_cast<size_t>(skelMesh->skeletonIndex)];
        jointOffset    = skelMesh->jointOffset;
        break;
    }

    if (targetSkeleton == nullptr) {
        ZHLN::Log("[ArticulationSystem] WARNING: BuildRagdoll found no skinned child of entity {}.", rootEntity.index);
        return false;
    }

    auto* joltSkel = new JPH::Skeleton();
    for (const auto& joint: targetSkeleton->joints) {
        std::string parentName = (joint.parentIndex >= 0) ? targetSkeleton->joints[joint.parentIndex].name.c_str() : "";
        joltSkel->AddJoint(joint.name.c_str(), parentName);
    }
    joltSkel->CalculateParentJointIndices();

    auto IsImportantJoint = [](std::string name) -> bool {
        std::ranges::transform(name, name.begin(), ::tolower);
        return name.contains("hip") || name.contains("pelvis") || name.contains("root") || name.contains("spine") || name.contains("chest") ||
               name.contains("torso") || name.contains("head") || name.contains("neck") || name.contains("arm") || name.contains("forearm") ||
               name.contains("thigh") || name.contains("calf") || name.contains("shin");
    };

    std::vector<Physics::RagdollPartParams> parts;
    for (size_t i = 0; i < targetSkeleton->joints.size(); ++i) {
        std::string name = targetSkeleton->joints[i].name.c_str();

        Physics::RagdollPartParams part;
        part.jointIndex       = static_cast<uint32_t>(i);
        part.parentJointIndex = targetSkeleton->joints[i].parentIndex;
        part.mass             = 1.0f;
        part.enableMotors     = false;

        JPH::Mat44 bindPose = targetSkeleton->joints[i].inverseBindMatrix.Inversed();
        part.position       = JPH::RVec3(bindPose.GetTranslation());
        part.rotation       = bindPose.GetQuaternion().Normalized();

        std::ranges::transform(name, name.begin(), ::tolower);
        if (name.contains("hip") || name.contains("pelvis") || name.contains("root")) {
            part.shape = pc.GetOrCreateShape(Physics::ShapeType::Capsule, 0.4f, 0.2f);
            part.mass  = 15.0f;
        } else if (name.contains("spine") || name.contains("chest") || name.contains("torso")) {
            part.shape         = pc.GetOrCreateShape(Physics::ShapeType::Capsule, 0.5f, 0.25f);
            part.mass          = 20.0f;
            part.enableMotors  = true;
            part.maxMotorForce = 250.0f;
        } else if (name.contains("head") || name.contains("neck")) {
            part.shape         = pc.GetOrCreateShape(Physics::ShapeType::Sphere, 0.3f);
            part.mass          = 8.0f;
            part.enableMotors  = true;
            part.maxMotorForce = 250.0f;
        } else if (IsImportantJoint(name)) {
            part.shape = pc.GetOrCreateShape(Physics::ShapeType::Capsule, 0.2f, 0.1f);
            part.mass  = 3.0f;
        } else {
            part.shape = pc.GetOrCreateShape(Physics::ShapeType::Sphere, 0.08f);
            part.mass  = 0.5f;
        }
        parts.push_back(part);
    }

    auto ragdollInstance = pc.CreateSkeletalRagdoll(joltSkel, parts);
    ragdollInstance->AddRef();

    BindSkeleton(jointOffset, *targetSkeleton);

    reg.Add(
        rootEntity, Components::RagdollComponent {
                        .ragdollInstance  = ragdollInstance.GetPtr(),
                        .skeletonAsset    = InvalidAssetID,
                        .state            = RagdollState::Inactive,
                        .prevState        = RagdollState::Inactive,
                        .jointOffset      = jointOffset,
                        .jointCount       = static_cast<uint32_t>(targetSkeleton->joints.size()),
                        .isAddedToPhysics = false
                    }
    );
    ZHLN::Log("[ArticulationSystem] Skeletal ragdoll built for entity {}.", rootEntity.index);
    return true;
}

void ArticulationSystem::Update(SystemContext& ctx, float dt) {
    Reconcile(ctx.registry, *ctx.physics);

    auto& reg = ctx.registry;
    auto& pc  = *ctx.physics;
    auto& rc  = *ctx.render;

    auto entities = reg.GetEntitiesWith<Components::RagdollComponent>();
    auto ragdolls = reg.GetRawArray<Components::RagdollComponent>();

    for (size_t i = 0; i < entities.size(); ++i) {
        Entity                        e       = entities[i];
        Components::RagdollComponent& ragComp = ragdolls[i];
        auto*                         phys    = reg.Get<Components::PhysicsComponent>(e);

        if (ragComp.ragdollInstance == nullptr) {
            continue;
        }
        Track(e, ragComp);

        uint32_t offset = ragComp.jointOffset;
        uint32_t count  = ragComp.jointCount;

        if (auto* hitCmd = reg.Get<Components::RagdollHitReactionCommand>(e)) {
            if (hitCmd->jointIndex < count) {
                uint32_t globalIdx                         = offset + hitCmd->jointIndex;
                _jointStates.jointBlendWeights[globalIdx] = std::clamp(hitCmd->weight, 0.0f, 1.0f);
                _jointStates.jointStiffness[globalIdx]    = std::clamp(hitCmd->stiffness, 0.0f, 1.0f);
                _jointStates.jointBlendDecay[globalIdx]   = std::max(0.0f, hitCmd->decayRate);

                ragComp.state = RagdollState::PartialBlend;
            }
            reg.Remove<Components::RagdollHitReactionCommand>(e);
        }

        if (auto* impulseCmd = reg.Get<Components::RagdollImpulseCommand>(e)) {
            pc.AddRagdollImpulse(*ragComp.ragdollInstance.GetPtr(), impulseCmd->jointIndex, impulseCmd->impulse);
            reg.Remove<Components::RagdollImpulseCommand>(e);
        }

        bool hasActiveBlend = false;
        for (uint32_t j = 0; j < count; ++j) {
            uint32_t globalIdx = offset + j;
            float    decay     = _jointStates.jointBlendDecay[globalIdx];

            if (decay > 0.0f) {
                _jointStates.jointBlendWeights[globalIdx] = std::max(0.0f, _jointStates.jointBlendWeights[globalIdx] - decay * dt);
                _jointStates.jointStiffness[globalIdx]    = std::min(1.0f, _jointStates.jointStiffness[globalIdx] + dt * 1.5f);

                if (_jointStates.jointBlendWeights[globalIdx] <= 0.0f) {
                    _jointStates.jointBlendDecay[globalIdx] = 0.0f;
                }
            }

            if (_jointStates.jointBlendWeights[globalIdx] > 0.001f) {
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
        if (phys != nullptr && !pc.TryGetBodyPosition(phys->physicsHandle, capsuleWorldPos)) {
            // The physics owner may have been queued for destruction. Its
            // identity root is the safe pose until synchronization catches up.
            capsuleWorldPos = JPH::RVec3::sZero();
        }

        JPH::SkeletonPose animPose;
        animPose.SetSkeleton(skel);
        animPose.SetRootOffset(capsuleWorldPos);

        JPH::Array<JPH::Mat44> localJoints(count, JPH::Mat44::sIdentity());
        for (uint32_t j = 0; j < count; ++j) {
            localJoints[j] = _jointStates.inverseBindMatrices[offset + j].Inversed();
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

        // Optional pose providers publish model-space motor targets through a
        // generic core component. Articulation does not depend on any provider.
        if (const auto* poseOverride = reg.Get<Components::KinematicPoseOverrideComponent>(e); poseOverride != nullptr && poseOverride->valid) {
            const uint32_t overrideCount = std::min<uint32_t>(count, poseOverride->jointCount);
            std::copy_n(poseOverride->modelTransforms.begin(), overrideCount, modelJoints.begin());
        }

        std::memcpy(animPose.GetJointMatrices().data(), modelJoints.data(), count * sizeof(JPH::Mat44));
        animPose.CalculateJointStates();

        if (ragComp.state != ragComp.prevState) {
            if (ragComp.state == RagdollState::Dynamic || ragComp.state == RagdollState::Kinematic || ragComp.state == RagdollState::PartialBlend) {
                if (!ragComp.isAddedToPhysics) {
                    const JPH::Vec3 initialVelocity = phys != nullptr ? pc.GetCharacterVelocity(phys->physicsHandle) : JPH::Vec3::sZero();
                    pc.ActivateRagdoll(*ragdoll, animPose, initialVelocity);
                    ragComp.isAddedToPhysics = true;
                }
            } else if (ragComp.state == RagdollState::Inactive && ragComp.isAddedToPhysics) {
                pc.RemoveRagdoll(*ragdoll);
                ragComp.isAddedToPhysics = false;
            }
            ragComp.prevState = ragComp.state;
        }
        Track(e, ragComp);

        if (ragComp.state == RagdollState::Kinematic || ragComp.state == RagdollState::PartialBlend) {
            pc.DriveRagdollPose(*ragdoll, animPose);
        }

        if (ragComp.state != RagdollState::Inactive) {
            JPH::Array<JPH::Mat44> physicalWorldJoints(count, JPH::Mat44::sIdentity());
            JPH::RVec3             actualRootOffset = JPH::RVec3::sZero();

            if (!pc.GetRagdollPose(*ragdoll, actualRootOffset, physicalWorldJoints.data())) {
                continue;
            }

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
                JPH::Mat44 ibm       = _jointStates.inverseBindMatrices[offset + j];
                JPH::Mat44 physModel = invRoot * physicalWorldJoints[j];
                JPH::Mat44 animModel = modelJoints[j];

                float blendWeight = (ragComp.state == RagdollState::Dynamic) ? 1.0f : _jointStates.jointBlendWeights[offset + j];

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

            rc.UpdateJointMatrices(offset, finalSkinningMatrices.data(), count);
        }
    }

    if constexpr (isDev) {
        ZHLN::Tests::VerifyArticulationStateConsistency(reg);
    }
}

} // namespace ZHLN
