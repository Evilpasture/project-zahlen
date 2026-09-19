// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/Reflection/Utilities.hpp
//
// What is built *out* of reflection: the generic comparison/hash/copy family
// (GenericEqual, GenericCompare, GenericLess, GenericHash,
// CopyMatchingFields, MapFieldIndex), GetSchemaNameOf, and the formatting layer
// that is the whole reason this header exists apart from the rest --
// ToDebugString and CustomFormatter on one side, the error path's
// FormatEnumMessage/FormatEnumMessageString/EnumToFlagsString on the other, and
// the std::formatter that closes the directory out: `Log("{}", anyEnum)` names
// the enumerator without a helper call at the call site.
//
// This is the one header in the directory that pulls <format>, <ranges> and
// <string>. Anything that only carries, compares or iterates data belongs in
// one of the others.

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
    // Plain loop rather than std::ranges::all_of: one shape for every walk in
    // this directory, and it is the shape the fallback stubs can carry too.
    for (auto member: members) {
        if (std::meta::type_of(member) != ^^float) {
            return 0;
        }
    }
    return members.size();
}

#else // No C++26 static reflection: this module's degraded stand-ins.

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumToFlagsString(E /*unused*/, std::string& out_buffer) {
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

} // namespace TemplatedDetail

template <typename T>
auto ToDebugString(const T& t) -> std::string;

template <typename T, typename = void>
struct CustomFormatter {
    static void format(const T& val, std::string& out) {
        using Decayed = std::remove_cvref_t<T>;

        // Enums are tested first because they are the one category whose
        // Formattable answer changed when the enum formatter below arrived: a
        // struct dump is a debug spelling, and the debug spelling of an enum is
        // its identifier (EnumToString), not the annotated message a log line
        // wants. Leaving this branch second would have handed every enum in
        // every dump over to std::format -- silently, and only after this
        // header gained the formatter.
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

} // namespace ZHLN::Reflect

namespace std {

/// Every enum formats as its annotated message, falling back to the
/// enumerator's identifier -- the same text Reflect::EnumToMessage answers
/// with, so `Log("{}", someErrorEnum)` and `Log("{}", ErrorCode(someErrorEnum))`
/// print the same sentence for the same failure instead of one printing a
/// sentence and the other a token.
///
/// The identifier alone is deliberately *not* what this prints: that spelling is
/// one call away (ZHLN::Reflect::EnumToString) and is the one debug output wants
/// -- CustomFormatter above still asks for it by name, which is why its enum
/// test comes before its Formattable test. Formatting is the logging boundary
/// here, exactly as it is for formatter<ZHLN::ErrorCode> in Zahlen/Error.hpp.
///
/// The constraint is the whole contract: `requires std::is_enum_v<E>` accepts
/// exactly what the formatter can name, so a class with no formatter of its own
/// still fails to compile with the library's own diagnostic rather than
/// printing a placeholder. It is a strictly narrower replacement for the deleted
/// ZHLN::ToString, which took three unrelated types under one name and
/// static_asserted on the rest -- and, for an enum, hid which of the two
/// spellings the caller wanted.
///
/// Two implementation notes, both load-bearing:
///
///   * The format member is templated on the context. The standard's formatter
///     requirements ask for any output iterator, and std::formattable probes
///     exactly that -- `format(t, ctx)` with basic_format_context<char*, char>.
///     A member written against format_context& still compiles for std::format
///     and std::vformat (what ZHLN::Log uses, so every call site in this tree)
///     but leaves std::formattable<E, char> false, and range formatting is
///     gated on it: std::format("{}", std::vector<E>) then fails with "call to
///     consteval function ... is not a constant expression", pointing at the
///     format string rather than at the formatter. (The two formatters in
///     Zahlen/Error.hpp still carry the narrower member; nothing formats a
///     range of Errors yet.)
///   * Inheriting formatter<string_view> forwards the whole string spec
///     ({:>12}, {:.3}), and the value reaches it as a string_view, so a `{}`
///     inside an annotation prints literally rather than being substituted --
///     annotations that are format templates belong to
///     Reflect::FormatEnumMessage, which fills them.
///
/// Note for a future toolchain: this specialization is `<E, char>`, so it is
/// more specialized than any library-provided `formatter<E, CharT>`; if libc++
/// ever ships an enum formatter, this one keeps winning for char and the two do
/// not collide.
/// The format member is templated on the context, and that is load-bearing.
/// The standard's formatter requirements ask a formatter to work for any output
/// iterator, which std::formattable tests by probing `format(t, ctx)` with
/// basic_format_context<char*, char>. A member written against format_context&
/// still compiles for std::format and std::vformat -- what ZHLN::Log uses, and
/// so what every call site in this tree does -- but leaves
/// `std::formattable<E, char>` false, and that is not academic: range
/// formatting is gated on it, so `std::format("{}", std::vector<E>)` fails to
/// compile with "call to consteval function ... is not a constant expression",
/// pointing at the format string rather than at the formatter. (The two
/// formatters in Zahlen/Error.hpp still carry the narrower member; nothing
/// formats a range of Errors yet.)
///
template <typename E>
    requires std::is_enum_v<E>
struct formatter<E, char>: formatter<string_view, char> {
    template <typename FormatContext>
    auto format(E val, FormatContext& ctx) const {
        return formatter<string_view, char>::format(ZHLN::Reflect::EnumToMessage(val), ctx);
    }
};

} // namespace std
