// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/ISimulation.hpp
//
// Physics as the public API sees it. Creation descriptors travel via reflected
// ChainInNode lists (exactly one Physics shape node + exactly one role node
// (StaticBody, KinematicBody, DynamicBody or CharacterBody) + any subset of
// SurfaceMaterial, CollisionFilter and MovingBroadphase) so new body
// properties or shape families never shift the vtable or read past
// caller-owned structs. The four *Raw creation methods are the tier below
// ergonomics; mutations are role-scoped and narrow through AsDynamic /
// AsKinematic / AsCharacter.

#include <Zahlen/COM/Types.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Collect.hpp>
#include <Zahlen/Core/COM/IObject.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Physics/PhysicsShapes.hpp>
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
    [[nodiscard]] virtual uint32_t BodiesInSphereRaw(DVec3 center, float radius, Physics::BodyID* bodies,
                                                  uint32_t capacity) const noexcept = 0;

    // COM::Option is the boundary's spelling of the facade's std::optional:
    // a fixed layout of the value plus a flag, returned by value, with an
    // implicit conversion to std::optional in the caller's own TU.
    [[nodiscard]] virtual Option<DVec3>                      BodyPosition(Physics::BodyID body) const noexcept = 0;
    [[nodiscard]] virtual Option<Physics::BodyStateSnapshot> BodyState(Physics::BodyID body) const noexcept = 0;
    [[nodiscard]] virtual Option<FVec3>                      BodyLinearVelocity(Physics::BodyID body) const noexcept = 0;

    [[nodiscard]] virtual FVec3 CharacterVelocity(Physics::CharacterID body) const noexcept = 0;
    [[nodiscard]] virtual float CharacterGroundOffset(Physics::CharacterID body) const noexcept = 0;
    [[nodiscard]] virtual bool  IsCharacterOnGround(Physics::CharacterID body) const noexcept = 0;
    [[nodiscard]] virtual bool  IsBodyDynamic(Physics::BodyID body) const noexcept = 0;

    // Role narrowing: BodyID -> typed ID while the slot is alive with that
    // role's motion type. Callers that only hold a generic PhysicsComponent
    // id pay this one check, then get compile-checked mutations.
    [[nodiscard]] virtual Option<Physics::DynamicBodyID>  AsDynamic(Physics::BodyID body) const noexcept = 0;
    [[nodiscard]] virtual Option<Physics::KinematicBodyID> AsKinematic(Physics::BodyID body) const noexcept = 0;
    [[nodiscard]] virtual Option<Physics::CharacterID>    AsCharacter(Physics::BodyID body) const noexcept = 0;

    // Role-scoped mutations: the typed ID is the proof of role, so a
    // character velocity can never be written to a box.
    virtual void AddImpulse(Physics::DynamicBodyID body, FVec3 impulse) noexcept = 0;
    virtual void SetVelocity(Physics::DynamicBodyID body, FVec3 velocity) noexcept = 0;
    virtual void SetVelocity(Physics::CharacterID body, FVec3 velocity) noexcept = 0;
    virtual void SetPose(Physics::KinematicBodyID body, DVec3 position, Quat rotation) noexcept = 0;
    virtual void SetPose(Physics::CharacterID body, DVec3 position) noexcept = 0;

    // Role-agnostic: any live rigid slot.
    virtual void SetCollisionFilter(Physics::BodyID body, Physics::CollisionFilter filter) noexcept = 0;
    // Fingerprinted material registration; identical (friction, restitution)
    // reuses one PhysicsMaterialID (Default = 0 is Jolt 0.2/0.0).
    virtual Physics::PhysicsMaterialID RegisterMaterial(Physics::SurfaceMaterial material) noexcept = 0;
    virtual void DestroyBody(Physics::BodyID body) noexcept = 0;
    virtual void DestroyBodies(Span<const Physics::BodyID> bodies) noexcept = 0;
    virtual void DestroyRagdolls(Span<const Physics::RagdollID> ragdolls) noexcept = 0;
    // Publishes model-space joint matrices for blended/kinematic ragdoll drive.
    virtual void QueueKinematicPose(uint32_t jointOffset, Span<const Mat4x4> modelJoints) noexcept = 0;
    virtual void ClearMeshShapeCache() noexcept = 0;

    // Advances physics by one step. Note that IEngine::Tick already steps the
    // simulation on its fixed-timestep schedule; call this only when driving
    // physics yourself.
    virtual void Step(float deltaTime) noexcept = 0;

    // Creation tier: the chain must carry exactly one shape node (the shapes
    // the role accepts), at most one role node (defaults when absent) and any
    // subset of SurfaceMaterial / CollisionFilter / MovingBroadphase. Each
    // method enforces the shape family its role allows: statics accept the
    // full set, kinematic/dynamic accept convex + compound (meshes cannot be
    // dynamic), characters accept Capsule and DualShape.
    [[nodiscard]] virtual Result<Physics::StaticBodyID> CreateStaticBodyRaw(const ChainInNode* shapeChain) noexcept = 0;
    [[nodiscard]] virtual Result<Physics::KinematicBodyID> CreateKinematicBodyRaw(const ChainInNode* shapeChain) noexcept = 0;
    [[nodiscard]] virtual Result<Physics::DynamicBodyID> CreateDynamicBodyRaw(const ChainInNode* shapeChain) noexcept = 0;
    [[nodiscard]] virtual Result<Physics::CharacterID> CreateCharacterRaw(const ChainInNode* shapeChain) noexcept = 0;

    // Borrowed vertex spans owned by the engine's debug buffers. They stay
    // valid until the next call or the next step, and are never freed by the
    // caller -- the memory belongs to the library that allocated it.
    [[nodiscard]] virtual Physics::DebugDrawData DebugGeometry(bool shapes, bool constraints, bool wireframe) noexcept = 0;

    // --- Ergonomics: compiled in the consumer's TU, never in the library ---

    // Default capsule hull (0.5 half-height, 0.3 radius), role defaults.
    [[nodiscard]] auto CreateCharacter(DVec3 position) noexcept -> Physics::BodyID {
        const Result<Physics::CharacterID> created =
            CreateCharacterRaw(InChain(Physics::Capsule {.halfHeight = 0.5f, .radius = 0.3f},
                                       Physics::CharacterBody {.position = position}));
        return created.HasValue() ? Physics::BodyID(created.value) : Physics::BodyID::Null();
    }

    auto CreateBoxBody(FVec3 halfExtents, const Physics::DynamicBody& role = {}) noexcept
        -> Result<Physics::DynamicBodyID> {
        return CreateDynamicBodyRaw(InChain(Physics::Box {.halfExtents = halfExtents}, role));
    }

    auto CreateSphereBody(float radius, const Physics::DynamicBody& role = {}) noexcept
        -> Result<Physics::DynamicBodyID> {
        return CreateDynamicBodyRaw(InChain(Physics::Sphere {.radius = radius}, role));
    }

    [[nodiscard]] auto Raycast(DVec3 origin, FVec3 direction) const noexcept -> Physics::RaycastResult {
        return Raycast(origin, direction, 1000.0F, Physics::BodyID {});
    }

    [[nodiscard]] auto DebugGeometry() noexcept -> Physics::DebugDrawData {
        return DebugGeometry(true, true, true);
    }

    // Count, then fill (see Collect). Safe to ask twice because this is a
    // pure query: the vector is allocated by the caller's allocator, the
    // library writes through the bare pointer, and the caller's allocator frees it.
    [[nodiscard]] auto BodiesInSphere(DVec3 center, float radius) const -> std::vector<Physics::BodyID> {
        return Collect<&ISimulation::BodiesInSphereRaw>(*this, center, radius);
    }

protected:
    ~ISimulation() = default;
};

} // namespace ZHLN::COM
