// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/Reflection/Enums.hpp
//
// Everything about an enum: how many enumerators there are, what they are
// called (EnumNames/EnumToString), the other direction (StringToEnum),
// whether a value names a real enumerator (EnumHasValue -- the guard
// Zahlen/Error.hpp's zero-value static_assert is built on), the annotated
// tables Error::Message() reads, and the dispatchers that turn a runtime
// value into a compile-time one (DispatchEnum/ForEachEnumerator).
//
// Deliberately <string>-free: the one enum helper that builds a std::string,
// EnumToFlagsString, lives in Utilities.hpp instead, because this header sits
// on the error path (ErrorCode.hpp -> Error.hpp) where a diagnostics-only
// allocation is exactly what should not be paid for by every failure.

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ZHLN::Reflect {

template <typename E>
    requires std::is_enum_v<E>
struct EnumMessageEntry {
    std::underlying_type_t<E> value {};
    std::string_view          message;
};

#if ZHLN_REFLECTION_AVAILABLE

namespace TemplatedDetail {

template <typename E>
consteval auto EnumeratorsOf() {
    return std::define_static_array(std::meta::enumerators_of(^^E));
}

} // namespace TemplatedDetail

template <typename E>
    requires std::is_enum_v<E>
constexpr auto EnumToString(E value) -> std::string_view {
    std::string_view result = "Unknown";
    [:Expand(TemplatedDetail::EnumeratorsOf<E>()):] >> [&]<auto enumerator>() -> auto {
        if (value == static_cast<E>([:enumerator:])) {
            result = std::meta::identifier_of(enumerator);
        }
    };
    return result;
}

template <typename E>
    requires std::is_enum_v<E>
constexpr auto StringToEnum(std::string_view name) -> std::optional<E> {
    // Match into a plain flag plus a value-initialized enum, then build the
    // optional once on a single non-lambda path. Holding the result in a
    // std::optional<E> that is only ever assigned from inside the expanded
    // lambda leaves GCC unable to prove the optional's payload was ever
    // constructed, and it reports -Wmaybe-uninitialized at every `*parsed` in
    // a caller. Value-initializing the enum keeps the read well-defined on the
    // no-match path too, where 0 is always in an enum's value range.
    bool found = false;
    E    value {};
    [:Expand(TemplatedDetail::EnumeratorsOf<E>()):] >> [&]<auto enumerator>() -> auto {
        if (name == std::meta::identifier_of(enumerator)) {
            value = static_cast<E>([:enumerator:]);
            found = true;
        }
    };
    if (!found) {
        return std::nullopt;
    }
    return value;
}

template <typename E>
    requires std::is_enum_v<E>
constexpr auto EnumHasValue(std::underlying_type_t<E> targetValue) noexcept -> bool {
    bool found = false;
    [:Expand(TemplatedDetail::EnumeratorsOf<E>()):] >> [&]<auto enumerator>() -> auto {
        if (static_cast<std::underlying_type_t<E>>([:enumerator:]) == targetValue) {
            found = true;
        }
    };
    return found;
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumCount() -> std::size_t {
    return std::meta::enumerators_of(^^E).size();
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumNames() {
    constexpr auto enumerators = TemplatedDetail::EnumeratorsOf<E>();
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> auto {
        return std::array<std::string_view, sizeof...(Is)> {std::meta::identifier_of(enumerators[Is])...};
    }(std::make_index_sequence<enumerators.size()>());
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumUnderlyingTypeName() -> std::string_view {
    return std::meta::display_string_of(std::meta::underlying_type(^^E));
}

template <typename E, typename F>
    requires std::is_enum_v<E>
constexpr void ForEachEnumerator(F&& f) {
    [:Expand(TemplatedDetail::EnumeratorsOf<E>()):] >> [&]<auto enumerator>() -> auto {
        constexpr E Val = static_cast<E>([:enumerator:]);
        f.template  operator()<Val>();
    };
}

template <typename Tag, typename E>
    requires std::is_enum_v<E>
constexpr auto GetEnumeratorAnnotation(E value) -> std::optional<Tag> {
    std::optional<Tag> result = std::nullopt;
    [:Expand(TemplatedDetail::EnumeratorsOf<E>()):] >> [&]<auto enumerator>() -> auto {
        auto annotation = GetAnnotation<Tag, enumerator>();
        if (value == static_cast<E>([:enumerator:])) {
            result = annotation;
        }
    };
    return result;
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto MakeEnumMessageTable() -> std::array<EnumMessageEntry<E>, TemplatedDetail::EnumeratorsOf<E>().size()> {
    std::array<EnumMessageEntry<E>, TemplatedDetail::EnumeratorsOf<E>().size()> table {};
    std::size_t                                                                 i = 0;
    [:Expand(TemplatedDetail::EnumeratorsOf<E>()):] >> [&]<auto enumerator>() -> auto {
        table[i].value = static_cast<std::underlying_type_t<E>>([:enumerator:]);
#ifndef ZHLN_NO_ANNOTATION_EXTRACT
        table[i].message = GetDescriptionText<enumerator>();
#endif
        ++i;
    };
    return table;
}

template <typename E>
    requires std::is_enum_v<E>
constexpr auto EnumMessageOf(E value) -> std::string_view {
    // MakeEnumMessageTable<E>() takes no runtime arguments, so the call is
    // always a constant expression and this function can never be
    // reclassified as immediate.
    constexpr auto   table = MakeEnumMessageTable<E>();
    std::string_view last {};
    bool             matched = false;
    for (const auto& entry: table) {
        if (static_cast<std::underlying_type_t<E>>(value) == entry.value) {
            // Last matching enumerator wins, mirroring the overwrite
            // semantics of GetEnumeratorAnnotation for aliased values.
            last    = entry.message;
            matched = true;
        }
    }
    return matched ? last : std::string_view {};
}

#else // No C++26 static reflection: this module's degraded stand-ins.

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumToString(E /*unused*/) {
    return "Unknown";
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumHasValue(std::underlying_type_t<E> /*targetValue*/) noexcept -> bool {
    return false; // Safe fallback when compiler reflection is disabled
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::optional<E> StringToEnum(std::string_view /*unused*/) {
    return std::nullopt;
}

template <typename E>
    requires std::is_enum_v<E>
consteval std::size_t EnumCount() {
    return 0;
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumNames() {
    return std::array<std::string_view, 0> {};
}

template <typename E>
    requires std::is_enum_v<E>
consteval std::string_view EnumUnderlyingTypeName() {
    return "";
}

template <typename E, typename F>
    requires std::is_enum_v<E>
constexpr void ForEachEnumerator(F&& /*unused*/) {
}

template <typename Tag, typename E>
    requires std::is_enum_v<E>
constexpr std::optional<Tag> GetEnumeratorAnnotation(E /*unused*/) {
    return std::nullopt;
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumMessageOf(E /*unused*/) {
    return {};
}

#endif

template <typename E, typename F>
    requires std::is_enum_v<E>
constexpr void DispatchEnum(E value, F&& f) {
    ForEachEnumerator<E>([&]<E Val>() -> auto {
        if (value == Val) {
            std::forward<F>(f).template operator()<Val>();
        }
    });
}

template <typename E>
    requires std::is_enum_v<E>
constexpr auto EnumToMessage(E value) -> std::string_view {
    // Annotation lookup happens entirely at compile time inside
    // EnumMessageOf's table; see the comment there for why no
    // reflection-dependent call may remain in this runtime path.
    if (auto message = EnumMessageOf(value); !message.empty()) {
        return message;
    }
    return EnumToString(value);
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumHasValue(E targetValue) noexcept -> bool {
    return EnumHasValue<E>(static_cast<std::underlying_type_t<E>>(targetValue));
}

} // namespace ZHLN::Reflect
