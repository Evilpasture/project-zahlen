// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Zahlen/Camera.hpp"
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Zahlen/Buffer.h>
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Geometry/Plane.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/physics/PhysicsHandles.hpp>
#include <Zahlen/Vertex.hpp>
// clang-format on

#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

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

struct DebugVertex {
    float    x, y, z;
    uint32_t color;
};

struct DebugDrawData {
    const DebugVertex* lines     = nullptr;
    size_t             lineCount = 0;
    const DebugVertex* triangles = nullptr;
    size_t             triangleCount = 0;
};

struct BodyStateSnapshot {
    JPH::Vec3 previousPosition = JPH::Vec3::sZero();
    JPH::Vec3 currentPosition  = JPH::Vec3::sZero();
    JPH::Quat previousRotation = JPH::Quat::sIdentity();
    JPH::Quat currentRotation  = JPH::Quat::sIdentity();
    bool      isCharacter      = false;
    bool      valid            = false;
};

enum class ShapeType : uint8_t { Box = 0, Sphere = 1, Capsule = 2, Cylinder = 3, Plane = 4 };

enum class ConstraintType : uint8_t { Fixed, Point, Hinge, Slider, Cone, Distance };

struct ConstraintParams {
    JPH::Vec3 pivot;
    JPH::Vec3 axis;
    float     limitMin;
    float     limitMax;
    bool  hasMotor;
    float target;
    float frequency;
    float damping;
    float maxForce;
    bool  disableCollisions;
};

struct ConstraintHandle {
    uint32_t                     index;
    uint32_t                     generation;
    [[nodiscard]] constexpr auto Pack() const noexcept -> uint64_t {
        return (static_cast<uint64_t>(generation) << 32) | index;
    }
};

static_assert((std::is_trivially_default_constructible_v<ConstraintHandle> && std::is_trivially_copyable_v<ConstraintHandle>) );
static_assert((std::is_trivially_default_constructible_v<ConstraintParams> && std::is_trivially_copyable_v<ConstraintParams>) );

struct RagdollPartParams {
    uint32_t       jointIndex;
    int            parentJointIndex = -1;
    JPH::ShapeRefC shape            = nullptr;
    float          mass             = 10.0f;

    JPH::RVec3 position = JPH::RVec3::sZero();
    JPH::Quat  rotation = JPH::Quat::sIdentity();

    JPH::Vec3 twistAxis   = JPH::Vec3::sAxisX();
    JPH::Vec3 planeNormal = JPH::Vec3::sAxisY();

    float coneAngle = 0.0f;
    float twistMin  = -0.1f;
    float twistMax  = 0.1f;

    bool  enableMotors  = true;
    float maxMotorForce = 100.0f;
};

struct RaycastResult {
    BodyHandle handle;
    JPH::Vec3  normal;
    JPH::RVec3 position;
    float      fraction;
    bool       hasHit;
};

struct RaycastPenetrationResult {
    BodyHandle handle;
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
    BodyHandle handle;
    JPH::RVec3 contactPoint;
    JPH::Vec3  contactNormal;
    float      fraction;
    bool       hasHit;
};

struct CullResult {
    Physics::BodyHandle* results;
    uint32_t      count;
};

static_assert(
    (std::is_trivially_default_constructible_v<RaycastResult> && std::is_trivially_copyable_v<RaycastResult>) &&
    (std::is_trivially_default_constructible_v<RaycastPenetrationResult> && std::is_trivially_copyable_v<RaycastPenetrationResult>) &&
    (std::is_trivially_default_constructible_v<ShapeCastResult> && std::is_trivially_copyable_v<ShapeCastResult>) &&
    (std::is_trivially_default_constructible_v<CullResult> && std::is_trivially_copyable_v<CullResult>)
);

auto CreateMeshShape(const VertexPosition* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount) -> JPH::ShapeRefC;
auto CreateHeightFieldShape(const float* heights, int sampleCount, float worldSize) -> JPH::ShapeRefC;
auto GetBodyID(const PhysicsWorld& world, Physics::BodyHandle handle) -> JPH::BodyID;

struct DualShapeConfig {
    float lifterRadius   = 0.40f;
    float bumperRadiusXZ = 0.50f;
    float bumperRadiusY  = 0.70f;

    [[nodiscard]] constexpr auto GetLifterOffsetY() const noexcept -> float {
        return lifterRadius;
    }

    [[nodiscard]] constexpr auto GetBumperOffsetY() const noexcept -> float {
        const float ratio = lifterRadius / bumperRadiusXZ;
        if (ratio >= 1.0f) {
            return lifterRadius;
        }
        return lifterRadius + (bumperRadiusY * std::sqrt(1.0f - (ratio * ratio)));
    }
};

auto CreateDualShape(const DualShapeConfig& config = {}) -> JPH::ShapeRefC;

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

class ZHLN_API PhysicsContext {
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

    auto GetOrCreateShape(Physics::ShapeType type, float p1, float p2 = 0.0f, float p3 = 0.0f, float p4 = 0.0f) -> JPH::ShapeRefC;

    auto CreateRigidBody(
        const JPH::ShapeRefC& shape,
        JPH::RVec3Arg         pos,
        JPH::QuatArg          rot,
        JPH::EMotionType      motion,
        Layers::ID            layer,
        uint32_t              materialID = 0,
        uint32_t              category   = 0xFFFFFFFF,
        uint32_t              mask       = 0xFFFFFFFF
    ) -> Physics::BodyHandle;

    auto CreateMeshBody(
        const VertexPosition* vertices,
        uint32_t              vertexCount,
        const uint32_t*       indices,
        uint32_t              indexCount,
        JPH::RVec3Arg         pos,
        JPH::QuatArg          rot,
        uint32_t              category = 0xFFFFFFFF,
        uint32_t              mask     = 0xFFFFFFFF
    ) -> Physics::BodyHandle;

    auto CreateCharacter(JPH::RVec3Arg position, const Physics::CharacterParams& params = {}) -> Physics::BodyHandle;

    // Physics owns the Jolt instance; ECS components borrow a generational handle.
    auto CreateSkeletalRagdoll(JPH::Ref<JPH::Skeleton> skeleton, const std::vector<Physics::RagdollPartParams>& parts) -> Physics::RagdollHandle;
    void DestroyRagdoll(Physics::RagdollHandle handle) noexcept;
    // Borrowed until DestroyRagdoll; use only while the owning component is live.
    [[nodiscard]] auto GetRagdoll(Physics::RagdollHandle handle) const noexcept -> JPH::Ragdoll*;
    void ActivateRagdoll(Physics::RagdollHandle handle, const JPH::SkeletonPose& pose, JPH::Vec3Arg initialVelocity) noexcept;
    void RemoveRagdoll(Physics::RagdollHandle handle) noexcept;
    void DriveRagdollPose(Physics::RagdollHandle handle, const JPH::SkeletonPose& pose) noexcept;
    void AddRagdollImpulse(Physics::RagdollHandle handle, uint32_t jointIndex, JPH::Vec3Arg impulse) noexcept;
    [[nodiscard]] bool TryGetBodyPosition(Physics::BodyHandle handle, JPH::RVec3& outPosition) const noexcept;
    [[nodiscard]] bool TryGetBodyState(Physics::BodyHandle handle, Physics::BodyStateSnapshot& outState) const noexcept;
    void FillBodyStates(std::span<const Physics::BodyHandle> handles, std::span<Physics::BodyStateSnapshot> outStates) const noexcept;
    [[nodiscard]] bool GetRagdollPose(Physics::RagdollHandle handle, JPH::RVec3& outRootOffset, JPH::Mat44* outWorldJoints) const noexcept;

    void               SetCollisionFilter(Physics::BodyHandle handle, uint32_t category, uint32_t mask);
    [[nodiscard]] auto GetDebugDrawData(bool drawShapes = true, bool drawConstraints = true, bool wireframe = true) const -> Physics::DebugDrawData;
    void               RegisterMaterial(uint32_t id, float friction, float restitution);

    void DestroyBody(Physics::BodyHandle handle);

    void SetLinearVelocity(Physics::BodyHandle handle, JPH::Vec3Arg velocity);
    void SetCharacterVelocity(Physics::BodyHandle handle, JPH::Vec3Arg velocity);
    void SetCharacterPosition(Physics::BodyHandle handle, JPH::RVec3Arg position);

    auto               GetCharacterVelocity(Physics::BodyHandle handle) const -> JPH::Vec3;
    [[nodiscard]] auto IsCharacterOnGround(Physics::BodyHandle handle) const -> bool;
    [[nodiscard]] auto IsBodyDynamic(Physics::BodyHandle handle) const -> bool;
    [[nodiscard]] auto GetPositionBuffer() const -> BufferView;
    auto               GetRotation(JPH::BodyID bodyID) const -> JPH::Quat;
    void               AddImpulse(Physics::BodyHandle handle, JPH::Vec3Arg impulse);
    void               AddImpulse(Physics::BodyHandle handle, JPH::Vec3Arg impulse, JPH::RVec3Arg position);

    void AddRadialImpulse(JPH::RVec3Arg center, float radius, float maxImpulse);

    [[nodiscard]] auto GetContactEvents() const -> std::pair<const Physics::ContactEvent*, size_t>;

    auto CreateConstraint(Physics::ConstraintType type, Physics::BodyHandle b1, Physics::BodyHandle b2, const Physics::ConstraintParams& params) -> Physics::ConstraintHandle;
    void SetConstraintTarget(Physics::ConstraintHandle handle, float value);

    [[nodiscard]] auto
        Raycast(JPH::RVec3Arg origin, JPH::Vec3Arg direction, float maxDistance = 1000.0f, Physics::BodyHandle ignore = {}) const -> Physics::RaycastResult;

    void RaycastAll(
        JPH::RVec3Arg                       origin,
        JPH::Vec3Arg                        direction,
        float                               maxDistance,
        JPH::Array<Physics::RaycastResult>& outResults,
        Physics::BodyHandle                 ignore = {}
    ) const;

    [[nodiscard]] auto RaycastPenetration(JPH::RVec3Arg origin, JPH::Vec3Arg direction, float maxDistance = 1000.0f, Physics::BodyHandle ignore = {}) const
        -> Physics::RaycastPenetrationResult;

    void RaycastAllPenetrations(
        JPH::RVec3Arg                                  origin,
        JPH::Vec3Arg                                   direction,
        float                                          maxDistance,
        JPH::Array<Physics::RaycastPenetrationResult>& outResults,
        Physics::BodyHandle                           ignore = {}
    ) const;

    [[nodiscard]] auto Shapecast(
        const JPH::ShapeRefC& shape,
        JPH::RVec3Arg         pos,
        JPH::QuatArg          rot,
        JPH::Vec3Arg          direction,
        float                 maxDistance = 1000.0f,
        Physics::BodyHandle  ignore      = {}
    ) const -> Physics::ShapeCastResult;

    void OverlapSphere(JPH::RVec3Arg center, float radius, JPH::Array<Physics::BodyHandle>& outResults) const;
    void OverlapAABB(JPH::RVec3Arg minBox, JPH::RVec3Arg maxBox, JPH::Array<Physics::BodyHandle>& outResults) const;
    void QueryAABB(JPH::Vec3Arg min, JPH::Vec3Arg max, JPH::Array<Physics::BodyHandle>& outHandles) const;
    void FrustumCull(const JPH::Mat44& viewProj, const Frustum& frustum, JPH::Array<Physics::BodyHandle>& outHandles) const;

    [[nodiscard]] auto GetBodyHandle(JPH::BodyID bodyID) const -> Physics::BodyHandle;

    [[nodiscard]] auto GetInternalSystem() noexcept -> JPH::PhysicsSystem&;
    [[nodiscard]] auto GetInternalSystem() const noexcept -> const JPH::PhysicsSystem&;
    [[nodiscard]] auto GetInternalWorld() noexcept -> Physics::PhysicsWorld&;
    [[nodiscard]] auto GetInternalWorld() const noexcept -> const Physics::PhysicsWorld&;

  private:
    auto RegisterRagdoll(JPH::Ref<JPH::Ragdoll> ragdoll) -> Physics::RagdollHandle;
    std::unique_ptr<Impl> _impl;
};

}
