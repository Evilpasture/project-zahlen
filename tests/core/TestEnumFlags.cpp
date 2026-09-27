// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/core/TestEnumFlags.cpp
//
// The whole contract of Core/EnumFlags.hpp is opt-in: bit operators exist only
// for enum types that switch EnableEnumFlags on, so an ordinary enum can never
// grow silent arithmetic. The operators are found by argument-dependent lookup,
// so the test enums are declared in ZHLN exactly like the production ones. The
// negative pins below are the gating spelled as compile-time facts; the test
// body pins the values the operators produce.

#include "TestsFramework.hpp"
#include <Zahlen/Core/EnumFlags.hpp>
#include <Zahlen/ErrorCode.hpp>
#include <cstdint>
#include <expected>
#include <type_traits>

namespace ZHLN {

enum class TexFlags : uint8_t {
    Srgb  = 1,
    Mips  = 2,
    Array = 4,
};
template <>
inline constexpr bool EnableEnumFlags<TexFlags> = true;

enum class PlainError : uint8_t {
    Bad   = 1,
    Worse = 2,
};

// A plain enum grows no operators: the concept is the gate. Detection idiom,
// so the absence is a fact on every compiler rather than an overload-resolution
// diagnostic some of them refuse to swallow.
template <typename T, typename = void>
struct HasBitOperators : std::false_type {};
template <typename T>
struct HasBitOperators<T, std::void_t<decltype(std::declval<T>() | std::declval<T>(), std::declval<T>() & std::declval<T>(), std::declval<T>() ^ std::declval<T>(), ~std::declval<T>())>> : std::true_type {};
static_assert(!HasBitOperators<PlainError>::value);
static_assert(HasBitOperators<TexFlags>::value);

// An opted-in enum gets the full set, valued on the underlying bits.
static_assert((TexFlags::Srgb | TexFlags::Mips) == static_cast<TexFlags>(3));
static_assert((static_cast<TexFlags>(3) & TexFlags::Mips) == TexFlags::Mips);
static_assert((static_cast<TexFlags>(3) ^ TexFlags::Mips) == TexFlags::Srgb);
static_assert((~TexFlags::Srgb) == static_cast<TexFlags>(0xFE));

} // namespace ZHLN

enum class FlagTestError : uint8_t {
    Failed = 1
};

struct EnumFlagsTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> compound_operators_mutate_in_place() {
            ZHLN::TexFlags flags = ZHLN::TexFlags::Srgb;

            flags |= ZHLN::TexFlags::Array;
            if (flags != static_cast<ZHLN::TexFlags>(5)) {
                return std::unexpected(FlagTestError::Failed);
            }

            flags &= ZHLN::TexFlags::Mips;
            if (flags != static_cast<ZHLN::TexFlags>(0)) {
                return std::unexpected(FlagTestError::Failed);
            }

            flags = ZHLN::TexFlags::Srgb | ZHLN::TexFlags::Mips;
            flags ^= ZHLN::TexFlags::Mips;
            if (flags != ZHLN::TexFlags::Srgb) {
                return std::unexpected(FlagTestError::Failed);
            }
            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunEnumFlagsSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<EnumFlagsTestSuite>();
}
