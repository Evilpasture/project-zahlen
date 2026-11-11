// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>
#include <compare>
#include <concepts>
#include <numbers>

namespace ZHLN {

template <std::floating_point T>
struct Radians;

template <std::floating_point T>
struct Degrees {
    T value{};

    constexpr Degrees() noexcept = default;
    constexpr explicit Degrees(T v) noexcept: value(v) {
    }

    [[nodiscard]] constexpr auto ToRadians() const noexcept -> Radians<T>;

    constexpr explicit operator T() const noexcept {
        return value;
    }
    [[nodiscard]] constexpr auto operator==(const Degrees&) const noexcept -> bool = default;
    [[nodiscard]] constexpr auto operator<=>(const Degrees&) const noexcept = default;
};

template <std::floating_point T>
struct Radians {
    T value{};

    constexpr Radians() noexcept = default;
    constexpr explicit Radians(T v) noexcept: value(v) {
    }

    [[nodiscard]] constexpr auto ToDegrees() const noexcept -> Degrees<T> {
        return Degrees<T>{value * (static_cast<T>(180.0) / std::numbers::pi_v<T>)};
    }

    constexpr explicit operator T() const noexcept {
        return value;
    }
    [[nodiscard]] constexpr auto operator==(const Radians&) const noexcept -> bool = default;
};

template <std::floating_point T>
constexpr auto Degrees<T>::ToRadians() const noexcept -> Radians<T> {
    return Radians<T>{value * (std::numbers::pi_v<T> / static_cast<T>(180.0))};
}

using DegF = Degrees<float>;
using RadF = Radians<float>;
using DegD = Degrees<double>;
using RadD = Radians<double>;

// Arithmetic preserves units; degrees and radians never mix implicitly.
template <std::floating_point T>
[[nodiscard]] constexpr auto operator-(Degrees<T> angle) noexcept -> Degrees<T> {
    return Degrees<T>{-angle.value};
}
template <std::floating_point T>
[[nodiscard]] constexpr auto operator+(Degrees<T> lhs, Degrees<T> rhs) noexcept -> Degrees<T> {
    return Degrees<T>{lhs.value + rhs.value};
}
template <std::floating_point T>
[[nodiscard]] constexpr auto operator-(Degrees<T> lhs, Degrees<T> rhs) noexcept -> Degrees<T> {
    return Degrees<T>{lhs.value - rhs.value};
}
template <std::floating_point T>
constexpr auto operator+=(Degrees<T>& lhs, Degrees<T> rhs) noexcept -> Degrees<T>& {
    return lhs = lhs + rhs;
}
template <std::floating_point T>
constexpr auto operator-=(Degrees<T>& lhs, Degrees<T> rhs) noexcept -> Degrees<T>& {
    return lhs = lhs - rhs;
}
template <std::floating_point T>
[[nodiscard]] constexpr auto operator*(Radians<T> angle, T scale) noexcept -> Radians<T> {
    return Radians<T>{angle.value * scale};
}
template <std::floating_point T>
[[nodiscard]] constexpr auto operator*(T scale, Radians<T> angle) noexcept -> Radians<T> {
    return angle * scale;
}
template <std::floating_point T>
[[nodiscard]] constexpr auto operator*(Degrees<T> angle, T scale) noexcept -> Degrees<T> {
    return Degrees<T>{angle.value * scale};
}
template <std::floating_point T>
[[nodiscard]] constexpr auto operator*(T scale, Degrees<T> angle) noexcept -> Degrees<T> {
    return angle * scale;
}

// Type-safe trigonometric wrappers
template <std::floating_point T>
[[nodiscard]] inline auto Sin(Radians<T> r) noexcept -> T {
    return std::sin(r.value);
}
template <std::floating_point T>
[[nodiscard]] inline auto Cos(Radians<T> r) noexcept -> T {
    return std::cos(r.value);
}
template <std::floating_point T>
[[nodiscard]] inline auto Tan(Radians<T> r) noexcept -> T {
    return std::tan(r.value);
}

template <std::floating_point T>
[[nodiscard]] inline auto Asin(T v) noexcept -> Radians<T> {
    return Radians<T>{std::asin(v)};
}
template <std::floating_point T>
[[nodiscard]] inline auto Acos(T v) noexcept -> Radians<T> {
    return Radians<T>{std::acos(v)};
}
template <std::floating_point T>
[[nodiscard]] inline auto Atan2(T y, T x) noexcept -> Radians<T> {
    return Radians<T>{std::atan2(y, x)};
}

namespace Literals {
constexpr auto operator""_deg(long double d) noexcept -> DegD {
    return DegD{static_cast<double>(d)};
}
constexpr auto operator""_degf(long double d) noexcept -> DegF {
    return DegF{static_cast<float>(d)};
}
constexpr auto operator""_rad(long double r) noexcept -> RadD {
    return RadD{static_cast<double>(r)};
}
constexpr auto operator""_radf(long double r) noexcept -> RadF {
    return RadF{static_cast<float>(r)};
}
} // namespace Literals

} // namespace ZHLN
