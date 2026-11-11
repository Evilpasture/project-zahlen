// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Concepts.hpp>
#include <cmath>
#include <cstddef>
#include <type_traits>

namespace ZHLN {

template <typename T>
struct Vec4 {
    T x, y, z, w;

    static constexpr auto Zero() -> Vec4 { return {T(0), T(0), T(0), T(0)}; }
    static constexpr auto One() -> Vec4 { return {T(1), T(1), T(1), T(1)}; }
    static constexpr auto Splat(T value) -> Vec4 { return {value, value, value, value}; }

    static constexpr auto UnitX() -> Vec4 { return {T(1), T(0), T(0), T(0)}; }
    static constexpr auto UnitY() -> Vec4 { return {T(0), T(1), T(0), T(0)}; }
    static constexpr auto UnitZ() -> Vec4 { return {T(0), T(0), T(1), T(0)}; }
    static constexpr auto UnitW() -> Vec4 { return {T(0), T(0), T(0), T(1)}; }

    // Spatial direction vectors have a homogeneous component of zero.
    static constexpr auto Right() -> Vec4 { return UnitX(); }
    static constexpr auto Left() -> Vec4 { return {-T(1), T(0), T(0), T(0)}; }
    static constexpr auto Up() -> Vec4 { return UnitY(); }
    static constexpr auto Down() -> Vec4 { return {T(0), -T(1), T(0), T(0)}; }
    static constexpr auto Forward() noexcept -> Vec4 { return {T(0), T(0), -T(1), T(0)}; }
    static constexpr auto Back() noexcept -> Vec4 { return UnitZ(); }

    // Precondition: index is 0, 1, 2 or 3.
    [[nodiscard]] constexpr auto operator[](std::size_t index) noexcept -> T& {
        return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w));
    }
    [[nodiscard]] constexpr auto operator[](std::size_t index) const noexcept -> const T& {
        return index == 0 ? x : (index == 1 ? y : (index == 2 ? z : w));
    }

    [[nodiscard]] constexpr auto operator==(const Vec4&) const -> bool = default;
    [[nodiscard]] constexpr auto operator-() const noexcept -> Vec4 { return {-x, -y, -z, -w}; }
    [[nodiscard]] constexpr auto operator+(Vec4 rhs) const noexcept -> Vec4 {
        return {x + rhs.x, y + rhs.y, z + rhs.z, w + rhs.w};
    }
    [[nodiscard]] constexpr auto operator-(Vec4 rhs) const noexcept -> Vec4 {
        return {x - rhs.x, y - rhs.y, z - rhs.z, w - rhs.w};
    }
    // Vector multiplication and division are component-wise.
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
    [[nodiscard]] friend constexpr auto operator*(T scalar, Vec4 v) noexcept -> Vec4 { return v * scalar; }
    constexpr auto operator+=(Vec4 rhs) noexcept -> Vec4& { return *this = *this + rhs; }
    constexpr auto operator-=(Vec4 rhs) noexcept -> Vec4& { return *this = *this - rhs; }
    constexpr auto operator*=(T scalar) noexcept -> Vec4& { return *this = *this * scalar; }
    constexpr auto operator/=(T scalar) noexcept -> Vec4& { return *this = *this / scalar; }

    [[nodiscard]] constexpr auto LengthSquared() const noexcept -> T { return x * x + y * y + z * z + w * w; }
    [[nodiscard]] auto Length() const noexcept -> T requires std::is_floating_point_v<T> {
        return std::hypot(std::hypot(x, y), std::hypot(z, w));
    }
    // Zero vectors normalize to zero; non-finite inputs propagate as usual.
    [[nodiscard]] auto Normalized() const noexcept -> Vec4 requires std::is_floating_point_v<T> {
        const T length = Length();
        return length == T(0) ? Zero() : *this / length;
    }
    [[nodiscard]] auto Floor() const noexcept -> Vec4 requires std::is_floating_point_v<T> {
        return {std::floor(x), std::floor(y), std::floor(z), std::floor(w)};
    }
    [[nodiscard]] auto Fract() const noexcept -> Vec4 requires std::is_floating_point_v<T> {
        return *this - Floor();
    }
};

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

// t is not clamped. It is converted to the vector's component type.
template <typename T>
[[nodiscard]] constexpr auto Lerp(Vec4<T> a, Vec4<T> b, std::type_identity_t<T> t) noexcept -> Vec4<T> {
    return a + (b - a) * t;
}

// Each period must be finite and nonzero. For positive periods the result
// lies in [0, period), subject to floating-point rounding, as in Math3D.
template <typename T>
    requires std::is_floating_point_v<T>
[[nodiscard]] auto Wrap(Vec4<T> p, Vec4<T> period) noexcept -> Vec4<T> {
    const auto wrap = [](T v, T cell) { return std::fmod(std::fmod(v, cell) + cell, cell); };
    return {wrap(p.x, period.x), wrap(p.y, period.y), wrap(p.z, period.z), wrap(p.w, period.w)};
}

using FVec4 = Vec4<float>;
using DVec4 = Vec4<double>;

static_assert(TriviallyCopyableStandardLayout<FVec4>);
static_assert(TriviallyCopyableStandardLayout<DVec4>);
} // namespace ZHLN
