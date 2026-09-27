// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/Enums.hpp>
#include <Zahlen/Core/Reflection/Structs.hpp>
#include <Zahlen/Core/Hash.hpp>
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

template <typename E>
    requires std::is_enum_v<E>
constexpr auto EnumToFlagsString(E value, std::string& out_buffer) -> std::string_view {
    out_buffer.clear();
    using Under    = std::underlying_type_t<E>;
    auto val_under = static_cast<Under>(value);

    [:Expand(TemplatedDetail::EnumeratorsOf<E>()):] >> [&]<auto enumerator>() -> auto {
        constexpr E                enum_val   = static_cast<E>([:enumerator:]);
        auto                       enum_under = static_cast<Under>(enum_val);
        constexpr std::string_view name       = std::meta::identifier_of(enumerator);

        if (enum_under != 0 && (val_under & enum_under) == enum_under) {
            if (!out_buffer.empty()) {
                out_buffer += " | ";
            }
            out_buffer += name;
        }
    };

    if (out_buffer.empty() && val_under == 0) {
        return EnumToString(value);
    }
    return out_buffer;
}

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

#else

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumToFlagsString(E , std::string& out_buffer) {
    out_buffer.clear();
    return "";
}

template <typename T>
consteval std::size_t GetFloatFieldsCount() {
    return 0;
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

}

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

template <typename E, typename... Args>
    requires std::is_enum_v<E>
inline auto FormatEnumMessage(E value, Args&&... args) -> std::string {
    std::string_view fmt = EnumToMessage(value);
    if constexpr (sizeof...(Args) == 0) {
        return std::string(fmt);
    } else {
        return std::vformat(fmt, std::make_format_args(args...));
    }
}

template <typename E, typename... Args>
    requires std::is_enum_v<E>
inline auto FormatEnumMessageString(E value, Args&&... args) -> std::string {
    std::string_view fmt = EnumToMessage(value);
    if constexpr (sizeof...(Args) == 0) {
        return std::string(fmt);
    } else {
        return std::vformat(fmt, std::make_format_args(args...));
    }
}

}

namespace std {

template <typename E>
    requires std::is_enum_v<E>
struct formatter<E, char>: formatter<string_view, char> {
    template <typename FormatContext>
    auto format(E val, FormatContext& ctx) const {
        return formatter<string_view, char>::format(ZHLN::Reflect::EnumToMessage(val), ctx);
    }
};

}
