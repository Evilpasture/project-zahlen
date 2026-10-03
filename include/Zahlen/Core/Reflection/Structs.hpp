// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


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

// One synthesized aggregate per (T, Transform): T's data members in declaration
// order, each replaced by Transform<member type>, each keeping its name.
//
// The transformation is the caller's -- `std::add_pointer_t` for a struct of
// stream pointers, `std::add_lvalue_reference_t` for the proxy one element is
// read through -- so a third shape is spelled at the use site rather than asked
// for here. What this owns is the part that has to be right either way: the
// names survive verbatim and in order, because they are the only identity
// between the source type and the synthesized one. Everything built on it
// (`TransformedStruct`, `MapConstruct`, SoABlock) matches the two up by name.
template <typename T, template <typename> class Transform>
struct StructTransformGenerator {
    struct type;

    consteval {
        std::vector<std::meta::info> specs;
        for (auto member: NonStaticDataMembers<T>()) {
            std::vector<std::meta::info> arguments {std::meta::type_of(member)};

            std::meta::data_member_options options;
            options.name = std::meta::has_identifier(member) ? std::meta::identifier_of(member) : std::string_view {};

            specs.push_back(std::meta::data_member_spec(std::meta::substitute(^^Transform, arguments), options));
        }
        std::meta::define_aggregate(std::meta::dealias(^^type), specs);
    }
};

}

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

// T with every data member transformed. The two consumers in the tree are the
// stream struct a structure-of-arrays block binds its arrays into
// (`std::add_pointer_t`) and the proxy reference one element is accessed
// through (`std::add_lvalue_reference_t`); see TemplatedDetail::
// StructTransformGenerator for what "transformed" preserves.
template <typename T, template <typename> class Transform>
using TransformedStruct = typename TemplatedDetail::StructTransformGenerator<std::remove_cvref_t<T>, Transform>::type;

// Memberwise construction of one transformed struct from another: `fn` sees each
// member of `src` in declaration order and its results become the members of
// `Dst`. This is the read half of an SoA access -- one reference per stream --
// and it exists because a struct whose members are references cannot be built by
// naming fields the caller does not have.
template <typename Dst, typename Src, typename Func>
constexpr auto MapConstruct(Src&& src, Func&& fn) -> Dst {
    return [&]<auto... members>(TemplatedDetail::ReplicatorType<members...>) -> Dst {
        return Dst {fn(std::forward<Src>(src).[:members:])...};
    }([:Expand(TemplatedDetail::NonStaticDataMembers<Src>()):]);
}

// Whether T can be split into parallel streams: an aggregate (so every data
// member is visible to the reflection query rather than filtered by access), and
// every data member carrying an identifier (the name is what binds a stream to
// its member) and being an object rather than a reference (a reference has no
// pointer form to store and no addressable stream to point at).
//
// A predicate rather than a generator-side refusal, so a caller that wants the
// layout can static_assert it and name the type that cannot have one.
template <typename T>
consteval auto IsStreamableStruct() -> bool {
    if constexpr (!std::is_aggregate_v<std::remove_cvref_t<T>>) {
        return false;
    } else {
        // The member walk has to go through the expansion statement rather than
        // a range-for: the loop variable of a range-for is not a constant
        // expression, so splicing the member it names is ill-formed -- the
        // member has to arrive as a template argument.
        bool streamable = true;
        [:Expand(TemplatedDetail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto {
            if (!std::meta::has_identifier(member)) {
                streamable = false;
            }
            if (std::is_reference_v<typename[:std::meta::type_of(member):]>) {
                streamable = false;
            }
        };
        return streamable;
    }
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

template <std::meta::info MemberInfo>
consteval auto MemberName() -> std::string_view {
    if constexpr (std::meta::has_identifier(MemberInfo)) {
        return std::meta::identifier_of(MemberInfo);
    }
    return {};
}

template <std::meta::info MemberInfo>
using MemberType = typename[:std::meta::type_of(MemberInfo):];

template <std::meta::info MemberInfo, typename T>
constexpr decltype(auto) MemberValue(T&& object) {
    return (std::forward<T>(object).[:MemberInfo:]);
}

#else

template <typename T, typename F>
constexpr void ForEachField(T&& , F&& ) {
}

template <typename T, typename F>
constexpr void ForEachFieldWithName(T&& , F&& ) {
}

template <typename T, typename F>
constexpr void ForEachDataMember(F&& ) {
}

template <typename T, typename F>
constexpr void ForEachFieldInfo(F&& ) {
}

template <typename T>
constexpr auto TieFields(T&& ) {
    return std::tuple {};
}

template <typename T>
constexpr auto ZipFieldsWithNames(T&& ) {
    return std::tuple {};
}

template <typename T>
constexpr std::size_t FieldCount() {
    return 0;
}

template <std::size_t N, typename T>
constexpr decltype(auto) GetField(T&& ) {
    struct Dummy {};
    static Dummy d;
    return d;
}

template <typename T, typename F>
constexpr bool VisitFieldByName(T&& , std::string_view , F&& ) {
    return false;
}

template <typename T>
consteval auto FieldNames() {
    return std::array<std::string_view, 0> {};
}

template <typename T>
consteval bool HasField(std::string_view ) {
    return false;
}

template <typename T, typename F>
constexpr void ForEachFieldIndexed(T&& , F&& ) {
}

template <typename Tag, typename T>
consteval bool HasTag(std::string_view ) {
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
constexpr decltype(auto) MemberValue(T&& ) {
    struct Dummy {};
    static Dummy d;
    return d;
}

template <StringLiteral NameConst, typename T>
constexpr decltype(auto) GetFieldByName(T&& ) {
    struct Dummy {};
    static Dummy d;
    return d;
}

template <StringLiteral NameConst, typename T>
consteval std::size_t IndexOfField() {
    return static_cast<std::size_t>(-1);
}

template <StringLiteral NameConst, typename T, typename ValueType>
constexpr bool SetFieldByName(T& , ValueType&& ) {
    return false;
}

template <typename T, typename Tuple>
constexpr T MakeFromTuple(Tuple&& ) {
    return T {};
}

template <typename T, typename F>
constexpr void ForEachFieldAdaptive(T&& , F&& ) {
}

template <typename Tag, typename T>
consteval bool ValidateSerializability() {
    return true;
}

template <typename Meta, typename T, typename F>
constexpr void ForEachReflectedField(T&& , F&& ) {
}

template <typename T, typename F>
constexpr void ForEachFieldAccessor(F&& ) {
}

// The transform family degrades to a type that cannot be used: there is no
// member list to transform without P2996, and `void` is what a caller binding it
// to a stream member finds out with. SoABlock is the only consumer in the tree
// and refuses to instantiate in this configuration on its own terms.
template <typename T, template <typename> class Transform>
using TransformedStruct = void;

template <typename Dst, typename Src, typename Func>
constexpr auto MapConstruct(Src&& , Func&& ) -> Dst {
    return Dst {};
}

template <typename T>
consteval bool IsStreamableStruct() {
    return false;
}

#endif

}
