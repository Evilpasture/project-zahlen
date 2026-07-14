// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace ZHLN::Reflect {

// ============================================================================
// 1. COMPLETELY INDEPENDENT UTILITIES (Defined Once)
// ============================================================================

// Literal class type to allow passing string literals as NTTPs
template <std::size_t N>
struct StringLiteral {
    std::array<char, N> value {};
    constexpr StringLiteral(const char (&str)[N]) {
        std::copy_n(str, N, value);
    }

    constexpr operator std::string_view() const {
        return {value, N - 1};
    }
};

// Represents a single field declaration in a schema.
template <typename T, StringLiteral FieldName>
struct Field {
    using type                             = T;
    static constexpr std::string_view name = FieldName;
};

template <typename T>
constexpr bool IsBracesConstructible() {
    return std::is_aggregate_v<std::remove_cvref_t<T>>;
}

} // namespace ZHLN::Reflect

// ============================================================================
// 2. REFLECTION-DEPENDENT CORE (Split by Guard)
// ============================================================================

#if defined(__cpp_impl_reflection)
#include "Loop.hpp"
#include <meta>

namespace ZHLN::Reflect {

namespace detail {
template <auto... vals>
struct ReplicatorType {
    template <typename F>
    constexpr void operator>>(F body) const {
        (body.template operator()<vals>(), ...);
    }
};

template <auto... vals>
ReplicatorType<vals...> Replicator {};

template <typename T>
struct TypeReflector {
    static consteval std::string_view name() {
        return std::meta::identifier_of(std::meta::dealias(^^T));
    }
};

template <StringLiteral Name, typename T>
consteval std::meta::info FindMember() {
    static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));
    constexpr std::string_view target_name = Name;
    for (auto m: members) {
        if (std::meta::identifier_of(m) == target_name) {
            return m;
        }
    }
    return std::meta::info {};
}

template <StringLiteral Name, typename T>
consteval std::size_t IndexOfField() {
    static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));
    constexpr std::string_view target_name = Name;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (std::meta::identifier_of(members[i]) == target_name) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

template <typename T, typename F, std::size_t Start, std::size_t Total>
constexpr void ChunkedFieldVisitor(T&& t, F&& f) {
    if constexpr (Start < Total) {
        constexpr std::size_t ChunkSize = (sizeof(T) > 32) ? 4 : 8;
        constexpr std::size_t Step      = (Start + ChunkSize > Total) ? (Total - Start) : ChunkSize;

        static constexpr auto members =
            std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

        ZHLN::Unroll<Step>([&](auto I) {
            constexpr std::size_t Index = Start + decltype(I)::value;
            f(t.[:members[Index]:]);
        });

        ChunkedFieldVisitor<T, F, Start + Step, Total>(t, f);
    }
}
} // namespace detail

template <std::ranges::range R>
consteval auto Expand(R&& range) {
    std::vector<std::meta::info> args;
    for (auto r: range) {
        args.push_back(std::meta::reflect_constant(r));
    }
    return std::meta::substitute(^^detail::Replicator, args);
}

template <typename T, typename F>
constexpr void ForEachField(T&& t, F&& f) {
    static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

    ZHLN::Unroll<members.size()>([&](auto I) { f(t.[:members[decltype(I)::value]:]); });
}

template <typename T, typename F>
constexpr void ForEachFieldWithName(T&& t, F&& f) {
    static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

    ZHLN::Unroll<members.size()>([&](auto I) {
        constexpr auto             member = members[decltype(I)::value];
        constexpr std::string_view name   = []() consteval {
            if (std::meta::has_identifier(member)) {
                return std::meta::identifier_of(member);
            }
            return std::string_view("");
        }();
        f(name, t.[:member:]);
    });
}

template <typename T>
constexpr auto TieFields(T&& t) {
    static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

    return [&]<std::size_t... Is>(std::index_sequence<Is...>) { return std::tie(t.[:members[Is]:]...); }(std::make_index_sequence<members.size()>());
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumToString(E value) {
    static constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^E));
    std::string_view      result      = "Unknown";

    ZHLN::Unroll<enumerators.size()>([&](auto ic) {
        constexpr auto             enumerator = enumerators[decltype(ic)::value];
        constexpr std::string_view name       = std::meta::identifier_of(enumerator);
        if (value == static_cast<E>([:enumerator:])) {
            result = name;
        }
    });
    return result;
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::optional<E> StringToEnum(std::string_view name) {
    std::optional<E> result = std::nullopt;

    [:Expand(std::define_static_array(std::meta::enumerators_of(^^E))):] >> [&]<auto enumerator> {
        if (name == std::meta::identifier_of(enumerator)) {
            result = static_cast<E>([:enumerator:]);
        }
    };
    return result;
}

template <typename T>
constexpr auto ZipFieldsWithNames(T&& t) {
    static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

    return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        return std::make_tuple(
            std::pair<std::string_view, decltype(std::forward<T>(t).[:members[Is]:])> {
                std::meta::identifier_of(members[Is]), std::forward<T>(t).[:members[Is]:]
            }...
        );
    }(std::make_index_sequence<members.size()>());
}

template <typename T>
constexpr std::size_t FieldCount() {
    return std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()).size();
}

template <std::size_t N, typename T>
constexpr decltype(auto) GetField(T&& t) {
    static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

    static_assert(N < members.size(), "Index out of bounds for field access.");
    return std::forward<T>(t).[:members[N]:];
}

template <typename T, typename F>
constexpr bool VisitFieldByName(T&& t, std::string_view name, F&& f) {
    bool found = false;
    [:Expand(
          std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()))
      ):] >> [&]<auto member> {
        if (!found && std::meta::identifier_of(member) == name) {
            f(std::forward<T>(t).[:member:]);
            found = true;
        }
    };
    return found;
}

template <typename T>
consteval auto FieldNames() {
    constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        return std::array<std::string_view, sizeof...(Is)> {std::meta::identifier_of(members[Is])...};
    }(std::make_index_sequence<members.size()>());
}

template <typename T>
consteval bool HasField(std::string_view name) {
    for (auto m: std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()))
        if (std::meta::identifier_of(m) == name)
            return true;
    return false;
}

template <typename E>
    requires std::is_enum_v<E>
consteval std::size_t EnumCount() {
    return std::meta::enumerators_of(^^E).size();
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumNames() {
    constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^E));
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        return std::array<std::string_view, sizeof...(Is)> {std::meta::identifier_of(enumerators[Is])...};
    }(std::make_index_sequence<enumerators.size()>());
}

template <typename T, typename F>
constexpr void ForEachFieldIndexed(T&& t, F&& f) {
    static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));
    [:Expand(std::views::iota(std::size_t {0}, members.size())):] >> [&]<std::size_t I> { f(I, t.[:members[I]:]); };
}

template <typename Tag, typename T>
consteval bool HasTag(std::string_view field_name) {
    using U = std::remove_cvref_t<T>;
    if constexpr (requires { typename U::ReflectMetadata; }) {
        using Meta                  = typename U::ReflectMetadata;
        constexpr auto meta_members = std::define_static_array(std::meta::nonstatic_data_members_of(^^Meta, std::meta::access_context::current()));

        for (auto m: meta_members) {
            if (std::meta::identifier_of(m) == field_name) {
                return std::meta::type_of(m) == ^^Tag;
            }
        }
    }
    return false;
}

template <std::size_t N, typename T>
using FieldType = typename[:[] {
    static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));
    static_assert(N < members.size(), "Index out of bounds.");
    return std::meta::type_of(members[N]);
}():];

template <typename T>
consteval auto BaseClasses() {
    return std::meta::bases_of(^^std::remove_cvref_t<T>, std::meta::access_context::current());
}

template <StringLiteral NameConst, typename T>
constexpr decltype(auto) GetFieldByName(T&& t) {
    constexpr auto found_member = detail::FindMember<NameConst, T>();
    static_assert(found_member != std::meta::info {}, "Field not found in type.");
    return std::forward<T>(t).[:found_member:];
}

template <typename T>
consteval std::string_view TypeName() {
    return detail::TypeReflector<std::remove_cvref_t<T>>::name();
}

template <typename T, typename F>
constexpr void ForEachBase(F&& f) {
    static constexpr auto bases = std::define_static_array(std::meta::bases_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

    [:Expand(bases):] >> [&]<auto base> { f.template operator()<typename[:std::meta::type_of(base):]>(); };
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumToFlagsString(E value, std::string& out_buffer) {
    out_buffer.clear();
    using Under    = std::underlying_type_t<E>;
    auto val_under = static_cast<Under>(value);

    [:Expand(std::define_static_array(std::meta::enumerators_of(^^E))):] >> [&]<auto enumerator> {
        constexpr E                enum_val   = static_cast<E>([:enumerator:]);
        auto                       enum_under = static_cast<Under>(enum_val);
        constexpr std::string_view name       = std::meta::identifier_of(enumerator);

        if (enum_under != 0 && (val_under & enum_under) == enum_under) {
            if (!out_buffer.empty())
                out_buffer += " | ";
            out_buffer += name;
        }
    };

    if (out_buffer.empty() && val_under == 0) {
        return EnumToString(value);
    }
    return out_buffer;
}

template <StringLiteral NameConst, typename T>
consteval std::size_t IndexOfField() {
    return detail::IndexOfField<NameConst, T>();
}

template <typename T>
consteval std::size_t MemberFunctionCount() {
    static constexpr auto all_members = std::define_static_array(std::meta::members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

    std::size_t count = 0;
    for (auto m: all_members) {
        if (std::meta::is_function(m) && std::meta::has_identifier(m)) {
            ++count;
        }
    }
    return count;
}

template <typename T>
consteval auto MemberFunctionNames() {
    static constexpr auto all_members = std::define_static_array(std::meta::members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));
    constexpr std::size_t count       = MemberFunctionCount<T>();

    return []<std::size_t... Is>(std::index_sequence<Is...>) {
        std::array<std::string_view, count> names {};
        [[maybe_unused]] std::size_t        idx = 0;
        (
            [&] {
                constexpr auto member = all_members[Is];
                if constexpr (std::meta::is_function(member) && std::meta::has_identifier(member)) {
                    names[idx++] = std::meta::identifier_of(member);
                }
            }(),
            ...);
        return names;
    }(std::make_index_sequence<all_members.size()>());
}

template <StringLiteral NameConst, typename T, typename ValueType>
constexpr bool SetFieldByName(T& t, ValueType&& new_value) {
    constexpr auto found_member = detail::FindMember<NameConst, T>();
    if constexpr (found_member != std::meta::info {}) {
        if constexpr (std::is_assignable_v<decltype(t.[:found_member:])&, ValueType>) {
            t.[:found_member:] = std::forward<ValueType>(new_value);
            return true;
        }
    }
    return false;
}

template <typename T, typename Tuple>
constexpr T MakeFromTuple(Tuple&& t) {
    static_assert(std::is_aggregate_v<T>, "Type must be an aggregate.");
    constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));

    return
        [&]<std::size_t... Is>(std::index_sequence<Is...>) { return T {std::get<Is>(std::forward<Tuple>(t))...}; }(std::make_index_sequence<members.size()>());
}

template <typename E>
    requires std::is_enum_v<E>
consteval std::string_view EnumUnderlyingTypeName() {
    return std::meta::display_string_of(std::meta::underlying_type(^^E));
}

template <typename T, typename F>
constexpr void ForEachFieldAdaptive(T&& t, F&& f) {
    constexpr std::size_t Count = FieldCount<T>();
    detail::ChunkedFieldVisitor<T, F, 0, Count>(std::forward<T>(t), std::forward<F>(f));
}

template <typename Tag, typename T>
consteval bool ValidateSerializability() {
    static constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));

    bool ok = true;
    ZHLN::Unroll<members.size()>([&](auto I) {
        constexpr auto             member = members[decltype(I)::value];
        constexpr std::string_view name   = std::meta::identifier_of(member);

        if constexpr (HasTag<Tag, T>(name)) {
            using FieldT = typename[:std::meta::type_of(member):];
            if constexpr (!std::is_trivially_copyable_v<FieldT>) {
                ok = false;
            }
        }
    });
    return ok;
}

template <typename T, typename F>
constexpr void ForEachNestedType(F&& f) {
    static constexpr auto members = std::define_static_array(std::meta::members_of(std::meta::dealias(^^T), std::meta::access_context::current()));

    [&]<size_t... Is>(std::index_sequence<Is...>) {
        (
            [&]() {
                constexpr auto member = members[Is];
                if constexpr (std::meta::is_type(member)) {
                    using NestedType = typename[:member:];
                    if constexpr (std::is_class_v<NestedType>) {
                        f.template operator()<NestedType>();
                    }
                }
            }(),
            ...);
    }(std::make_index_sequence<members.size()> {});
}

template <StringLiteral Name, typename... Fields>
struct Define {
    struct type;

    friend constexpr std::string_view GetSchemaName(type*) {
        return Name;
    }

    consteval {
        constexpr size_t             NumFields = sizeof...(Fields);
        std::vector<std::meta::info> specs;
        specs.reserve(NumFields);

        auto build_field = [&]<typename F>() {
            std::meta::data_member_options opts;
            opts.name = std::string(F::name);
            specs.push_back(std::meta::data_member_spec(^^typename F::type, opts));
        };

        (build_field.template operator()<Fields>(), ...);

        std::meta::define_aggregate(std::meta::dealias(^^type), specs);
    }
};

template <typename Meta, typename T, typename F>
constexpr void ForEachReflectedField(T&& t, F&& f) {
    static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));
    [[maybe_unused]] static constexpr auto metaMembers =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^Meta, std::meta::access_context::current()));

    ZHLN::Unroll<members.size()>([&](auto ic) {
        constexpr size_t           I     = decltype(ic)::value;
        constexpr std::string_view name  = std::meta::identifier_of(members[I]);
        constexpr auto             found = [&]() consteval -> std::meta::info {
            for (auto m: metaMembers) {
                if (std::meta::identifier_of(m) == name)
                    return m;
            }
            return std::meta::info {};
        }();
        if constexpr (found != std::meta::info {}) {
            using Tag = typename[:std::meta::type_of(found):];
            f.template operator()<Tag>(t.[:members[I]:]);
        }
    });
}

template <typename E, typename F>
    requires std::is_enum_v<E>
constexpr void ForEachEnumerator(F&& f) {
    static constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^E));
    ZHLN::Unroll<enumerators.size()>([&](auto ic) {
        constexpr auto enumerator = enumerators[decltype(ic)::value];
        constexpr E    Val        = static_cast<E>([:enumerator:]);
        f.template     operator()<Val>();
    });
}

} // namespace ZHLN::Reflect

#else // Standard C++26 Fallback (No <meta> support)

#include <tuple>
#include <utility>

// ============================================================================
// Fallback Opt-in Macros (For properties impossible to deduce automatically)
// ============================================================================
namespace ZHLN::Reflect::detail {
template <typename T>
struct StructReflectionTraits {
    static constexpr bool registered = false;
};
template <typename T>
struct BaseClassTraits {
    static constexpr bool registered = false;
};
template <typename T, StringLiteral Name>
struct TagTraits {
    using Tag                     = void;
    static constexpr bool has_tag = false;
};
} // namespace ZHLN::Reflect::detail

// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define ZHLN_REFLECT_NAMES(Type, ...)                                                                     \
    template <>                                                                                           \
    struct ZHLN::Reflect::detail::StructReflectionTraits<Type> {                                          \
        static constexpr bool                                                 registered = true;          \
        static constexpr std::array<std::string_view, sizeof...(__VA_ARGS__)> names      = {__VA_ARGS__}; \
    }

#define ZHLN_REFLECT_BASES(Type, ...)                               \
    template <>                                                     \
    struct ZHLN::Reflect::detail::BaseClassTraits<Type> {           \
        static constexpr bool registered = true;                    \
        using BasesTuple                 = std::tuple<__VA_ARGS__>; \
    }

#define ZHLN_REFLECT_TAG(Type, FieldName, TagType)             \
    template <>                                                \
    struct ZHLN::Reflect::detail::TagTraits<Type, FieldName> { \
        using Tag                     = TagType;               \
        static constexpr bool has_tag = true;                  \
    }
// NOLINTEND(cppcoreguidelines-macro-usage)

namespace ZHLN::Reflect {

// ============================================================================
// 1. Type & Enum Names (Magic Enum Lite via __PRETTY_FUNCTION__)
// ============================================================================
namespace detail {
template <typename T>
consteval std::string_view GetTypeName() {
    std::string_view func = __PRETTY_FUNCTION__;
#if defined(__clang__)
    size_t start = func.find("T = ") + 4;
    size_t end   = func.find_last_of(']');
#elif defined(__GNUC__)
    size_t start = func.find("with T = ") + 9;
    size_t end   = func.find_last_of(';');
#elif defined(_MSC_VER)
    size_t start = func.find("GetTypeName<") + 12;
    size_t end   = func.find_last_of('>');
#else
    return "UnsupportedCompiler";
#endif
    std::string_view name = func.substr(start, end - start);
    if (name.starts_with("struct ")) {
        name.remove_prefix(7);
    }
    if (name.starts_with("class ")) {
        name.remove_prefix(6);
    }
    if (name.starts_with("enum ")) {
        name.remove_prefix(5);
    }
    return name;
}

template <typename E, E V>
consteval std::string_view ProbeEnumName() {
    std::string_view name = __PRETTY_FUNCTION__;
#if defined(__clang__) || defined(__GNUC__)
    size_t start = name.find("V = ") + 4;
    size_t end   = name.find_last_of(']');
#elif defined(_MSC_VER)
    size_t start = name.find("ProbeEnumName<") + 14;
    size_t end   = name.find_last_of('>');
#endif
    std::string_view sub = name.substr(start, end - start);
    if (sub.contains(')') || sub.find_first_of("0123456789") == 0) {
        return "";
    }

    size_t colon = sub.find_last_of(':');
    if (colon != std::string_view::npos) {
        sub.remove_prefix(colon + 1);
    }
    return sub;
}

template <typename E, int V>
struct EnumProber {
    static consteval std::string_view name() {
        return ProbeEnumName<E, static_cast<E>(V)>();
    }
};

template <typename E, int Min, int Max>
struct EnumExtractor {
    static constexpr int Range = Max - Min + 1;

    static consteval int Count() {
        int c = 0;
        [&]<int... Is>(std::integer_sequence<int, Is...>) {
            (((EnumProber<E, Is + Min>::name().empty()) ? 0 : ++c), ...);
        }(std::make_integer_sequence<int, Range> {});
        return c;
    }

    static constexpr int ValidCount = Count();

    struct Result {
        std::array<E, ValidCount>                v {};
        std::array<std::string_view, ValidCount> n {};
    };

    static consteval Result Extract() {
        Result res {};
        int    idx = 0;
        [&]<int... Is>(std::integer_sequence<int, Is...>) {
            (((EnumProber<E, Is + Min>::name().empty()) ? 0 : (res.v[idx] = static_cast<E>(Is + Min), res.n[idx] = EnumProber<E, Is + Min>::name(), ++idx)),
             ...);
        }(std::make_integer_sequence<int, Range> {});
        return res;
    }
};
} // namespace detail

template <typename T>
consteval std::string_view TypeName() {
    return detail::GetTypeName<std::remove_cvref_t<T>>();
}

template <typename E>
    requires std::is_enum_v<E>
consteval std::string_view EnumUnderlyingTypeName() {
    return TypeName<std::underlying_type_t<E>>();
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumToString(E value) {
    constexpr auto data = detail::EnumExtractor<E, -128, 128>::Extract();
    for (std::size_t i = 0; i < data.v.size(); ++i) {
        if (data.v[i] == value) {
            return data.n[i];
        }
    }
    return "Unknown";
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::optional<E> StringToEnum(std::string_view name) {
    constexpr auto data = detail::EnumExtractor<E, -128, 128>::Extract();
    for (std::size_t i = 0; i < data.v.size(); ++i) {
        if (data.n[i] == name) {
            return data.v[i];
        }
    }
    return std::nullopt;
}

template <typename E>
    requires std::is_enum_v<E>
consteval std::size_t EnumCount() {
    return detail::EnumExtractor<E, -128, 128>::ValidCount;
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumNames() {
    return detail::EnumExtractor<E, -128, 128>::Extract().n;
}

template <typename E, typename F>
    requires std::is_enum_v<E>
constexpr void ForEachEnumerator(F&& f) {
    constexpr auto data = detail::EnumExtractor<E, -128, 128>::Extract();
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (std::forward<F>(f).template operator()<data.v[Is]>(), ...);
    }(std::make_index_sequence<data.v.size()> {});
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumToFlagsString(E value, std::string& out_buffer) {
    out_buffer.clear();
    using Under              = std::underlying_type_t<E>;
    auto           val_under = static_cast<Under>(value);
    constexpr auto data      = detail::EnumExtractor<E, -128, 128>::Extract();

    for (std::size_t i = 0; i < data.v.size(); ++i) {
        auto enum_under = static_cast<Under>(data.v[i]);
        if (enum_under != 0 && (val_under & enum_under) == enum_under) {
            if (!out_buffer.empty()) {
                out_buffer += " | ";
            }
            out_buffer += data.n[i];
        }
    }
    if (out_buffer.empty() && val_under == 0) {
        return EnumToString(value);
    }
    return out_buffer;
}

// ============================================================================
// 2. Aggregate Introspection (PFR Lite)
// ============================================================================
namespace detail {
struct AnyType {
    template <typename T>
    constexpr operator T() const;
};

template <typename T, typename... Args>
consteval auto test_aggregate(int) -> decltype(T {std::declval<Args>()...}, true) {
    return true;
}
template <typename T, typename... Args>
consteval bool test_aggregate(...) {
    return false;
}

template <typename T, std::size_t N>
consteval bool is_aggregate_constructible() {
    return []<std::size_t... I>(std::index_sequence<I...>) { return test_aggregate<T, decltype(I, AnyType {})...>(0); }(std::make_index_sequence<N> {});
}

template <typename T, std::size_t N = 16>
consteval std::size_t count_fields() {
    if constexpr (N == 0) {
        return 0;
    } else if constexpr (is_aggregate_constructible<T, N>()) {
        return N;
    } else {
        return count_fields<T, N - 1>();
    }
}
} // namespace detail

template <typename T>
constexpr std::size_t FieldCount() {
    return detail::count_fields<std::remove_cvref_t<T>>();
}

template <typename T>
constexpr auto TieFields(T&& t) {
    using M                 = std::remove_cvref_t<T>;
    constexpr std::size_t N = FieldCount<M>();

    if constexpr (N == 0) {
        return std::tuple {};
    } else if constexpr (N == 1) {
        auto& [a] = t;
        return std::tie(a);
    } else if constexpr (N == 2) {
        auto& [a, b] = t;
        return std::tie(a, b);
    } else if constexpr (N == 3) {
        auto& [a, b, c] = t;
        return std::tie(a, b, c);
    } else if constexpr (N == 4) {
        auto& [a, b, c, d] = t;
        return std::tie(a, b, c, d);
    } else if constexpr (N == 5) {
        auto& [a, b, c, d, e] = t;
        return std::tie(a, b, c, d, e);
    } else if constexpr (N == 6) {
        auto& [a, b, c, d, e, f] = t;
        return std::tie(a, b, c, d, e, f);
    } else if constexpr (N == 7) {
        auto& [a, b, c, d, e, f, g] = t;
        return std::tie(a, b, c, d, e, f, g);
    } else if constexpr (N == 8) {
        auto& [a, b, c, d, e, f, g, h] = t;
        return std::tie(a, b, c, d, e, f, g, h);
    } else if constexpr (N == 9) {
        auto& [a, b, c, d, e, f, g, h, i] = t;
        return std::tie(a, b, c, d, e, f, g, h, i);
    } else if constexpr (N == 10) {
        auto& [a, b, c, d, e, f, g, h, i, j] = t;
        return std::tie(a, b, c, d, e, f, g, h, i, j);
    } else if constexpr (N == 11) {
        auto& [a, b, c, d, e, f, g, h, i, j, k] = t;
        return std::tie(a, b, c, d, e, f, g, h, i, j, k);
    } else if constexpr (N == 12) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l] = t;
        return std::tie(a, b, c, d, e, f, g, h, i, j, k, l);
    } else if constexpr (N == 13) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m] = t;
        return std::tie(a, b, c, d, e, f, g, h, i, j, k, l, m);
    } else if constexpr (N == 14) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n] = t;
        return std::tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n);
    } else if constexpr (N == 15) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o] = t;
        return std::tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o);
    } else if constexpr (N == 16) {
        auto& [a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p] = t;
        return std::tie(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p);
    } else {
        static_assert(N <= 16, "TieFields fallback only supports up to 16 fields");
        return std::tuple {};
    }
}

template <typename T, typename F>
constexpr void ForEachField(T&& t, F&& f) {
    std::apply([&](auto&&... args) { (std::forward<F>(f)(std::forward<decltype(args)>(args)), ...); }, TieFields(std::forward<T>(t)));
}

template <typename T, typename F>
constexpr void ForEachFieldIndexed(T&& t, F&& f) {
    auto tup = TieFields(std::forward<T>(t));
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (std::forward<F>(f)(Is, std::get<Is>(tup)), ...);
    }(std::make_index_sequence<std::tuple_size_v<decltype(tup)>> {});
}

template <typename T, typename F>
constexpr void ForEachFieldAdaptive(T&& t, F&& f) {
    ForEachField(std::forward<T>(t), std::forward<F>(f));
}

template <std::size_t N, typename T>
using FieldType = std::tuple_element_t<N, decltype(TieFields(std::declval<T&>()))>;

template <std::size_t N, typename T>
constexpr decltype(auto) GetField(T&& t) {
    return std::get<N>(TieFields(std::forward<T>(t)));
}

template <typename T, typename Tuple>
constexpr T MakeFromTuple(Tuple&& t) {
    return std::make_from_tuple<T>(std::forward<Tuple>(t));
}

// ============================================================================
// 3. Name-Based & Macro Features
// ============================================================================

template <typename T>
consteval auto FieldNames() {
    constexpr std::size_t N = FieldCount<T>();
    if constexpr (detail::StructReflectionTraits<T>::registered) {
        static_assert(detail::StructReflectionTraits<T>::names.size() == N, "ZHLN_REFLECT_NAMES count mismatch.");
        return detail::StructReflectionTraits<T>::names;
    } else {
        return std::array<std::string_view, N> {};
    }
}

template <typename T, typename F>
constexpr void ForEachFieldWithName(T&& t, F&& f) {
    constexpr auto names = FieldNames<std::remove_cvref_t<T>>();
    auto           tup   = TieFields(std::forward<T>(t));
    [&]<std::size_t... Is>(std::index_sequence<Is...>) { (std::forward<F>(f)(names[Is], std::get<Is>(tup)), ...); }(std::make_index_sequence<names.size()> {});
}

template <typename T>
consteval bool HasField(std::string_view name) {
    constexpr auto names = FieldNames<T>();
    for (auto n: names) {
        if (n == name)
            return true;
    }
    return false;
}

template <StringLiteral NameConst, typename T>
consteval std::size_t IndexOfField() {
    constexpr auto names = FieldNames<T>();
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == std::string_view(NameConst.value.data(), NameConst.value.size() - 1)) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

template <typename T, typename F>
constexpr bool VisitFieldByName(T&& t, std::string_view name, F&& f) {
    bool found = false;
    ForEachFieldWithName(std::forward<T>(t), [&](std::string_view n, auto&& val) {
        if (!found && n == name) {
            std::forward<F>(f)(std::forward<decltype(val)>(val));
            found = true;
        }
    });
    return found;
}

template <StringLiteral NameConst, typename T>
constexpr decltype(auto) GetFieldByName(T&& t) {
    constexpr std::size_t idx = IndexOfField<NameConst, std::remove_cvref_t<T>>();
    static_assert(idx != static_cast<std::size_t>(-1), "Field not found.");
    return std::get<idx>(TieFields(std::forward<T>(t)));
}

template <StringLiteral NameConst, typename T, typename ValueType>
constexpr bool SetFieldByName(T& t, ValueType&& value) {
    constexpr std::size_t idx = IndexOfField<NameConst, std::remove_cvref_t<T>>();
    if constexpr (idx != static_cast<std::size_t>(-1)) {
        auto tup = TieFields(t);
        if constexpr (std::is_assignable_v<decltype(std::get<idx>(tup))&, ValueType>) {
            std::get<idx>(tup) = std::forward<ValueType>(value);
            return true;
        }
    }
    return false;
}

template <typename T>
constexpr auto ZipFieldsWithNames(T&& t) {
    constexpr auto names = FieldNames<std::remove_cvref_t<T>>();
    auto           tup   = TieFields(std::forward<T>(t));
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        return std::make_tuple(std::pair<std::string_view, decltype(std::get<Is>(tup))> {names[Is], std::get<Is>(tup)}...);
    }(std::make_index_sequence<names.size()> {});
}

// ============================================================================
// 4. Custom Tags and Sub-Types
// ============================================================================

template <typename Tag, typename T>
consteval bool HasTag([[maybe_unused]] std::string_view field_name) {
    // Unsupported automatically without compiler reflection
    return false;
}

template <typename Tag, typename T>
consteval bool ValidateSerializability() {
    return std::is_trivially_copyable_v<T>;
}

template <typename Meta, typename T, typename F>
constexpr void ForEachReflectedField(T&& t, F&& f) {
    // This is 100% implementable! It's used by the RenderGraph to auto-bind resources.
    if constexpr (detail::StructReflectionTraits<std::remove_cvref_t<T>>::registered && detail::StructReflectionTraits<std::remove_cvref_t<Meta>>::registered) {
        constexpr auto t_names    = FieldNames<std::remove_cvref_t<T>>();
        constexpr auto meta_names = FieldNames<std::remove_cvref_t<Meta>>();

        auto t_tup = TieFields(std::forward<T>(t));

        // We instantiate a dummy Meta just so we can run TieFields to extract its types
        std::remove_cvref_t<Meta> dummy_meta {};
        auto                      meta_tup = TieFields(dummy_meta);

        // Iterate over every field in Meta
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
                [&]() {
                    constexpr std::string_view meta_name = meta_names[Is];
                    using TagType                        = std::remove_cvref_t<std::tuple_element_t<Is, decltype(meta_tup)>>;

                    // Find the matching field name in T
                    [&]<std::size_t... Js>(std::index_sequence<Js...>) {
                        (
                            [&]() {
                                if constexpr (t_names[Js] == meta_name) {
                                    // Found a match! Invoke the lambda with the TagType and the actual data from T
                                    std::forward<F>(f).template operator()<TagType>(std::get<Js>(t_tup));
                                }
                            }(),
                            ...);
                    }(std::make_index_sequence<t_names.size()> {});
                }(),
                ...);
        }(std::make_index_sequence<meta_names.size()> {});
    }
}

template <typename T, typename F>
constexpr void ForEachNestedType(F&& /*f*/) {
    // Requires standard C++26 <meta> reflection. Safe to omit in fallback.
}

template <std::ranges::range R>
consteval int Expand(R&& /*unused*/) {
    return 0;
} // <meta> placeholder stub

// ============================================================================
// 5. Classes and Methods
// ============================================================================

template <typename T>
consteval auto BaseClasses() {
    return std::array<int, 0> {};
}

template <typename T, typename F>
constexpr void ForEachBase(F&& f) {
    if constexpr (detail::BaseClassTraits<std::remove_cvref_t<T>>::registered) {
        using Bases = typename detail::BaseClassTraits<std::remove_cvref_t<T>>::BasesTuple;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (std::forward<F>(f).template operator()<std::tuple_element_t<Is, Bases>>(), ...);
        }(std::make_index_sequence<std::tuple_size_v<Bases>> {});
    }
}

template <typename T>
consteval std::size_t MemberFunctionCount() {
    return 0;
}

template <typename T>
consteval auto MemberFunctionNames() {
    return std::array<std::string_view, 0> {};
}

template <StringLiteral Name, typename... Fields>
struct Define {
    struct type {};
    friend constexpr std::string_view GetSchemaName(type* /*unused*/) {
        return Name;
    }
};

} // namespace ZHLN::Reflect
#endif
// ============================================================================
// 3. DERIVED UTILITIES & DIAGNOSTICS (Defined Once)
// ============================================================================

namespace ZHLN::Reflect {

template <typename T>
constexpr std::string_view GetSchemaNameOf() noexcept {
    return GetSchemaName(static_cast<T*>(nullptr));
}

template <typename T>
constexpr bool GenericEqual(const T& lhs, const T& rhs) {
    return TieFields(lhs) == TieFields(rhs);
}

template <typename T>
constexpr auto GenericCompare(const T& lhs, const T& rhs) {
    return TieFields(lhs) <=> TieFields(rhs);
}

template <typename T>
constexpr bool GenericLess(const T& lhs, const T& rhs) {
    return TieFields(lhs) < TieFields(rhs);
}

template <typename T>
constexpr std::size_t GenericHash(const T& t) {
    std::size_t seed = 0;
    ForEachField(t, [&](auto&& field) { seed ^= std::hash<std::remove_cvref_t<decltype(field)>> {}(field) + 0x9e3779b9 + (seed << 6) + (seed >> 2); });
    return seed;
}

template <typename Dst, typename Src>
constexpr void CopyMatchingFields(Dst& dst, const Src& src) {
    ForEachFieldWithName(src, [&](std::string_view name, auto&& value) {
        VisitFieldByName(dst, name, [&](auto&& dstField) {
            if constexpr (std::is_assignable_v<decltype(dstField)&, decltype(value)>) {
                dstField = value;
            }
        });
    });
}

template <typename T>
constexpr bool HasBases() {
    return !BaseClasses<T>().empty();
}

template <typename From, typename To>
consteval std::size_t MapFieldIndex(std::size_t fromIdx) {
    constexpr auto fromNames = FieldNames<From>();
    constexpr auto toNames   = FieldNames<To>();
    for (std::size_t i = 0; i < toNames.size(); ++i) {
        if (toNames[i] == fromNames[fromIdx]) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

template <typename E, typename F>
    requires std::is_enum_v<E>
constexpr void DispatchEnum(E value, F&& f) {
    ForEachEnumerator<E>([&]<E Val>() {
        if (value == Val) {
            std::forward<F>(f).template operator()<Val>();
        }
    });
}

namespace detail {
template <typename T>
concept Formattable = requires(const T& val, std::format_context ctx) { std::formatter<std::remove_cvref_t<T>, char>().format(val, ctx); };
} // namespace detail

template <typename T>
std::string ToDebugString(const T& t);

template <typename T, typename = void>
struct CustomFormatter {
    static void format(const T& val, std::string& out) {
        using Decayed = std::remove_cvref_t<T>;

        if constexpr (detail::Formattable<Decayed>) {
            out += std::format("{}", val);
        } else if constexpr (std::is_enum_v<Decayed>) {
            out += EnumToString(val);
        } else if constexpr (std::ranges::input_range<Decayed>) { // added support for vectors/arrays
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
                ForEachFieldWithName(val, [&](std::string_view name, auto&& value) {
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
std::string ToDebugString(const T& t) {
    std::string out;
    CustomFormatter<std::remove_cvref_t<T>>::format(t, out);
    return out;
}

} // namespace ZHLN::Reflect
