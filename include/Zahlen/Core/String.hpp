// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <format>
#include <string_view>

namespace ZHLN {

/**
 * @brief A fixed-capacity, null-terminated string that never allocates.
 * @tparam Capacity The maximum number of characters including the null terminator.
 */
template <size_t Capacity>
class FixedString {
    static_assert(Capacity > 0, "Capacity must be at least 1 for null terminator");

  public:
    constexpr FixedString() noexcept = default;

    constexpr FixedString(const char* s) noexcept {
        assign(s);
    }

    constexpr FixedString(std::string_view sv) noexcept {
        assign(sv);
    }

    constexpr void assign(std::string_view sv) noexcept {
        _len = std::min(sv.size(), Capacity - 1);
        for (size_t i = 0; i < _len; ++i) {
            _data[i] = sv[i];
        }
        _data[_len] = '\0';
    }

    constexpr void append(std::string_view sv) noexcept {
        size_t remaining = Capacity - 1 - _len;
        size_t to_copy   = std::min(sv.size(), remaining);
        for (size_t i = 0; i < to_copy; ++i) {
            _data[_len + i] = sv[i];
        }
        _len += to_copy;
        _data[_len] = '\0';
    }

    [[nodiscard]] constexpr auto data() noexcept -> char* {
        return _data.data();
    }
    [[nodiscard]] constexpr auto data() const noexcept -> const char* {
        return _data.data();
    }
    [[nodiscard]] constexpr auto c_str() const noexcept -> const char* {
        return _data.data();
    }
    [[nodiscard]] constexpr auto size() const noexcept -> size_t {
        return _len;
    }
    [[nodiscard]] constexpr auto empty() const noexcept -> bool {
        return _len == 0;
    }
    [[nodiscard]] constexpr auto capacity() const noexcept -> size_t {
        return Capacity;
    }

    constexpr auto operator[](size_t i) noexcept -> char& {
        return _data[i];
    }
    constexpr auto operator[](size_t i) const noexcept -> const char& {
        return _data[i];
    }

    [[nodiscard]] constexpr operator std::string_view() const noexcept {
        return std::string_view {_data.data(), _len};
    }

    constexpr auto operator<=>(const FixedString& other) const noexcept {
        return std::string_view(*this) <=> std::string_view(other);
    }

    constexpr auto operator==(const FixedString& other) const noexcept -> bool {
        return std::string_view(*this) == std::string_view(other);
    }

    constexpr void clear() noexcept {
        _len     = 0;
        _data[0] = '\0';
    }

    template <size_t N>
    constexpr void copy_to(char (&dest)[N]) const noexcept {
        size_t to_copy = std::min(_len, N - 1);
        for (size_t i = 0; i < to_copy; ++i) {
            dest[i] = _data[i];
        }
        dest[to_copy] = '\0';
    }

  private:
    std::array<char, Capacity> _data {};
    size_t                     _len = 0;
};

// Helper for type deduction: ZHLN::FixedString str{"Hello"};
template <size_t N>
FixedString(const char (&)[N]) -> FixedString<N>;

// Common aliases
using String32  = FixedString<32>;
using String64  = FixedString<64>;
using String128 = FixedString<128>;
using String256 = FixedString<256>;

} // namespace ZHLN

namespace std {
template <size_t N>
struct formatter<ZHLN::FixedString<N>, char>: formatter<string_view, char> {
    auto format(const ZHLN::FixedString<N>& str, format_context& ctx) const {
        return formatter<string_view, char>::format(string_view(str), ctx);
    }
};
} // namespace std
