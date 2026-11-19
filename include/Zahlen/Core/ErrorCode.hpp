// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace ZHLN {

class Error;


struct ErrorCategory {
    std::string_view name;
    std::string_view (*to_string)(uint32_t) noexcept;
    std::string_view (*to_name)(uint32_t) noexcept;
};

namespace TemplatedDetail {

constexpr auto HashTypeName(std::string_view str) noexcept -> uint32_t {
    return Hash32(str);
}

struct EnumErrorEntry {
    uint32_t         value = 0;
    std::string_view name {};
    std::string_view message {};
};

struct ResolvedErrorStrings {
    std::string_view category = "None";
    std::string_view name     = "None";
    std::string_view message  = "None";
};

// The error-category registry lives inside libzahlen_engine so the engine, the
// host executable, and dynamically loaded plugin shared libraries share one
// table across ELF, Mach-O, and PE. Enum categories copy their reflected name
// and description strings into engine-owned storage on first registration so a
// plugin can return a typed ErrorCode and be unloaded via dlclose/FreeLibrary
// without leaving dangling .data/.rodata/.text pointers behind.
ZHLN_API void RegisterEnumCategory(
    uint32_t              hash,
    std::string_view      categoryName,
    const EnumErrorEntry* entries,
    std::size_t           entryCount
) noexcept;

ZHLN_API auto LookupErrorStrings(uint32_t hash, uint32_t value) noexcept -> ResolvedErrorStrings;

template <typename E>
    requires std::is_enum_v<E>
consteval auto MakeEnumErrorTable() -> std::array<EnumErrorEntry, Reflect::kEnumEntries<E>.size()> {
    constexpr auto&                                             src = Reflect::kEnumEntries<E>;
    std::array<EnumErrorEntry, Reflect::kEnumEntries<E>.size()> out {};
    for (std::size_t i = 0; i < src.size(); ++i) {
        out[i] = EnumErrorEntry {
            .value   = static_cast<uint32_t>(src[i].value),
            .name    = src[i].name,
            .message = !src[i].message.empty() ? src[i].message : src[i].name,
        };
    }
    return out;
}

template <typename E>
    requires std::is_enum_v<E>
struct CategoryRegistration {
    static inline bool registered = []() noexcept -> bool {
        static constexpr auto table = MakeEnumErrorTable<E>();
        RegisterEnumCategory(
            HashTypeName(ZHLN::Reflect::ScopedTypeName<E>()),
            ZHLN::Reflect::ScopedTypeName<E>(),
            table.data(),
            table.size()
        );
        return true;
    }();
};

} // namespace TemplatedDetail

ZHLN_API void RegisterErrorCategory(uint32_t hash, const ErrorCategory* category) noexcept;

extern void ERROR_CODE_CANNOT_BE_ZERO();


struct ErrorCode {
    constexpr ErrorCode() noexcept = default;
    constexpr ErrorCode(uint32_t cat, uint32_t val) noexcept: category(cat), value(val) {
    }

    template <typename E>
        requires std::is_enum_v<E>
    constexpr ErrorCode(E val) noexcept: category(Hash32(Reflect::ScopedTypeName<E>())), value(static_cast<uint32_t>(val)) {
        static_assert(
            !Reflect::EnumHasValue<E>(0),
            "Error enums must not contain a 0 enumerator: ErrorCode's value word uses 0 for 'no "
            "error'. Start error enumerators at 1; convert foreign codes (e.g. VkResult) "
            "with Vk::ToError instead."
        );
        if (static_cast<uint32_t>(val) == 0) {
            if consteval {
                ERROR_CODE_CANNOT_BE_ZERO();
            } else {
                DebugBreak();
            }
        }
        if consteval {
        } else {
            [[maybe_unused]] bool dummy = TemplatedDetail::CategoryRegistration<E>::registered;
        }
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value != 0;
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr bool Is() const noexcept {
        return category == Hash32(Reflect::ScopedTypeName<E>());
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr bool Is(E val) const noexcept {
        return Is<E>() && value == static_cast<uint32_t>(val);
    }

    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr E As() const noexcept {
        return static_cast<E>(value);
    }

    constexpr auto operator==(const ErrorCode& other) const noexcept -> bool = default;

    [[nodiscard]] Error ToError() const noexcept;

  private:
    uint32_t category = 0;
    uint32_t value    = 0;

    friend class Error;
};

static_assert(sizeof(ErrorCode) == 8);
static_assert(std::is_standard_layout_v<ErrorCode> && std::is_trivially_copyable_v<ErrorCode>);

} // namespace ZHLN
