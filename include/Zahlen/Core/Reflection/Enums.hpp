// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
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

template <typename E>
    requires std::is_enum_v<E>
struct EnumEntry {
    E                value {};
    std::string_view name {};
    std::string_view message {};
};

#if ZHLN_REFLECTION_AVAILABLE

namespace TemplatedDetail {

template <typename E>
inline constexpr auto kEnumeratorsOf = std::define_static_array(std::meta::enumerators_of(^^E));

template <typename E>
consteval auto EnumeratorsOf() {
    return kEnumeratorsOf<E>;
}

template <typename E>
consteval auto HasAnyEnumeratorAnnotation() noexcept -> bool {
#ifndef ZHLN_NO_ANNOTATION_EXTRACT
    for (auto enumerator : kEnumeratorsOf<E>) {
        if (!std::meta::annotations_of(enumerator).empty()) {
            return true;
        }
    }
#endif
    return false;
}

} // namespace TemplatedDetail

// One memoized reflection table per enum type E per translation unit. Enums
// without enumerator annotations populate in a pure consteval loop with zero
// Expand()/ReplicatorType template instantiations; annotated enums extract
// Description annotations once and cache them alongside the name and value.
template <typename E>
    requires std::is_enum_v<E>
inline constexpr auto kEnumEntries = []() consteval {
    constexpr auto                               enumerators = TemplatedDetail::kEnumeratorsOf<E>;
    std::array<EnumEntry<E>, enumerators.size()> entries {};
    if constexpr (!TemplatedDetail::HasAnyEnumeratorAnnotation<E>()) {
        for (std::size_t i = 0; i < enumerators.size(); ++i) {
            entries[i] = EnumEntry<E> {
                .value   = std::meta::extract<E>(enumerators[i]),
                .name    = std::meta::identifier_of(enumerators[i]),
                .message = {},
            };
        }
    } else {
        std::size_t i = 0;
        [:Expand(enumerators):] >> [&]<auto enumerator>() -> auto {
            entries[i++] = EnumEntry<E> {
                .value   = static_cast<E>([:enumerator:]),
                .name    = std::meta::identifier_of(enumerator),
#ifndef ZHLN_NO_ANNOTATION_EXTRACT
                .message = GetDescriptionText<enumerator>(),
#endif
            };
        };
    }
    return entries;
}();

template <typename E>
    requires std::is_enum_v<E>
constexpr auto EnumToString(E value) -> std::string_view {
    const auto& entries = kEnumEntries<E>;
    for (std::size_t i = entries.size(); i > 0; --i) {
        if (entries[i - 1].value == value) {
            return entries[i - 1].name;
        }
    }
    return "Unknown";
}

template <typename E>
    requires std::is_enum_v<E>
constexpr auto StringToEnum(std::string_view name) -> std::optional<E> {
    const auto& entries = kEnumEntries<E>;
    for (std::size_t i = entries.size(); i > 0; --i) {
        if (entries[i - 1].name == name) {
            return entries[i - 1].value;
        }
    }
    return std::nullopt;
}

template <typename E>
    requires std::is_enum_v<E>
constexpr auto EnumHasValue(std::underlying_type_t<E> targetValue) noexcept -> bool {
    for (const auto& entry : kEnumEntries<E>) {
        if (static_cast<std::underlying_type_t<E>>(entry.value) == targetValue) {
            return true;
        }
    }
    return false;
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumCount() -> std::size_t {
    return TemplatedDetail::kEnumeratorsOf<E>.size();
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumNames() {
    constexpr auto&                              entries = kEnumEntries<E>;
    std::array<std::string_view, entries.size()> names {};
    for (std::size_t i = 0; i < entries.size(); ++i) {
        names[i] = entries[i].name;
    }
    return names;
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumUnderlyingTypeName() -> std::string_view {
    return std::meta::display_string_of(std::meta::underlying_type(^^E));
}

template <typename E, typename F>
    requires std::is_enum_v<E>
constexpr void ForEachEnumerator(F&& f) {
    [:Expand(TemplatedDetail::kEnumeratorsOf<E>):] >> [&]<auto enumerator>() -> auto {
        constexpr E Val = static_cast<E>([:enumerator:]);
        f.template  operator()<Val>();
    };
}

template <typename Tag, typename E>
    requires std::is_enum_v<E>
constexpr auto GetEnumeratorAnnotation(E value) -> std::optional<Tag> {
    std::optional<Tag> result = std::nullopt;
    [:Expand(TemplatedDetail::kEnumeratorsOf<E>):] >> [&]<auto enumerator>() -> auto {
        auto annotation = GetAnnotation<Tag, enumerator>();
        if (value == static_cast<E>([:enumerator:])) {
            result = annotation;
        }
    };
    return result;
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto MakeEnumMessageTable() -> std::array<EnumMessageEntry<E>, TemplatedDetail::kEnumeratorsOf<E>.size()> {
    constexpr auto&                                                             entries = kEnumEntries<E>;
    std::array<EnumMessageEntry<E>, TemplatedDetail::kEnumeratorsOf<E>.size()> table {};
    for (std::size_t i = 0; i < entries.size(); ++i) {
        table[i].value   = static_cast<std::underlying_type_t<E>>(entries[i].value);
        table[i].message = entries[i].message;
    }
    return table;
}

template <typename E>
    requires std::is_enum_v<E>
constexpr auto EnumMessageOf(E value) -> std::string_view {
    const auto& entries = kEnumEntries<E>;
    for (std::size_t i = entries.size(); i > 0; --i) {
        if (entries[i - 1].value == value) {
            return entries[i - 1].message;
        }
    }
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
#if ZHLN_REFLECTION_AVAILABLE
    const auto& entries = kEnumEntries<E>;
    for (std::size_t i = entries.size(); i > 0; --i) {
        if (entries[i - 1].value == value) {
            return !entries[i - 1].message.empty() ? entries[i - 1].message : entries[i - 1].name;
        }
    }
    return "Unknown";
#else
    if (auto message = EnumMessageOf(value); !message.empty()) {
        return message;
    }
    return EnumToString(value);
#endif
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumHasValue(E targetValue) noexcept -> bool {
    return EnumHasValue<E>(static_cast<std::underlying_type_t<E>>(targetValue));
}

} // namespace ZHLN::Reflect
