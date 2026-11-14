// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Concepts.hpp>
#include <Zahlen/Math/Scalar.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <type_traits>

namespace ZHLN {

template <typename T>
struct Vec4 {
    T x, y, z, w;

    constexpr Vec4() noexcept = default;
    constexpr Vec4(T inX, T inY, T inZ, T inW) noexcept: x(inX), y(inY), z(inZ), w(inW) {
    }
    constexpr Vec4(Vec3<T> v, T inW = T(0)) noexcept: x(v.x), y(v.y), z(v.z), w(inW) {
    }

    // --- Uniform & Basis Factories ---
    static constexpr auto Zero() noexcept -> Vec4 {
        return {T(0), T(0), T(0), T(0)};
    }
    static constexpr auto One() noexcept -> Vec4 {
        return {T(1), T(1), T(1), T(1)};
    }
    static constexpr auto Splat(T value) noexcept -> Vec4 {
        return {value, value, value, value};
    }

    static constexpr auto UnitX() noexcept -> Vec4 {
        return {T(1), T(0), T(0), T(0)};
    }
    static constexpr auto UnitY() noexcept -> Vec4 {
        return {T(0), T(1), T(0), T(0)};
    }
    static constexpr auto UnitZ() noexcept -> Vec4 {
        return {T(0), T(0), T(1), T(0)};
    }
    static constexpr auto UnitW() noexcept -> Vec4 {
        return {T(0), T(0), T(0), T(1)};
    }

    // Spatial direction vectors have a homogeneous component of zero.
    static constexpr auto Right() noexcept -> Vec4 {
        return UnitX();
    }
    static constexpr auto Left() noexcept -> Vec4 {
        return {-T(1), T(0), T(0), T(0)};
    }
    static constexpr auto Up() noexcept -> Vec4 {
        return UnitY();
    }
    static constexpr auto Down() noexcept -> Vec4 {
        return {T(0), -T(1), T(0), T(0)};
    }
    static constexpr auto Forward() noexcept -> Vec4 {
        return {T(0), T(0), -T(1), T(0)};
    }
    static constexpr auto Back() noexcept -> Vec4 {
        return UnitZ();
    }

    // --- Sub-vector Extraction ---
    [[nodiscard]] constexpr auto XYZ() const noexcept -> Vec3<T> {
        return {x, y, z};
    }

    // --- Component Access ---
    [[nodiscard]] constexpr auto operator[](std::size_t index) noexcept -> T& {
        return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w));
    }
    [[nodiscard]] constexpr auto operator[](std::size_t index) const noexcept -> const T& {
        return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w));
    }

    [[nodiscard]] constexpr auto operator==(const Vec4&) const -> bool = default;
    [[nodiscard]] constexpr auto operator-() const noexcept -> Vec4 {
        return {-x, -y, -z, -w};
    }
    [[nodiscard]] constexpr auto operator+(Vec4 rhs) const noexcept -> Vec4 {
        return {x + rhs.x, y + rhs.y, z + rhs.z, w + rhs.w};
    }
    [[nodiscard]] constexpr auto operator-(Vec4 rhs) const noexcept -> Vec4 {
        return {x - rhs.x, y - rhs.y, z - rhs.z, w - rhs.w};
    }
    [[nodiscard]] constexpr auto operator*(Vec4 rhs) const noexcept -> Vec4 {
        return {x * rhs.x, y * rhs.y, z * rhs.z, w * rhs.w};
    }
    [[nodiscard]] constexpr auto operator/(Vec4 rhs) const noexcept -> Vec4 {
        return {x / rhs.x, y / rhs.y, z / rhs.z, w / rhs.w};
    }
    [[nodiscard]] constexpr auto operator*(T scalar) const noexcept -> Vec4 {
        return {x * scalar, y * scalar, z * scalar, w * scalar};
    }
    [[nodiscard]] constexpr auto operator/(T scalar) const noexcept -> Vec4 {
        return {x / scalar, y / scalar, z / scalar, w / scalar};
    }
    [[nodiscard]] friend constexpr auto operator*(T scalar, Vec4 v) noexcept -> Vec4 {
        return v * scalar;
    }

    constexpr auto operator+=(Vec4 rhs) noexcept -> Vec4& {
        return *this = *this + rhs;
    }
    constexpr auto operator-=(Vec4 rhs) noexcept -> Vec4& {
        return *this = *this - rhs;
    }
    constexpr auto operator*=(T scalar) noexcept -> Vec4& {
        return *this = *this * scalar;
    }
    constexpr auto operator/=(T scalar) noexcept -> Vec4& {
        return *this = *this / scalar;
    }

    // maxDistanceSquared is the squared Euclidean distance.
    [[nodiscard]] constexpr auto IsClose(Vec4 other, T maxDistanceSquared = (sizeof(T) <= sizeof(float) ? T(1e-12) : T(1e-24))) const noexcept -> bool
        requires std::is_floating_point_v<T> {
        return (*this - other).LengthSquared() <= maxDistanceSquared;
    }
    // For signed integers, the absolute value must be representable in T.
    [[nodiscard]] constexpr auto Abs() const noexcept -> Vec4 {
        return {ZHLN::Abs(x), ZHLN::Abs(y), ZHLN::Abs(z), ZHLN::Abs(w)};
    }
    [[nodiscard]] constexpr auto LengthSquared() const noexcept -> T {
        return x * x + y * y + z * z + w * w;
    }
    [[nodiscard]] auto Length() const noexcept -> T
        requires std::is_floating_point_v<T>
    {
        return std::hypot(std::hypot(x, y), std::hypot(z, w));
    }
    [[nodiscard]] auto Normalized() const noexcept -> Vec4
        requires std::is_floating_point_v<T>
    {
        const T len = Length();
        return len == T(0) ? Zero() : *this / len;
    }
    [[nodiscard]] auto Floor() const noexcept -> Vec4
        requires std::is_floating_point_v<T>
    {
        return {std::floor(x), std::floor(y), std::floor(z), std::floor(w)};
    }
    [[nodiscard]] auto Fract() const noexcept -> Vec4
        requires std::is_floating_point_v<T>
    {
        return *this - Floor();
    }

    // --- Non-Jolt Comparisons (Clean predicate tests) ---
    [[nodiscard]] constexpr auto AnyLess(Vec4 rhs) const noexcept -> bool {
        return x < rhs.x || y < rhs.y || z < rhs.z || w < rhs.w;
    }
    [[nodiscard]] constexpr auto AllLess(Vec4 rhs) const noexcept -> bool {
        return x < rhs.x && y < rhs.y && z < rhs.z && w < rhs.w;
    }
    [[nodiscard]] constexpr auto AnyGreater(Vec4 rhs) const noexcept -> bool {
        return x > rhs.x || y > rhs.y || z > rhs.z || w > rhs.w;
    }
    [[nodiscard]] constexpr auto AllGreater(Vec4 rhs) const noexcept -> bool {
        return x > rhs.x && y > rhs.y && z > rhs.z && w > rhs.w;
    }
};

// --- Free Functions ---
template <typename T>
[[nodiscard]] constexpr auto Dot(Vec4<T> a, Vec4<T> b) noexcept -> T {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

template <typename T>
[[nodiscard]] constexpr auto DistanceSquared(Vec4<T> a, Vec4<T> b) noexcept -> T {
    return (a - b).LengthSquared();
}

template <typename T>
    requires std::is_floating_point_v<T>
[[nodiscard]] auto Distance(Vec4<T> a, Vec4<T> b) noexcept -> T {
    return (a - b).Length();
}

template <typename T>
[[nodiscard]] constexpr auto Lerp(Vec4<T> a, Vec4<T> b, std::type_identity_t<T> t) noexcept -> Vec4<T> {
    return a + (b - a) * t;
}

template <typename T>
    requires std::is_floating_point_v<T>
[[nodiscard]] auto Wrap(Vec4<T> p, Vec4<T> period) noexcept -> Vec4<T> {
    const auto wrap = [](T v, T cell) { return std::fmod(std::fmod(v, cell) + cell, cell); };
    return {wrap(p.x, period.x), wrap(p.y, period.y), wrap(p.z, period.z), wrap(p.w, period.w)};
}

template <typename T>
[[nodiscard]] constexpr auto Min(Vec4<T> a, Vec4<T> b) noexcept -> Vec4<T> {
    return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z), std::min(a.w, b.w)};
}

template <typename T>
[[nodiscard]] constexpr auto Max(Vec4<T> a, Vec4<T> b) noexcept -> Vec4<T> {
    return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z), std::max(a.w, b.w)};
}

template <typename T>
[[nodiscard]] constexpr auto Clamp(Vec4<T> v, Vec4<T> lo, Vec4<T> hi) noexcept -> Vec4<T> {
    return {std::clamp(v.x, lo.x, hi.x), std::clamp(v.y, lo.y, hi.y), std::clamp(v.z, lo.z, hi.z), std::clamp(v.w, lo.w, hi.w)};
}

using FVec4 = Vec4<float>;
using DVec4 = Vec4<double>;

static_assert(TriviallyCopyableStandardLayout<FVec4>);
static_assert(TriviallyCopyableStandardLayout<DVec4>);

} // namespace ZHLN
