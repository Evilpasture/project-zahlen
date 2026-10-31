// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Math/ValueTypes.hpp
//
// The three value types the engine's public surface is spelled in: Float3,
// Float4 and Double3. They hold the same members, in the same order, as the Jolt
// storage classes of the same names, and convert to and from them -- and to and
// from the SIMD classes Vec3, Vec4, Quat and RVec3 -- implicitly, in both
// directions.
//
// Why these exist
// ---------------
// A signature that takes `JPH::Float3` cannot be called with a `JPH::Vec3`, and
// Jolt makes both halves of that crossing explicit (`explicit Vec3(const Float3 &)`
// going in, `StoreFloat3(&out)` coming out). With a data model that stores SIMD
// and an API that takes POD, every call site had to spell the crossing: 322 of
// them did, through a To*/From* family that existed only to be deleted later.
//
// These types make the boundary a language conversion instead of a function call.
// A caller holding a Vec3, a Quat, an RVec3 or a brace-initialised literal passes
// it to a Float3/Float4/Double3 parameter and writes nothing; a caller holding one
// of these passes it to any Jolt API that wants Vec3Arg, QuatArg, RVec3Arg or the
// storage class. `{1.0f, 2.0f, 3.0f}` keeps working, and so does every aggregate
// that carries one of these as a member.
//
// What they are not
// -----------------
// No arithmetic. This is the storage spelling of a vector, not a maths library:
// a calculation loads a Vec3/RVec3 where it is written. And no new members: an
// addition here changes the size of every aggregate and every buffer element that
// embeds one, which the static_asserts at the bottom of this file would catch.
//
// Two rules follow from a type that converts to several Jolt types at once
// ----------------------------------------------------------------------
// 1. A value converts to a parameter, not to a signature. An overload set that
//    names two different Jolt types in the same position --
//
//        void F(JPH::Float3);
//        void F(JPH::Vec3Arg);      // F(someFloat3) is now ambiguous
//
//    -- can no longer be resolved from one of these and is reported as ambiguous
//    by both Clang and GCC. Name one type per position.
// 2. The conversion is implicit, so the boundary is no longer visible in the
//    source. Where the store matters (a per-vertex loop, a byte-for-byte copy of
//    a block), take JPH::Float3 in the signature and let the type say so.

// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Math/Double3.h>
#include <Jolt/Math/Float3.h>
#include <Jolt/Math/Float4.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Real.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <type_traits>

namespace ZHLN {

class Double3;

// Three floats, 12 bytes: positions, extents, directions, normals.
class [[nodiscard]] Float3 {
  public:
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Float3() noexcept = default;
    constexpr Float3(float inX, float inY, float inZ) noexcept: x(inX), y(inY), z(inZ) {
    }

    Float3(JPH::Float3 inV) noexcept: x(inV.x), y(inV.y), z(inV.z) {
    }
    Float3(JPH::Vec3Arg inV) noexcept {
        const JPH::Float3 pod = Load(inV);
        x                     = pod.x;
        y                     = pod.y;
        z                     = pod.z;
    }
    // A world-space position handed to a local-space spelling, which is the one
    // narrowing this file allows: a placement in metres becomes a local offset.
    // Defined below Double3, whose members it reads.
    Float3(const Double3& inV) noexcept;
#ifdef JPH_DOUBLE_PRECISION
    // Same narrowing, from Jolt's SIMD world-space type.
    Float3(JPH::RVec3Arg inV) noexcept: Float3(static_cast<JPH::Vec3>(inV)) {
    }
#endif

    operator JPH::Float3() const noexcept {
        return JPH::Float3 {x, y, z};
    }
    operator JPH::Vec3() const noexcept {
        return JPH::Vec3(static_cast<JPH::Float3>(*this));
    }
#ifdef JPH_DOUBLE_PRECISION
    operator JPH::RVec3() const noexcept {
        return JPH::RVec3(static_cast<JPH::Vec3>(*this));
    }
#endif

    [[nodiscard]] constexpr auto operator[](int inCoordinate) const noexcept -> float {
        return inCoordinate == 0 ? x : (inCoordinate == 1 ? y : z);
    }

    bool operator==(const Float3&) const = default;

  private:
    [[nodiscard]] static auto Load(JPH::Vec3Arg inV) noexcept -> JPH::Float3 {
        JPH::Float3 pod;
        inV.StoreFloat3(&pod);
        return pod;
    }
};

// Four floats, 16 bytes. Carries a colour and a quaternion alike, which is what
// the boundary already did: JPH::Float4 is both.
class [[nodiscard]] Float4 {
  public:
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    constexpr Float4() noexcept = default;
    constexpr Float4(float inX, float inY, float inZ, float inW) noexcept: x(inX), y(inY), z(inZ), w(inW) {
    }

    Float4(JPH::Float4 inV) noexcept: x(inV.x), y(inV.y), z(inV.z), w(inV.w) {
    }
    Float4(JPH::Vec4Arg inV) noexcept {
        const JPH::Float4 pod = Load(inV);
        x                     = pod.x;
        y                     = pod.y;
        z                     = pod.z;
        w                     = pod.w;
    }
    Float4(JPH::QuatArg inQ) noexcept {
        const JPH::Float4 pod = Load(inQ);
        x                     = pod.x;
        y                     = pod.y;
        z                     = pod.z;
        w                     = pod.w;
    }

    operator JPH::Float4() const noexcept {
        return JPH::Float4 {x, y, z, w};
    }
    operator JPH::Vec4() const noexcept {
        const JPH::Float4 pod {x, y, z, w};
        return JPH::Vec4::sLoadFloat4(&pod);
    }
    operator JPH::Quat() const noexcept {
        return JPH::Quat(static_cast<JPH::Float4>(*this));
    }

    [[nodiscard]] constexpr auto operator[](int inCoordinate) const noexcept -> float {
        switch (inCoordinate) {
            case 0:
                return x;
            case 1:
                return y;
            case 2:
                return z;
            default:
                return w;
        }
    }

    bool operator==(const Float4&) const = default;

  private:
    [[nodiscard]] static auto Load(JPH::Vec4Arg inV) noexcept -> JPH::Float4 {
        JPH::Float4 pod;
        inV.StoreFloat4(&pod);
        return pod;
    }
    [[nodiscard]] static auto Load(JPH::QuatArg inQ) noexcept -> JPH::Float4 {
        JPH::Float4 pod;
        inQ.StoreFloat4(&pod);
        return pod;
    }
};

// Three doubles, 24 bytes: the world-space position of a double-precision build.
class [[nodiscard]] Double3 {
  public:
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    constexpr Double3() noexcept = default;
    constexpr Double3(double inX, double inY, double inZ) noexcept: x(inX), y(inY), z(inZ) {
    }

    Double3(JPH::Double3 inV) noexcept: x(inV.x), y(inV.y), z(inV.z) {
    }
    // A local-space POD placement promoted to a world placement.
    Double3(JPH::Float3 inV) noexcept: x(static_cast<double>(inV.x)), y(static_cast<double>(inV.y)), z(static_cast<double>(inV.z)) {
    }
    // A local offset promoted to a world placement. Nothing is lost; the casts
    // are here so the promotion is stated rather than implied to -Wdouble-promotion.
    Double3(const Float3& inV) noexcept: x(static_cast<double>(inV.x)), y(static_cast<double>(inV.y)), z(static_cast<double>(inV.z)) {
    }
    Double3(JPH::RVec3Arg inV) noexcept {
        const JPH::Double3 pod = Load(inV);
        x                      = pod.x;
        y                      = pod.y;
        z                      = pod.z;
    }
#ifdef JPH_DOUBLE_PRECISION
    // RVec3 is DVec3 here, so a Vec3 is the local-space spelling of the same
    // point. Without the define RVec3 *is* Vec3 and this would duplicate the
    // constructor above.
    Double3(JPH::Vec3Arg inV) noexcept: Double3(JPH::RVec3(inV)) {
    }
#endif

    operator JPH::Double3() const noexcept {
        return JPH::Double3 {x, y, z};
    }
#ifdef JPH_DOUBLE_PRECISION
    operator JPH::RVec3() const noexcept {
        return JPH::RVec3(static_cast<JPH::Double3>(*this));
    }
#else
    // No double-precision world: RVec3 is Vec3 and this conversion narrows.
    operator JPH::RVec3() const noexcept {
        return JPH::Vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    }
#endif

    [[nodiscard]] constexpr auto operator[](int inCoordinate) const noexcept -> double {
        return inCoordinate == 0 ? x : (inCoordinate == 1 ? y : z);
    }

    bool operator==(const Double3&) const = default;

  private:
#ifdef JPH_DOUBLE_PRECISION
    [[nodiscard]] static auto Load(JPH::RVec3Arg inV) noexcept -> JPH::Double3 {
        JPH::Double3 pod;
        inV.StoreDouble3(&pod);
        return pod;
    }
#else
    // RVec3 is Vec3 here, and a Vec3 stores a Double3 only by losing precision:
    // the widen is three converts rather than a store.
    [[nodiscard]] static auto Load(JPH::RVec3Arg inV) noexcept -> JPH::Double3 {
        return JPH::Double3 {static_cast<double>(inV.GetX()), static_cast<double>(inV.GetY()), static_cast<double>(inV.GetZ())};
    }
#endif
};

inline Float3::Float3(const Double3& inV) noexcept: x(static_cast<float>(inV.x)), y(static_cast<float>(inV.y)), z(static_cast<float>(inV.z)) {
}

// The conversions above are only correct if the storage is. These fail here, once,
// instead of reading past a member at a call site.
static_assert(sizeof(Float3) == sizeof(JPH::Float3) && alignof(Float3) == alignof(JPH::Float3), "Float3 must stay layout-identical to JPH::Float3");
static_assert(sizeof(Float4) == sizeof(JPH::Float4) && alignof(Float4) == alignof(JPH::Float4), "Float4 must stay layout-identical to JPH::Float4");
static_assert(sizeof(Double3) == sizeof(JPH::Double3) && alignof(Double3) == alignof(JPH::Double3), "Double3 must stay layout-identical to JPH::Double3");
static_assert(
    std::is_trivially_copyable_v<Float3> && std::is_trivially_copyable_v<Float4> && std::is_trivially_copyable_v<Double3>,
    "value types cross the boundary by value and must stay trivially copyable"
);
static_assert(
    std::is_standard_layout_v<Float3> && std::is_standard_layout_v<Float4> && std::is_standard_layout_v<Double3>,
    "value types stand in for the Jolt storage classes and must stay standard-layout"
);

} // namespace ZHLN
