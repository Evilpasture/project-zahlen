// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/Reflection/Structs.hpp
//
// Aggregate shape: fields, their names, offsets and types, and every walk over
// them -- ForEachField/ForEachFieldWithName/ForEachFieldInfo, TieFields,
// ZipFieldsWithNames, FieldCount/FieldNames/HasField, Get/SetFieldByName,
// IndexOfField, MakeFromTuple, and MemberName/MemberType/MemberValue, the only
// member handles that leave the reflection headers.
//
// No <ranges> and no <algorithm> here on purpose: the predicates that wanted
// std::ranges::any_of are plain loops, which is what makes this header cheap
// enough for the field-iteration paths that include it and nothing else --
// RenderInternal.hpp, Vertex.hpp, extras/toml/TOML.hpp, the render passes.
//
// Field is the descriptor Dynamic.hpp's Define consumes. It lives here rather
// than in Core.hpp because it is a field, and because its FieldName is a
// ZHLN::StringLiteral -- an annotation-vocabulary type, not a P2996 one.

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN::Reflect {

// A field descriptor: a type and the name it is reflected under. Dynamic.hpp's
// Define<Name, Fields...> consumes these to name a generated aggregate's
// members, and the name is a ZHLN::StringLiteral (Zahlen/Core/Description.hpp)
// because it has to be a non-type template argument.
template <typename T, ZHLN::StringLiteral FieldName>
struct Field {
    using type                             = T;
    static constexpr std::string_view name = FieldName;
};

#if ZHLN_REFLECTION_AVAILABLE

namespace TemplatedDetail {

template <typename T>
consteval auto NonStaticDataMembers() {
    return std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));
}

template <typename Meta>
consteval auto FindMetaMemberNamed(std::string_view name) -> std::meta::info {
    for (auto m: std::meta::nonstatic_data_members_of(^^Meta, std::meta::access_context::current())) {
        if (std::meta::identifier_of(m) == name) {
            return m;
        }
    }
    return std::meta::info {};
}

template <StringLiteral Name, typename T>
consteval auto FindMember() -> std::meta::info {
    constexpr std::string_view target_name = Name;
    for (auto m: NonStaticDataMembers<T>()) {
        if (std::meta::identifier_of(m) == target_name) {
            return m;
        }
    }
    return std::meta::info {};
}

template <StringLiteral Name, typename T>
consteval auto IndexOfField() -> std::size_t {
    constexpr auto             members     = NonStaticDataMembers<T>();
    constexpr std::string_view target_name = Name;
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (std::meta::identifier_of(members[i]) == target_name) {
            return i;
        }
    }
    return static_cast<std::size_t>(-1);
}

} // namespace TemplatedDetail

template <typename T, typename F>
constexpr void ForEachField(T&& t, F&& f) {
    [:Expand(TemplatedDetail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto { f(std::forward<T>(t).[:member:]); };
}

template <typename T, typename F>
constexpr void ForEachFieldWithName(T&& t, F&& f) {
    [:Expand(TemplatedDetail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto {
        constexpr std::string_view name = std::meta::has_identifier(member) ? std::meta::identifier_of(member) : std::string_view("");
        f(name, std::forward<T>(t).[:member:]);
    };
}

template <typename T, typename F>
constexpr void ForEachDataMember(F&& f) {
    [:Expand(TemplatedDetail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto { f.template operator()<member>(); };
}

template <typename T, typename F>
constexpr void ForEachFieldInfo(F&& f) {
    [:Expand(TemplatedDetail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto {
        constexpr std::string_view name   = std::meta::identifier_of(member);
        constexpr std::size_t      offset = std::meta::offset_of(member).bytes;
        using FieldType                   = typename[:std::meta::type_of(member):];

        f.template operator()<FieldType>(name, offset);
    };
}

template <typename T>
constexpr auto TieFields(T&& t) {
    return [&]<auto... members>(TemplatedDetail::ReplicatorType<members...>) -> auto {
        return std::tie(std::forward<T>(t).[:members:]...);
    }([:Expand(TemplatedDetail::NonStaticDataMembers<T>()):]);
}

template <typename T>
constexpr auto ZipFieldsWithNames(T&& t) {
    return [&]<auto... members>(TemplatedDetail::ReplicatorType<members...>) -> auto {
        return std::make_tuple(
            std::pair<std::string_view, decltype(std::forward<T>(t).[:members:])> {
                std::meta::has_identifier(members) ? std::meta::identifier_of(members) : "", std::forward<T>(t).[:members:]
            }...
        );
    }([:Expand(TemplatedDetail::NonStaticDataMembers<T>()):]);
}

template <typename T>
consteval auto FieldCount() -> std::size_t {
    return std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()).size();
}

template <std::size_t N, typename T>
constexpr auto GetField(T&& t) -> decltype(auto) {
    return (std::forward<T>(t).[:TemplatedDetail::NonStaticDataMembers<T>()[N]:]);
}

template <typename T, typename F>
constexpr auto VisitFieldByName(T&& t, std::string_view name, F&& f) -> bool {
    bool found = false;
    [:Expand(TemplatedDetail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto {
        if (!found && std::meta::identifier_of(member) == name) {
            f(std::forward<T>(t).[:member:]);
            found = true;
        }
    };
    return found;
}

template <typename T>
consteval auto FieldNames() {
    constexpr auto members = TemplatedDetail::NonStaticDataMembers<T>();
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> auto {
        return std::array<std::string_view, sizeof...(Is)> {std::meta::identifier_of(members[Is])...};
    }(std::make_index_sequence<members.size()>());
}

template <typename T>
consteval auto HasField(std::string_view name) -> bool {
    // A plain loop rather than std::ranges::any_of: this header carries no
    // <ranges> or <algorithm>, and the walk is over a define_static_array.
    for (auto member: TemplatedDetail::NonStaticDataMembers<T>()) {
        if (std::meta::identifier_of(member) == name) {
            return true;
        }
    }
    return false;
}

template <typename T, typename F>
constexpr void ForEachFieldIndexed(T&& t, F&& f) {
    std::size_t idx = 0;
    [:Expand(TemplatedDetail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto { f(idx++, std::forward<T>(t).[:member:]); };
}

template <typename Tag, typename T>
consteval auto HasTag(std::string_view field_name) -> bool {
    using U = std::remove_cvref_t<T>;
    if constexpr (requires { typename U::ReflectMetadata; }) {
        using Meta [[maybe_unused]] = typename U::ReflectMetadata;
        // Plain loop, not std::ranges::any_of: no <ranges> in this header.
        for (auto member: TemplatedDetail::NonStaticDataMembers<Meta>()) {
            if (std::meta::identifier_of(member) == field_name && std::meta::type_of(member) == ^^Tag) {
                return true;
            }
        }
    }
    return false;
}

template <std::size_t N, typename T>
using FieldType = typename[:std::meta::type_of(TemplatedDetail::NonStaticDataMembers<T>()[N]):];

template <StringLiteral NameConst, typename T>
constexpr auto GetFieldByName(T&& t) -> decltype(auto) {
    constexpr auto found_member = TemplatedDetail::FindMember<NameConst, T>();
    static_assert(found_member != std::meta::info {}, "Field not found in type.");
    return (std::forward<T>(t).[:found_member:]);
}

template <StringLiteral NameConst, typename T>
consteval auto IndexOfField() -> std::size_t {
    return TemplatedDetail::IndexOfField<NameConst, T>();
}

template <StringLiteral NameConst, typename T, typename ValueType>
constexpr auto SetFieldByName(T& t, ValueType&& new_value) -> bool {
    constexpr auto found_member = TemplatedDetail::FindMember<NameConst, T>();
    if constexpr (found_member != std::meta::info {}) {
        if constexpr (std::is_assignable_v<decltype(t.[:found_member:])&, ValueType>) {
            t.[:found_member:] = std::forward<ValueType>(new_value);
            return true;
        }
    }
    return false;
}

template <typename T, typename Tuple>
constexpr auto MakeFromTuple(Tuple&& t) -> T {
    static_assert(std::is_aggregate_v<T>, "Type must be an aggregate.");
    return [&]<auto... members>(TemplatedDetail::ReplicatorType<members...>) -> auto {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> auto {
            return T {std::get<Is>(std::forward<Tuple>(t))...};
        }(std::make_index_sequence<sizeof...(members)>());
    }([:Expand(TemplatedDetail::NonStaticDataMembers<T>()):]);
}

template <typename T, typename F>
constexpr void ForEachFieldAdaptive(T&& t, F&& f) {
    ForEachField(std::forward<T>(t), std::forward<F>(f));
}

template <typename Tag, typename T>
consteval auto ValidateSerializability() -> bool {
    bool ok = true;
    [:Expand(TemplatedDetail::NonStaticDataMembers<T>()):] >> [&]<auto member>() {
        if constexpr (std::meta::type_of(member) == ^^Tag) {
            using FieldT = typename[:std::meta::type_of(member):];
            if constexpr (!std::is_trivially_copyable_v<FieldT>) {
                ok = false;
            }
        }
    };
    return ok;
}

template <typename Meta, typename T, typename F>
constexpr void ForEachReflectedField(T&& t, F&& f) {
    [:Expand(TemplatedDetail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto {
        constexpr std::string_view name  = std::meta::has_identifier(member) ? std::meta::identifier_of(member) : std::string_view("");
        constexpr auto             found = TemplatedDetail::FindMetaMemberNamed<Meta>(name);
        if constexpr (found != std::meta::info {}) {
            using Tag = typename[:std::meta::type_of(found):];
            std::forward<F>(f).template operator()<Tag>(std::forward<T>(t).[:member:]);
        }
    };
}

template <typename T, typename F>
constexpr void ForEachFieldAccessor(F&& f) {
    using U = std::remove_cvref_t<T>;
    [:Expand(TemplatedDetail::NonStaticDataMembers<U>()):] >> [&]<auto member>() -> auto {
        constexpr std::string_view name = std::meta::identifier_of(member);
        using FieldType                 = typename[:std::meta::type_of(member):];

        auto const_getter = [](const U& inst) -> const FieldType& { return inst.[:member:]; };
        auto mut_getter   = [](U& inst) -> FieldType& { return inst.[:member:]; };
        auto setter       = [](U& inst, const FieldType& val) -> void {
            if constexpr (std::is_array_v<FieldType>) {
                for (std::size_t i = 0; i < std::extent_v<FieldType>; ++i) {
                    inst.[:member:][i] = val[i];
                }
            } else if constexpr (std::is_copy_assignable_v<FieldType>) {
                inst.[:member:] = val;
            } else if constexpr (std::is_move_assignable_v<FieldType>) {
                inst.[:member:] = std::move(const_cast<FieldType&>(val));
            }
        };

        f.template operator()<FieldType>(name, const_getter, mut_getter, setter);
    };
}

// Spelling of a reflected data member (a handle as handed to
// ForEachDataMember); empty when the compiler reports no identifier.
template <std::meta::info MemberInfo>
consteval auto MemberName() -> std::string_view {
    if constexpr (std::meta::has_identifier(MemberInfo)) {
        return std::meta::identifier_of(MemberInfo);
    }
    return {};
}

// Declared type of a reflected data member.
template <std::meta::info MemberInfo>
using MemberType = typename[:std::meta::type_of(MemberInfo):];

// Reference to a reflected data member of an object. MemberInfo must be one
// of the handles ForEachDataMember passes to its callback.
template <std::meta::info MemberInfo, typename T>
constexpr decltype(auto) MemberValue(T&& object) {
    return (std::forward<T>(object).[:MemberInfo:]);
}

#else // No C++26 static reflection: this module's degraded stand-ins.

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
constexpr void ForEachFieldInfo(F&& /*unused*/) {
}

template <typename T>
constexpr auto TieFields(T&& /*unused*/) {
    return std::tuple {};
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

template <typename T, typename F>
constexpr void ForEachFieldIndexed(T&& /*unused*/, F&& /*unused*/) {
}

template <typename Tag, typename T>
consteval bool HasTag(std::string_view /*unused*/) {
    return false;
}

template <std::size_t N, typename T>
using FieldType = void;

template <auto MemberInfo>
consteval std::string_view MemberName() {
    return {};
}

template <auto MemberInfo>
using MemberType = void;

template <auto MemberInfo, typename T>
constexpr decltype(auto) MemberValue(T&& /*object*/) {
    struct Dummy {};
    static Dummy d;
    return d;
}

template <StringLiteral NameConst, typename T>
constexpr decltype(auto) GetFieldByName(T&& /*unused*/) {
    struct Dummy {};
    static Dummy d;
    return d;
}

template <StringLiteral NameConst, typename T>
consteval std::size_t IndexOfField() {
    return static_cast<std::size_t>(-1);
}

template <StringLiteral NameConst, typename T, typename ValueType>
constexpr bool SetFieldByName(T& /*unused*/, ValueType&& /*unused*/) {
    return false;
}

template <typename T, typename Tuple>
constexpr T MakeFromTuple(Tuple&& /*unused*/) {
    return T {};
}

template <typename T, typename F>
constexpr void ForEachFieldAdaptive(T&& /*unused*/, F&& /*unused*/) {
}

template <typename Tag, typename T>
consteval bool ValidateSerializability() {
    return true;
}

template <typename Meta, typename T, typename F>
constexpr void ForEachReflectedField(T&& /*unused*/, F&& /*unused*/) {
}

template <typename T, typename F>
constexpr void ForEachFieldAccessor(F&& /*unused*/) {
}

#endif

} // namespace ZHLN::Reflect
