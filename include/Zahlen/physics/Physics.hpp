// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Zahlen/Camera.hpp"
#include <Jolt/Physics/Ragdoll/Ragdoll.h>
#include <Zahlen/Buffer.h>
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Types.hpp>
// clang-format on

#include <cstdint>
#include <memory>

namespace ZHLN {

namespace Layers {
enum ID : JPH::ObjectLayer { NON_MOVING = 0, MOVING = 1, NUM_LAYERS = 2 };
}
namespace BroadPhaseLayers {
enum ID : uint8_t { NON_MOVING = 0, MOVING = 1, NUM_LAYERS = 2 };
}

namespace Physics {
struct PhysicsWorld;
struct DebugDrawData;
struct ContactEvent;
enum class ShapeType : uint8_t { Box = 0, Sphere = 1, Capsule = 2, Cylinder = 3, Plane = 4 };

enum class ConstraintType : uint8_t { Fixed, Point, Hinge, Slider, Cone, Distance };

struct ConstraintParams {
    JPH::Vec3 pivot;
    JPH::Vec3 axis;
    float     limitMin;
    float     limitMax;
    // Motor/Spring
    bool  hasMotor;
    float target; // Angle or Position
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
    ZHLN::Entity handle;
    JPH::Vec3    normal;
    JPH::RVec3   position;
    float        fraction;
    bool         hasHit;
};

struct RaycastPenetrationResult {
    ZHLN::Entity handle;
    JPH::RVec3   entryPosition;
    JPH::RVec3   exitPosition;
    JPH::Vec3    entryNormal;
    JPH::Vec3    exitNormal;
    float        entryFraction;
    float        exitFraction;
    float        thickness;
    uint32_t     materialID;
    bool         hasHit;
};

struct ShapeCastResult {
    ZHLN::Entity handle;
    JPH::RVec3   contactPoint;
    JPH::Vec3    contactNormal;
    float        fraction;
    bool         hasHit;
};

struct CullResult {
    ZHLN::Entity* results;
    uint32_t      count;
};

static_assert(
    (std::is_trivially_default_constructible_v<RaycastResult> && std::is_trivially_copyable_v<RaycastResult>) &&
    (std::is_trivially_default_constructible_v<RaycastPenetrationResult> && std::is_trivially_copyable_v<RaycastPenetrationResult>) &&
    (std::is_trivially_default_constructible_v<ShapeCastResult> && std::is_trivially_copyable_v<ShapeCastResult>) &&
    (std::is_trivially_default_constructible_v<CullResult> && std::is_trivially_copyable_v<CullResult>)
);

// --- Standalone Shape Helpers ---
auto CreateMeshShape(const VertexPosition* vertices, uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount) -> JPH::ShapeRefC;
auto CreateHeightFieldShape(const float* heights, int sampleCount, float worldSize) -> JPH::ShapeRefC;
auto GetBodyID(const PhysicsWorld& world, ZHLN::Entity handle) -> JPH::BodyID;

} // namespace Physics

static_assert(std::is_trivially_copyable_v<ZHLN::Entity>);
static_assert((std::is_trivially_default_constructible_v<ZHLN::Entity> && std::is_trivially_copyable_v<ZHLN::Entity>) );

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

    struct Impl;
    [[nodiscard]] auto GetImpl() const -> Impl* {
        return _impl.get();
    }
    [[nodiscard]] auto GetWorld() const -> const Physics::PhysicsWorld&;

    void OptimizeBroadphase();

    // --- Shape Caching ---
    auto GetOrCreateShape(Physics::ShapeType type, float p1, float p2 = 0.0f, float p3 = 0.0f, float p4 = 0.0f) -> JPH::ShapeRefC;

    // --- Body / Character / Ragdoll Creation ---
    auto CreateRigidBody(
        const JPH::ShapeRefC& shape,
        JPH::RVec3Arg         pos,
        JPH::QuatArg          rot,
        JPH::EMotionType      motion,
        JPH::ObjectLayer      layer,
        uint32_t              materialID = 0,
        uint32_t              category   = 0xFFFFFFFF,
        uint32_t              mask       = 0xFFFFFFFF
    ) -> ZHLN::Entity;

    auto CreateMeshBody(
        const VertexPosition* vertices,
        uint32_t              vertexCount,
        const uint32_t*       indices,
        uint32_t              indexCount,
        JPH::RVec3Arg         pos,
        JPH::QuatArg          rot,
        uint32_t              category = 0xFFFFFFFF,
        uint32_t              mask     = 0xFFFFFFFF
    ) -> ZHLN::Entity;

    auto CreateCharacter(JPH::RVec3Arg position, uint32_t category = 0xFFFFFFFF, uint32_t mask = 0xFFFFFFFF) -> ZHLN::Entity;

    auto CreateSkeletalRagdoll(JPH::Ref<JPH::Skeleton> skeleton, const std::vector<Physics::RagdollPartParams>& parts) -> JPH::Ref<JPH::Ragdoll>;

    // --- Actions & Settings ---
    void               SetCollisionFilter(ZHLN::Entity handle, uint32_t category, uint32_t mask);
    [[nodiscard]] auto GetDebugDrawData(bool drawShapes = true, bool drawConstraints = true, bool wireframe = true) const -> Physics::DebugDrawData;
    void               RegisterMaterial(uint32_t id, float friction, float restitution);

    void DestroyBody(ZHLN::Entity handle);
    void SetLinearVelocity(ZHLN::Entity handle, JPH::Vec3Arg velocity);
    void SetCharacterVelocity(ZHLN::Entity handle, JPH::Vec3Arg velocity);
    void SetCharacterPosition(ZHLN::Entity handle, JPH::RVec3Arg position);

    auto               GetCharacterVelocity(ZHLN::Entity handle) const -> JPH::Vec3;
    [[nodiscard]] auto IsCharacterOnGround(ZHLN::Entity handle) const -> bool;
    [[nodiscard]] auto GetPositionBuffer() const -> BufferView;
    auto               GetRotation(JPH::BodyID bodyID) const -> JPH::Quat;
    void               AddImpulse(ZHLN::Entity handle, JPH::Vec3Arg impulse);
    void               AddImpulse(ZHLN::Entity handle, JPH::Vec3Arg impulse, JPH::RVec3Arg position);

    [[nodiscard]] auto GetContactEvents() const -> std::pair<const Physics::ContactEvent*, size_t>;

    // --- Constraints ---
    auto CreateConstraint(Physics::ConstraintType type, ZHLN::Entity b1, ZHLN::Entity b2, const Physics::ConstraintParams& params) -> Physics::ConstraintHandle;
    void SetConstraintTarget(Physics::ConstraintHandle handle, float value);

    // --- Queries ---
    [[nodiscard]] auto
        Raycast(JPH::RVec3Arg origin, JPH::Vec3Arg direction, float maxDistance = 1000.0f, ZHLN::Entity ignore = {}) const -> Physics::RaycastResult;

    void RaycastAll(
        JPH::RVec3Arg                       origin,
        JPH::Vec3Arg                        direction,
        float                               maxDistance,
        JPH::Array<Physics::RaycastResult>& outResults,
        ZHLN::Entity                        ignore = {}
    ) const;

    [[nodiscard]] auto RaycastPenetration(JPH::RVec3Arg origin, JPH::Vec3Arg direction, float maxDistance = 1000.0f, ZHLN::Entity ignore = {}) const
        -> Physics::RaycastPenetrationResult;

    void RaycastAllPenetrations(
        JPH::RVec3Arg                                  origin,
        JPH::Vec3Arg                                   direction,
        float                                          maxDistance,
        JPH::Array<Physics::RaycastPenetrationResult>& outResults,
        ZHLN::Entity                                   ignore = {}
    ) const;

    [[nodiscard]] auto Shapecast(
        const JPH::ShapeRefC& shape,
        JPH::RVec3Arg         pos,
        JPH::QuatArg          rot,
        JPH::Vec3Arg          direction,
        float                 maxDistance = 1000.0f,
        ZHLN::Entity          ignore      = {}
    ) const -> Physics::ShapeCastResult;

    void OverlapSphere(JPH::RVec3Arg center, float radius, JPH::Array<ZHLN::Entity>& outResults) const;
    void OverlapAABB(JPH::RVec3Arg minBox, JPH::RVec3Arg maxBox, JPH::Array<ZHLN::Entity>& outResults) const;
    void QueryAABB(JPH::Vec3Arg min, JPH::Vec3Arg max, JPH::Array<ZHLN::Entity>& outEntities) const;
    void FrustumCull(const JPH::Mat44& viewProj, const Frustum& frustum, JPH::Array<ZHLN::Entity>& outEntities) const;

    // --- Mapping Helpers ---
    [[nodiscard]] auto GetEntityHandle(JPH::BodyID bodyID) const -> ZHLN::Entity;

    [[nodiscard]] auto GetInternalSystem() noexcept -> JPH::PhysicsSystem&;
    [[nodiscard]] auto GetInternalSystem() const noexcept -> const JPH::PhysicsSystem&;
    [[nodiscard]] auto GetInternalWorld() noexcept -> Physics::PhysicsWorld&;
    [[nodiscard]] auto GetInternalWorld() const noexcept -> const Physics::PhysicsWorld&;

  private:
    std::unique_ptr<Impl> _impl;
};

} // namespace ZHLN
