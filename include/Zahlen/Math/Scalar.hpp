// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <concepts>

namespace ZHLN {

[[nodiscard]] constexpr auto Abs(std::floating_point auto val) noexcept {
    return val < 0 ? -val : val;
}

// For signed integers, the absolute value must be representable in the input type.
template <std::integral T>
[[nodiscard]] constexpr auto Abs(T val) noexcept -> T {
    if constexpr (std::unsigned_integral<T>) return val;
    else return static_cast<T>(val < 0 ? -val : val);
}

} // namespace ZHLN
