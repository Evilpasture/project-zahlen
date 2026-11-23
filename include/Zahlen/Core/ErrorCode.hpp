// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <optional>
#include <type_traits>

namespace ZHLN {

extern void ERROR_CODE_CANNOT_BE_ZERO();

struct ErrorText {
    std::string_view name;
    std::string_view message;
};

// One enumerator of a statically anchored error category. Registration
// COPIES these into engine-owned strings (see ErrorRegistry.cpp), so no
// function pointer or string view into a registrant's DSO is ever stored:
// a plugin may be dlclosed without leaving anything dangling behind, and
// no Unregister call is required.
struct StaticErrorEntry {
    uint32_t         value = 0;
    std::string_view name;
    std::string_view message;
};

namespace TemplatedDetail {

// The static error-category table lives in libzahlen_engine (one copy per
// process, reached through this exported function) -- never as an inline
// table in this header, which every DSO would duplicate under `local: *;`
// symbol hiding.
ZHLN_API auto RegisterStaticErrorCategoryRaw(uint32_t categoryHash, std::string_view categoryName, std::span<const StaticErrorEntry> entries) noexcept -> bool;

// Anchors enum E's reflection entries to its ErrorCode category, once per
// enum per process. Called from the ErrorCode(E) constructor, so any enum
// that is ever carried by an ErrorCode can be formatted readably -- no
// per-consumer RegisterEnum bookkeeping.
template <typename E>
inline void AnchorEnumCategory() noexcept {
    static const bool anchored = []() noexcept {
        std::array<StaticErrorEntry, Reflect::kEnumEntries<E>.size()> entries {};
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = Reflect::kEnumEntries<E>[i];
            entries[i]        = StaticErrorEntry {
                .value   = static_cast<uint32_t>(entry.value),
                .name    = entry.name,
                .message = !entry.message.empty() ? entry.message : entry.name,
            };
        }
        return RegisterStaticErrorCategoryRaw(Hash32(Reflect::ScopedTypeName<E>()), Reflect::ScopedTypeName<E>(), entries);
    }();
    (void) anchored;
}

} // namespace TemplatedDetail


// The one error carrier (the former ZHLN::Error wrapper is gone: it was the
// same two words with a second, divergent category hash). ErrorCode is pure
// data -- constructing one has no side effects and touches no registry. The
// category word is Hash32 of the enum's canonical type name, the same name
// ChainTypeID and InterfaceIdOf hash, so an enumerator's category, its
// IErrorRegistry registration, and get<E>() can never disagree. Turning a code
// into text is IErrorRegistry::Describe's job (Zahlen/COM/IErrorRegistry.hpp);
// nothing in this header knows that interface exists. The name hashed is
// Reflect::ScopedTypeName, not CanonicalTypeName: canonical names reject
// anonymous-namespace enums, which are legitimate .cpp-local error enums.
// Constructing from an enum also anchors that enum's category in the
// engine-owned static describe table (run-time path only; consteval
// construction stays side-effect free). The two words remain pure data.
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
        } else {
            if consteval {
            } else {
                // Anchor at construction: formatting this code anywhere --
                // any DSO, any thread, no registry in hand -- resolves to
                // "Category::Name: message" (see Zahlen/Core/ErrorDescribe.hpp).
                TemplatedDetail::AnchorEnumCategory<E>();
            }
        }
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return value != 0;
    }

    // Monadic extraction (mirrors std::get_if / any_cast): engaged iff this
    // code's category is E. Value matching is plain equality
    // (`code == E::Member` or `code.get<E>() == E::Member`).
    template <typename E>
        requires std::is_enum_v<E>
    [[nodiscard]] constexpr auto get() const noexcept -> std::optional<E> {
        if (category == Hash32(Reflect::ScopedTypeName<E>())) {
            return static_cast<E>(value);
        }
        return std::nullopt;
    }

    [[nodiscard]] constexpr auto CategoryHash() const noexcept -> uint32_t {
        return category;
    }
    [[nodiscard]] constexpr auto Value() const noexcept -> uint32_t {
        return value;
    }

    constexpr auto operator==(const ErrorCode& other) const noexcept -> bool = default;

  private:
    uint32_t category = 0;
    uint32_t value    = 0;
};

static_assert(sizeof(ErrorCode) == 8);
static_assert(std::is_standard_layout_v<ErrorCode> && std::is_trivially_copyable_v<ErrorCode>);

} // namespace ZHLN
