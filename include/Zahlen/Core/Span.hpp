// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Platform.hpp"
#include <array>
#include <type_traits>

namespace ZHLN {
template <typename T>
struct RestrictSpan {
    using element_type = T;
    using value_type   = std::remove_cv_t<T>;
    using pointer      = T*;
    using reference    = T&;

    pointer ZHLN_RESTRICT _ptr;
    size_t                _len;

    // 1. Manual constructor (like std::span(ptr, len))
    [[gnu::always_inline]]
    constexpr RestrictSpan(pointer p, size_t l) noexcept: _ptr(p), _len(l) {
    }

    // Allows RestrictSpan<T> to convert to RestrictSpan<const T>
    template <typename U>
        requires std::is_convertible_v<U*, T*>
    [[gnu::always_inline]]
    constexpr RestrictSpan(const RestrictSpan<U>& other) noexcept: _ptr(other.data()), _len(other.size()) {
    }

    // 2. C-style array constructor (like std::span(T(&arr)[N]))
    template <size_t N>
    [[gnu::always_inline]]
    // NOLINTNEXTLINE(modernize-avoid-c-arrays)
    constexpr RestrictSpan(T (&arr)[N]) noexcept: _ptr(arr), _len(N) {
    }

    // 3. std::array constructor (handles const/non-const element types)
    template <size_t N>
    [[gnu::always_inline]]
    constexpr RestrictSpan(std::array<std::remove_const_t<T>, N>& arr) noexcept: _ptr(arr.data()), _len(N) {
    }

    // 4. Subspan helper (Crucial for your BATCH_SIZE logic)
    [[nodiscard, gnu::always_inline]]
    constexpr auto first(size_t count) const noexcept -> RestrictSpan<T> {
        return RestrictSpan<T>(_ptr, count);
    }

    // Standard Accessors
    [[nodiscard, gnu::always_inline]] constexpr auto operator[](size_t i) const noexcept -> reference {
        return _ptr[i];
    }
    [[nodiscard, gnu::always_inline]] constexpr auto data() const noexcept -> pointer {
        return _ptr;
    }
    [[nodiscard, gnu::always_inline]] constexpr auto size() const noexcept -> size_t {
        return _len;
    }
    [[nodiscard, gnu::always_inline]] auto begin() const noexcept -> pointer {
        return _ptr;
    }
    [[nodiscard, gnu::always_inline]] auto end() const noexcept -> pointer {
        return _ptr + _len;
    }
};
// NOLINTNEXTLINE(modernize-avoid-c-arrays)
template <typename T, std::size_t N>
RestrictSpan(T (&)[N]) -> RestrictSpan<T>;
template <typename T, std::size_t N>
RestrictSpan(std::array<T, N>&) -> RestrictSpan<T>;
} // namespace ZHLN