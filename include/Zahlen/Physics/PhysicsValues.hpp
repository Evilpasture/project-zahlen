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
#include <Zahlen/Physics/PhysicsShapes.hpp>
#include <cstddef>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <type_traits>

namespace ZHLN::Physics {

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

// --- Body roles ----------------------------------------------------------------
// The role IS the creation payload: pose plus motion policy in one wire node.
// Layer, activation and sleep policy are derived from the role (see
// PhysicsContext), so they can no longer contradict it.

struct StaticBody {
    ZHLN::DVec3 position {};
    ZHLN::Quat  rotation = ZHLN::Quat::Identity();
    ZHLN::FVec3 scale {1.0f, 1.0f, 1.0f};
};

struct KinematicBody {
    ZHLN::DVec3 position {};
    ZHLN::Quat  rotation = ZHLN::Quat::Identity();
    ZHLN::FVec3 scale {1.0f, 1.0f, 1.0f};
};

struct DynamicBody {
    ZHLN::DVec3 position {};
    ZHLN::Quat  rotation = ZHLN::Quat::Identity();
    ZHLN::FVec3 scale {1.0f, 1.0f, 1.0f};
    ZHLN::FVec3 velocity {};
    // Spawn asleep (Jolt DontActivate); an active body or an impulse/velocity
    // change wakes it.
    bool startAsleep = false;
};

struct CharacterBody {
    ZHLN::DVec3 position {};
    // 45 degrees.
    RadF  maxSlopeAngle {0.785398163f};
    float maxStrength              = 100.0f;
    float padding                  = 0.02f;
    float penetrationRecoverySpeed = 1.0f;
    // Local-space half-space (n.Dot(p) + c <= 0) whose contacts count as
    // floor support. Absent = lower half of the resolved shape's bounds.
    COM::Option<Plane> supportingVolume {};
};

// --- Body creation modifiers ---------------------------------------------------
// Any subset may travel a creation chain alongside exactly one role node.

struct SurfaceMaterial {
    float friction    = 0.2f;
    float restitution = 0.0f;
};

struct CollisionFilter {
    uint32_t category = 0xFFFFFFFF;
    uint32_t mask     = 0xFFFFFFFF;
};

// Marker: park this static body in Jolt's MOVING broadphase (scripted statics
// that must be found by moving-layer queries). Kinematic and dynamic bodies
// are already there, so the marker changes nothing for them.
struct MovingBroadphase {};

// Registered materials are identified by a wire-safe handle; Default (0) is
// Jolt friction 0.2 / restitution 0.0.
enum class PhysicsMaterialID : uint32_t { Default = 0 };

// Role-specific modifier policies: characters carry no Jolt rigid-body surface
// material, and moving bodies already sit in the MOVING broadphase so
// MovingBroadphase is only meaningful on statics.
template <typename T>
concept CharacterModifier = std::same_as<T, CollisionFilter>;

template <typename T>
concept RigidBodyModifier =
    std::same_as<T, SurfaceMaterial> || std::same_as<T, PhysicsMaterialID> || std::same_as<T, CollisionFilter>;

template <typename T>
concept StaticBodyModifier = RigidBodyModifier<T> || std::same_as<T, MovingBroadphase>;

// --- Jolt-free query results ----------------------------------------------------
// ZHLN value types only: ISimulation can return these across the boundary.

struct RaycastPenetrationResult {
    BodyID            bodyID;
    ZHLN::DVec3       entryPosition;
    ZHLN::DVec3       exitPosition;
    ZHLN::FVec3       entryNormal;
    ZHLN::FVec3       exitNormal;
    float             entryFraction;
    float             exitFraction;
    float             thickness;
    PhysicsMaterialID materialID;
    bool              hasHit;
};

struct ShapeCastResult {
    BodyID      bodyID;
    ZHLN::DVec3 contactPoint;
    ZHLN::FVec3 contactNormal;
    float       fraction;
    bool        hasHit;
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
    // World-space hinge anchor and rotation axis.
    ZHLN::FVec3        pivot {};
    ZHLN::FVec3        axis {0.0f, 1.0f, 0.0f};
    RadF               minAngle {0.0f};
    RadF               maxAngle {0.0f};
    bool               limitEnabled      = false;
    COM::Option<RadF>  targetAngle {};
    float              maxFrictionTorque = 0.0f;
    MotorSettings      motor {};
};

struct SliderConstraint {
    // World-space anchor and travel axis; absent pivot = Jolt auto-detects it
    // from the bodies' current relative pose.
    COM::Option<ZHLN::FVec3> pivot {};
    ZHLN::FVec3              axis {1.0f, 0.0f, 0.0f};
    float                    minPosition      = 0.0f;
    float                    maxPosition      = 0.0f;
    bool                     limitEnabled     = false;
    COM::Option<float>       targetPosition {};
    float                    maxFrictionForce = 0.0f;
    MotorSettings            motor {};
};

struct ConeConstraint {
    // World-space cone apex, twist axis and half-cone angle in radians.
    ZHLN::FVec3 pivot {};
    ZHLN::FVec3 axis {0.0f, 1.0f, 0.0f};
    RadF        halfAngle {0.0f};
};

template <typename C>
concept ConstraintDesc =
    std::same_as<std::remove_cvref_t<C>, FixedConstraint> ||
    std::same_as<std::remove_cvref_t<C>, PointConstraint> ||
    std::same_as<std::remove_cvref_t<C>, DistanceConstraint> ||
    std::same_as<std::remove_cvref_t<C>, HingeConstraint> ||
    std::same_as<std::remove_cvref_t<C>, SliderConstraint> ||
    std::same_as<std::remove_cvref_t<C>, ConeConstraint>;

static_assert(std::is_standard_layout_v<StaticBody> && std::is_trivially_copyable_v<StaticBody>);
static_assert(std::is_standard_layout_v<KinematicBody> && std::is_trivially_copyable_v<KinematicBody>);
static_assert(std::is_standard_layout_v<DynamicBody> && std::is_trivially_copyable_v<DynamicBody>);
static_assert(std::is_standard_layout_v<CharacterBody> && std::is_trivially_copyable_v<CharacterBody>);
static_assert(std::is_standard_layout_v<SurfaceMaterial> && std::is_trivially_copyable_v<SurfaceMaterial>);
static_assert(std::is_standard_layout_v<CollisionFilter> && std::is_trivially_copyable_v<CollisionFilter>);
static_assert(std::is_standard_layout_v<MovingBroadphase> && std::is_trivially_copyable_v<MovingBroadphase>);
static_assert(std::is_standard_layout_v<RaycastPenetrationResult> && std::is_trivially_copyable_v<RaycastPenetrationResult>);
static_assert(std::is_standard_layout_v<ShapeCastResult> && std::is_trivially_copyable_v<ShapeCastResult>);
static_assert(std::is_standard_layout_v<MotorSettings> && std::is_trivially_copyable_v<MotorSettings>);
static_assert(std::is_standard_layout_v<FixedConstraint> && std::is_trivially_copyable_v<FixedConstraint>);
static_assert(std::is_standard_layout_v<PointConstraint> && std::is_trivially_copyable_v<PointConstraint>);
static_assert(std::is_standard_layout_v<DistanceConstraint> && std::is_trivially_copyable_v<DistanceConstraint>);
static_assert(std::is_standard_layout_v<HingeConstraint> && std::is_trivially_copyable_v<HingeConstraint>);
static_assert(std::is_standard_layout_v<SliderConstraint> && std::is_trivially_copyable_v<SliderConstraint>);
static_assert(std::is_standard_layout_v<ConeConstraint> && std::is_trivially_copyable_v<ConeConstraint>);


} // namespace ZHLN::Physics
