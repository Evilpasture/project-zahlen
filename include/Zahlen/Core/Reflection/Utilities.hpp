// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/EnumFormat.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <cstddef>
#include <format>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ZHLN::Reflect {

#if ZHLN_REFLECTION_AVAILABLE

template <typename T>
consteval auto GetFloatFieldsCount() -> std::size_t {
    using U      = std::remove_cvref_t<T>;
    auto members = TemplatedDetail::NonStaticDataMembers<U>();
    if (members.empty()) {
        return 0;
    }
    for (auto member: members) {
        if (std::meta::type_of(member) != ^^float) {
            return 0;
        }
    }
    return members.size();
}

#endif

template <typename T>
constexpr auto GetSchemaNameOf() noexcept -> std::string_view {
    return GetSchemaName(static_cast<T*>(nullptr));
}

template <typename T>
constexpr auto GenericEqual(const T& lhs, const T& rhs) -> bool {
    return TieFields(lhs) == TieFields(rhs);
}

template <typename T>
constexpr auto GenericCompare(const T& lhs, const T& rhs) {
    return TieFields(lhs) <=> TieFields(rhs);
}

template <typename T>
constexpr auto GenericLess(const T& lhs, const T& rhs) -> bool {
    return TieFields(lhs) < TieFields(rhs);
}

template <typename T>
constexpr auto GenericHash(const T& t) -> std::size_t {
    std::size_t seed = 0;
    ForEachField(t, [&](auto&& field) -> auto { HashCombine(seed, std::hash<std::remove_cvref_t<decltype(field)>> {}(field)); });
    return seed;
}

template <typename Dst, typename Src>
constexpr void CopyMatchingFields(Dst& dst, const Src& src) {
    ForEachFieldWithName(src, [&](std::string_view name, auto&& value) -> auto {
        VisitFieldByName(dst, name, [&](auto&& dstField) -> auto {
            if constexpr (std::is_assignable_v<decltype(dstField)&, decltype(value)>) {
                dstField = value;
            }
        });
    });
}

template <typename From, typename To>
consteval auto MapFieldIndex(std::size_t fromIdx) -> std::size_t {
    constexpr auto fromNames = FieldNames<From>();
    constexpr auto toNames   = FieldNames<To>();
    for (std::size_t i = 0; i < toNames.size(); ++i) {
        if (toNames[i] == fromNames[fromIdx]) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

namespace TemplatedDetail {

template <typename T>
concept Formattable = requires(const T& val, std::format_context ctx) { std::formatter<std::remove_cvref_t<T>, char>().format(val, ctx); };

} // namespace TemplatedDetail

template <typename T>
auto ToDebugString(const T& t) -> std::string;

template <typename T, typename = void>
struct CustomFormatter {
    static void format(const T& val, std::string& out) {
        using Decayed = std::remove_cvref_t<T>;

        if constexpr (std::is_enum_v<Decayed>) {
            out += EnumToString(val);
        } else if constexpr (TemplatedDetail::Formattable<Decayed>) {
            out += std::format("{}", val);
        } else if constexpr (std::ranges::input_range<Decayed>) {
            out += "[";
            bool first = true;
            for (const auto& elem: val) {
                if (!first) {
                    out += ", ";
                }
                first = false;
                out += ToDebugString(elem);
            }
            out += "]";
        } else if constexpr (std::is_class_v<Decayed>) {
            if constexpr (FieldCount<Decayed>() > 0) {
                out += "{";
                bool first = true;
                ForEachFieldWithName(val, [&](std::string_view name, auto&& value) -> auto {
                    if (!first) {
                        out += ", ";
                    }
                    first = false;
                    out += std::string(name) + "=" + ToDebugString(value);
                });
                out += "}";
            } else {
                out += TypeName<Decayed>();
            }
        } else {
            out += "?";
        }
    }
};

template <typename T>
auto ToDebugString(const T& t) -> std::string {
    std::string out;
    CustomFormatter<std::remove_cvref_t<T>>::format(t, out);
    return out;
}

} // namespace ZHLN::Reflect
