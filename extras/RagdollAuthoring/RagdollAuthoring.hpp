// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/RagdollAuthoring/RagdollAuthoring.hpp
//
// Procedural ragdoll *authoring*: the heuristics that decide what a skeleton's
// physical body looks like -- collider shape, mass, joint limits and motors,
// per bone. This is data production, not simulation. The engine core
// (ArticulationSystem) only executes an authored Physics::RagdollPartParams
// list; the humanoid name matching below is gameplay policy and can be
// replaced by cooked collider data or a scene document without touching the
// articulation runtime.
#pragma once

#include <Zahlen/Entity.hpp>
#include <cstdint>

namespace ZHLN {

namespace ECS {
class Registry;
} // namespace ECS

class PhysicsContext;
class ArticulationSystem;

namespace RagdollAuthoring {

// Resolves the rig `rootEntity` owns -- the prefab from its
// AnimatorComponent, the skeleton and joint offset from one of its skinned
// children -- and authors a procedural humanoid biped for it: joint-name
// heuristics pick the shape, mass and joint limits per bone, and the result
// is attached through ArticulationSystem::AttachRagdoll.
//
// Returns false when no skeleton could be resolved, leaving the entity
// untouched.
[[nodiscard]] auto BuildHumanoidBipedRagdoll(
    Entity rootEntity, ECS::Registry& reg, PhysicsContext& pc, ArticulationSystem& articulation
) -> bool;

} // namespace RagdollAuthoring
} // namespace ZHLN
