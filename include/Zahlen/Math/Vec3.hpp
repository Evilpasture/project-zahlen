#pragma once

#include <Zahlen/Core/Concepts.hpp>
#include <cmath>
#include <cstddef>
#include <type_traits>

namespace ZHLN {

template <typename T>
struct Vec3 {
    T x, y, z;

    // --- Existing ---
    static constexpr auto Zero() -> Vec3 {
        return {T(0), T(0), T(0)};
    }

    // --- Uniform Factories ---
    static constexpr auto One() -> Vec3 {
        return {T(1), T(1), T(1)};
    }

    // Broadcasts a single value to all three components (often called Splat)
    static constexpr auto Splat(T value) -> Vec3 {
        return {value, value, value};
    }

    // --- Standard Basis / Axis Vectors ---
    static constexpr auto UnitX() -> Vec3 {
        return {T(1), T(0), T(0)};
    }
    static constexpr auto UnitY() -> Vec3 {
        return {T(0), T(1), T(0)};
    }
    static constexpr auto UnitZ() -> Vec3 {
        return {T(0), T(0), T(1)};
    }

    // --- Directional Aliases (Common in Engines/Graphics) ---
    static constexpr auto Right() -> Vec3 {
        return {T(1), T(0), T(0)};
    }
    static constexpr auto Left() -> Vec3 {
        return {-T(1), T(0), T(0)};
    }
    static constexpr auto Up() -> Vec3 {
        return {T(0), T(1), T(0)};
    }
    static constexpr auto Down() -> Vec3 {
        return {T(0), -T(1), T(0)};
    }
    static constexpr auto Forward() noexcept -> Vec3 {
        return {T(0), T(0), -T(1)};
    }
    static constexpr auto Back() noexcept -> Vec3 {
        return {T(0), T(0), T(1)};
    }

    // Precondition: index is 0, 1 or 2.
    [[nodiscard]] constexpr auto operator[](std::size_t index) noexcept -> T& {
        return index == 0 ? x : (index == 1 ? y : z);
    }

    [[nodiscard]] constexpr auto operator[](std::size_t index) const noexcept -> const T& {
        return index == 0 ? x : (index == 1 ? y : z);
    }

    [[nodiscard]] constexpr auto operator==(const Vec3&) const -> bool = default;

    [[nodiscard]] constexpr auto operator-() const noexcept -> Vec3 {
        return {-x, -y, -z};
    }
    [[nodiscard]] constexpr auto operator+(Vec3 rhs) const noexcept -> Vec3 {
        return {x + rhs.x, y + rhs.y, z + rhs.z};
    }
    [[nodiscard]] constexpr auto operator-(Vec3 rhs) const noexcept -> Vec3 {
        return {x - rhs.x, y - rhs.y, z - rhs.z};
    }
    // Vector multiplication and division are component-wise.
    [[nodiscard]] constexpr auto operator*(Vec3 rhs) const noexcept -> Vec3 {
        return {x * rhs.x, y * rhs.y, z * rhs.z};
    }
    [[nodiscard]] constexpr auto operator/(Vec3 rhs) const noexcept -> Vec3 {
        return {x / rhs.x, y / rhs.y, z / rhs.z};
    }
    [[nodiscard]] constexpr auto operator*(T scalar) const noexcept -> Vec3 {
        return {x * scalar, y * scalar, z * scalar};
    }
    [[nodiscard]] constexpr auto operator/(T scalar) const noexcept -> Vec3 {
        return {x / scalar, y / scalar, z / scalar};
    }
    [[nodiscard]] friend constexpr auto operator*(T scalar, Vec3 v) noexcept -> Vec3 {
        return v * scalar;
    }
    constexpr auto operator+=(Vec3 rhs) noexcept -> Vec3& { return *this = *this + rhs; }
    constexpr auto operator-=(Vec3 rhs) noexcept -> Vec3& { return *this = *this - rhs; }
    constexpr auto operator*=(T scalar) noexcept -> Vec3& { return *this = *this * scalar; }
    constexpr auto operator/=(T scalar) noexcept -> Vec3& { return *this = *this / scalar; }

    [[nodiscard]] constexpr auto LengthSquared() const noexcept -> T { return x * x + y * y + z * z; }
    [[nodiscard]] auto Length() const noexcept -> T requires std::is_floating_point_v<T> {
        return std::hypot(x, y, z);
    }
    // Zero vectors normalize to zero; non-finite inputs propagate as usual.
    [[nodiscard]] auto Normalized() const noexcept -> Vec3 requires std::is_floating_point_v<T> {
        const T length = Length();
        return length == T(0) ? Zero() : *this / length;
    }

    // Component-wise equivalents of Math3D's FVec3 helpers.
    [[nodiscard]] auto Floor() const noexcept -> Vec3 requires std::is_floating_point_v<T> {
        return {std::floor(x), std::floor(y), std::floor(z)};
    }
    [[nodiscard]] auto Fract() const noexcept -> Vec3 requires std::is_floating_point_v<T> {
        return *this - Floor();
    }
};

// Binary vector maths lives outside the type; unary operations remain members.
template <typename T>
[[nodiscard]] constexpr auto Dot(Vec3<T> a, Vec3<T> b) noexcept -> T {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

template <typename T>
[[nodiscard]] constexpr auto Cross(Vec3<T> a, Vec3<T> b) noexcept -> Vec3<T> {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

template <typename T>
[[nodiscard]] constexpr auto DistanceSquared(Vec3<T> a, Vec3<T> b) noexcept -> T {
    return (a - b).LengthSquared();
}

template <typename T>
    requires std::is_floating_point_v<T>
[[nodiscard]] auto Distance(Vec3<T> a, Vec3<T> b) noexcept -> T {
    return (a - b).Length();
}

// t is not clamped, matching Math::Lerp. The scalar is converted to the
// vector's component type (rather than participating in template deduction).
template <typename T>
[[nodiscard]] constexpr auto Lerp(Vec3<T> a, Vec3<T> b, std::type_identity_t<T> t) noexcept -> Vec3<T> {
    return a + (b - a) * t;
}

// Each period must be finite and nonzero. For positive periods the result
// lies in [0, period), subject to floating-point rounding, as in Math3D.
template <typename T>
    requires std::is_floating_point_v<T>
[[nodiscard]] auto Wrap(Vec3<T> p, Vec3<T> period) noexcept -> Vec3<T> {
    const auto wrap = [](T v, T cell) { return std::fmod(std::fmod(v, cell) + cell, cell); };
    return {wrap(p.x, period.x), wrap(p.y, period.y), wrap(p.z, period.z)};
}

using FVec3 = Vec3<float>;
using DVec3 = Vec3<double>;

static_assert(TriviallyCopyableStandardLayout<Vec3<float>>);
static_assert(TriviallyCopyableStandardLayout<Vec3<double>>);
} // namespace ZHLN
