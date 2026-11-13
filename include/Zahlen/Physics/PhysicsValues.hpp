// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Query results and creation descriptors that cross the engine's public
// boundary. Every field is spelled in ZHLN value types, so this header names no
// JPH type, no STL container and adds no Jolt include. PhysicsIDs.hpp is header
// only. Physics.hpp includes this header, so existing code sees the same types
// in the same namespace.

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Physics/PhysicsIDs.hpp>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ZHLN::Physics {

enum class MotionType : uint8_t { Static, Kinematic, Dynamic };
enum class ObjectLayer : uint8_t { NonMoving, Moving };
enum class Activation : uint8_t { Activate, DontActivate };

struct DebugVertex {
    float    x, y, z;
    uint32_t color;
};

struct DebugDrawData {
    const DebugVertex* lines         = nullptr;
    size_t             lineCount     = 0;
    const DebugVertex* triangles     = nullptr;
    size_t             triangleCount = 0;
};

struct BodyStateSnapshot {
    ZHLN::FVec3 previousPosition {0.0f, 0.0f, 0.0f};
    ZHLN::FVec3 currentPosition {0.0f, 0.0f, 0.0f};
    ZHLN::FVec4 previousRotation {0.0f, 0.0f, 0.0f, 1.0f};
    ZHLN::FVec4 currentRotation {0.0f, 0.0f, 0.0f, 1.0f};
    bool        isCharacter = false;
    bool        valid       = false;
};

struct RaycastResult {
    BodyID      bodyID;
    ZHLN::FVec3 normal;
    ZHLN::DVec3 position;
    float       fraction;
    bool        hasHit;
};

// --- Chainable fixed-layout shape & rigid-body descriptors -------------------

struct BoxShapeDesc {
    ZHLN::FVec3 halfExtents {0.5f, 0.5f, 0.5f};
};

struct SphereShapeDesc {
    float radius = 0.5f;
};

struct CapsuleShapeDesc {
    float halfHeight = 0.5f;
    float radius     = 0.25f;
};

struct CylinderShapeDesc {
    float halfHeight = 0.5f;
    float radius     = 0.25f;
};

struct PlaneShapeDesc {
    ZHLN::FVec3 normal {0.0f, 1.0f, 0.0f};
    float       constant = 0.0f;
};

// Borrows `vertices` and `indices` only for the duration of CreateRigidBody, or
// resolves a pre-cached collision mesh by `asset` when non-Invalid.
struct MeshShapeDesc {
    CollisionAssetID   asset       = CollisionAssetID::Invalid;
    const ZHLN::FVec3* vertices    = nullptr;
    size_t             vertexCount = 0;
    const uint32_t*    indices     = nullptr;
    size_t             indexCount  = 0;
};

struct HeightFieldShapeDesc {
    const float* heights     = nullptr;
    uint32_t     sampleCount = 0;
    float        worldSize   = 0.0f;
};

enum class PrimitiveColliderKind : uint8_t {
    Box      = 0,
    Sphere   = 1,
    Capsule  = 2,
    Cylinder = 3,
    Plane    = 4,
};

struct CompoundChildDesc {
    PrimitiveColliderKind kind       = PrimitiveColliderKind::Box;
    ZHLN::FVec3           position {0.0f, 0.0f, 0.0f};
    ZHLN::Quat            rotation   = ZHLN::Quat::Identity();
    ZHLN::FVec3           dimensions {0.5f, 0.5f, 0.5f};
    float                 constant   = 0.0f;

    [[nodiscard]] static constexpr auto Box(ZHLN::FVec3 halfExtents, ZHLN::FVec3 localPos = {},
                                            ZHLN::Quat localRot = ZHLN::Quat::Identity()) noexcept -> CompoundChildDesc {
        return {.kind = PrimitiveColliderKind::Box, .position = localPos, .rotation = localRot, .dimensions = halfExtents};
    }

    [[nodiscard]] static constexpr auto Sphere(float radius, ZHLN::FVec3 localPos = {},
                                               ZHLN::Quat localRot = ZHLN::Quat::Identity()) noexcept -> CompoundChildDesc {
        return {.kind = PrimitiveColliderKind::Sphere, .position = localPos, .rotation = localRot, .dimensions = {radius, 0.0f, 0.0f}};
    }

    [[nodiscard]] static constexpr auto Capsule(float halfHeight, float radius, ZHLN::FVec3 localPos = {},
                                                ZHLN::Quat localRot = ZHLN::Quat::Identity()) noexcept -> CompoundChildDesc {
        return {.kind = PrimitiveColliderKind::Capsule, .position = localPos, .rotation = localRot, .dimensions = {halfHeight, radius, 0.0f}};
    }

    [[nodiscard]] static constexpr auto Cylinder(float halfHeight, float radius, ZHLN::FVec3 localPos = {},
                                                 ZHLN::Quat localRot = ZHLN::Quat::Identity()) noexcept -> CompoundChildDesc {
        return {.kind = PrimitiveColliderKind::Cylinder, .position = localPos, .rotation = localRot, .dimensions = {halfHeight, radius, 0.0f}};
    }

    [[nodiscard]] static constexpr auto Plane(ZHLN::FVec3 normal, float planeConstant = 0.0f, ZHLN::FVec3 localPos = {},
                                              ZHLN::Quat localRot = ZHLN::Quat::Identity()) noexcept -> CompoundChildDesc {
        return {.kind       = PrimitiveColliderKind::Plane,
                .position   = localPos,
                .rotation   = localRot,
                .dimensions = normal,
                .constant   = planeConstant};
    }
};

struct CompoundShapeDesc {
    const CompoundChildDesc* children   = nullptr;
    uint32_t                 childCount = 0;
};

struct RigidBodyPose {
    ZHLN::DVec3 position {};
    ZHLN::Quat  rotation   = ZHLN::Quat::Identity();
    ZHLN::FVec3 scale {1.0f, 1.0f, 1.0f};
    MotionType  motionType = MotionType::Dynamic;
    Activation  activation = Activation::Activate;
};

struct RigidBodySurface {
    uint32_t materialID       = 0;
    float    friction         = 0.2f;
    float    restitution      = 0.0f;
    bool     registerMaterial = false;
};

struct RigidBodyFilter {
    ObjectLayer layer            = ObjectLayer::Moving;
    bool        hasExplicitLayer = false;
    uint32_t    category         = 0xFFFFFFFFu;
    uint32_t    mask             = 0xFFFFFFFFu;
};

struct CharacterCreateDesc {
    ZHLN::DVec3 position {};
    float       capsuleHalfHeight        = 0.5f;
    float       capsuleRadius            = 0.3f;
    bool        useCustomCapsule         = false;
    float       maxSlopeRadians          = 0.785398163f;
    float       maxStrength              = 100.0f;
    float       characterPadding         = 0.02f;
    float       penetrationRecoverySpeed = 1.0f;
    uint32_t    category                 = 0xFFFFFFFFu;
    uint32_t    mask                     = 0xFFFFFFFFu;
};

static_assert(std::is_standard_layout_v<BoxShapeDesc> && std::is_trivially_copyable_v<BoxShapeDesc>);
static_assert(std::is_standard_layout_v<SphereShapeDesc> && std::is_trivially_copyable_v<SphereShapeDesc>);
static_assert(std::is_standard_layout_v<CapsuleShapeDesc> && std::is_trivially_copyable_v<CapsuleShapeDesc>);
static_assert(std::is_standard_layout_v<CylinderShapeDesc> && std::is_trivially_copyable_v<CylinderShapeDesc>);
static_assert(std::is_standard_layout_v<PlaneShapeDesc> && std::is_trivially_copyable_v<PlaneShapeDesc>);
static_assert(std::is_standard_layout_v<MeshShapeDesc> && std::is_trivially_copyable_v<MeshShapeDesc>);
static_assert(std::is_standard_layout_v<HeightFieldShapeDesc> && std::is_trivially_copyable_v<HeightFieldShapeDesc>);
static_assert(std::is_standard_layout_v<CompoundChildDesc> && std::is_trivially_copyable_v<CompoundChildDesc>);
static_assert(std::is_standard_layout_v<CompoundShapeDesc> && std::is_trivially_copyable_v<CompoundShapeDesc>);
static_assert(std::is_standard_layout_v<RigidBodyPose> && std::is_trivially_copyable_v<RigidBodyPose>);
static_assert(std::is_standard_layout_v<RigidBodySurface> && std::is_trivially_copyable_v<RigidBodySurface>);
static_assert(std::is_standard_layout_v<RigidBodyFilter> && std::is_trivially_copyable_v<RigidBodyFilter>);
static_assert(std::is_standard_layout_v<CharacterCreateDesc> && std::is_trivially_copyable_v<CharacterCreateDesc>);

} // namespace ZHLN::Physics
