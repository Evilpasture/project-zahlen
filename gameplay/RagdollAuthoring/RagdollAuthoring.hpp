// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// gameplay/RagdollAuthoring/RagdollAuthoring.hpp
//
// Data-driven ragdoll authoring: reflected recipe structs (`RagdollRecipe`)
// hydrated from Lua (`config/ragdoll_humanoid.lua`) or constructed in C++ and
// lowered into `Physics::RagdollPartParams` for `ArticulationSystem::AttachRagdoll`.
#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Engine/GameModule.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN {

namespace ECS {
class Registry;
} // namespace ECS

namespace Physics {
struct RagdollPartParams;
} // namespace Physics

struct Skeleton;
class AssetManager;
class PhysicsContext;
class ArticulationSystem;

namespace RagdollAuthoring {

enum class RagdollShapeKind : uint8_t {
    Capsule = 0,
    Sphere,
};

struct RagdollPartSpec {
    RagdollShapeKind shape         = RagdollShapeKind::Sphere;
    float            halfHeight    = 0.2f;
    float            radius        = 0.08f;
    float            mass          = 0.5f;
    bool             enableMotors  = false;
    float            maxMotorForce = 0.0f;
};

struct RagdollJointRule {
    std::vector<std::string> patterns;
    RagdollPartSpec          spec;
};

struct RagdollRecipe {
    std::vector<RagdollJointRule> rules;
    RagdollPartSpec               fallback;
};

[[nodiscard]] auto DefaultHumanoidRecipe() -> RagdollRecipe;

[[nodiscard]] auto LoadRecipeFromLuaString(std::string_view chunk, std::string_view chunkName = "ragdoll.lua")
    -> std::expected<RagdollRecipe, ErrorCode>;

[[nodiscard]] auto LoadRecipeFromLuaFile(const std::filesystem::path& path) -> std::expected<RagdollRecipe, ErrorCode>;

[[nodiscard]] auto AuthorPartsFromRecipe(const Skeleton& skeleton, const RagdollRecipe& recipe, PhysicsContext& pc)
    -> std::vector<Physics::RagdollPartParams>;

[[nodiscard]] auto BuildRagdollFromRecipe(
    Entity               rootEntity,
    const RagdollRecipe& recipe,
    ECS::Registry&       reg,
    const AssetManager&  assets,
    PhysicsContext&      pc,
    ArticulationSystem&  articulation
) -> bool;

[[nodiscard]] auto BuildHumanoidBipedRagdoll(
    Entity rootEntity, ECS::Registry& reg, const AssetManager& assets, PhysicsContext& pc, ArticulationSystem& articulation
) -> bool;

auto Describe() -> Module;

void Install(Engine& engine);

} // namespace RagdollAuthoring
} // namespace ZHLN
