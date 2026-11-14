// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Concepts.hpp>
#include <cstddef>
#include <type_traits>

namespace ZHLN {

template <typename T>
struct Vec2 {
    T x, y;

    constexpr Vec2() noexcept = default;
    constexpr Vec2(T inX, T inY) noexcept: x(inX), y(inY) {
    }

    static constexpr auto Zero() noexcept -> Vec2 {
        return {T(0), T(0)};
    }
    static constexpr auto One() noexcept -> Vec2 {
        return {T(1), T(1)};
    }
    static constexpr auto Splat(T value) noexcept -> Vec2 {
        return {value, value};
    }

    [[nodiscard]] constexpr auto operator[](std::size_t index) noexcept -> T& {
        return index == 0 ? x : y;
    }
    [[nodiscard]] constexpr auto operator[](std::size_t index) const noexcept -> const T& {
        return index == 0 ? x : y;
    }

    [[nodiscard]] constexpr auto operator==(const Vec2&) const -> bool = default;
    [[nodiscard]] constexpr auto operator-() const noexcept -> Vec2 {
        return {-x, -y};
    }
    [[nodiscard]] constexpr auto operator+(Vec2 rhs) const noexcept -> Vec2 {
        return {x + rhs.x, y + rhs.y};
    }
    [[nodiscard]] constexpr auto operator-(Vec2 rhs) const noexcept -> Vec2 {
        return {x - rhs.x, y - rhs.y};
    }
    [[nodiscard]] constexpr auto operator*(Vec2 rhs) const noexcept -> Vec2 {
        return {x * rhs.x, y * rhs.y};
    }
    [[nodiscard]] constexpr auto operator/(Vec2 rhs) const noexcept -> Vec2 {
        return {x / rhs.x, y / rhs.y};
    }
    [[nodiscard]] constexpr auto operator*(T s) const noexcept -> Vec2 {
        return {x * s, y * s};
    }
    [[nodiscard]] constexpr auto operator/(T s) const noexcept -> Vec2 {
        return {x / s, y / s};
    }
    [[nodiscard]] friend constexpr auto operator*(T s, Vec2 v) noexcept -> Vec2 {
        return v * s;
    }
};

template <typename T>
[[nodiscard]] constexpr auto Dot(Vec2<T> a, Vec2<T> b) noexcept -> T {
    return a.x * b.x + a.y * b.y;
}

using FVec2 = Vec2<float>;
using DVec2 = Vec2<double>;

static_assert(TriviallyCopyableStandardLayout<FVec2>);
static_assert(TriviallyCopyableStandardLayout<DVec2>);

} // namespace ZHLN
