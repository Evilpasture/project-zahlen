#pragma once

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>

namespace ZHLN::Physics {
struct PhysicsWorld; // Forward declaration
} // namespace ZHLN::Physics

namespace ZHLN::Physics::Sync {

/**
 * @brief The "Magic One-Liner": Synchronizes all Jolt state to the SoA World.
 * Handles Rigid Bodies, Characters, and executes optimized SIMD batch copies.
 *
 * @param world The SoA PhysicsWorld to write to.
 * @param system The active Jolt PhysicsSystem.
 * @param activeCharacters The array of active CharacterVirtuals.
 */
void Execute(PhysicsWorld& world, const JPH::PhysicsSystem* const system,
			 const JPH::Array<JPH::CharacterVirtual*>& activeCharacters) noexcept;

} // namespace ZHLN::Physics::Sync