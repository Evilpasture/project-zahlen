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
#include <vector>

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

template <std::size_t N>
StringLiteral(const char (&)[N]) -> StringLiteral<N>;

template <typename T, StringLiteral FieldName>
struct Field {
    using type                             = T;
    static constexpr std::string_view name = FieldName;
};

template <typename T>
constexpr auto IsBracesConstructible() -> bool {
    return std::is_aggregate_v<std::remove_cvref_t<T>>;
}

template <StringLiteral Text>
struct Description {
    static constexpr std::string_view message = Text;
};

} // namespace ZHLN::Reflect

// ============================================================================
// 2. REFLECTION-DEPENDENT CORE (Split by Guard)
// ============================================================================

#if defined(__cpp_impl_reflection) || (defined(__has_feature) && __has_feature(reflection))
#include <meta>

namespace ZHLN::Reflect {

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
consteval auto NonStaticDataMembers() {
    return std::define_static_array(std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));
}

template <typename T>
consteval auto MembersOf() {
    return std::define_static_array(std::meta::members_of(std::meta::dealias(^^std::remove_cvref_t<T>), std::meta::access_context::current()));
}

template <typename E>
consteval auto EnumeratorsOf() {
    return std::define_static_array(std::meta::enumerators_of(^^E));
}

template <typename T>
consteval auto BasesOf() {
    return std::define_static_array(std::meta::bases_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));
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

template <typename T>
struct TypeReflector {
    static consteval auto name() -> std::string_view {
        constexpr auto info = std::meta::dealias(^^T);
        if constexpr (std::meta::has_identifier(info)) {
            return std::meta::identifier_of(info);
        } else {
            return "TemplateSpecialization";
        }
    }
};

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
    static consteval auto get_count() -> std::size_t {
        std::size_t c = 0;
        for (auto m: MembersOf<T>()) {
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
        for (auto m: MembersOf<T>()) {
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
    static consteval auto Of() noexcept -> TypeDescriptor {
        return TypeDescriptor(^^std::remove_cvref_t<T>);
    }

    static consteval auto String() noexcept -> TypeDescriptor {
        return Of<std::string_view>();
    }

    static consteval auto Int64() noexcept -> TypeDescriptor {
        return Of<int64_t>();
    }

    static consteval auto Float64() noexcept -> TypeDescriptor {
        return Of<double>();
    }

    static consteval auto Boolean() noexcept -> TypeDescriptor {
        return Of<bool>();
    }

    static consteval auto Null() noexcept -> TypeDescriptor {
        return Of<std::nullptr_t>();
    }

    static consteval auto Void() noexcept -> TypeDescriptor {
        return Of<void>();
    }

    static consteval auto ArrayOf(TypeDescriptor elemType, size_t count) noexcept -> TypeDescriptor {
        std::vector<std::meta::info> template_args = {elemType.m_handle, std::meta::reflect_constant(count)};
        return TypeDescriptor(std::meta::substitute(^^std::array, template_args));
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

template <typename TargetStruct>
class AggregateBuilder {
  public:
    consteval AggregateBuilder() noexcept: m_targetInfo(std::meta::dealias(^^TargetStruct)) {
    }

    template <typename T>
    consteval auto AddField(std::string_view name) noexcept -> AggregateBuilder& {
        return AddField(name, TypeDescriptor::Of<T>());
    }

    consteval auto AddField(std::string_view name, TypeDescriptor typeDesc) noexcept -> AggregateBuilder& {
        std::meta::data_member_options opts;
        opts.name = name;
        m_specs.push_back(std::meta::data_member_spec(typeDesc.Handle(), opts));
        return *this;
    }

    template <typename T>
    consteval auto AddArrayField(std::string_view name, size_t count) noexcept -> AggregateBuilder& {
        return AddField(name, TypeDescriptor::ArrayOf(TypeDescriptor::Of<T>(), count));
    }

    template <size_t NodeID, typename ConfigFn>
    consteval auto AddNestedObject(std::string_view name, ConfigFn&& configFn) noexcept -> TypeDescriptor {
        using NestedType = typename detail::AnonymousNode<NodeID>::type;
        AggregateBuilder<NestedType> nestedBuilder;
        configFn(nestedBuilder);
        TypeDescriptor nestedDesc = nestedBuilder.Build();

        AddField(name, nestedDesc);
        return nestedDesc;
    }

    consteval auto Build() noexcept -> TypeDescriptor {
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

// ----------------------------------------------------------------------------
// Runtime-Callable Generic Reflection Iterators (Zero Immediate Escalation)
// ----------------------------------------------------------------------------

template <typename T, typename F>
constexpr void ForEachField(T&& t, F&& f) {
    [:Expand(detail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto { f(std::forward<T>(t).[:member:]); };
}

template <typename T, typename F>
constexpr void ForEachFieldWithName(T&& t, F&& f) {
    [:Expand(detail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto {
        constexpr std::string_view name = std::meta::has_identifier(member) ? std::meta::identifier_of(member) : std::string_view("");
        f(name, std::forward<T>(t).[:member:]);
    };
}

template <typename T, typename F>
constexpr void ForEachDataMember(F&& f) {
    [:Expand(detail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto { f.template operator()<member>(); };
}

template <typename T, typename F>
constexpr void ForEachMemberFunction(F&& f) {
    [:Expand(detail::MembersOf<T>()):] >> [&]<auto member>() -> auto {
        if constexpr (std::meta::is_function(member) && std::meta::has_identifier(member)) {
            f.template operator()<member>();
        }
    };
}

template <typename T, typename F>
constexpr void ForEachFieldInfo(F&& f) {
    [:Expand(detail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto {
        constexpr std::string_view name   = std::meta::identifier_of(member);
        constexpr std::size_t      offset = std::meta::offset_of(member).bytes;
        using FieldType                   = typename[:std::meta::type_of(member):];

        f.template operator()<FieldType>(name, offset);
    };
}

template <typename T>
constexpr auto TieFields(T&& t) {
    return [&]<auto... members>(detail::ReplicatorType<members...>) -> auto {
        return std::tie(std::forward<T>(t).[:members:]...);
    }([:Expand(detail::NonStaticDataMembers<T>()):]);
}

template <typename E>
    requires std::is_enum_v<E>
constexpr auto EnumToString(E value) -> std::string_view {
    std::string_view result = "Unknown";
    [:Expand(detail::EnumeratorsOf<E>()):] >> [&]<auto enumerator>() -> auto {
        if (value == static_cast<E>([:enumerator:])) {
            result = std::meta::identifier_of(enumerator);
        }
    };
    return result;
}

template <typename E>
    requires std::is_enum_v<E>
constexpr auto StringToEnum(std::string_view name) -> std::optional<E> {
    std::optional<E> result = std::nullopt;
    [:Expand(detail::EnumeratorsOf<E>()):] >> [&]<auto enumerator>() -> auto {
        if (name == std::meta::identifier_of(enumerator)) {
            result = static_cast<E>([:enumerator:]);
        }
    };
    return result;
}

template <typename E>
    requires std::is_enum_v<E>
constexpr auto EnumHasValue(std::underlying_type_t<E> targetValue) noexcept -> bool {
    bool found = false;
    [:Expand(detail::EnumeratorsOf<E>()):] >> [&]<auto enumerator>() -> auto {
        if (static_cast<std::underlying_type_t<E>>([:enumerator:]) == targetValue) {
            found = true;
        }
    };
    return found;
}

template <typename T>
constexpr auto ZipFieldsWithNames(T&& t) {
    return [&]<auto... members>(detail::ReplicatorType<members...>) -> auto {
        return std::make_tuple(
            std::pair<std::string_view, decltype(std::forward<T>(t).[:members:])> {
                std::meta::has_identifier(members) ? std::meta::identifier_of(members) : "", std::forward<T>(t).[:members:]
            }...
        );
    }([:Expand(detail::NonStaticDataMembers<T>()):]);
}

template <typename T>
consteval auto FieldCount() -> std::size_t {
    return std::meta::nonstatic_data_members_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()).size();
}

template <std::size_t N, typename T>
constexpr auto GetField(T&& t) -> decltype(auto) {
    return (std::forward<T>(t).[:detail::NonStaticDataMembers<T>()[N]:]);
}

template <typename T, typename F>
constexpr auto VisitFieldByName(T&& t, std::string_view name, F&& f) -> bool {
    bool found = false;
    [:Expand(detail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto {
        if (!found && std::meta::identifier_of(member) == name) {
            f(std::forward<T>(t).[:member:]);
            found = true;
        }
    };
    return found;
}

template <typename T>
consteval auto FieldNames() {
    constexpr auto members = detail::NonStaticDataMembers<T>();
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> auto {
        return std::array<std::string_view, sizeof...(Is)> {std::meta::identifier_of(members[Is])...};
    }(std::make_index_sequence<members.size()>());
}

template <typename T>
consteval auto HasField(std::string_view name) -> bool {
    return std::ranges::any_of(detail::NonStaticDataMembers<T>(), [name](auto m) -> auto { return std::meta::identifier_of(m) == name; });
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumCount() -> std::size_t {
    return std::meta::enumerators_of(^^E).size();
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumNames() {
    constexpr auto enumerators = detail::EnumeratorsOf<E>();
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> auto {
        return std::array<std::string_view, sizeof...(Is)> {std::meta::identifier_of(enumerators[Is])...};
    }(std::make_index_sequence<enumerators.size()>());
}

template <typename T, typename F>
constexpr void ForEachFieldIndexed(T&& t, F&& f) {
    std::size_t idx = 0;
    [:Expand(detail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto { f(idx++, std::forward<T>(t).[:member:]); };
}

template <typename Tag, typename T>
consteval auto HasTag(std::string_view field_name) -> bool {
    using U = std::remove_cvref_t<T>;
    if constexpr (requires { typename U::ReflectMetadata; }) {
        using Meta [[maybe_unused]] = typename U::ReflectMetadata;
        return std::ranges::any_of(detail::NonStaticDataMembers<Meta>(), [field_name](auto m) -> auto {
            return std::meta::identifier_of(m) == field_name && std::meta::type_of(m) == ^^Tag;
        });
    }
    return false;
}

template <std::size_t N, typename T>
using FieldType = typename[:std::meta::type_of(detail::NonStaticDataMembers<T>()[N]):];

template <typename T>
consteval auto BaseClasses() {
    return std::meta::bases_of(^^std::remove_cvref_t<T>, std::meta::access_context::current());
}

template <typename T>
consteval auto HasVirtualBases() -> bool {
    using U [[maybe_unused]] = std::remove_cvref_t<T>;
    return std::ranges::any_of(detail::BasesOf<U>(), [](auto b) -> auto { return std::meta::is_virtual(b); });
}

template <StringLiteral NameConst, typename T>
constexpr auto GetFieldByName(T&& t) -> decltype(auto) {
    constexpr auto found_member = detail::FindMember<NameConst, T>();
    static_assert(found_member != std::meta::info {}, "Field not found in type.");
    return (std::forward<T>(t).[:found_member:]);
}

template <typename T>
consteval auto TypeName() -> std::string_view {
    return detail::TypeReflector<std::remove_cvref_t<T>>::name();
}

template <typename T, typename F>
constexpr void ForEachBase(F&& f) {
    [:Expand(detail::BasesOf<T>()):] >> [&]<auto base>() -> auto { f.template operator()<typename[:std::meta::type_of(base):]>(); };
}

template <typename E>
    requires std::is_enum_v<E>
constexpr auto EnumToFlagsString(E value, std::string& out_buffer) -> std::string_view {
    out_buffer.clear();
    using Under    = std::underlying_type_t<E>;
    auto val_under = static_cast<Under>(value);

    [:Expand(detail::EnumeratorsOf<E>()):] >> [&]<auto enumerator>() -> auto {
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
consteval auto IndexOfField() -> std::size_t {
    return detail::IndexOfField<NameConst, T>();
}

template <typename T>
consteval auto MemberFunctionCount() -> std::size_t {
    std::size_t count = 0;
    for (auto m: detail::MembersOf<T>()) {
        if (std::meta::is_function(m) && std::meta::has_identifier(m)) {
            ++count;
        }
    }
    return count;
}

template <typename T>
consteval auto MemberFunctionNames() {
    constexpr std::size_t count = MemberFunctionCount<T>();
    return []<std::size_t... Is>(std::index_sequence<Is...>) -> auto {
        std::array<std::string_view, count> names {};
        [[maybe_unused]] std::size_t        idx = 0;
        (
            [&] -> auto {
                constexpr auto member = detail::MembersOf<T>()[Is];
                if constexpr (std::meta::is_function(member) && std::meta::has_identifier(member)) {
                    names[idx++] = std::meta::identifier_of(member);
                }
            }(),
            ...);
        return names;
    }(std::make_index_sequence<detail::MembersOf<T>().size()>());
}

template <StringLiteral NameConst, typename T, typename ValueType>
constexpr auto SetFieldByName(T& t, ValueType&& new_value) -> bool {
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
constexpr auto MakeFromTuple(Tuple&& t) -> T {
    static_assert(std::is_aggregate_v<T>, "Type must be an aggregate.");
    return [&]<auto... members>(detail::ReplicatorType<members...>) -> auto {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) -> auto {
            return T {std::get<Is>(std::forward<Tuple>(t))...};
        }(std::make_index_sequence<sizeof...(members)>());
    }([:Expand(detail::NonStaticDataMembers<T>()):]);
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto EnumUnderlyingTypeName() -> std::string_view {
    return std::meta::display_string_of(std::meta::underlying_type(^^E));
}

template <typename T, typename F>
constexpr void ForEachFieldAdaptive(T&& t, F&& f) {
    ForEachField(std::forward<T>(t), std::forward<F>(f));
}

template <typename Tag, typename T>
consteval auto ValidateSerializability() -> bool {
    bool ok = true;
    [:Expand(detail::NonStaticDataMembers<T>()):] >> [&]<auto member>() {
        if constexpr (std::meta::type_of(member) == ^^Tag) {
            using FieldT = typename[:std::meta::type_of(member):];
            if constexpr (!std::is_trivially_copyable_v<FieldT>) {
                ok = false;
            }
        }
    };
    return ok;
}

template <typename T, typename F>
constexpr void ForEachNestedType(F&& f) {
    [:Expand(detail::MembersOf<T>()):] >> [&]<auto member>() -> auto {
        if constexpr (std::meta::is_type(member)) {
            using NestedType = typename[:member:];
            if constexpr (std::is_class_v<NestedType>) {
                f.template operator()<NestedType>();
            }
        }
    };
}

template <StringLiteral Name, typename... Fields>
struct Define {
    struct type;

    friend constexpr auto GetSchemaName(type* /*unused*/) -> std::string_view {
        return Name;
    }

    consteval {
        constexpr size_t             NumFields = sizeof...(Fields);
        std::vector<std::meta::info> specs;
        specs.reserve(NumFields);

        auto build_field = [&]<typename F>() -> auto {
            std::meta::data_member_options opts;
            opts.name = static_cast<std::string_view>(F::name);
            specs.push_back(std::meta::data_member_spec(^^typename F::type, opts));
        };

        (build_field.template operator()<Fields>(), ...);
        std::meta::define_aggregate(std::meta::dealias(^^type), specs);
    }
};

template <typename Meta, typename T, typename F>
constexpr void ForEachReflectedField(T&& t, F&& f) {
    [:Expand(detail::NonStaticDataMembers<T>()):] >> [&]<auto member>() -> auto {
        constexpr std::string_view name  = std::meta::has_identifier(member) ? std::meta::identifier_of(member) : std::string_view("");
        constexpr auto             found = detail::FindMetaMemberNamed<Meta>(name);
        if constexpr (found != std::meta::info {}) {
            using Tag = typename[:std::meta::type_of(found):];
            std::forward<F>(f).template operator()<Tag>(std::forward<T>(t).[:member:]);
        }
    };
}

template <typename E, typename F>
    requires std::is_enum_v<E>
constexpr void ForEachEnumerator(F&& f) {
    [:Expand(detail::EnumeratorsOf<E>()):] >> [&]<auto enumerator>() -> auto {
        constexpr E Val = static_cast<E>([:enumerator:]);
        f.template  operator()<Val>();
    };
}

template <typename T, typename F>
constexpr void ForEachFieldAccessor(F&& f) {
    using U = std::remove_cvref_t<T>;
    [:Expand(detail::NonStaticDataMembers<U>()):] >> [&]<auto member>() -> auto {
        constexpr std::string_view name = std::meta::identifier_of(member);
        using FieldType                 = typename[:std::meta::type_of(member):];

        auto const_getter = [](const U& inst) -> const FieldType& { return inst.[:member:]; };
        auto mut_getter   = [](U& inst) -> FieldType& { return inst.[:member:]; };
        auto setter       = [](U& inst, const FieldType& val) -> auto { inst.[:member:] = val; };

        f.template operator()<FieldType>(name, const_getter, mut_getter, setter);
    };
}

template <typename T, typename F>
constexpr void ForEachMethodPointer(F&& f) {
    [:Expand(detail::MembersOf<T>()):] >> [&]<auto member>() -> auto {
        if constexpr (std::meta::is_function(member) && std::meta::has_identifier(member)) {
            constexpr std::string_view name = std::meta::identifier_of(member);
            constexpr auto             pmf  = &[:member:];
            f(name, pmf);
        }
    };
}

template <typename Tag>
consteval auto AnnotationHasType(std::meta::info annotation) -> bool {
    const auto actualType = std::meta::dealias(std::meta::type_of(annotation));
    // P3394 annotations represent constant values. GCC therefore reports
    // `const Tag`, while the Bloomberg/Clang implementation historically
    // reported `Tag`. Accept both without making either compiler's behavior
    // leak into callers.
    return actualType == std::meta::dealias(^^Tag) || actualType == std::meta::dealias(^^std::add_const_t<Tag>);
}

template <typename Tag, auto EntityInfo>
consteval auto HasAnnotation() -> bool {
    for (auto a: std::meta::annotations_of(EntityInfo)) {
        if (AnnotationHasType<Tag>(a)) {
            return true;
        }
    }
    return false;
}

template <auto ScopeInfo, typename Tag, typename F>
constexpr void ForEachAnnotatedTypeInScope(F&& f) {
    constexpr auto members = std::define_static_array(std::meta::members_of(ScopeInfo, std::meta::access_context::current()));
    [:Expand(members):] >> [&]<auto m>() -> auto {
        if constexpr (std::meta::is_type(m)) {
            if constexpr (HasAnnotation<Tag, m>()) {
                using TargetType = typename[:m:];
                f.template operator()<TargetType>();
            }
        }
    };
}

// Walk every type annotated with Tag in the namespace (or class) that declared Tag.
// A namespace is not a type, so this cannot be ForEachAnnotatedType<SomeNamespace, Tag>.
template <typename Tag, typename F>
constexpr void ForEachAnnotatedType(F&& f) {
    ForEachAnnotatedTypeInScope<std::meta::parent_of(^^Tag), Tag>(std::forward<F>(f));
}

template <typename Tag, auto EntityInfo>
consteval auto GetAnnotation() -> std::optional<Tag> {
    for (auto a: std::meta::annotations_of(EntityInfo)) {
        if (AnnotationHasType<Tag>(a)) {
            // Materialize into a named local before constructing the
            // optional. Building std::optional<Tag> directly from the
            // extract prvalue (return std::meta::extract<Tag>(a);) fails
            // constant evaluation on some Clang-P2996/libc++ combinations
            // with 'read of object outside its lifetime' inside the
            // optional's inherited-constructor chain. The named local gets
            // the prvalue via guaranteed copy elision and the optional then
            // copies from a live object. This failure is not benign: it
            // makes constexpr EnumToMessage an immediate function, and the
            // runtime call in Error.hpp's category lambda then links as an
            // undefined symbol (observed on macOS/arm64).
            const Tag value = std::meta::extract<Tag>(a);
            return value;
        }
    }
    return std::nullopt;
}

template <typename Tag, typename E>
    requires std::is_enum_v<E>
constexpr auto GetEnumeratorAnnotation(E value) -> std::optional<Tag> {
    std::optional<Tag> result = std::nullopt;
    [:Expand(detail::EnumeratorsOf<E>()):] >> [&]<auto enumerator>() -> auto {
        auto annotation = GetAnnotation<Tag, enumerator>();
        if (value == static_cast<E>([:enumerator:])) {
            result = annotation;
        }
    };
    return result;
}

template <typename E>
    requires std::is_enum_v<E>
struct EnumMessageEntry {
    std::underlying_type_t<E> value {};
    std::string_view          message;
};

template <auto a>
consteval auto ExtractDescriptionText() -> std::string_view {
    constexpr auto type = std::meta::remove_const(std::meta::dealias(std::meta::type_of(a)));
    if constexpr (std::meta::has_template_arguments(type)) {
        if constexpr (std::meta::template_of(type) == ^^Description) {
            using DescType = typename[:type:];
            return DescType::message;
        }
    }
    return {};
}

template <auto EntityInfo>
consteval auto GetDescriptionText() -> std::string_view {
    std::string_view result {};
    constexpr auto   annotations = std::define_static_array(std::meta::annotations_of(EntityInfo));
    [:Expand(annotations):] >> [&]<auto a>() -> auto {
        if (!result.empty()) {
            return;
        }
        if (auto text = ExtractDescriptionText<a>(); !text.empty()) {
            result = text;
        }
    };
    return result;
}

template <typename T>
consteval auto AnnotatedName() -> std::string_view {
    constexpr auto info = ^^std::remove_cvref_t<T>;
    constexpr auto desc = GetDescriptionText<info>();
    if (!desc.empty()) {
        return desc;
    }
    return TypeName<T>();
}

template <typename E>
    requires std::is_enum_v<E>
consteval auto MakeEnumMessageTable() -> std::array<EnumMessageEntry<E>, detail::EnumeratorsOf<E>().size()> {
    std::array<EnumMessageEntry<E>, detail::EnumeratorsOf<E>().size()> table {};
    std::size_t                                                        i = 0;
    [:Expand(detail::EnumeratorsOf<E>()):] >> [&]<auto enumerator>() -> auto {
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

template <typename T>
consteval auto GetFloatFieldsCount() -> std::size_t {
    using U      = std::remove_cvref_t<T>;
    auto members = detail::NonStaticDataMembers<U>();
    if (members.empty()) {
        return 0;
    }
    bool all_float = std::ranges::all_of(members, [](auto m) -> auto { return std::meta::type_of(m) == ^^float; });
    return all_float ? members.size() : 0;
}

template <typename T>
constexpr auto CollectMethodResults(const T& inst) {
    using Collector = detail::MethodCollector<std::remove_cvref_t<T>>;
    return [&]<size_t... Is>(std::index_sequence<Is...>) -> auto {
        return std::tuple_cat(detail::ToTuple((inst.[:Collector::method_handles[Is]:]()))...);
    }(std::make_index_sequence<Collector::count> {});
}

} // namespace ZHLN::Reflect

#else // Standard C++26 Fallback (Stubs - Waiting for compiler reflection)

namespace ZHLN::Reflect {

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
consteval auto EnumHasValue(std::underlying_type_t<E> /*targetValue*/) noexcept -> bool {
    return false; // Safe fallback when compiler reflection is disabled
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

template <typename Tag, typename F>
constexpr void ForEachAnnotatedType(F&& /*unused*/) {
}

template <typename Tag, typename T>
consteval std::optional<Tag> GetAnnotation() {
    return std::nullopt;
}

template <typename T>
consteval std::string_view AnnotatedName() {
    return TypeName<T>();
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
    ForEachField(t, [&](auto&& field) -> auto { seed ^= std::hash<std::remove_cvref_t<decltype(field)>> {}(field) + 0x9e3779b9 + (seed << 6) + (seed >> 2); });
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

template <typename T>
constexpr auto HasBases() -> bool {
    return !BaseClasses<T>().empty();
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

template <typename E, typename F>
    requires std::is_enum_v<E>
constexpr void DispatchEnum(E value, F&& f) {
    ForEachEnumerator<E>([&]<E Val>() -> auto {
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
auto ToDebugString(const T& t) -> std::string;

template <typename T, typename = void>
struct CustomFormatter {
    static void format(const T& val, std::string& out) {
        using Decayed = std::remove_cvref_t<T>;

        if constexpr (detail::Formattable<Decayed>) {
            out += std::format("{}", val);
        } else if constexpr (std::is_enum_v<Decayed>) {
            out += EnumToString(val);
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
