// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Math/ValueTypes.hpp
//
// The six value types the engine's public surface is spelled in: FVec2, FVec3,
// FVec4, DVec3, Quat and Mat4x4. They hold the same members, in the same order,
// as the Jolt storage classes of the same names, and convert to and from them --
// and to and from the SIMD classes Vec3, Vec4, Quat and RVec3 -- implicitly, in
// both directions.
//
// Why these exist
// ---------------
// A signature that takes `JPH::Float3` cannot be called with a `JPH::Vec3`, and
// Jolt makes both halves of that crossing explicit (`explicit Vec3(const FVec3 &)`
// going in, `StoreFloat3(&out)` coming out). With a data model that stores SIMD
// and an API that takes POD, every call site had to spell the crossing: 322 of
// them did, through a To*/From* family that existed only to be deleted later.
//
// These types make the boundary a language conversion instead of a function call.
// A caller holding a Vec3, a Quat, an RVec3 or a brace-initialised literal passes
// it to a FVec3/FVec4/DVec3 parameter and writes nothing; a caller holding one
// of these passes it to any Jolt API that wants Vec3Arg, QuatArg, RVec3Arg or the
// storage class. `{1.0f, 2.0f, 3.0f}` keeps working, and so does every aggregate
// that carries one of these as a member.
//
// What they are not
// -----------------
// No arithmetic. This is the storage spelling of a vector, not a maths library:
// a calculation loads a Vec3/RVec3 where it is written, and a matrix multiplies
// as a Mat4x4. No operator+, no Dot, no Normalized, no operator* on Mat4x4. And no
// new members: an addition here changes the size of every aggregate and every
// buffer element that embeds one, which the static_asserts at the bottom of this
// file would catch.
//
// Quat and Mat4x4 are here for a second reason. A rotation and a colour are the
// same sixteen bytes, and a transform is the same sixty-four bytes Jolt hands the
// shaders; naming them in public headers is what lets include/Zahlen stop
// depending on Jolt's headers for the shapes it stores.
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
#include <Jolt/Math/DVec3.h>
#include <Jolt/Math/Vec2.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Jolt/Math/Mat4x4.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Real.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <type_traits>

namespace ZHLN {

class DVec3;

// Two floats, 8 bytes: UV offsets and scales, 2D sizes, extents. Jolt has no Vec2,
// so this is the one type in the file with no SIMD counterpart to convert to: a
// JPH::Float2 is the only other spelling of the same eight bytes.
class [[nodiscard]] FVec2 {
  public:
    float x;
    float y;

    constexpr FVec2() noexcept = default;
    constexpr FVec2(float inX, float inY) noexcept: x(inX), y(inY) {
    }

    FVec2(JPH::Float2 inV) noexcept: x(inV.x), y(inV.y) {
    }

    operator JPH::Float2() const noexcept {
        return JPH::Float2 {x, y};
    }

    [[nodiscard]] constexpr auto operator[](int inCoordinate) const noexcept -> float {
        return inCoordinate == 0 ? x : y;
    }

    bool operator==(const FVec2&) const = default;
};

// Three floats, 12 bytes: positions, extents, directions, normals.
class [[nodiscard]] FVec3 {
  public:
    float x;
    float y;
    float z;

    constexpr FVec3() noexcept = default;
    constexpr FVec3(float inX, float inY, float inZ) noexcept: x(inX), y(inY), z(inZ) {
    }

    FVec3(JPH::Float3 inV) noexcept: x(inV.x), y(inV.y), z(inV.z) {
    }
    FVec3(JPH::Vec3Arg inV) noexcept {
        const JPH::Float3 pod = Load(inV);
        x                     = pod.x;
        y                     = pod.y;
        z                     = pod.z;
    }
    // A world-space position handed to a local-space spelling, which is the one
    // narrowing this file allows: a placement in metres becomes a local offset.
    // Defined below DVec3, whose members it reads.
    FVec3(const DVec3& inV) noexcept;
#ifdef JPH_DOUBLE_PRECISION
    // Same narrowing, from Jolt's SIMD world-space type.
    FVec3(JPH::RVec3Arg inV) noexcept: FVec3(JPH::Vec3(inV.x, inV.y, inV.z)) {
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
        return JPH::RVec3(JPH::Vec3(*this.x, *this.y, *this.z));
    }
#endif

    [[nodiscard]] constexpr auto operator[](int inCoordinate) const noexcept -> float {
        return inCoordinate == 0 ? x : (inCoordinate == 1 ? y : z);
    }

    bool operator==(const FVec3&) const = default;

  private:
    [[nodiscard]] static auto Load(JPH::Vec3Arg inV) noexcept -> JPH::Float3 {
        JPH::Float3 pod;
        inV.StoreFloat3(&pod);
        return pod;
    }
};

// Four floats, 16 bytes. Carries a colour and a quaternion alike, which is what
// the boundary already did: JPH::Float4 is both.
class [[nodiscard]] FVec4 {
  public:
    float x;
    float y;
    float z;
    float w;

    constexpr FVec4() noexcept = default;
    constexpr FVec4(float inX, float inY, float inZ, float inW) noexcept: x(inX), y(inY), z(inZ), w(inW) {
    }

    FVec4(JPH::Float4 inV) noexcept: x(inV.x), y(inV.y), z(inV.z), w(inV.w) {
    }
    FVec4(JPH::Vec4Arg inV) noexcept {
        const JPH::Float4 pod = Load(inV);
        x                     = pod.x;
        y                     = pod.y;
        z                     = pod.z;
        w                     = pod.w;
    }
    FVec4(JPH::QuatArg inQ) noexcept {
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

    bool operator==(const FVec4&) const = default;

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
class [[nodiscard]] DVec3 {
  public:
    double x;
    double y;
    double z;

    constexpr DVec3() noexcept = default;
    constexpr DVec3(double inX, double inY, double inZ) noexcept: x(inX), y(inY), z(inZ) {
    }

    DVec3(JPH::Double3 inV) noexcept: x(inV.x), y(inV.y), z(inV.z) {
    }
    // A local-space POD placement promoted to a world placement.
    DVec3(JPH::Float3 inV) noexcept: x(static_cast<double>(inV.x)), y(static_cast<double>(inV.y)), z(static_cast<double>(inV.z)) {
    }
    // A local offset promoted to a world placement. Nothing is lost; the casts
    // are here so the promotion is stated rather than implied to -Wdouble-promotion.
    DVec3(const FVec3& inV) noexcept: x(static_cast<double>(inV.x)), y(static_cast<double>(inV.y)), z(static_cast<double>(inV.z)) {
    }
    DVec3(JPH::RVec3Arg inV) noexcept {
        const JPH::Double3 pod = Load(inV);
        x                      = pod.x;
        y                      = pod.y;
        z                      = pod.z;
    }
#ifdef JPH_DOUBLE_PRECISION
    // RVec3 is DVec3 here, so a Vec3 is the local-space spelling of the same
    // point. Without the define RVec3 *is* Vec3 and this would duplicate the
    // constructor above.
    DVec3(JPH::Vec3Arg inV) noexcept: DVec3(JPH::RVec3(inV)) {
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

    bool operator==(const DVec3&) const = default;

  private:
#ifdef JPH_DOUBLE_PRECISION
    [[nodiscard]] static auto Load(JPH::RVec3Arg inV) noexcept -> JPH::Double3 {
        JPH::Double3 pod;
        inV.StoreDouble3(&pod);
        return pod;
    }
#else
    // RVec3 is Vec3 here, and a Vec3 stores a DVec3 only by losing precision:
    // the widen is three converts rather than a store.
    [[nodiscard]] static auto Load(JPH::RVec3Arg inV) noexcept -> JPH::Double3 {
        return JPH::Double3 {static_cast<double>(inV.GetX()), static_cast<double>(inV.GetY()), static_cast<double>(inV.GetZ())};
    }
#endif
};

// Four floats, 16 bytes, xyzw: a rotation. The same bytes as FVec4 and a
// different meaning, which is the reason the type exists -- a signature that takes
// a Quat cannot be handed a colour, and a caller holding a Quat does not have to
// remember whether this parameter is quaternion-first or colour-first.
//
// The container does not initialise itself -- the same rule as the lane pods -- so
// the identity rotation is spelled `Quat {0.0f, 0.0f, 0.0f, 1.0f}` or taken from a
// Jolt quaternion, never assumed.
class [[nodiscard]] alignas(alignof(JPH::Quat)) Quat {
  public:
    float x;
    float y;
    float z;
    float w;

    constexpr Quat() noexcept = default;
    constexpr Quat(float inX, float inY, float inZ, float inW) noexcept: x(inX), y(inY), z(inZ), w(inW) {
    }

    Quat(JPH::QuatArg inQ) noexcept {
        const JPH::Float4 pod = Load(inQ);
        x                     = pod.x;
        y                     = pod.y;
        z                     = pod.z;
        w                     = pod.w;
    }

    operator JPH::Quat() const noexcept {
        return JPH::Quat(JPH::Float4 {x, y, z, w});
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

    bool operator==(const Quat&) const = default;

  private:
    [[nodiscard]] static auto Load(JPH::QuatArg inQ) noexcept -> JPH::Float4 {
        JPH::Float4 pod;
        inQ.StoreFloat4(&pod);
        return pod;
    }
};

// Sixteen floats, 64 bytes, column-major: the storage of a transform, in the order
// Jolt stores it and the shaders read it, so a field that becomes a Mat4x4 does not
// change a single byte of any upload.
//
// sIdentity and sZero are construction, not arithmetic: they exist because a
// storage field needs a default, and `Mat4x4 transform = ZHLN::Mat4x4::Identity()`
// is what the declaration it replaces already said.
class [[nodiscard]] alignas(alignof(JPH::Mat44)) Mat4x4 {
  public:
    // Element (row r, column c) is column[c][r]; the four floats of a column are
    // what a shader reads as one float4. No initialiser: `{}` at the use site
    // already means zero, and an initialiser here would cost the type its trivial
    // default construction, which the draw queue's structs depend on.
    FVec4 column[4];

    constexpr Mat4x4() noexcept = default;

    Mat4x4(const JPH::Mat44& inM) noexcept {
        for (int index = 0; index < 4; ++index) {
            column[index] = FromColumn(inM, index);
        }
    }

    operator JPH::Mat44() const noexcept {
        return JPH::Mat44(ToColumn(0), ToColumn(1), ToColumn(2), ToColumn(3));
    }

    [[nodiscard]] static constexpr auto sIdentity() noexcept -> Mat4x4 {
        Mat4x4 out {};
        out.column[0].x = 1.0f;
        out.column[1].y = 1.0f;
        out.column[2].z = 1.0f;
        out.column[3].w = 1.0f;
        return out;
    }
    [[nodiscard]] static constexpr auto sZero() noexcept -> Mat4x4 {
        return Mat4x4 {};
    }

    bool operator==(const Mat4x4&) const = default;

  private:
    [[nodiscard]] static auto FromColumn(const JPH::Mat44& inM, int inIndex) noexcept -> FVec4 {
        JPH::Float4 pod;
        inM.GetColumn4(inIndex).StoreFloat4(&pod);
        return FVec4(pod.x, pod.y, pod.z, pod.w);
    }
    [[nodiscard]] auto ToColumn(int inIndex) const noexcept -> JPH::Vec4 {
        const FVec4& source = column[inIndex];
        return JPH::Vec4(source.x, source.y, source.z, source.w);
    }
};

inline FVec3::FVec3(const DVec3& inV) noexcept: x(static_cast<float>(inV.x)), y(static_cast<float>(inV.y)), z(static_cast<float>(inV.z)) {
}

// The conversions above are only correct if the storage is. These fail here, once,
// instead of reading past a member at a call site.
static_assert(sizeof(FVec2) == sizeof(JPH::Float2) && alignof(FVec2) == alignof(JPH::Float2), "FVec2 must stay layout-identical to JPH::Float2");
static_assert(sizeof(FVec3) == sizeof(JPH::Float3) && alignof(FVec3) == alignof(JPH::Float3), "FVec3 must stay layout-identical to JPH::Float3");
static_assert(sizeof(FVec4) == sizeof(JPH::Float4) && alignof(FVec4) == alignof(JPH::Float4), "FVec4 must stay layout-identical to JPH::Float4");
static_assert(sizeof(DVec3) == sizeof(JPH::Double3) && alignof(DVec3) == alignof(JPH::Double3), "DVec3 must stay layout-identical to JPH::Double3");
static_assert(sizeof(Quat) == sizeof(JPH::Quat) && alignof(Quat) == alignof(JPH::Quat), "Quat must stay layout-identical to JPH::Quat");
static_assert(sizeof(Mat4x4) == sizeof(JPH::Mat44) && alignof(Mat4x4) == alignof(JPH::Mat44), "Mat4x4 must stay layout-identical to JPH::Mat44");
static_assert(
    std::is_trivially_constructible_v<FVec2> && std::is_trivially_constructible_v<FVec3> && std::is_trivially_constructible_v<FVec4> &&
        std::is_trivially_constructible_v<DVec3> && std::is_trivially_constructible_v<Quat> && std::is_trivially_constructible_v<Mat4x4>,
    "a container gets memcpy'd into queues and task scratch; its default construction must stay trivial, so it must not initialise its lanes"
);
static_assert(
    std::is_trivially_copyable_v<FVec2> && std::is_trivially_copyable_v<FVec3> && std::is_trivially_copyable_v<FVec4> &&
        std::is_trivially_copyable_v<DVec3> && std::is_trivially_copyable_v<Quat> && std::is_trivially_copyable_v<Mat4x4>,
    "value types cross the boundary by value and must stay trivially copyable"
);
static_assert(
    std::is_standard_layout_v<FVec2> && std::is_standard_layout_v<FVec3> && std::is_standard_layout_v<FVec4> && std::is_standard_layout_v<DVec3> &&
        std::is_standard_layout_v<Quat> && std::is_standard_layout_v<Mat4x4>,
    "value types stand in for the Jolt storage classes and must stay standard-layout"
);

} // namespace ZHLN
