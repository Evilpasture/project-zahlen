// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


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

}

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
    constexpr auto   table = MakeEnumMessageTable<E>();
    std::string_view last {};
    bool             matched = false;
    for (const auto& entry: table) {
        if (static_cast<std::underlying_type_t<E>>(value) == entry.value) {
            last    = entry.message;
            matched = true;
        }
    }
    return matched ? last : std::string_view {};
}

#else

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumToString(E ) {
    return "Unknown";
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumHasValue(std::underlying_type_t<E> ) noexcept -> bool {
    return false;
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::optional<E> StringToEnum(std::string_view ) {
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
constexpr void ForEachEnumerator(F&& ) {
}

template <typename Tag, typename E>
    requires std::is_enum_v<E>
constexpr std::optional<Tag> GetEnumeratorAnnotation(E ) {
    return std::nullopt;
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumMessageOf(E ) {
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

}
