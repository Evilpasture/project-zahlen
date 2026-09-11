// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ZHLN {

inline constexpr uint32_t kFnvOffset32 = 2166136261u;
inline constexpr uint32_t kFnvPrime32  = 16777619u;
inline constexpr uint64_t kFnvOffset64 = 0xcbf29ce484222325ull;
inline constexpr uint64_t kFnvPrime64  = 0x100000001b3ull;

/// 2^32 / phi and 2^64 / phi. Fibonacci hashing / hash_combine.
inline constexpr uint32_t kGolden32 = 0x9E3779B9u;
inline constexpr uint64_t kGolden64 = 0x9E3779B97F4A7C15ull;

[[nodiscard]] constexpr auto Hash32(const char* data, size_t length) noexcept -> uint32_t {
    uint32_t hash = kFnvOffset32;
    for (size_t i = 0; i < length; ++i) {
        hash ^= static_cast<uint8_t>(data[i]);
        hash *= kFnvPrime32;
    }
    return hash;
}

[[nodiscard]] constexpr auto Hash32(std::string_view str) noexcept -> uint32_t {
    return Hash32(str.data(), str.size());
}

[[nodiscard]] constexpr auto Hash64(const char* data, size_t length) noexcept -> uint64_t {
    uint64_t hash = kFnvOffset64;
    for (size_t i = 0; i < length; ++i) {
        hash ^= static_cast<uint8_t>(data[i]);
        hash *= kFnvPrime64;
    }
    return hash;
}

[[nodiscard]] constexpr auto Hash64(std::string_view str) noexcept -> uint64_t {
    return Hash64(str.data(), str.size());
}

[[nodiscard]] constexpr auto Mix32(uint32_t x) noexcept -> uint32_t {
    return x * kGolden32;
}

[[nodiscard]] constexpr auto Mix64(uint64_t x) noexcept -> uint64_t {
    return x * kGolden64;
}

constexpr void HashCombine(std::size_t& seed, std::size_t value) noexcept {
    seed ^= value + static_cast<std::size_t>(kGolden32) + (seed << 6) + (seed >> 2);
}

} // namespace ZHLN
