// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "Print.hpp"
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

template <std::size_t N>
struct StringLiteral {
    std::array<char, N> value {};
    constexpr StringLiteral(const char (&str)[N]) {
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = str[i];
        }
    }

    constexpr operator std::string_view() const {
        return {value.data(), N - 1};
    }
};

template <typename T, StringLiteral FieldName>
struct Field {
    using type                             = T;
    static constexpr std::string_view name = FieldName;
};

template <typename T>
constexpr bool IsBracesConstructible() {
    return std::is_aggregate_v<std::remove_cvref_t<T>>;
}

struct MetaDescription {
    const char* text;
};

} // namespace ZHLN::Reflect

// ============================================================================
// 2. REFLECTION-DEPENDENT CORE (Split by Guard)
// ============================================================================

#if defined(__cpp_impl_reflection) || (defined(__has_feature) && __has_feature(reflection))
#include "Loop.hpp"
#include <meta>

namespace ZHLN::Reflect {

consteval auto Description(std::string_view text) -> MetaDescription {
    return MetaDescription {std::define_static_string(text)};
}

namespace detail {
template <auto... vals>
struct ReplicatorType {
    template <typename F>
    constexpr void operator>>([[maybe_unused]] F body) const {
        (body.template operator()<vals>(), ...);
    }
};

template <auto... vals>
ReplicatorType<vals...> Replicator {};

template <typename T>
struct TypeReflector {
    static consteval std::string_view name() {
        constexpr auto info = std::meta::dealias(^^T);
        if constexpr (std::meta::has_identifier(info)) {
            return std::meta::identifier_of(info);
        } else {
            return "TemplateSpecialization";
        }
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

        Unroll<Step>([&](auto I) {
            constexpr std::size_t Index = Start + decltype(I)::value;
            f(t.[:members[Index]:]);
        });

        ChunkedFieldVisitor<T, F, Start + Step, Total>(std::forward<T>(t), std::forward<F>(f));
    }
}

template <std::size_t N, typename T>
consteval std::meta::info GetFieldTypeInfo() {
    constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));
    static_assert(N < members.size(), "Index out of bounds.");
    return std::meta::type_of(members[N]);
}

template <typename T>
constexpr auto ToTuple(T&& obj) {
    if constexpr (requires { std::tuple_size<std::decay_t<T>>::value; }) {
        return std::forward<T>(obj);
    } else {
        return std::make_tuple(std::forward<T>(obj));
    }
}

template <size_t ID>
struct AnonymousNode {
    struct type;
};

template <typename T, size_t N>
struct FixedArrayBinding {
    using type = std::array<T, N>;
};

template <typename T>
struct MethodCollector {
    static constexpr auto all_members = std::define_static_array(std::meta::members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

    static consteval std::size_t get_count() {
        std::size_t c = 0;
        for (auto m: all_members) {
            if (std::meta::is_function(m) && std::meta::has_identifier(m)) {
                ++c;
            }
        }
        return c;
    }

    static constexpr std::size_t count = get_count();

    static consteval auto get_methods() {
        std::array<std::meta::info, count> methods {};
        std::size_t                        idx = 0;
        for (auto m: all_members) {
            if (std::meta::is_function(m) && std::meta::has_identifier(m)) {
                methods[idx++] = m;
            }
        }
        return methods;
    }

    static constexpr auto method_handles = get_methods();
};

} // namespace detail

class TypeDescriptor {
  public:
    consteval TypeDescriptor() noexcept = default;

    template <typename T>
    static consteval TypeDescriptor Of() noexcept {
        return TypeDescriptor(^^std::remove_cvref_t<T>);
    }

    static consteval TypeDescriptor String() noexcept {
        return Of<std::string_view>();
    }

    static consteval TypeDescriptor Int64() noexcept {
        return Of<int64_t>();
    }

    static consteval TypeDescriptor Float64() noexcept {
        return Of<double>();
    }

    static consteval TypeDescriptor Boolean() noexcept {
        return Of<bool>();
    }

    static consteval TypeDescriptor Null() noexcept {
        return Of<std::nullptr_t>();
    }

    static consteval TypeDescriptor Void() noexcept {
        return Of<void>();
    }

    static consteval TypeDescriptor ArrayOf(TypeDescriptor elemType, size_t count) noexcept {
        std::vector<std::meta::info> template_args = {elemType.m_handle, std::meta::reflect_constant(count)};
        return TypeDescriptor(std::meta::type_of(std::meta::substitute(^^detail::FixedArrayBinding, template_args)));
    }

    [[nodiscard]] consteval auto Handle() const noexcept {
        return m_handle;
    }

  private:
    explicit consteval TypeDescriptor(std::meta::info handle) noexcept: m_handle(handle) {
    }
    std::meta::info m_handle = {};

    template <typename TargetStruct>
    friend class AggregateBuilder;
};

/// High-level builder for synthesizing C++ aggregate structures at compile time
template <typename TargetStruct>
class AggregateBuilder {
  public:
    consteval AggregateBuilder() noexcept: m_targetInfo(std::meta::dealias(^^TargetStruct)) {
    }

    /// Adds a typed field to the structure using C++ types
    template <typename T>
    consteval AggregateBuilder& AddField(std::string_view name) noexcept {
        return AddField(name, TypeDescriptor::Of<T>());
    }

    /// Adds a field using a TypeDescriptor
    consteval AggregateBuilder& AddField(std::string_view name, TypeDescriptor typeDesc) noexcept {
        std::meta::data_member_options opts;
        opts.name = std::string(name);
        m_specs.push_back(std::meta::data_member_spec(typeDesc.Handle(), opts));
        return *this;
    }

    /// Adds a fixed-size array field
    template <typename T>
    consteval AggregateBuilder& AddArrayField(std::string_view name, size_t count) noexcept {
        return AddField(name, TypeDescriptor::ArrayOf(TypeDescriptor::Of<T>(), count));
    }

    /// Synthesizes a nested struct and adds it as a member
    template <size_t NodeID, typename ConfigFn>
    consteval TypeDescriptor AddNestedObject(std::string_view name, ConfigFn&& configFn) noexcept {
        using NestedType = typename detail::AnonymousNode<NodeID>::type;
        AggregateBuilder<NestedType> nestedBuilder;
        configFn(nestedBuilder);
        TypeDescriptor nestedDesc = nestedBuilder.Build();

        AddField(name, nestedDesc);
        return nestedDesc;
    }

    /// Injects all registered member specifications into the target aggregate AST
    consteval TypeDescriptor Build() noexcept {
        std::meta::define_aggregate(m_targetInfo, m_specs);
        return TypeDescriptor(m_targetInfo);
    }

  private:
    std::meta::info              m_targetInfo;
    std::vector<std::meta::info> m_specs;
};

template <std::ranges::range R>
consteval auto Expand(R&& range) {
    std::vector<std::meta::info> args;
    args.reserve(range.size());
    for (auto r: range) {
        args.push_back(std::meta::reflect_constant(r));
    }
    return std::meta::substitute(^^detail::Replicator, args);
}

template <typename T, typename F>
constexpr void ForEachField(T&& t, F&& f) {
    static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

    Unroll<members.size()>([&](auto I) { f(t.[:members[decltype(I)::value]:]); });
}

template <typename T, typename F>
constexpr void ForEachFieldWithName(T&& t, F&& f) {
    static constexpr auto members =
        std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

    Unroll<members.size()>([&](auto I) {
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

// ----------------------------------------------------------------------------
// Pure Generic Introspection Primitives
// ----------------------------------------------------------------------------

/**
 * @brief Iterates over non-static data members at compile time, passing the raw std::meta::info handle to F.
 */
template <typename T, typename F>
constexpr void ForEachDataMember(F&& f) {
    constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

    // Wrap Expand in splice brackets to instantiate the ReplicatorType
    [:Expand(members):] >> [&]<auto member>() { f.template operator()<member>(); };
}

/**
 * @brief Iterates over member functions at compile time, passing the raw std::meta::info handle to F.
 */
template <typename T, typename F>
constexpr void ForEachMemberFunction(F&& f) {
    constexpr auto members = std::define_static_array(std::meta::members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

    // Wrap Expand in splice brackets to instantiate the ReplicatorType
    [:Expand(members):] >> [&]<auto member>() {
        if constexpr (std::meta::is_function(member) && std::meta::has_identifier(member)) {
            f.template operator()<member>();
        }
    };
}

template <typename T, typename F>
constexpr void ForEachFieldInfo(F&& f) {
    constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

    [:Expand(members):] >> [&]<auto member>() {
        constexpr std::string_view name   = std::meta::identifier_of(member);
        constexpr std::size_t      offset = std::meta::offset_of(member).bytes;
        using FieldType                   = typename[:std::meta::type_of(member):];

        f.template operator()<FieldType>(name, offset);
    };
}

// ----------------------------------------------------------------------------

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

    Unroll<enumerators.size()>([&](auto ic) {
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

    static constexpr auto names = []<std::size_t... Is>(std::index_sequence<Is...>) {
        return std::array<std::string_view, sizeof...(Is)> {std::meta::identifier_of(members[Is])...};
    }(std::make_index_sequence<members.size()>());

    return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        return std::make_tuple(std::pair<std::string_view, decltype(std::forward<T>(t).[:members[Is]:])> {names[Is], std::forward<T>(t).[:members[Is]:]}...);
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
    constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
    return std::ranges::any_of(members, [name](auto m) { return std::meta::identifier_of(m) == name; });
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
        using Meta [[maybe_unused]] = typename U::ReflectMetadata;
        constexpr auto meta_members = std::define_static_array(std::meta::nonstatic_data_members_of(^^Meta, std::meta::access_context::current()));

        return std::ranges::any_of(meta_members, [field_name](auto m) { return std::meta::identifier_of(m) == field_name && std::meta::type_of(m) == ^^Tag; });
    }
    return false;
}

template <std::size_t N, typename T>
using FieldType = typename[:detail::GetFieldTypeInfo<N, T>():];

template <typename T>
consteval auto BaseClasses() {
    return std::meta::bases_of(^^std::remove_cvref_t<T>, std::meta::access_context::current());
}

template <typename T>
consteval bool HasVirtualBases() {
    using U [[maybe_unused]] = std::remove_cvref_t<T>;
    constexpr auto bases     = std::define_static_array(std::meta::bases_of(^^U, std::meta::access_context::current()));
    return std::ranges::any_of(bases, [](auto b) { return std::meta::is_virtual(b); });
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
    Unroll<members.size()>([&](auto I) {
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

    friend constexpr std::string_view GetSchemaName(type* /*unused*/) {
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

    Unroll<members.size()>([&](auto ic) {
        constexpr size_t           I     = decltype(ic)::value;
        constexpr std::string_view name  = std::meta::identifier_of(members[I]);
        constexpr auto             found = [&]() consteval -> std::meta::info {
            for (auto m: metaMembers) {
                if (std::meta::identifier_of(m) == name) {
                    return m;
                }
            }
            return std::meta::info {};
        }();
        if constexpr (found != std::meta::info {}) {
            using Tag = typename[:std::meta::type_of(found):];
            std::forward<F>(f).template operator()<Tag>(std::forward<T>(t).[:members[I]:]);
        }
    });
}

template <typename E, typename F>
    requires std::is_enum_v<E>
constexpr void ForEachEnumerator(F&& f) {
    static constexpr auto enumerators = std::define_static_array(std::meta::enumerators_of(^^E));
    Unroll<enumerators.size()>([&](auto ic) {
        constexpr auto enumerator = enumerators[decltype(ic)::value];
        constexpr E    Val        = static_cast<E>([:enumerator:]);
        f.template     operator()<Val>();
    });
}

// ----------------------------------------------------------------------------
// Pure Introspection Primitives (Encapsulating std::meta and Splicing)
// ----------------------------------------------------------------------------

/**
 * @brief Iterates over data members, passing (Name, GetterLambda, SetterLambda) with pure C++ types.
 */
template <typename T, typename F>
constexpr void ForEachFieldAccessor(F&& f) {
    using U                = std::remove_cvref_t<T>;
    constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^U, std::meta::access_context::current()));

    [:ZHLN::Reflect::Expand(members):] >> [&]<auto member>() {
        constexpr std::string_view name = std::meta::identifier_of(member);
        using FieldType                 = typename[:std::meta::type_of(member):];

        auto const_getter = [](const U& inst) -> const FieldType& { return inst.[:member:]; };
        auto mut_getter   = [](U& inst) -> FieldType& { return inst.[:member:]; };
        auto setter       = [](U& inst, const FieldType& val) { inst.[:member:] = val; };

        f.template operator()<FieldType>(name, const_getter, mut_getter, setter);
    };
}

/**
 * @brief Iterates over member functions, passing (Name, PointerToMemberFunction).
 */
template <typename T, typename F>
constexpr void ForEachMethodPointer(F&& f) {
    constexpr auto members = std::define_static_array(std::meta::members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));

    [:Reflect::Expand(members):] >> [&]<auto member>() {
        if constexpr (std::meta::is_function(member) && std::meta::has_identifier(member)) {
            constexpr std::string_view name = std::meta::identifier_of(member);
            constexpr auto             pmf  = &[:member:];
            f(name, pmf);
        }
    };
}

/**
 * @brief Scans a namespace/scope for types tagged with 'Tag', invoking F.template operator()<TargetType>().
 */
template <auto ScopeInfo, typename Tag, typename F>
constexpr void ForEachAnnotatedTypeInScope(F&& f) {
    constexpr auto members = std::define_static_array(std::meta::members_of(ScopeInfo, std::meta::access_context::current()));

    // Wrap Expand in splice brackets to instantiate the ReplicatorType
    [:Expand(members):] >> [&]<auto m>() {
        if constexpr (std::meta::is_type(m)) {
            if constexpr (std::ranges::any_of(std::meta::annotations_of(m), [](auto a) { return std::meta::type_of(a) == ^^Tag; })) {
                using TargetType = typename[:m:];
                f.template operator()<TargetType>();
            }
        }
    };
}

template <typename Tag, auto EntityInfo>
consteval std::optional<Tag> GetAnnotation() {
    for (auto a: std::meta::annotations_of(EntityInfo)) {
        if (std::meta::type_of(a) == ^^Tag) {
            return std::meta::extract<Tag>(a);
        }
    }
    return std::nullopt;
}

template <typename Tag, typename E>
    requires std::is_enum_v<E>
constexpr std::optional<Tag> GetEnumeratorAnnotation(E value) {
    std::optional<Tag> result = std::nullopt;

    [:Expand(std::define_static_array(std::meta::enumerators_of(^^E))):] >> [&]<auto enumerator> {
        constexpr auto annotation = GetAnnotation<Tag, enumerator>();
        if (value == static_cast<E>([:enumerator:])) {
            result = annotation;
        }
    };

    return result;
}

template <typename T>
consteval std::size_t GetFloatFieldsCount() {
    using U [[maybe_unused]] = std::remove_cvref_t<T>;
    constexpr auto members   = std::define_static_array(std::meta::nonstatic_data_members_of(^^U, std::meta::access_context::current()));
    if (members.empty()) {
        return 0;
    }
    bool all_float = std::ranges::all_of(members, [](auto m) { return std::meta::type_of(m) == ^^float; });
    return all_float ? members.size() : 0;
}

/**
 * @brief Reflects on all member methods of an instance in source declaration order,
 * executes them with no arguments, and returns their concatenated results in a std::tuple.
 */
template <typename T>
constexpr auto CollectMethodResults(const T& inst) {
    using Collector = detail::MethodCollector<std::remove_cvref_t<T>>;
    return [&]<size_t... Is>(std::index_sequence<Is...>) {
        return std::tuple_cat(detail::ToTuple((inst.[:Collector::method_handles[Is]:]()))...);
    }(std::make_index_sequence<Collector::count> {});
}

} // namespace ZHLN::Reflect

#else // Standard C++26 Fallback (Stubs - Waiting for compiler reflection)

namespace ZHLN::Reflect {

constexpr auto Description(std::string_view text) -> MetaDescription {
    // Fallback for non-reflecting environments / standard compilation
    return MetaDescription {text.data()};
}

template <std::ranges::range R>
consteval int Expand(R&& /*unused*/) {
    return 0;
}

template <typename T, typename F>
constexpr void ForEachField(T&& /*unused*/, F&& /*unused*/) {
}

template <typename T, typename F>
constexpr void ForEachFieldWithName(T&& /*unused*/, F&& /*unused*/) {
}

template <typename T, typename F>
constexpr void ForEachDataMember(F&& /*unused*/) {
}

template <typename T, typename F>
constexpr void ForEachMemberFunction(F&& /*unused*/) {
}

template <typename T, typename F>
constexpr void ForEachFieldInfo(F&& /*unused*/) {
}

template <typename T>
constexpr auto TieFields(T&& /*unused*/) {
    return std::tuple {};
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumToString(E /*unused*/) {
    return "Unknown";
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::optional<E> StringToEnum(std::string_view /*unused*/) {
    return std::nullopt;
}

template <typename T>
constexpr auto ZipFieldsWithNames(T&& /*unused*/) {
    return std::tuple {};
}

template <typename T>
constexpr std::size_t FieldCount() {
    return 0;
}

template <std::size_t N, typename T>
constexpr decltype(auto) GetField(T&& /*unused*/) {
    struct Dummy {};
    static Dummy d;
    return d;
}

template <typename T, typename F>
constexpr bool VisitFieldByName(T&& /*unused*/, std::string_view /*unused*/, F&& /*unused*/) {
    return false;
}

template <typename T>
consteval auto FieldNames() {
    return std::array<std::string_view, 0> {};
}

template <typename T>
consteval bool HasField(std::string_view /*unused*/) {
    return false;
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

template <typename T, typename F>
constexpr void ForEachFieldIndexed(T&& /*unused*/, F&& /*unused*/) {
}

template <typename Tag, typename T>
consteval bool HasTag(std::string_view /*unused*/) {
    return false;
}

template <std::size_t N, typename T>
using FieldType = void;

template <typename T>
consteval auto BaseClasses() {
    return std::array<int, 0> {};
}

template <typename T>
consteval bool HasVirtualBases() {
    return false;
}

template <StringLiteral NameConst, typename T>
constexpr decltype(auto) GetFieldByName(T&& /*unused*/) {
    struct Dummy {};
    static Dummy d;
    return d;
}

template <typename T>
consteval std::string_view TypeName() {
    return "";
}

template <typename T, typename F>
constexpr void ForEachBase(F&& /*unused*/) {
}

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumToFlagsString(E /*unused*/, std::string& out_buffer) {
    out_buffer.clear();
    return "";
}

template <StringLiteral NameConst, typename T>
consteval std::size_t IndexOfField() {
    return static_cast<std::size_t>(-1);
}

template <typename T>
consteval std::size_t MemberFunctionCount() {
    return 0;
}

template <typename T>
consteval auto MemberFunctionNames() {
    return std::array<std::string_view, 0> {};
}

template <StringLiteral NameConst, typename T, typename ValueType>
constexpr bool SetFieldByName(T& /*unused*/, ValueType&& /*unused*/) {
    return false;
}

template <typename T, typename Tuple>
constexpr T MakeFromTuple(Tuple&& /*unused*/) {
    return T {};
}

template <typename E>
    requires std::is_enum_v<E>
consteval std::string_view EnumUnderlyingTypeName() {
    return "";
}

template <typename T, typename F>
constexpr void ForEachFieldAdaptive(T&& /*unused*/, F&& /*unused*/) {
}

template <typename Tag, typename T>
consteval bool ValidateSerializability() {
    return true;
}

template <typename T, typename F>
constexpr void ForEachNestedType(F&& /*unused*/) {
}

template <StringLiteral Name, typename... Fields>
struct Define {
    struct type {};
    friend constexpr std::string_view GetSchemaName(type* /*unused*/) {
        return Name;
    }
};

template <typename Meta, typename T, typename F>
constexpr void ForEachReflectedField(T&& /*unused*/, F&& /*unused*/) {
}

template <typename E, typename F>
    requires std::is_enum_v<E>
constexpr void ForEachEnumerator(F&& /*unused*/) {
}

template <typename T, typename F>
constexpr void ForEachFieldAccessor(F&& /*unused*/) {
}

template <typename T, typename F>
constexpr void ForEachMethodPointer(F&& /*unused*/) {
}

template <auto ScopeInfo, typename Tag, typename F>
constexpr void ForEachAnnotatedTypeInScope(F&& /*unused*/) {
}

template <typename Tag, typename T>
consteval std::optional<Tag> GetAnnotation() {
    return std::nullopt;
}

template <typename Tag, typename E>
    requires std::is_enum_v<E>
constexpr std::optional<Tag> GetEnumeratorAnnotation(E /*unused*/) {
    return std::nullopt;
}

template <typename T>
consteval std::size_t GetFloatFieldsCount() {
    return 0;
}

template <typename T>
constexpr auto CollectMethodResults(const T& /*inst*/) {
    return std::tuple {};
}

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

template <typename E>
    requires std::is_enum_v<E>
constexpr std::string_view EnumToMessage(E value) {
    if (auto desc = GetEnumeratorAnnotation<MetaDescription>(value)) {
        return desc->text;
    }
    return EnumToString(value);
}

/**
 * @brief Formats an enum's reflected description template with the provided arguments
 * using the engine's zero-allocation signal-safe formatter.
 *
 * Example:
 *   auto msg = ZHLN::Reflect::FormatEnumMessage(HandleError::GenerationMismatch, 2, 1);
 *   ZHLN::Println("{}", msg.string_view());
 */
template <typename E, typename... Args>
    requires std::is_enum_v<E>
inline auto FormatEnumMessage(E value, Args&&... args) {
    std::string_view fmt = EnumToMessage(value);
    return ZHLN::Format(fmt, std::forward<Args>(args)...);
}

/**
 * @brief Formats an enum's reflected description template into a standard std::string.
 */
template <typename E, typename... Args>
    requires std::is_enum_v<E>
inline std::string FormatEnumMessageString(E value, Args&&... args) {
    std::string_view fmt = EnumToMessage(value);
    if constexpr (sizeof...(Args) == 0) {
        return std::string(fmt);
    } else {
        return std::vformat(fmt, std::make_format_args(args...));
    }
}

} // namespace ZHLN::Reflect
