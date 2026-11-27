// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <optional>
#include <Zahlen/Core/Buffer.h>
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Geometry/Plane.h>
#include <Jolt/Math/DVec3.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Physics/PhysicsIDs.hpp>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <Zahlen/Physics/PhysicsShapes.hpp>
#include <Zahlen/Render/Vertex.hpp>
// clang-format on

#include <cstdint>
#include <memory>
#include <span>
#include <variant>
#include <vector>
#include <type_traits>

namespace JPH {
class SkeletonPose;
}

namespace ZHLN {

namespace Layers {
enum class ID : JPH::ObjectLayer { NON_MOVING = 0, MOVING = 1 };
}
namespace BroadPhaseLayers {
enum class ID : uint8_t { NON_MOVING = 0, MOVING = 1 };
}

namespace Physics {
struct PhysicsWorld;
struct ContactEvent;

struct RagdollPartParams {
    uint32_t       jointIndex;
    int            parentJointIndex = -1;
    JPH::ShapeRefC shape            = nullptr;
    float          mass             = 10.0f;

    ZHLN::DVec3 position {};
    JPH::Quat     rotation = JPH::Quat::sIdentity();

    ZHLN::FVec3 twistAxis {1.0f, 0.0f, 0.0f};
    ZHLN::FVec3 planeNormal {0.0f, 1.0f, 0.0f};

    float coneAngle = 0.0f;
    float twistMin  = -0.1f;
    float twistMax  = 0.1f;

    bool  enableMotors  = true;
    float maxMotorForce = 100.0f;
};

struct RaycastPenetrationResult {
    BodyID     bodyID;
    JPH::RVec3 entryPosition;
    JPH::RVec3 exitPosition;
    JPH::Vec3  entryNormal;
    JPH::Vec3  exitNormal;
    float      entryFraction;
    float      exitFraction;
    float      thickness;
    uint32_t   materialID;
    bool       hasHit;
};

struct ShapeCastResult {
    BodyID     bodyID;
    JPH::RVec3 contactPoint;
    JPH::Vec3  contactNormal;
    float      fraction;
    bool       hasHit;
};

struct CullResult {
    Physics::BodyID* results;
    uint32_t      count;
};

static_assert(
    (std::is_trivially_default_constructible_v<RaycastResult> && std::is_trivially_copyable_v<RaycastResult>) &&
    (std::is_trivially_default_constructible_v<RaycastPenetrationResult> && std::is_trivially_copyable_v<RaycastPenetrationResult>) &&
    (std::is_trivially_default_constructible_v<ShapeCastResult> && std::is_trivially_copyable_v<ShapeCastResult>) &&
    (std::is_trivially_default_constructible_v<CullResult> && std::is_trivially_copyable_v<CullResult>)
);

// Runtime-held body shape for chain dispatch and prefab colliders. Not a
// wire type and never an argument to creation: a caller that only knows the
// shape at runtime holds one of these, then re-dispatches through the typed
// CreateRigidBody<S> so each creation call still names its shape statically.
using ShapeVariant = std::variant<Box, Sphere, Capsule, Cylinder, Plane, TriangleMesh, MeshAsset, HeightField, Compound>;

// Runtime-held constraint parameters for the mailbox; creation goes through
// the typed CreateConstraint<C> template, never through this variant directly.
using ConstraintVariant =
    std::variant<FixedConstraint, PointConstraint, DistanceConstraint, HingeConstraint, SliderConstraint, ConeConstraint>;

auto CreateMeshShape(const VertexPosition* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount) -> JPH::ShapeRefC;
auto CreateHeightFieldShape(const float* heights, int sampleCount, float worldSize) -> JPH::ShapeRefC;
auto GetBodyID(const PhysicsWorld& world, Physics::BodyID handle) -> JPH::BodyID;

auto CreateDualShape(const DualShape& shape = {}) -> JPH::ShapeRefC;

// Character creation owns Jolt shape/plane objects, so it belongs with the
// physics implementation rather than in the Jolt-free query value types.
struct CharacterParams {
    JPH::ShapeRefC shape = nullptr;

    float          maxSlopeAngle            = JPH::DegreesToRadians(45.0f);
    float          maxStrength              = 100.0f;
    float          characterPadding         = 0.02f;
    float          penetrationRecoverySpeed = 1.0f;
    JPH::Plane     supportingVolume         = JPH::Plane(JPH::Vec3::sAxisY(), -0.4f);
    uint32_t       category                 = 0xFFFFFFFF;
    uint32_t       mask                     = 0xFFFFFFFF;
};

}

class PhysicsContext {
  public:
    PhysicsContext();
    ~PhysicsContext();

    PhysicsContext(const PhysicsContext&)                    = delete;
    auto operator=(const PhysicsContext&) -> PhysicsContext& = delete;

    PhysicsContext(const PhysicsConfig& cfg);

    void               Step(float deltaTime);
    [[nodiscard]] auto GetActiveBodyCount() const -> uint32_t;
    [[nodiscard]] auto GetMemoryUsage() const -> size_t;
    void TraceDiagnostics() const;

    struct Impl;
    [[nodiscard]] auto GetWorld() const -> const Physics::PhysicsWorld&;

    void OptimizeBroadphase();

    // Level-unload seam: drop cached mesh flyweights; bodies and queued
    // commands keep their own shape references. Primitive cache is unchanged.
    void ClearMeshShapeCache() noexcept;

    // Bind only while no threads are resolving shapes. The caller keeps the
    // resolver context alive until this PhysicsContext is destroyed or unbound.
    void BindCollisionAssetResolver(Physics::CollisionAssetResolver resolver, void* context) noexcept;

    // Typed creation: the shape travels as its value struct, so a wrong
    // shape/wrong-argument pairing cannot compile. Runtime-held shapes (a
    // chain node, a prefab collider) live in ShapeVariant and re-dispatch
    // through this template; no runtime type enum crosses the API.
    template <typename S>
    [[nodiscard]] auto CreateRigidBody(
        const S& shape, DVec3 pos, FVec4 rot, Physics::MotionType motion, Physics::ObjectLayer layer,
        uint32_t materialID = 0, uint32_t category = 0xFFFFFFFF, uint32_t mask = 0xFFFFFFFF,
        Physics::Activation activation = Physics::Activation::Activate, FVec3 scale = {1, 1, 1}
    ) -> Physics::BodyID {
        return CreateRigidBodyFromVariant(
            ShapeVariant {shape}, pos, rot, motion, layer, materialID, category, mask, activation, scale
        );
    }

    auto CreateCharacter(ZHLN::DVec3 position, const Physics::CharacterParams& params = {}) -> Physics::BodyID;
    auto CreateCustomCharacter(const Physics::CharacterCreateDesc& desc) -> Physics::BodyID;
    [[nodiscard]] auto CharacterVelocity(Physics::BodyID handle) const -> ZHLN::FVec3;

    // Physics owns the Jolt instance; ECS components borrow a generational handle.
    auto CreateSkeletalRagdoll(JPH::Ref<JPH::Skeleton> skeleton, std::span<const Physics::RagdollPartParams> parts) -> Physics::RagdollID;
    void DestroyRagdoll(Physics::RagdollID handle) noexcept;
    void DestroyRagdolls(std::span<const Physics::RagdollID> handles) noexcept;
    // Borrowed until DestroyRagdoll; use only while the owning component is live.
    [[nodiscard]] auto GetRagdoll(Physics::RagdollID handle) const noexcept -> ZHLN::Optional<JPH::Ragdoll&>;
    void               ActivateRagdoll(Physics::RagdollID handle, const JPH::SkeletonPose& pose, ZHLN::FVec3 initialVelocity) noexcept;
    void               RemoveRagdoll(Physics::RagdollID handle) noexcept;
    void               DriveRagdollPose(Physics::RagdollID handle, const JPH::SkeletonPose& pose) noexcept;
    void               AddRagdollImpulse(Physics::RagdollID handle, uint32_t jointIndex, ZHLN::FVec3 impulse) noexcept;
    // Generation-safe lookups: the optional ties the result to its proof of
    // validity; a stale handle simply has no value to hand back.
    [[nodiscard]] auto BodyPosition(Physics::BodyID handle) const noexcept -> std::optional<DVec3>;
    [[nodiscard]] auto BodyState(Physics::BodyID handle) const noexcept -> std::optional<Physics::BodyStateSnapshot>;
    [[nodiscard]] auto BodyLinearVelocity(Physics::BodyID handle) const noexcept -> std::optional<FVec3>;
    void FillBodyStates(std::span<const Physics::BodyID> handles, std::span<Physics::BodyStateSnapshot> outStates) const noexcept;
    [[nodiscard]] bool GetRagdollPose(Physics::RagdollID handle, JPH::RVec3& outRootOffset, JPH::Mat44* outWorldJoints) const noexcept;

    void               SetCollisionFilter(Physics::BodyID handle, uint32_t category, uint32_t mask);
    [[nodiscard]] auto GetDebugDrawData(bool drawShapes = true, bool drawConstraints = true, bool wireframe = true) const -> Physics::DebugDrawData;
    void               RegisterMaterial(uint32_t id, float friction, float restitution);

    void DestroyBody(Physics::BodyID handle);
    // Queue an entire scene cleanup under one shadow lock. Stale, null and
    // duplicate handles are ignored using the same validation as DestroyBody.
    void DestroyBodies(std::span<const Physics::BodyID> handles);

    void SetLinearVelocity(Physics::BodyID handle, ZHLN::FVec3 velocity);
    void SetCharacterVelocity(Physics::BodyID handle, ZHLN::FVec3 velocity);
    void SetCharacterPosition(Physics::BodyID handle, ZHLN::DVec3 position);
    // Teleports a kinematic (scripted) rigid body. SetCharacterPosition only
    // works on CharacterVirtual bodies; rigid-body handles need this instead.
    // Ignored for static bodies, which cannot be moved in the broadphase.
    void SetKinematicPose(Physics::BodyID handle, ZHLN::DVec3 position, ZHLN::FVec4 rotation);

    auto               GetCharacterVelocity(Physics::BodyID handle) const -> JPH::Vec3;
    // Vertical half-extent of a character's collision shape, i.e. the distance
    // from the body origin (capsule centre) to the soles of its feet. Gameplay
    // needs this to place feet-referenced spawn points -- Source's
    // info_player_start and info_teleport_destination origins are at the floor.
    // Returns 0 for stale or non-character handles.
    [[nodiscard]] auto GetCharacterGroundOffset(Physics::BodyID handle) const -> float;
    [[nodiscard]] auto IsCharacterOnGround(Physics::BodyID handle) const -> bool;
    [[nodiscard]] auto IsBodyDynamic(Physics::BodyID handle) const -> bool;
    [[nodiscard]] auto GetPositionBuffer() const -> BufferView;
    auto               GetRotation(JPH::BodyID bodyID) const -> JPH::Quat;
    void               AddImpulse(Physics::BodyID handle, ZHLN::FVec3 impulse);
    void               AddImpulse(Physics::BodyID handle, ZHLN::FVec3 impulse, ZHLN::DVec3 position);

    void AddRadialImpulse(ZHLN::DVec3 center, float radius, float maxImpulse);

    [[nodiscard]] auto GetContactEvents() const -> std::pair<const Physics::ContactEvent*, size_t>;

    // Typed constraint creation: the parameter struct names the kind, so a
    // hinge's limits can never be fed to a slider. The mailbox re-dispatches
    // through ConstraintVariant; no runtime type enum crosses this API.
    template <typename C>
    [[nodiscard]] auto CreateConstraint(Physics::BodyID b1, Physics::BodyID b2, const C& params)
        -> Physics::TypedConstraintID<C> {
        return Physics::TypedConstraintID<C>::From(CreateConstraintVariant(b1, b2, Physics::ConstraintVariant {params}));
    }
    void SetTargetAngle(Physics::TypedConstraintID<Physics::HingeConstraint> handle, ZHLN::RadF angle);
    void SetTargetPosition(Physics::TypedConstraintID<Physics::SliderConstraint> handle, float position);

    [[nodiscard]] auto
        Raycast(ZHLN::DVec3 origin, ZHLN::FVec3 direction, float maxDistance = 1000.0f, Physics::BodyID ignore = {}) const -> Physics::RaycastResult;

    // Queries return their own result vector. No caller-owned container
    // reference crosses this boundary: JPH::Array is a vendor type, and an
    // out-param `Container<T>&` is the same mutable-ownership leak as a
    // container-returning getter.
    [[nodiscard]] auto RaycastAll(
        ZHLN::DVec3       origin,
        ZHLN::FVec3        direction,
        float               maxDistance,
        Physics::BodyID ignore = {}
    ) const -> std::vector<Physics::RaycastResult>;

    [[nodiscard]] auto RaycastPenetration(ZHLN::DVec3 origin, ZHLN::FVec3 direction, float maxDistance = 1000.0f, Physics::BodyID ignore = {}) const
        -> Physics::RaycastPenetrationResult;

    [[nodiscard]] auto RaycastAllPenetrations(
        ZHLN::DVec3       origin,
        ZHLN::FVec3        direction,
        float               maxDistance,
        Physics::BodyID ignore = {}
    ) const -> std::vector<Physics::RaycastPenetrationResult>;

    [[nodiscard]] auto Shapecast(
        const JPH::ShapeRefC& shape,
        ZHLN::DVec3         pos,
        ZHLN::FVec4          rot,
        ZHLN::FVec3          direction,
        float                 maxDistance = 1000.0f,
        Physics::BodyID   ignore      = {}
    ) const -> Physics::ShapeCastResult;

    [[nodiscard]] auto OverlapSphere(ZHLN::DVec3 center, float radius) const -> std::vector<Physics::BodyID>;
    [[nodiscard]] auto OverlapAABB(ZHLN::DVec3 minBox, ZHLN::DVec3 maxBox) const -> std::vector<Physics::BodyID>;
    [[nodiscard]] auto QueryAABB(ZHLN::FVec3 min, ZHLN::FVec3 max) const -> std::vector<Physics::BodyID>;
    [[nodiscard]] auto FrustumCull(const JPH::Mat44& viewProj, const Frustum& frustum) const -> std::vector<Physics::BodyID>;

    [[nodiscard]] auto GetBodyID(JPH::BodyID bodyID) const -> Physics::BodyID;

    [[nodiscard]] auto GetInternalSystem() noexcept -> JPH::PhysicsSystem&;
    [[nodiscard]] auto GetInternalSystem() const noexcept -> const JPH::PhysicsSystem&;
    [[nodiscard]] auto GetInternalWorld() noexcept -> Physics::PhysicsWorld&;
    [[nodiscard]] auto GetInternalWorld() const noexcept -> const Physics::PhysicsWorld&;

  private:
    // Jolt-facing plumbing: resolve a runtime-held shape, then create the
    // body. The public template is the only door in.
    [[nodiscard]] auto CreateConstraintVariant(Physics::BodyID b1, Physics::BodyID b2, const Physics::ConstraintVariant& params)
        -> Physics::ConstraintID;
    [[nodiscard]] auto CreateRigidBodyFromVariant(
        const ShapeVariant& shape, DVec3 pos, FVec4 rot, Physics::MotionType motion, Physics::ObjectLayer layer,
        uint32_t materialID, uint32_t category, uint32_t mask, Physics::Activation activation, FVec3 scale
    ) -> Physics::BodyID;
    auto RegisterRagdoll(JPH::Ref<JPH::Ragdoll> ragdoll) -> Physics::RagdollID;
    std::unique_ptr<Impl> _impl;
};

}
