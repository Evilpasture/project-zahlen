// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Query results and creation descriptors that cross the engine's public
// boundary. Every field is spelled in ZHLN value types, so this header names no
// JPH type, no STL container and adds no Jolt include. PhysicsIDs.hpp is header
// only. Physics.hpp includes this header, so existing code sees the same types
// in the same namespace.

#include <Zahlen/Core/AssetID.hpp>
// COM::Option is the boundary spelling of an optional field (constraint
// pivots, motor targets); sanctioned in configure/check_com_boundary.py.
#include <Zahlen/Core/COM/Types.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Physics/PhysicsIDs.hpp>
#include <cstddef>
#include <cmath>
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

// --- Chainable fixed-layout rigid-body descriptors ----------------------------
// Shape vocabulary (Box, Sphere, ... Compound) lives in PhysicsShapes.hpp.

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

// --- Constraint wire structs ---------------------------------------------------
// One struct per constraint kind: the call carries the struct itself, never a
// runtime type tag plus a shared parameter bag, so a hinge's limits and a
// slider's travel cannot be mixed up at the call site.

struct MotorSettings {
    float maxImpulse = 0.0f;
};

struct FixedConstraint {
    // World-space weld point; absent = Jolt auto-detects it from the bodies'
    // current relative pose.
    COM::Option<FVec3> pivot {};
};

struct PointConstraint {
    // World-space anchor; absent = each body's origin.
    COM::Option<FVec3> pivot {};
};

struct DistanceConstraint {
    float minDistance = 0.0f;
    float maxDistance = 0.0f;
};

struct HingeConstraint {
    RadF               minAngle {0.0f};
    RadF               maxAngle {0.0f};
    bool               limitEnabled   = false;
    COM::Option<RadF>  targetAngle    {};
    float              maxFrictionTorque = 0.0f;
    MotorSettings      motor {};
};

struct SliderConstraint {
    float              minPosition = 0.0f;
    float              maxPosition = 0.0f;
    bool               limitEnabled     = false;
    COM::Option<float> targetPosition   {};
    float              maxFrictionForce = 0.0f;
    MotorSettings      motor {};
};

struct ConeConstraint {
    // Half angle of the free cone, radians.
    RadF halfAngle {0.0f};
};

static_assert(std::is_standard_layout_v<MotorSettings> && std::is_trivially_copyable_v<MotorSettings>);
static_assert(std::is_standard_layout_v<FixedConstraint> && std::is_trivially_copyable_v<FixedConstraint>);
static_assert(std::is_standard_layout_v<PointConstraint> && std::is_trivially_copyable_v<PointConstraint>);
static_assert(std::is_standard_layout_v<DistanceConstraint> && std::is_trivially_copyable_v<DistanceConstraint>);
static_assert(std::is_standard_layout_v<HingeConstraint> && std::is_trivially_copyable_v<HingeConstraint>);
static_assert(std::is_standard_layout_v<SliderConstraint> && std::is_trivially_copyable_v<SliderConstraint>);
static_assert(std::is_standard_layout_v<ConeConstraint> && std::is_trivially_copyable_v<ConeConstraint>);

static_assert(std::is_standard_layout_v<RigidBodyPose> && std::is_trivially_copyable_v<RigidBodyPose>);
static_assert(std::is_standard_layout_v<RigidBodySurface> && std::is_trivially_copyable_v<RigidBodySurface>);
static_assert(std::is_standard_layout_v<RigidBodyFilter> && std::is_trivially_copyable_v<RigidBodyFilter>);
static_assert(std::is_standard_layout_v<CharacterCreateDesc> && std::is_trivially_copyable_v<CharacterCreateDesc>);

} // namespace ZHLN::Physics
