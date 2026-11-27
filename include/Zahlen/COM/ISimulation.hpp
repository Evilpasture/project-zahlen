// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/COM/ISimulation.hpp
//
// Physics as the public API sees it. Creation descriptors travel via reflected
// ChainInNode lists (exactly one Physics shape node + at most one role node
// (StaticBody, KinematicBody, DynamicBody or CharacterBody) + role-legal
// modifiers from SurfaceMaterial, PhysicsMaterialID, CollisionFilter and
// MovingBroadphase) so new body properties or shape families never shift the
// vtable or read past caller-owned structs. The four *Raw creation methods are
// the tier below ergonomics; ECS components carry role-typed IDs directly, and
// role-agnostic queries narrow through AsDynamic / AsKinematic / AsCharacter.

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

    [[nodiscard]] virtual Result<Physics::ShapeCastResult> ShapecastRaw(
        const ChainInNode* shapeChain, DVec3 position, Quat rotation, FVec3 direction, float maxDistance,
        Physics::BodyID ignore) const noexcept = 0;

    // Writes up to `capacity` ids and returns how many bodies are actually in
    // the volume, which may be more. A caller that wants them all sizes its
    // buffer from the first answer and asks again; nothing is allocated on
    // either side of the boundary.
    [[nodiscard]] virtual uint32_t BodiesInSphereRaw(DVec3 center, float radius, Physics::BodyID* bodies,
                                                     uint32_t capacity) const noexcept = 0;

    [[nodiscard]] virtual Result<uint32_t> OverlapRaw(const ChainInNode* shapeChain, DVec3 position, Quat rotation,
                                                      Physics::BodyID* bodies, uint32_t capacity) const noexcept = 0;

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
    // role's motion type. Query hits (Raycast / Overlap / BodiesInSphere) hand
    // back role-agnostic BodyIDs and narrow here before mutating.
    [[nodiscard]] virtual Option<Physics::DynamicBodyID>   AsDynamic(Physics::BodyID body) const noexcept = 0;
    [[nodiscard]] virtual Option<Physics::KinematicBodyID> AsKinematic(Physics::BodyID body) const noexcept = 0;
    [[nodiscard]] virtual Option<Physics::CharacterID>     AsCharacter(Physics::BodyID body) const noexcept = 0;

    // Role-scoped mutations: the typed ID is the proof of role, so a
    // character velocity can never be written to a box.
    virtual void AddImpulse(Physics::DynamicBodyID body, FVec3 impulse) noexcept = 0;
    virtual void SetVelocity(Physics::DynamicBodyID body, FVec3 velocity) noexcept = 0;
    virtual void SetVelocity(Physics::CharacterID body, FVec3 velocity) noexcept = 0;
    virtual void SetPose(Physics::KinematicBodyID body, DVec3 position, Quat rotation) noexcept = 0;
    virtual void SetPose(Physics::CharacterID body, DVec3 position) noexcept = 0;

    // Constraints: creation takes a single-node ChainInNode carrying one
    // ConstraintDesc struct; motor target setters require the role-typed ID.
    [[nodiscard]] virtual Result<Physics::ConstraintID> CreateConstraintRaw(
        Physics::BodyID body1, Physics::BodyID body2, const ChainInNode* constraintChain) noexcept = 0;
    virtual void SetTargetAngle(Physics::HingeConstraintID constraint, RadF angle) noexcept = 0;
    virtual void SetTargetPosition(Physics::SliderConstraintID constraint, float position) noexcept = 0;

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
    // role-legal modifiers (SurfaceMaterial or PhysicsMaterialID,
    // CollisionFilter, and MovingBroadphase on statics only).
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
    [[nodiscard]] auto CreateCharacter(DVec3 position) noexcept -> Physics::CharacterID {
        const Result<Physics::CharacterID> created =
            CreateCharacterRaw(InChain(Physics::Capsule {.halfHeight = 0.5f, .radius = 0.3f},
                                       Physics::CharacterBody {.position = position}));
        return created.HasValue() ? created.value : Physics::CharacterID::Null();
    }

    [[nodiscard]] auto CreateCapsuleCharacter(float halfHeight, float radius,
                                              const Physics::CharacterBody& role = {}) noexcept
        -> Result<Physics::CharacterID> {
        return CreateCharacterRaw(InChain(Physics::Capsule {.halfHeight = halfHeight, .radius = radius}, role));
    }

    auto CreateBoxBody(FVec3 halfExtents, const Physics::DynamicBody& role = {}) noexcept
        -> Result<Physics::DynamicBodyID> {
        return CreateDynamicBodyRaw(InChain(Physics::Box {.halfExtents = halfExtents}, role));
    }

    auto CreateSphereBody(float radius, const Physics::DynamicBody& role = {}) noexcept
        -> Result<Physics::DynamicBodyID> {
        return CreateDynamicBodyRaw(InChain(Physics::Sphere {.radius = radius}, role));
    }

    // Typed creation without a hand-built chain: the shape and role travel as
    // values, the chain is assembled here. A FixedCompound stamps its node
    // array into this frame and goes through the same raw as any other
    // compound -- the requires-clause is the role's child policy, so a
    // FixedCompound<Placed<Plane>> into a dynamic body fails at compile time.
    template <Physics::StaticShape S, Physics::StaticBodyModifier... Mods>
        requires (!Physics::kIsFixedCompound<S> &&
                  !((std::same_as<Mods, Physics::SurfaceMaterial> || ...) &&
                    (std::same_as<Mods, Physics::PhysicsMaterialID> || ...)))
    [[nodiscard]] auto CreateBody(const S& shape, const Physics::StaticBody& role, const Mods&... mods) noexcept
        -> Result<Physics::StaticBodyID> {
        return CreateStaticBodyRaw(InChain(shape, role, mods...));
    }
    template <typename... Children, Physics::StaticBodyModifier... Mods>
        requires ((Physics::StaticCompoundChild<Children> && ...) &&
                  !((std::same_as<Mods, Physics::SurfaceMaterial> || ...) &&
                    (std::same_as<Mods, Physics::PhysicsMaterialID> || ...)))
    [[nodiscard]] auto CreateBody(const Physics::FixedCompound<Children...>& shape, const Physics::StaticBody& role,
                                  const Mods&... mods) noexcept -> Result<Physics::StaticBodyID> {
        const auto nodes = shape.MakeNodes();
        return CreateStaticBodyRaw(InChain(Physics::Compound {.children = nodes}, role, mods...));
    }

    template <Physics::DynamicShape S, Physics::RigidBodyModifier... Mods>
        requires (!Physics::kIsFixedCompound<S> &&
                  !((std::same_as<Mods, Physics::SurfaceMaterial> || ...) &&
                    (std::same_as<Mods, Physics::PhysicsMaterialID> || ...)))
    [[nodiscard]] auto CreateBody(const S& shape, const Physics::KinematicBody& role, const Mods&... mods) noexcept
        -> Result<Physics::KinematicBodyID> {
        return CreateKinematicBodyRaw(InChain(shape, role, mods...));
    }
    template <typename... Children, Physics::RigidBodyModifier... Mods>
        requires ((Physics::DynamicCompoundChild<Children> && ...) &&
                  !((std::same_as<Mods, Physics::SurfaceMaterial> || ...) &&
                    (std::same_as<Mods, Physics::PhysicsMaterialID> || ...)))
    [[nodiscard]] auto CreateBody(const Physics::FixedCompound<Children...>& shape, const Physics::KinematicBody& role,
                                  const Mods&... mods) noexcept -> Result<Physics::KinematicBodyID> {
        const auto nodes = shape.MakeNodes();
        return CreateKinematicBodyRaw(InChain(Physics::Compound {.children = nodes}, role, mods...));
    }

    template <Physics::DynamicShape S, Physics::RigidBodyModifier... Mods>
        requires (!Physics::kIsFixedCompound<S> &&
                  !((std::same_as<Mods, Physics::SurfaceMaterial> || ...) &&
                    (std::same_as<Mods, Physics::PhysicsMaterialID> || ...)))
    [[nodiscard]] auto CreateBody(const S& shape, const Physics::DynamicBody& role, const Mods&... mods) noexcept
        -> Result<Physics::DynamicBodyID> {
        return CreateDynamicBodyRaw(InChain(shape, role, mods...));
    }
    template <typename... Children, Physics::RigidBodyModifier... Mods>
        requires ((Physics::DynamicCompoundChild<Children> && ...) &&
                  !((std::same_as<Mods, Physics::SurfaceMaterial> || ...) &&
                    (std::same_as<Mods, Physics::PhysicsMaterialID> || ...)))
    [[nodiscard]] auto CreateBody(const Physics::FixedCompound<Children...>& shape, const Physics::DynamicBody& role,
                                  const Mods&... mods) noexcept -> Result<Physics::DynamicBodyID> {
        const auto nodes = shape.MakeNodes();
        return CreateDynamicBodyRaw(InChain(Physics::Compound {.children = nodes}, role, mods...));
    }

    template <Physics::CharacterShape S, Physics::CharacterModifier... Mods>
        requires (!Physics::kIsFixedCompound<S>)
    [[nodiscard]] auto CreateBody(const S& shape, const Physics::CharacterBody& role, const Mods&... mods) noexcept
        -> Result<Physics::CharacterID> {
        return CreateCharacterRaw(InChain(shape, role, mods...));
    }
    template <typename... Children, Physics::CharacterModifier... Mods>
        requires (Physics::DynamicCompoundChild<Children> && ...)
    [[nodiscard]] auto CreateBody(const Physics::FixedCompound<Children...>& shape, const Physics::CharacterBody& role,
                                  const Mods&... mods) noexcept -> Result<Physics::CharacterID> {
        const auto nodes = shape.MakeNodes();
        return CreateCharacterRaw(InChain(Physics::Compound {.children = nodes}, role, mods...));
    }

    template <Physics::ConstraintDesc C>
    [[nodiscard]] auto CreateConstraint(Physics::BodyID body1, Physics::BodyID body2, const C& desc) noexcept
        -> Result<Physics::TypedConstraintID<std::remove_cvref_t<C>>> {
        const Result<Physics::ConstraintID> created = CreateConstraintRaw(body1, body2, InChain(desc));
        if (!created.HasValue()) return created.error;
        return Physics::TypedConstraintID<std::remove_cvref_t<C>>::From(created.value);
    }

    [[nodiscard]] auto Raycast(DVec3 origin, FVec3 direction) const noexcept -> Physics::RaycastResult {
        return Raycast(origin, direction, 1000.0F, Physics::BodyID::Null());
    }

    [[nodiscard]] auto Raycast(DVec3 origin, FVec3 direction, float maxDistance) const noexcept -> Physics::RaycastResult {
        return Raycast(origin, direction, maxDistance, Physics::BodyID::Null());
    }

    template <Physics::ConvexShape S>
    [[nodiscard]] auto Shapecast(const S& shape, DVec3 position, Quat rotation, FVec3 direction,
                                 float maxDistance = 1000.0F, Physics::BodyID ignore = Physics::BodyID::Null()) const noexcept
        -> Result<Physics::ShapeCastResult> {
        return ShapecastRaw(InChain(shape), position, rotation, direction, maxDistance, ignore);
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

    template <Physics::ConvexShape S>
    [[nodiscard]] auto Overlap(const S& shape, DVec3 position, Quat rotation = Quat::Identity()) const
        -> std::expected<std::vector<Physics::BodyID>, ErrorCode> {
        const auto chain = InChain(shape);
        return Collect<&ISimulation::OverlapRaw>(*this, static_cast<const ChainInNode*>(chain), position, rotation);
    }

protected:
    ~ISimulation() = default;
};

} // namespace ZHLN::COM
