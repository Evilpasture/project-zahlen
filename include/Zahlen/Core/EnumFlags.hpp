// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <type_traits>

namespace ZHLN {

template <typename T>
inline constexpr bool EnableEnumFlags = false;

template <typename T>
concept EnumFlag = std::is_enum_v<T> && EnableEnumFlags<T>;

template <EnumFlag T>
constexpr T operator|(T a, T b) noexcept {
    return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) | static_cast<std::underlying_type_t<T>>(b));
}

template <EnumFlag T>
constexpr T operator&(T a, T b) noexcept {
    return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) & static_cast<std::underlying_type_t<T>>(b));
}

template <EnumFlag T>
constexpr T operator^(T a, T b) noexcept {
    return static_cast<T>(static_cast<std::underlying_type_t<T>>(a) ^ static_cast<std::underlying_type_t<T>>(b));
}

template <EnumFlag T>
constexpr T operator~(T a) noexcept {
    return static_cast<T>(~static_cast<std::underlying_type_t<T>>(a));
}

template <EnumFlag T>
constexpr T& operator|=(T& a, T b) noexcept {
    a = a | b;
    return a;
}

template <EnumFlag T>
constexpr T& operator&=(T& a, T b) noexcept {
    a = a & b;
    return a;
}

template <EnumFlag T>
constexpr T& operator^=(T& a, T b) noexcept {
    a = a ^ b;
    return a;
}

template <EnumFlag T>
constexpr bool operator==(T a, T b) noexcept {
    return static_cast<std::underlying_type_t<T>>(a) == static_cast<std::underlying_type_t<T>>(b);
}

template <EnumFlag T>
constexpr bool operator!=(T a, T b) noexcept {
    return !(a == b);
}

}
