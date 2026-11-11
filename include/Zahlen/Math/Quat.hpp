// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/EulerAngles.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ZHLN {

// Hamilton quaternion, stored x, y, z, w. A * B applies B first, then A.
// Rotations passed to Rotate and Slerp are expected to be unit quaternions.
struct alignas(16) Quat {
    float x, y, z, w;

    constexpr Quat() noexcept = default;
    constexpr Quat(float x_, float y_, float z_, float w_) noexcept: x(x_), y(y_), z(z_), w(w_) {}
    explicit constexpr Quat(FVec4 v) noexcept: x(v.x), y(v.y), z(v.z), w(v.w) {}

    [[nodiscard]] static constexpr auto Identity() noexcept -> Quat { return {0.0f, 0.0f, 0.0f, 1.0f}; }
    // Axis must be unit length; the angle is in radians.
    [[nodiscard]] static auto FromAxisAngle(FVec3 axis, RadF radians) noexcept -> Quat {
        const auto half = radians * 0.5f;
        const auto s = Sin(half);
        return {axis.x * s, axis.y * s, axis.z * s, Cos(half)};
    }
    // XYZ angles in radians; rotation order is Z * Y * X.
    [[nodiscard]] static auto FromEuler(EulerRadians radians) noexcept -> Quat {
        const auto sx = std::sin(radians.value.x * 0.5f), cx = std::cos(radians.value.x * 0.5f);
        const auto sy = std::sin(radians.value.y * 0.5f), cy = std::cos(radians.value.y * 0.5f);
        const auto sz = std::sin(radians.value.z * 0.5f), cz = std::cos(radians.value.z * 0.5f);
        return {cz * sx * cy - sz * cx * sy, cz * cx * sy + sz * sx * cy,
                sz * cx * cy - cz * sx * sy, cz * cx * cy + sz * sx * sy};
    }
    [[nodiscard]] static auto FromEulerDegrees(EulerDegrees degrees) noexcept -> Quat {
        return FromEuler(degrees.ToRadians());
    }
    [[nodiscard]] constexpr auto AsVec4() const noexcept -> FVec4 { return {x, y, z, w}; }
    [[nodiscard]] constexpr auto XYZ() const noexcept -> FVec3 { return {x, y, z}; }
    // Shortest-axis representation of a unit quaternion; q and -q agree.
    void ToAxisAngle(FVec3& axis, RadF& angle) const noexcept {
        const auto q = w < 0.0f ? -*this : *this;
        if (q.w >= 1.0f) { axis = FVec3::Zero(); angle = RadF{}; return; }
        axis = q.XYZ().Normalized();
        angle = Acos(std::clamp(q.w, 0.0f, 1.0f)) * 2.0f;
    }
    [[nodiscard]] constexpr auto operator[](std::size_t i) noexcept -> float& { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    [[nodiscard]] constexpr auto operator[](std::size_t i) const noexcept -> const float& { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }
    [[nodiscard]] constexpr auto operator-() const noexcept -> Quat { return {-x, -y, -z, -w}; }
    [[nodiscard]] constexpr auto LengthSquared() const noexcept -> float { return x*x + y*y + z*z + w*w; }
    [[nodiscard]] auto Length() const noexcept -> float { return std::hypot(std::hypot(x, y), std::hypot(z, w)); }
    // A zero quaternion has no rotation; use identity rather than divide by zero.
    [[nodiscard]] auto Normalized() const noexcept -> Quat {
        const auto length = Length();
        return length == 0.0f ? Identity() : Quat {x / length, y / length, z / length, w / length};
    }
    // Returns XYZ radians in the same Z * Y * X convention as FromEuler.
    [[nodiscard]] auto EulerAngles() const noexcept -> EulerRadians {
        return {{std::atan2(2.0f * (w*x + y*z), 1.0f - 2.0f * (x*x + y*y)),
                 std::asin(std::clamp(2.0f * (w*y - z*x), -1.0f, 1.0f)),
                 std::atan2(2.0f * (w*z + x*y), 1.0f - 2.0f * (y*y + z*z))}};
    }
    // Same convention as EulerAngles(), returned in degrees.
    [[nodiscard]] auto EulerAnglesDegrees() const noexcept -> EulerDegrees {
        return EulerAngles().ToDegrees();
    }
    [[nodiscard]] constexpr auto Conjugated() const noexcept -> Quat { return {-x, -y, -z, w}; }
    [[nodiscard]] constexpr auto Inversed() const noexcept -> Quat {
        const auto lengthSquared = LengthSquared();
        return lengthSquared == 0.0f ? Identity() : Quat {-x / lengthSquared, -y / lengthSquared, -z / lengthSquared, w / lengthSquared};
    }
};

[[nodiscard]] constexpr auto operator==(Quat a, Quat b) noexcept -> bool {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}
[[nodiscard]] constexpr auto operator+(Quat a, Quat b) noexcept -> Quat { return {a.x+b.x, a.y+b.y, a.z+b.z, a.w+b.w}; }
[[nodiscard]] constexpr auto operator-(Quat a, Quat b) noexcept -> Quat { return {a.x-b.x, a.y-b.y, a.z-b.z, a.w-b.w}; }
[[nodiscard]] constexpr auto operator*(Quat q, float s) noexcept -> Quat { return {q.x*s, q.y*s, q.z*s, q.w*s}; }
[[nodiscard]] constexpr auto operator*(float s, Quat q) noexcept -> Quat { return q * s; }
[[nodiscard]] constexpr auto operator/(Quat q, float s) noexcept -> Quat { return {q.x/s, q.y/s, q.z/s, q.w/s}; }
[[nodiscard]] constexpr auto operator*(Quat a, Quat b) noexcept -> Quat {
    return {a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
            a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
            a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z};
}
constexpr auto operator*=(Quat& a, Quat b) noexcept -> Quat& { return a = a * b; }

[[nodiscard]] constexpr auto Dot(Quat a, Quat b) noexcept -> float {
    return a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
}

// Rotates a vector by a unit quaternion without constructing a matrix.
[[nodiscard]] constexpr auto Rotate(Quat q, FVec3 v) noexcept -> FVec3 {
    const auto u = FVec3 {q.x, q.y, q.z};
    const auto t = 2.0f * Cross(u, v);
    return v + q.w * t + Cross(u, t);
}

[[nodiscard]] constexpr auto operator*(Quat q, FVec3 v) noexcept -> FVec3 { return Rotate(q, v); }

// Shortest arc from one direction to another. Zero vectors yield identity;
// exact antiparallel directions use a deterministic perpendicular axis.
[[nodiscard]] inline auto FromTo(FVec3 from, FVec3 to) noexcept -> Quat {
    from = from.Normalized();
    to = to.Normalized();
    if (from.LengthSquared() == 0.0f || to.LengthSquared() == 0.0f) return Quat::Identity();
    const double dot = static_cast<double>(from.x)*to.x + static_cast<double>(from.y)*to.y + static_cast<double>(from.z)*to.z;
    const double cx = static_cast<double>(from.y)*to.z - static_cast<double>(from.z)*to.y;
    const double cy = static_cast<double>(from.z)*to.x - static_cast<double>(from.x)*to.z;
    const double cz = static_cast<double>(from.x)*to.y - static_cast<double>(from.y)*to.x;
    const double crossLength = std::hypot(cx, cy, cz);
    if (crossLength < 1e-12 && dot < 0.0) {
        const auto axis = (std::abs(from.x) > std::abs(from.y) ? FVec3 {from.z, 0.0f, -from.x} : FVec3 {0.0f, from.z, -from.y}).Normalized();
        return {axis.x, axis.y, axis.z, 0.0f};
    }
    const double angle = std::atan2(crossLength, dot) * 0.5;
    if (crossLength == 0.0) return Quat::Identity();
    const auto s = std::sin(angle) / crossLength;
    return {static_cast<float>(cx*s), static_cast<float>(cy*s), static_cast<float>(cz*s), static_cast<float>(std::cos(angle))};
}

// Spherical interpolation on the shortest arc; t is not clamped.
[[nodiscard]] inline auto Slerp(Quat from, Quat to, float t) noexcept -> Quat {
    auto cosine = Dot(from, to);
    auto sign = 1.0f;
    if (cosine < 0.0f) { cosine = -cosine; sign = -1.0f; }
    auto a = 1.0f - t;
    auto b = sign * t;
    if (1.0f - cosine > 0.0001f) {
        const auto omega = std::acos(std::clamp(cosine, -1.0f, 1.0f));
        const auto invSin = 1.0f / std::sin(omega);
        a = std::sin((1.0f - t) * omega) * invSin;
        b = sign * std::sin(t * omega) * invSin;
    }
    return (from * a + to * b).Normalized();
}

static_assert(TriviallyCopyableStandardLayout<Quat>);
} // namespace ZHLN
