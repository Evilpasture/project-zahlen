// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// gameplay/RagdollAuthoring/RagdollAuthoring.hpp
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
#include <Zahlen/GameModule.hpp>
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

// The script-facing argument blob for the "SetupRagdoll" command.
//
// It lives here, and not in the scripting layer, for the same reason the
// command body does: the layout is this module's ABI with the scripts that
// call it. The Lua-side ffi.cdef spelling of this struct (see
// extensions/Scripting/Lua/scripts/core/ffi_cdef.fnl) is the other half of the
// contract and has to match it field for field.
struct SetupRagdollArgs {
    uint64_t playerEntity;
};

// Contributes "SetupRagdoll" to the host's script command registry. A host with
// no script runtime still installs the module -- the function is simply never
// called, and nothing about the ragdoll authoring depends on one.
void RegisterScriptCommands(ScriptCommandRegistry& commands);

// The module as a value: this module has no frame steps, no components and no
// graph systems of its own, so its entire contribution is the verb above.
auto Describe() -> Module;

// Convenience for a host that does not want to report a placement failure
// itself. The script command is the only thing that can fail, and Install logs.
void Install(Engine& engine);

} // namespace RagdollAuthoring
} // namespace ZHLN
