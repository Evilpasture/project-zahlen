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

struct CullResult {
    Physics::BodyID* results;
    uint32_t      count;
};

static_assert(
    (std::is_trivially_default_constructible_v<RaycastResult> && std::is_trivially_copyable_v<RaycastResult>) &&
    (std::is_trivially_default_constructible_v<CullResult> && std::is_trivially_copyable_v<CullResult>)
);

// Runtime-held body shape for chain dispatch and prefab colliders. Not a
// wire type and never an argument to creation: a caller that only knows the
// shape at runtime holds one of these, then re-dispatches through the typed
// CreateBody<S> so each creation call still names its shape statically.
using ShapeVariant = std::variant<Box, Sphere, Capsule, Cylinder, Plane, TriangleMesh, MeshAsset, HeightField, Compound>;

// Runtime-held constraint parameters for the mailbox; creation goes through
// the typed CreateConstraint<C> template, never through this variant directly.
using ConstraintVariant =
    std::variant<FixedConstraint, PointConstraint, DistanceConstraint, HingeConstraint, SliderConstraint, ConeConstraint>;

// Resolved body-creation modifiers (a chain's SurfaceMaterial / CollisionFilter
// / MovingBroadphase nodes, or defaults). Not a wire type: chain dispatchers
// fill one and hand it to CreateBody.
struct BodyModifiers {
    SurfaceMaterial surface {};
    CollisionFilter filter {};
    bool            hasSurface       = false;
    bool            movingBroadphase = false;
};

auto CreateMeshShape(const VertexPosition* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount) -> JPH::ShapeRefC;
auto CreateHeightFieldShape(const float* heights, int sampleCount, float worldSize) -> JPH::ShapeRefC;
auto GetBodyID(const PhysicsWorld& world, Physics::BodyID handle) -> JPH::BodyID;

auto CreateDualShape(const DualShape& shape = {}) -> JPH::ShapeRefC;

} // namespace Physics

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

    // --- Typed body creation -------------------------------------------------
    // One overload per role: the role node carries pose + motion policy, the
    // shape carries geometry, BodyModifiers carry material/filter/broadphase
    // details. Layer, activation and sleep policy are derived from the role,
    // so they cannot contradict it at the call site.
    template <Physics::StaticShape S, Physics::BodyModifier... Mods>
    [[nodiscard]] auto CreateBody(const S& shape, const Physics::StaticBody& role, const Mods&... mods) -> Physics::StaticBodyID {
        return CreateBodyImpl(ShapeVariant {shape}, role, ResolveModifiers(mods...));
    }
    template <Physics::DynamicShape S, Physics::BodyModifier... Mods>
    [[nodiscard]] auto CreateBody(const S& shape, const Physics::KinematicBody& role, const Mods&... mods)
        -> Physics::KinematicBodyID {
        return CreateBodyImpl(ShapeVariant {shape}, role, ResolveModifiers(mods...));
    }
    template <Physics::DynamicShape S, Physics::BodyModifier... Mods>
    [[nodiscard]] auto CreateBody(const S& shape, const Physics::DynamicBody& role, const Mods&... mods) -> Physics::DynamicBodyID {
        return CreateBodyImpl(ShapeVariant {shape}, role, ResolveModifiers(mods...));
    }
    template <Physics::CharacterShape S, Physics::BodyModifier... Mods>
    [[nodiscard]] auto CreateBody(const S& shape, const Physics::CharacterBody& role, const Mods&... mods) -> Physics::CharacterID {
        return CreateBodyImpl(shape, role, ResolveModifiers(mods...));
    }
    // Overloads for callers that already resolved modifiers (chain dispatchers).
    template <Physics::StaticShape S>
    [[nodiscard]] auto CreateBody(const S& shape, const Physics::StaticBody& role, const Physics::BodyModifiers& mods) -> Physics::StaticBodyID {
        return CreateBodyImpl(ShapeVariant {shape}, role, mods);
    }
    template <Physics::DynamicShape S>
    [[nodiscard]] auto CreateBody(const S& shape, const Physics::KinematicBody& role, const Physics::BodyModifiers& mods)
        -> Physics::KinematicBodyID {
        return CreateBodyImpl(ShapeVariant {shape}, role, mods);
    }
    template <Physics::DynamicShape S>
    [[nodiscard]] auto CreateBody(const S& shape, const Physics::DynamicBody& role, const Physics::BodyModifiers& mods)
        -> Physics::DynamicBodyID {
        return CreateBodyImpl(ShapeVariant {shape}, role, mods);
    }
    template <Physics::CharacterShape S>
    [[nodiscard]] auto CreateBody(const S& shape, const Physics::CharacterBody& role, const Physics::BodyModifiers& mods) -> Physics::CharacterID {
        return CreateBodyImpl(shape, role, mods);
    }

    // --- Role narrowing (ISimulation tier) ------------------------------------
    // BodyID -> typed ID while the slot is alive with that role's motion type;
    // std::nullopt otherwise (wrong role, stale generation, wrong state).
    [[nodiscard]] auto AsDynamic(Physics::BodyID body) const noexcept -> std::optional<Physics::DynamicBodyID>;
    [[nodiscard]] auto AsKinematic(Physics::BodyID body) const noexcept -> std::optional<Physics::KinematicBodyID>;
    [[nodiscard]] auto AsCharacter(Physics::BodyID body) const noexcept -> std::optional<Physics::CharacterID>;

    [[nodiscard]] auto CharacterVelocity(Physics::CharacterID handle) const -> ZHLN::FVec3;

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

    void               SetCollisionFilter(Physics::BodyID handle, Physics::CollisionFilter filter);
    [[nodiscard]] auto GetDebugDrawData(bool drawShapes = true, bool drawConstraints = true, bool wireframe = true) const -> Physics::DebugDrawData;
    // Fingerprinted: identical (friction, restitution) reuses one handle, so
    // body creation need not re-register. PhysicsMaterialID::Default (0) is
    // Jolt friction 0.2 / restitution 0.0.
    [[nodiscard]] auto RegisterMaterial(Physics::SurfaceMaterial material) -> Physics::PhysicsMaterialID;

    void DestroyBody(Physics::BodyID handle);
    // Queue an entire scene cleanup under one shadow lock. Stale, null and
    // duplicate handles are ignored using the same validation as DestroyBody.
    void DestroyBodies(std::span<const Physics::BodyID> handles);

    // Role-scoped mutations: the typed ID is the proof that the slot holds
    // that role, so a character velocity can never be written to a box.
    void SetVelocity(Physics::DynamicBodyID handle, ZHLN::FVec3 velocity);
    void SetVelocity(Physics::CharacterID handle, ZHLN::FVec3 velocity);
    void SetPose(Physics::CharacterID handle, ZHLN::DVec3 position);
    // Teleports a kinematic (scripted) rigid body. SetPose(CharacterID) only
    // works on CharacterVirtual bodies; rigid-body handles need this instead.
    // Static bodies cannot be moved in the broadphase and have no setter.
    void SetPose(Physics::KinematicBodyID handle, ZHLN::DVec3 position, ZHLN::FVec4 rotation);

    auto               GetCharacterVelocity(Physics::CharacterID handle) const -> JPH::Vec3;
    // Vertical half-extent of a character's collision shape, i.e. the distance
    // from the body origin (capsule centre) to the soles of its feet. Gameplay
    // needs this to place feet-referenced spawn points -- Source's
    // info_player_start and info_teleport_destination origins are at the floor.
    // Returns 0 for stale or non-character handles.
    [[nodiscard]] auto GetCharacterGroundOffset(Physics::CharacterID handle) const -> float;
    [[nodiscard]] auto IsCharacterOnGround(Physics::CharacterID handle) const -> bool;
    [[nodiscard]] auto IsBodyDynamic(Physics::BodyID handle) const -> bool;
    [[nodiscard]] auto GetPositionBuffer() const -> BufferView;
    auto               GetRotation(JPH::BodyID bodyID) const -> JPH::Quat;
    void               AddImpulse(Physics::DynamicBodyID handle, ZHLN::FVec3 impulse);

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
    [[nodiscard]] auto CreateConstraintVariant(Physics::BodyID b1, Physics::BodyID b2, const Physics::ConstraintVariant& params)
        -> Physics::ConstraintID;

    [[nodiscard]] static auto ResolveModifiers() noexcept -> Physics::BodyModifiers { return {}; }
    [[nodiscard]] static auto ResolveModifiers(const Physics::SurfaceMaterial& m) noexcept -> Physics::BodyModifiers {
        return Physics::BodyModifiers {.surface = m, .hasSurface = true};
    }
    [[nodiscard]] static auto ResolveModifiers(const Physics::CollisionFilter& f) noexcept -> Physics::BodyModifiers {
        return Physics::BodyModifiers {.filter = f};
    }
    [[nodiscard]] static auto ResolveModifiers(const Physics::MovingBroadphase&) noexcept -> Physics::BodyModifiers {
        return Physics::BodyModifiers {.movingBroadphase = true};
    }
    template <Physics::BodyModifier First, Physics::BodyModifier... Rest>
    [[nodiscard]] static auto ResolveModifiers(const First& first, const Rest&... rest) noexcept -> Physics::BodyModifiers {
        Physics::BodyModifiers out = ResolveModifiers(first);
        const auto    merge = [&out](const auto& mod) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(mod)>, Physics::SurfaceMaterial>) {
                out.surface    = mod;
                out.hasSurface = true;
            } else if constexpr (std::is_same_v<std::remove_cvref_t<decltype(mod)>, Physics::CollisionFilter>) {
                out.filter = mod;
            } else {
                out.movingBroadphase = true;
            }
        };
        (merge(rest), ...);
        return out;
    }

    // Jolt-facing plumbing behind the public CreateBody overloads.
    [[nodiscard]] auto ResolveShape(const Physics::ShapeVariant& shape, ZHLN::FVec3 scale) -> JPH::ShapeRefC;
    [[nodiscard]] auto CreateRigidBodyCore(const JPH::ShapeRefC& pinnedShape, ZHLN::DVec3 position, ZHLN::FVec4 rotation,
                                           JPH::EMotionType motionType, Layers::ID layer, JPH::EActivation activation,
                                           ZHLN::FVec3 velocity, uint32_t materialID, uint32_t category, uint32_t mask)
        -> Physics::BodyID;
    [[nodiscard]] auto CreateBodyImpl(const Physics::ShapeVariant& shape, const Physics::StaticBody& role, const Physics::BodyModifiers& mods)
        -> Physics::StaticBodyID;
    [[nodiscard]] auto CreateBodyImpl(const Physics::ShapeVariant& shape, const Physics::KinematicBody& role, const Physics::BodyModifiers& mods)
        -> Physics::KinematicBodyID;
    [[nodiscard]] auto CreateBodyImpl(const Physics::ShapeVariant& shape, const Physics::DynamicBody& role, const Physics::BodyModifiers& mods)
        -> Physics::DynamicBodyID;
    [[nodiscard]] auto CreateBodyImpl(const Physics::DualShape& shape, const Physics::CharacterBody& role, const Physics::BodyModifiers& mods)
        -> Physics::CharacterID;
    [[nodiscard]] auto CreateBodyImpl(const Physics::Capsule& shape, const Physics::CharacterBody& role, const Physics::BodyModifiers& mods)
        -> Physics::CharacterID;
    [[nodiscard]] auto CreateCharacterBody(const JPH::ShapeRefC& pinnedShape, float planeC, const Physics::CharacterBody& role,
                                           const Physics::BodyModifiers& mods) -> Physics::BodyID;
    auto RegisterRagdoll(JPH::Ref<JPH::Ragdoll> ragdoll) -> Physics::RagdollID;
    std::unique_ptr<Impl> _impl;
};

}
