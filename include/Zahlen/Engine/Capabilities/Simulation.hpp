// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The simulation capability. Queries return owned values; mutations are named
// operations. Results are spelled in ZHLN value types only: no JPH type, no
// Jolt header and no mutable physics-world reference crosses this boundary.

#include <Zahlen/Common.h>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <Zahlen/Physics/PhysicsShapes.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ZHLN {

class PhysicsContext;

namespace Physics {
struct CharacterParams;
}

class ZHLN_API Simulation {
  public:
    // Resolves the value synchronously; mesh spans are borrowed only until this returns.
    [[nodiscard]] auto CreateRigidBody(
        const Physics::AnyShape& shape, DVec3 position, FVec4 rotation, Physics::MotionType motion, Physics::ObjectLayer layer,
        uint32_t materialID = 0, uint32_t category = 0xFFFFFFFF, uint32_t mask = 0xFFFFFFFF,
        Physics::Activation activation = Physics::Activation::Activate, FVec3 scale = {1, 1, 1}
    ) -> Physics::BodyID;
    template <Physics::PhysicsShape S>
    [[nodiscard]] auto CreateRigidBody(
        const S& shape, DVec3 position, FVec4 rotation, Physics::MotionType motion, Physics::ObjectLayer layer,
        uint32_t materialID = 0, uint32_t category = 0xFFFFFFFF, uint32_t mask = 0xFFFFFFFF,
        Physics::Activation activation = Physics::Activation::Activate, FVec3 scale = {1, 1, 1}
    ) -> Physics::BodyID {
        return CreateRigidBody(Physics::AnyShape(shape), position, rotation, motion, layer, materialID, category, mask, activation, scale);
    }

    // Drop cached mesh flyweights at a level-unload boundary. Existing bodies
    // and queued commands remain valid; primitive flyweights stay cached.
    void ClearMeshShapeCache() noexcept;

    [[nodiscard]] auto Raycast(DVec3 origin, FVec3 direction, float maxDistance = 1000.0f, Physics::BodyID ignore = {}) const
        -> Physics::RaycastResult;
    [[nodiscard]] auto BodiesInSphere(DVec3 center, float radius) const -> std::vector<Physics::BodyID>;
    [[nodiscard]] auto DebugDrawData(bool shapes = true, bool constraints = true, bool wireframe = true) const -> Physics::DebugDrawData;
    [[nodiscard]] auto BodyPosition(Physics::BodyID handle) const noexcept -> std::optional<DVec3>;
    [[nodiscard]] auto BodyState(Physics::BodyID handle) const noexcept -> std::optional<Physics::BodyStateSnapshot>;
    [[nodiscard]] auto BodyLinearVelocity(Physics::BodyID handle) const noexcept -> std::optional<FVec3>;
    [[nodiscard]] auto CharacterVelocity(Physics::BodyID handle) const -> FVec3;
    [[nodiscard]] auto IsCharacterOnGround(Physics::BodyID handle) const -> bool;
    [[nodiscard]] auto IsBodyDynamic(Physics::BodyID handle) const -> bool;

    void AddImpulse(Physics::BodyID handle, FVec3 impulse);
    void SetLinearVelocity(Physics::BodyID handle, FVec3 velocity);
    void SetKinematicPose(Physics::BodyID handle, DVec3 position, FVec4 rotation);
    void SetCollisionFilter(Physics::BodyID handle, uint32_t category, uint32_t mask);
    void RegisterMaterial(uint32_t id, float friction, float restitution);
    void DestroyBody(Physics::BodyID handle);
    void DestroyBodies(std::span<const Physics::BodyID> handles);
    void SetCharacterVelocity(Physics::BodyID handle, FVec3 velocity);
    void Step(float deltaTime);
    [[nodiscard]] auto CreateCharacter(DVec3 position) -> Physics::BodyID;
    [[nodiscard]] auto CreateCharacter(DVec3 position, const Physics::CharacterParams& params) -> Physics::BodyID;
    [[nodiscard]] auto CreateCustomCharacter(const Physics::CharacterCreateDesc& desc) -> Physics::BodyID;
    void               DestroyRagdolls(std::span<const Physics::RagdollID> handles) noexcept;


  private:
    friend class Engine;
    explicit Simulation(PhysicsContext& ctx) noexcept: _ctx(&ctx) {
    }
    PhysicsContext* _ctx;
};

} // namespace ZHLN
