#pragma once

#include <Zahlen/Core/Concepts.hpp>

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

    [[nodiscard]] constexpr auto operator[](size_t index) noexcept -> T& {
        return (&x)[index];
    }

    [[nodiscard]] constexpr auto operator[](size_t index) const noexcept -> const T& {
        return (&x)[index];
    }
};

using FVec3 = Vec3<float>;
using DVec3 = Vec3<double>;

static_assert(TriviallyCopyableStandardLayout<Vec3<float>>);
static_assert(TriviallyCopyableStandardLayout<Vec3<double>>);
} // namespace ZHLN
