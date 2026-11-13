// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/ISimulation.hpp
//
// Physics as the public API sees it. Creation descriptors travel via reflected
// ChainInNode lists (Physics::BoxShapeDesc, SphereShapeDesc, CapsuleShapeDesc,
// CylinderShapeDesc, PlaneShapeDesc, MeshShapeDesc, HeightFieldShapeDesc,
// CompoundShapeDesc + optional RigidBodyPose, RigidBodySurface, RigidBodyFilter)
// so new body properties or shape families never shift the vtable or read past
// caller-owned structs.

#include <Zahlen/COM/COMTypes.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Physics/PhysicsValues.hpp>

#include <cstdint>
#include <type_traits>
#include <vector>

namespace ZHLN::COM {

// The physics vocabulary, checked rather than assumed.
static_assert(std::is_standard_layout_v<Physics::BodyID> && std::is_trivially_copyable_v<Physics::BodyID>);
static_assert(std::is_standard_layout_v<Physics::RaycastResult> && std::is_trivially_copyable_v<Physics::RaycastResult>);
static_assert(std::is_standard_layout_v<Physics::BodyStateSnapshot> && std::is_trivially_copyable_v<Physics::BodyStateSnapshot>);
static_assert(std::is_standard_layout_v<Physics::DebugDrawData> && std::is_trivially_copyable_v<Physics::DebugDrawData>);
static_assert(std::is_standard_layout_v<FVec3> && std::is_trivially_copyable_v<FVec3>);
static_assert(std::is_standard_layout_v<DVec3> && std::is_trivially_copyable_v<DVec3>);
static_assert(std::is_standard_layout_v<Quat> && std::is_trivially_copyable_v<Quat>);

struct ISimulation : IObject {
    [[nodiscard]] virtual Physics::RaycastResult Raycast(DVec3 origin, FVec3 direction, float maxDistance,
                                                         Physics::BodyID ignore) const noexcept = 0;

    // Writes up to `capacity` ids and returns how many bodies are actually in
    // the sphere, which may be more. A caller that wants them all sizes its
    // buffer from the first answer and asks again; nothing is allocated on
    // either side of the boundary.
    [[nodiscard]] virtual uint32_t BodiesInSphere(DVec3 center, float radius, Physics::BodyID* bodies,
                                                  uint32_t capacity) const noexcept = 0;

    // COM::Option is the boundary's spelling of the facade's std::optional:
    // a fixed layout of the value plus a flag, returned by value, with an
    // implicit conversion to std::optional in the caller's own TU.
    [[nodiscard]] virtual Option<DVec3>                      BodyPosition(Physics::BodyID body) const noexcept = 0;
    [[nodiscard]] virtual Option<Physics::BodyStateSnapshot> BodyState(Physics::BodyID body) const noexcept = 0;
    [[nodiscard]] virtual Option<FVec3>                      BodyLinearVelocity(Physics::BodyID body) const noexcept = 0;

    [[nodiscard]] virtual FVec3 CharacterVelocity(Physics::BodyID body) const noexcept = 0;
    [[nodiscard]] virtual bool  IsCharacterOnGround(Physics::BodyID body) const noexcept = 0;
    [[nodiscard]] virtual bool  IsBodyDynamic(Physics::BodyID body) const noexcept = 0;

    virtual void AddImpulse(Physics::BodyID body, FVec3 impulse) noexcept = 0;
    virtual void SetBodyLinearVelocity(Physics::BodyID body, FVec3 velocity) noexcept = 0;
    virtual void SetKinematicPose(Physics::BodyID body, DVec3 position, Quat rotation) noexcept = 0;
    virtual void SetCollisionFilter(Physics::BodyID body, uint32_t category, uint32_t mask) noexcept = 0;
    virtual void RegisterPhysicsMaterial(uint32_t materialID, float friction, float restitution) noexcept = 0;
    virtual void SetCharacterVelocity(Physics::BodyID body, FVec3 velocity) noexcept = 0;
    virtual void DestroyBody(Physics::BodyID body) noexcept = 0;
    virtual void DestroyBodies(Span<const Physics::BodyID> bodies) noexcept = 0;
    virtual void DestroyRagdolls(Span<const Physics::RagdollID> ragdolls) noexcept = 0;
    virtual void ClearMeshShapeCache() noexcept = 0;

    // Advances physics by one step. Note that IEngine::Tick already steps the
    // simulation on its fixed-timestep schedule; call this only when driving
    // physics yourself.
    virtual void Step(float deltaTime) noexcept = 0;

    // Requires exactly one shape node (BoxShapeDesc, SphereShapeDesc,
    // CapsuleShapeDesc, CylinderShapeDesc, PlaneShapeDesc, MeshShapeDesc,
    // HeightFieldShapeDesc, or CompoundShapeDesc) plus any subset of
    // RigidBodyPose, RigidBodySurface and RigidBodyFilter.
    [[nodiscard]] virtual Result<Physics::BodyID> CreateRigidBody(const ChainInNode* descChain) noexcept = 0;

    // Accepts Physics::CharacterCreateDesc in `descChain` (nullptr uses defaults
    // at the origin).
    [[nodiscard]] virtual Result<Physics::BodyID> CreateCharacter(const ChainInNode* descChain) noexcept = 0;

    // Borrowed vertex spans owned by the engine's debug buffers. They stay
    // valid until the next call or the next step, and are never freed by the
    // caller -- the memory belongs to the library that allocated it.
    [[nodiscard]] virtual Physics::DebugDrawData DebugGeometry(bool shapes, bool constraints, bool wireframe) noexcept = 0;

    // --- Ergonomics: compiled in the consumer's TU, never in the library ---

    [[nodiscard]] auto CreateCharacter(DVec3 position) noexcept -> Physics::BodyID {
        const Result<Physics::BodyID> created =
            CreateCharacter(InChain(Physics::CharacterCreateDesc {.position = position}));
        return created.HasValue() ? created.value : Physics::BodyID::Null();
    }

    [[nodiscard]] auto CreateBoxBody(FVec3 halfExtents, const Physics::RigidBodyPose& pose = {}) noexcept
        -> Result<Physics::BodyID> {
        return CreateRigidBody(InChain(Physics::BoxShapeDesc {.halfExtents = halfExtents}, pose));
    }

    [[nodiscard]] auto CreateSphereBody(float radius, const Physics::RigidBodyPose& pose = {}) noexcept
        -> Result<Physics::BodyID> {
        return CreateRigidBody(InChain(Physics::SphereShapeDesc {.radius = radius}, pose));
    }

    [[nodiscard]] auto Raycast(DVec3 origin, FVec3 direction) const noexcept -> Physics::RaycastResult {
        return Raycast(origin, direction, 1000.0F, Physics::BodyID {});
    }

    [[nodiscard]] auto DebugGeometry() noexcept -> Physics::DebugDrawData {
        return DebugGeometry(true, true, true);
    }

    // Count, then fill. Safe to ask twice because this is a pure query: the
    // vector is allocated by the caller's allocator, the library writes
    // through the bare pointer, and the caller's allocator frees it.
    [[nodiscard]] auto BodiesInSphere(DVec3 center, float radius) const -> std::vector<Physics::BodyID> {
        const uint32_t total = BodiesInSphere(center, radius, nullptr, 0);
        if (total == 0) return {};

        std::vector<Physics::BodyID> bodies(total);
        bodies.resize(BodiesInSphere(center, radius, bodies.data(), total));
        return bodies;
    }

protected:
    ~ISimulation() = default;
};

} // namespace ZHLN::COM
