// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace ZHLN {

template <typename T>
inline constexpr bool EnableEnumFlags = false;

template <typename T>
concept EnumFlag = std::is_enum_v<T> && EnableEnumFlags<T>;

template <EnumFlag T>
[[nodiscard]] constexpr auto operator|(T a, T b) noexcept -> T {
    return static_cast<T>(std::to_underlying(a) | std::to_underlying(b));
}

template <EnumFlag T>
[[nodiscard]] constexpr auto operator&(T a, T b) noexcept -> T {
    return static_cast<T>(std::to_underlying(a) & std::to_underlying(b));
}

template <EnumFlag T>
[[nodiscard]] constexpr auto operator^(T a, T b) noexcept -> T {
    return static_cast<T>(std::to_underlying(a) ^ std::to_underlying(b));
}

template <EnumFlag T>
[[nodiscard]] constexpr auto operator~(T a) noexcept -> T {
    return static_cast<T>(~std::to_underlying(a));
}

template <EnumFlag T>
constexpr auto operator|=(T& a, T b) noexcept -> T& {
    a = a | b;
    return a;
}

template <EnumFlag T>
constexpr auto operator&=(T& a, T b) noexcept -> T& {
    a = a & b;
    return a;
}

template <EnumFlag T>
constexpr auto operator^=(T& a, T b) noexcept -> T& {
    a = a ^ b;
    return a;
}

template <EnumFlag T>
[[nodiscard]] constexpr auto operator==(T a, T b) noexcept -> bool {
    return std::to_underlying(a) == std::to_underlying(b);
}

template <EnumFlag T>
[[nodiscard]] constexpr auto operator!=(T a, T b) noexcept -> bool {
    return !(a == b);
}

template <EnumFlag T>
[[nodiscard]] constexpr auto HasFlag(T mask, T flag) noexcept -> bool {
    return (std::to_underlying(flag) != 0) && ((mask & flag) == flag);
}

template <EnumFlag T>
[[nodiscard]] constexpr auto HasAnyFlags(T mask, T flags) noexcept -> bool {
    return std::to_underlying(mask & flags) != 0;
}

template <typename E>
    requires std::is_enum_v<E>
[[nodiscard]] constexpr auto EnumIndex(E value) noexcept -> std::size_t {
    using Underlying = std::underlying_type_t<E>;
    if constexpr (std::is_signed_v<Underlying>) {
        const auto raw = std::to_underlying(value);
        return raw < 0 ? static_cast<std::size_t>(-1) : static_cast<std::size_t>(raw);
    } else {
        return static_cast<std::size_t>(std::to_underlying(value));
    }
}

// Compact 64-slot bitmask indexed by ordinal enumerators (0..63), avoiding
// manual `uint64_t{1} << static_cast<uint32_t>(e)` bit shifts on non-flag enums.
template <typename E>
    requires std::is_enum_v<E>
class EnumBitMask {
  public:
    static constexpr std::size_t kMaxBits = 64;

    constexpr EnumBitMask() noexcept = default;

    [[nodiscard]] static constexpr auto Bit(E value) noexcept -> EnumBitMask {
        EnumBitMask mask {};
        mask.Set(value);
        return mask;
    }

    constexpr void Set(E value) noexcept {
        const std::size_t idx = EnumIndex(value);
        if (idx < kMaxBits) {
            _bits |= (uint64_t {1} << idx);
        }
    }

    constexpr void Clear(E value) noexcept {
        const std::size_t idx = EnumIndex(value);
        if (idx < kMaxBits) {
            _bits &= ~(uint64_t {1} << idx);
        }
    }

    constexpr void Reset() noexcept {
        _bits = 0;
    }

    [[nodiscard]] constexpr auto Contains(E value) const noexcept -> bool {
        const std::size_t idx = EnumIndex(value);
        return idx < kMaxBits && ((_bits & (uint64_t {1} << idx)) != 0U);
    }

    [[nodiscard]] constexpr auto Any() const noexcept -> bool {
        return _bits != 0U;
    }

    [[nodiscard]] constexpr auto Empty() const noexcept -> bool {
        return _bits == 0U;
    }

    [[nodiscard]] constexpr auto RawBits() const noexcept -> uint64_t {
        return _bits;
    }

    [[nodiscard]] friend constexpr auto operator|(EnumBitMask lhs, EnumBitMask rhs) noexcept -> EnumBitMask {
        return EnumBitMask {lhs._bits | rhs._bits};
    }

    [[nodiscard]] friend constexpr auto operator&(EnumBitMask lhs, EnumBitMask rhs) noexcept -> EnumBitMask {
        return EnumBitMask {lhs._bits & rhs._bits};
    }

    constexpr auto operator|=(EnumBitMask rhs) noexcept -> EnumBitMask& {
        _bits |= rhs._bits;
        return *this;
    }

    constexpr auto operator&=(EnumBitMask rhs) noexcept -> EnumBitMask& {
        _bits &= rhs._bits;
        return *this;
    }

    constexpr auto operator==(const EnumBitMask&) const noexcept -> bool = default;

  private:
    explicit constexpr EnumBitMask(uint64_t raw) noexcept : _bits(raw) {}

    uint64_t _bits = 0;
};

} // namespace ZHLN
