// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// gameplay/RagdollAuthoring/RagdollAuthoring.cpp
#include "RagdollAuthoring.hpp"

#include "engine/system/ArticulationSystem.hpp"

#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Physics/Jolt.hpp>
#include <Zahlen/SkeletalAnimation.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/physics/Physics.hpp>

#if defined(ZHLN_RAGDOLL_HAS_LUA)
#include <lua/ReflectLua.hpp>
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace ZHLN::RagdollAuthoring {

namespace {

    enum class RagdollRecipeError : uint8_t {
        LuaUnavailable ZHLN_ANNOTATION(ZHLN::Description<"RagdollAuthoring was built without plugins/lua support"> {}) = 1,
    };

    auto LowerAscii(std::string s) -> std::string {
        std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    auto MatchesAnyPattern(std::string_view loweredJointName, const std::vector<std::string>& patterns) -> bool {
        for (const std::string& rawPat: patterns) {
            const std::string loweredPat = LowerAscii(rawPat);
            if (!loweredPat.empty() && loweredJointName.find(loweredPat) != std::string_view::npos) {
                return true;
            }
        }
        return false;
    }

    auto ResolveSpec(std::string_view loweredJointName, const RagdollRecipe& recipe) -> const RagdollPartSpec& {
        for (const RagdollJointRule& rule: recipe.rules) {
            if (MatchesAnyPattern(loweredJointName, rule.patterns)) {
                return rule.spec;
            }
        }
        return recipe.fallback;
    }

} // namespace

auto DefaultHumanoidRecipe() -> RagdollRecipe {
    return RagdollRecipe {
        .rules =
            {
                RagdollJointRule {
                    .patterns = {"hip", "pelvis", "root"},
                    .spec     = {.shape = RagdollShapeKind::Capsule, .halfHeight = 0.4f, .radius = 0.2f, .mass = 15.0f},
                },
                RagdollJointRule {
                    .patterns = {"spine", "chest", "torso"},
                    .spec     = {
                        .shape         = RagdollShapeKind::Capsule,
                        .halfHeight    = 0.5f,
                        .radius        = 0.25f,
                        .mass          = 20.0f,
                        .enableMotors  = true,
                        .maxMotorForce = 250.0f,
                    },
                },
                RagdollJointRule {
                    .patterns = {"head", "neck"},
                    .spec     = {
                        .shape         = RagdollShapeKind::Sphere,
                        .radius        = 0.3f,
                        .mass          = 8.0f,
                        .enableMotors  = true,
                        .maxMotorForce = 250.0f,
                    },
                },
                RagdollJointRule {
                    .patterns = {"arm", "forearm", "thigh", "calf", "shin"},
                    .spec     = {.shape = RagdollShapeKind::Capsule, .halfHeight = 0.2f, .radius = 0.1f, .mass = 3.0f},
                },
            },
        .fallback = {.shape = RagdollShapeKind::Sphere, .radius = 0.08f, .mass = 0.5f},
    };
}

auto LoadRecipeFromLuaString([[maybe_unused]] std::string_view chunk, [[maybe_unused]] std::string_view chunkName)
    -> std::expected<RagdollRecipe, ErrorCode> {
#if defined(ZHLN_RAGDOLL_HAS_LUA)
    return ReflectLua::EvaluateString<RagdollRecipe>(chunk, chunkName);
#else
    return std::unexpected(RagdollRecipeError::LuaUnavailable);
#endif
}

auto LoadRecipeFromLuaFile([[maybe_unused]] const std::filesystem::path& path) -> std::expected<RagdollRecipe, ErrorCode> {
#if defined(ZHLN_RAGDOLL_HAS_LUA)
    return ReflectLua::EvaluateFile<RagdollRecipe>(path);
#else
    return std::unexpected(RagdollRecipeError::LuaUnavailable);
#endif
}

auto AuthorPartsFromRecipe(const Skeleton& skeleton, const RagdollRecipe& recipe, PhysicsContext& pc)
    -> std::vector<Physics::RagdollPartParams> {
    std::vector<Physics::RagdollPartParams> parts;
    parts.reserve(skeleton.joints.size());

    for (size_t i = 0; i < skeleton.joints.size(); ++i) {
        const std::string      lowered = LowerAscii(skeleton.joints[i].name.c_str());
        const RagdollPartSpec& spec    = ResolveSpec(lowered, recipe);

        Physics::RagdollPartParams part {};
        part.jointIndex       = static_cast<uint32_t>(i);
        part.parentJointIndex = skeleton.joints[i].parentIndex;
        part.mass             = spec.mass;
        part.enableMotors     = spec.enableMotors;
        part.maxMotorForce    = spec.maxMotorForce;

        const JPH::Mat44 bindPose = skeleton.joints[i].inverseBindMatrix.Inversed();
        part.position             = JPH::RVec3(bindPose.GetTranslation());
        part.rotation             = bindPose.GetQuaternion().Normalized();

        switch (spec.shape) {
            case RagdollShapeKind::Capsule:
                part.shape = pc.GetOrCreateShape(Physics::ShapeType::Capsule, spec.halfHeight, spec.radius);
                break;
            case RagdollShapeKind::Sphere:
                part.shape = pc.GetOrCreateShape(Physics::ShapeType::Sphere, spec.radius);
                break;
        }
        parts.push_back(part);
    }

    return parts;
}

auto BuildRagdollFromRecipe(
    Entity               rootEntity,
    const RagdollRecipe& recipe,
    ECS::Registry&       reg,
    const AssetManager&  assets,
    PhysicsContext&      pc,
    ArticulationSystem&  articulation
) -> bool {
    const auto animComp = reg.Get<Components::AnimatorComponent>(rootEntity);
    if (!animComp || animComp->prefabAsset == InvalidAssetID) {
        ZHLN::LogWarning("[RagdollAuthoring] BuildRagdollFromRecipe found no AnimatorComponent with a prefabAsset on entity {}.", rootEntity.index);
        return false;
    }

    const auto prefab = assets.GetCachedPrefab(animComp->prefabAsset);
    if (!prefab) {
        ZHLN::LogWarning("[RagdollAuthoring] BuildRagdollFromRecipe could not resolve cached prefab for entity {}.", rootEntity.index);
        return false;
    }

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
        ZHLN::LogWarning("[RagdollAuthoring] BuildRagdollFromRecipe found no skinned child of entity {}.", rootEntity.index);
        return false;
    }

    auto parts = AuthorPartsFromRecipe(*targetSkeleton, recipe, pc);
    return articulation.AttachRagdoll(rootEntity, reg, pc, *targetSkeleton, parts, jointOffset);
}

auto BuildHumanoidBipedRagdoll(
    Entity rootEntity, ECS::Registry& reg, const AssetManager& assets, PhysicsContext& pc, ArticulationSystem& articulation
) -> bool {
    constexpr std::string_view kDefaultRecipePath = "config/ragdoll_humanoid.lua";
    if (std::filesystem::exists(kDefaultRecipePath)) {
        if (auto loaded = LoadRecipeFromLuaFile(kDefaultRecipePath)) {
            return BuildRagdollFromRecipe(rootEntity, *loaded, reg, assets, pc, articulation);
        }
    }
    return BuildRagdollFromRecipe(rootEntity, DefaultHumanoidRecipe(), reg, assets, pc, articulation);
}

auto Describe() -> Module {
    return Module {
        .name = "RagdollAuthoring",
    };
}

void Install(Engine& engine) {
    if (auto installed = engine.InstallModule(Describe()); !installed) {
        LogError("[RagdollAuthoring] module install failed: {}", ToString(installed.error()));
    }
}

} // namespace ZHLN::RagdollAuthoring
