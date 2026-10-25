// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// gameplay/RagdollAuthoring/RagdollAuthoring.cpp
#include "RagdollAuthoring.hpp"

#include "engine/system/ArticulationSystem.hpp"
#include "Zahlen/AssetManager.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Log.hpp"
#include "Zahlen/ModelPrefab.hpp"
#include "Zahlen/SkeletalAnimation.hpp"
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <string>
#include <vector>

namespace ZHLN::RagdollAuthoring {

// Authoring policy: which joints of a humanoid biped deserve a collider, and
// what kind. Wrong by construction for anything whose joints are not named
// like a human -- that is the point of keeping it out of the engine core.
auto AuthorHumanoidParts(const Skeleton& skeleton, PhysicsContext& pc) -> std::vector<Physics::RagdollPartParams> {
    auto IsImportantJoint = [](std::string name) -> bool {
        std::ranges::transform(name, name.begin(), ::tolower);
        return name.contains("hip") || name.contains("pelvis") || name.contains("root") || name.contains("spine") || name.contains("chest") ||
               name.contains("torso") || name.contains("head") || name.contains("neck") || name.contains("arm") || name.contains("forearm") ||
               name.contains("thigh") || name.contains("calf") || name.contains("shin");
    };

    std::vector<Physics::RagdollPartParams> parts;
    parts.reserve(skeleton.joints.size());

    for (size_t i = 0; i < skeleton.joints.size(); ++i) {
        std::string name = skeleton.joints[i].name.c_str();

        Physics::RagdollPartParams part;
        part.jointIndex       = static_cast<uint32_t>(i);
        part.parentJointIndex = skeleton.joints[i].parentIndex;
        part.mass             = 1.0f;
        part.enableMotors     = false;

        JPH::Mat44 bindPose = skeleton.joints[i].inverseBindMatrix.Inversed();
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
    return parts;
}

auto BuildHumanoidBipedRagdoll(
    Entity rootEntity, ECS::Registry& reg, const AssetManager& assets, PhysicsContext& pc, ArticulationSystem& articulation
) -> bool {
    // The root owns the rig description: its AnimatorComponent holds the
    // prefabAsset whose cached ModelPrefab owns the skeletons.
    const auto animComp = reg.Get<Components::AnimatorComponent>(rootEntity);
    if (!animComp || animComp->prefabAsset == InvalidAssetID) {
        ZHLN::LogWarning("[RagdollAuthoring] BuildHumanoidBipedRagdoll found no AnimatorComponent with a prefabAsset on entity {}.", rootEntity.index);
        return false;
    }

    const auto prefab = assets.GetCachedPrefab(animComp->prefabAsset);
    if (!prefab) {
        ZHLN::LogWarning("[RagdollAuthoring] BuildHumanoidBipedRagdoll could not resolve cached prefab for entity {}.", rootEntity.index);
        return false;
    }

    // One of the root's skinned children carries the rest: which skeleton of
    // that prefab, and the joint offset its matrices were allocated at.
    const Skeleton* targetSkeleton = nullptr;
    uint32_t        jointOffset    = 0;
    for (const Entity child: reg.GetEntitiesWith<Components::SkeletalMeshComponent>()) {
        const auto skelMesh = reg.Get<Components::SkeletalMeshComponent>(child);
        const auto hier     = reg.Get<Components::HierarchyComponent>(child);
        if (!skelMesh || !hier || hier->parent != rootEntity) {
            continue;
        }
        if (skelMesh->skeletonIndex < 0 || static_cast<size_t>(skelMesh->skeletonIndex) >= prefab->skeletons.size()) {
            continue;
        }
        targetSkeleton = &prefab->skeletons[static_cast<size_t>(skelMesh->skeletonIndex)];
        jointOffset    = skelMesh->jointOffset;
        break;
    }

    if (targetSkeleton == nullptr) {
        ZHLN::LogWarning("[RagdollAuthoring] BuildHumanoidBipedRagdoll found no skinned child of entity {}.", rootEntity.index);
        return false;
    }

    auto parts = AuthorHumanoidParts(*targetSkeleton, pc);
    return articulation.AttachRagdoll(rootEntity, reg, pc, *targetSkeleton, parts, jointOffset);
}

// The verb, contributed by name.
//
// This body used to be a command in extensions/Scripting/Lua/Scripting.cpp,
// which meant the generic scripting layer included
// <RagdollAuthoring/RagdollAuthoring.hpp> and linked a gameplay module: the
// script host had to compile against a domain it does not own, and the domain
// could not exist without the script host. Both halves of that are gone now.
// The scripting layer resolves a *name*; this module is what knows the name
// means a biped ragdoll.
namespace {

auto SetupRagdollCommand(Engine& engine, const void* args) -> uint64_t {
    if (args == nullptr) {
        return 0;
    }
    const auto& a = *static_cast<const SetupRagdollArgs*>(args);

    // Ragdoll authoring is a gameplay-layer concern: the procedural biped
    // generator resolves the rig and authors the parts, and the core
    // articulation system only executes the result.
    const bool built = BuildHumanoidBipedRagdoll(
        Entity::Unpack(a.playerEntity), engine.GetRegistry(), engine.GetAssetManager(), engine.GetPhysicsContext(), engine.GetArticulationSystem()
    );
    return built ? 1u : 0u;
}

} // namespace

void RegisterScriptCommands(ScriptCommandRegistry& commands) {
    if (!commands.Add(ScriptCommand {.name = "SetupRagdoll", .invoke = &SetupRagdollCommand})) {
        // Either the name is empty, the function is null, or another module
        // already claimed "SetupRagdoll" -- the last of which is the one worth
        // knowing about.
        LogError("[RagdollAuthoring] could not contribute the 'SetupRagdoll' script command.");
    }
}

auto Describe() -> Module {
    return Module{
        .name                   = "RagdollAuthoring",
        .registerScriptCommands = &RegisterScriptCommands,
    };
}

void Install(Engine& engine) {
    if (auto installed = engine.InstallModule(Describe()); !installed) {
        LogError("[RagdollAuthoring] module install failed: {}", ToString(installed.error()));
    }
}

} // namespace ZHLN::RagdollAuthoring
