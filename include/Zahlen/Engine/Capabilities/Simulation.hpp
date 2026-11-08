// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The simulation capability. Queries return owned values; mutations are named
// operations. Results are spelled in ZHLN value types only: no JPH type, no
// Jolt header and no mutable physics-world reference crosses this boundary.

#include <Zahlen/Common.h>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <Zahlen/Math/ValueTypes.hpp>
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
    [[nodiscard]] auto Raycast(Double3 origin, Float3 direction, float maxDistance = 1000.0f, Physics::BodyID ignore = {}) const
        -> Physics::RaycastResult;
    [[nodiscard]] auto BodiesInSphere(Double3 center, float radius) const -> std::vector<Physics::BodyID>;
    [[nodiscard]] auto DebugDrawData(bool shapes = true, bool constraints = true, bool wireframe = true) const -> Physics::DebugDrawData;
    [[nodiscard]] auto BodyPosition(Physics::BodyID handle) const noexcept -> std::optional<Double3>;
    [[nodiscard]] auto BodyState(Physics::BodyID handle) const noexcept -> std::optional<Physics::BodyStateSnapshot>;
    [[nodiscard]] auto CharacterVelocity(Physics::BodyID handle) const -> Float3;
    [[nodiscard]] auto IsCharacterOnGround(Physics::BodyID handle) const -> bool;
    [[nodiscard]] auto IsBodyDynamic(Physics::BodyID handle) const -> bool;

    void AddImpulse(Physics::BodyID handle, Float3 impulse);
    void DestroyBody(Physics::BodyID handle);
    void DestroyBodies(std::span<const Physics::BodyID> handles);
    void SetCharacterVelocity(Physics::BodyID handle, Float3 velocity);
    void Step(float deltaTime);
    [[nodiscard]] auto CreateCharacter(Double3 position) -> Physics::BodyID;
    [[nodiscard]] auto CreateCharacter(Double3 position, const Physics::CharacterParams& params) -> Physics::BodyID;
    void               DestroyRagdolls(std::span<const Physics::RagdollID> handles) noexcept;


  private:
    friend class Engine;
    friend struct EngineAccess;
    explicit Simulation(PhysicsContext& ctx) noexcept: _ctx(&ctx) {
    }
    PhysicsContext* _ctx;
};

} // namespace ZHLN
