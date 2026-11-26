// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <bit>
#include <concepts>
#include <cstdint>
#include <limits>

namespace ZHLN {

[[nodiscard]] constexpr auto IsNaN(std::floating_point auto val) noexcept {
    // NOLINTNEXTLINE(misc-redundant-expression)
    return val != val;
}

[[nodiscard]] constexpr auto Abs(std::floating_point auto val) noexcept {
    return val < 0 ? -val : val;
}

// For signed integers, the absolute value must be representable in the input type.
template <std::integral T>
[[nodiscard]] constexpr auto Abs(T val) noexcept -> T {
    if constexpr (std::unsigned_integral<T>) {
        return val;
    } else {
        return static_cast<T>(val < 0 ? -val : val);
    }
}

template <typename T>
    requires(std::floating_point<T>)
[[nodiscard]] constexpr auto Sqrt(T val) noexcept -> T {
    if (IsNaN(val)) { // NaN
        return val;
    }
    if (val <= T(0)) { // negatives and +-0 -> 0
        return T(0);
    }
    if (val == std::numeric_limits<T>::infinity()) {
        return val;
    }

    using U          = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
    constexpr U bias = sizeof(T) == 4 ? U(0x1FC00000) : U(0x1FF8000000000000);

    // Halve the exponent: a ~6% accurate starting guess.
    T curr = std::bit_cast<T>((std::bit_cast<U>(val) >> 1) + bias);

    // The first step always lands at or above the true root (AM-GM)...
    curr = T(0.5) * (curr + val / curr);

    // ...after which iterates decrease monotonically until they stop improving.
    for (;;) {
        const T next = T(0.5) * (curr + val / curr);
        if (next >= curr) {
            return curr;
        }
        curr = next;
    }
}

} // namespace ZHLN
