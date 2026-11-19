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
#include <Zahlen/Core/ErrorCode.hpp>
#include <cstdint>
#include <expected>
#include <type_traits>

namespace ZHLN {

enum class TexFlags : uint8_t {
    None  = 0,
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

enum class RenderPassStage : uint8_t {
    Shadow = 0,
    Main   = 1,
    Post   = 2,
    UI     = 3,
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

template <typename T, typename = void>
struct HasFlagQueries : std::false_type {};
template <typename T>
struct HasFlagQueries<T, std::void_t<decltype(HasFlag(std::declval<T>(), std::declval<T>()), HasAnyFlags(std::declval<T>(), std::declval<T>()))>> : std::true_type {};
static_assert(!HasFlagQueries<PlainError>::value);
static_assert(HasFlagQueries<TexFlags>::value);

// An opted-in enum gets the full set, valued on the underlying bits.
static_assert((TexFlags::Srgb | TexFlags::Mips) == static_cast<TexFlags>(3));
static_assert((static_cast<TexFlags>(3) & TexFlags::Mips) == TexFlags::Mips);
static_assert((static_cast<TexFlags>(3) ^ TexFlags::Mips) == TexFlags::Srgb);
static_assert((~TexFlags::Srgb) == static_cast<TexFlags>(0xFE));
static_assert(HasFlag(TexFlags::Srgb | TexFlags::Mips, TexFlags::Srgb));
static_assert(!HasFlag(TexFlags::Srgb | TexFlags::Mips, TexFlags::Array));
static_assert(!HasFlag(TexFlags::Srgb | TexFlags::Mips, TexFlags::None));
static_assert(HasFlag(TexFlags::Srgb | TexFlags::Mips | TexFlags::Array, TexFlags::Srgb | TexFlags::Mips));
static_assert(!HasFlag(TexFlags::Srgb, TexFlags::Srgb | TexFlags::Mips));
static_assert(HasAnyFlags(TexFlags::Srgb, TexFlags::Srgb | TexFlags::Mips));
static_assert(!HasAnyFlags(TexFlags::Array, TexFlags::Srgb | TexFlags::Mips));

static_assert(EnumIndex(PlainError::Worse) == 2);
static_assert(EnumBitMask<RenderPassStage>::Bit(RenderPassStage::Post).Contains(RenderPassStage::Post));
static_assert(!EnumBitMask<RenderPassStage>::Bit(RenderPassStage::Post).Contains(RenderPassStage::Shadow));

} // namespace ZHLN

enum class FlagTestError : uint8_t {
    Failed = 1
};

namespace {

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

        std::expected<void, ZHLN::ErrorCode> enum_bit_mask_tracks_typed_enumerators() {
            ZHLN::EnumBitMask<ZHLN::RenderPassStage> mask {};
            if (!mask.Empty()) {
                return std::unexpected(FlagTestError::Failed);
            }

            mask.Set(ZHLN::RenderPassStage::Shadow);
            mask.Set(ZHLN::RenderPassStage::UI);
            if (!mask.Contains(ZHLN::RenderPassStage::Shadow) || !mask.Contains(ZHLN::RenderPassStage::UI) ||
                mask.Contains(ZHLN::RenderPassStage::Main)) {
                return std::unexpected(FlagTestError::Failed);
            }

            mask.Clear(ZHLN::RenderPassStage::Shadow);
            if (mask.Contains(ZHLN::RenderPassStage::Shadow) || !mask.Contains(ZHLN::RenderPassStage::UI)) {
                return std::unexpected(FlagTestError::Failed);
            }

            mask.Reset();
            if (!mask.Empty()) {
                return std::unexpected(FlagTestError::Failed);
            }
            return {};
        }
    };
};

} // namespace
