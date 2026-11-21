// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Core/Reflection/Entity.hpp
//
// Domain-agnostic C++ entity, enclosing-scope, and template-argument
// introspection. Every public descriptor is a plain value or C++ type tag so
// consumers never touch std::meta::info, and no ABI, naming, or subsystem
// policy lives here.

#include <Zahlen/Core/Reflection/Core.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ZHLN::Reflect {

#if ZHLN_REFLECTION_AVAILABLE

enum class EntityKind : uint8_t {
    Namespace,
    Class,
    Union,
    Enum,
    Template,
    Function,
    Other,
};

// Opaque compile-time identity token for a dealiased C++ entity (type,
// namespace, template, or function). Two tokens compare equal iff they name
// the same underlying entity; aliases collapse to their target while distinct
// cv-qualifiers, pointers, arrays, functions, and specializations stay distinct.
struct EntityToken {
    const void* tag = nullptr;
    /*
     * TODO(Evilpasture): Temporary. When -fsanitize=undefined (from -fsanitize=address,undefined) is enabled, GCC's UBSan null-check handling
     * (-fsanitize=null) prevents GCC from constant-folding (&symbol) != nullptr / == nullptr in constexpr/consteval contexts, producing a bogus compiler error.
     */
    bool valid = false;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid;
    }

    [[nodiscard]] constexpr auto operator==(const EntityToken& other) const noexcept -> bool {
        if (valid != other.valid) {
            return false;
        }
        if (!valid) {
            return true;
        }
        if consteval {
            // GCC with ASan/UBSan can also reject equality of two non-null
            // static tag addresses. Compare their reflected pointer values
            // instead, preserving identity without a pointer comparison.
            return std::meta::reflect_constant(tag) == std::meta::reflect_constant(other.tag);
        } else {
            return tag == other.tag;
        }
    }
};

struct EntityDescriptor {
    EntityKind       kind = EntityKind::Other;
    std::string_view name;
    bool             hasIdentifier            = false;
    bool             isGlobal                 = false;
    bool             isTopLevel               = false;
    bool             isFunctionLocal          = false;
    bool             isTemplateSpecialization = false;
    bool             isVariadicSpecialization = false;
    EntityToken      id                       = {};
    EntityToken      templateId               = {};
};

using ScopeDescriptor = EntityDescriptor;

namespace TemplatedDetail {

template <typename T>
struct TypeEntityTag {
    static constexpr unsigned char kByte = 0;
};

template <std::meta::info Entity>
struct MetaEntityTag {
    static constexpr unsigned char kByte = 0;
};

template <typename>
struct VariadicProbeTemplate {};

template <std::meta::info Entity>
consteval auto MakeEntityToken() noexcept -> EntityToken {
    constexpr std::meta::info kCanonical = std::meta::dealias(Entity);
    if constexpr (std::meta::is_type(kCanonical)) {
        return EntityToken {&TypeEntityTag<typename[:kCanonical:]>::kByte, true};
    } else {
        return EntityToken {&MetaEntityTag<kCanonical>::kByte, true};
    }
}

consteval auto ClassifyEntity(std::meta::info raw) noexcept -> EntityKind {
    const std::meta::info info = std::meta::dealias(raw);
    if (std::meta::is_namespace(info)) {
        return EntityKind::Namespace;
    }
    if (std::meta::is_template(info)) {
        return EntityKind::Template;
    }
    if (std::meta::is_function(info)) {
        return EntityKind::Function;
    }
    if (std::meta::is_type(info)) {
        if (std::meta::is_class_type(info)) {
            return EntityKind::Class;
        }
        if (std::meta::is_union_type(info)) {
            return EntityKind::Union;
        }
        if (std::meta::is_enum_type(info)) {
            return EntityKind::Enum;
        }
    }
    return EntityKind::Other;
}

consteval auto HasDeclaringScope(std::meta::info raw) noexcept -> bool {
    const std::meta::info info = std::meta::dealias(raw);
    if (info == ^^::) {
        return false;
    }
    return ClassifyEntity(info) != EntityKind::Other;
}

consteval auto IsEntityFunctionLocal(std::meta::info raw) noexcept -> bool {
    std::meta::info current = std::meta::dealias(raw);
    while (HasDeclaringScope(current)) {
        current = std::meta::dealias(std::meta::parent_of(current));
        if (current == ^^::) {
            return false;
        }
        if (std::meta::is_function(current)) {
            return true;
        }
    }
    return false;
}

template <std::meta::info Spec>
consteval auto IsVariadicSpecInfo() noexcept -> bool {
    constexpr std::meta::info kInfo = std::meta::dealias(Spec);
    if constexpr (!std::meta::is_type(kInfo) || !std::meta::has_template_arguments(kInfo)) {
        return false;
    } else {
        constexpr std::meta::info          kTmpl = std::meta::template_of(kInfo);
        const std::vector<std::meta::info> args  = std::meta::template_arguments_of(kInfo);
        // Primary templates always declare at least one parameter, and
        // template_arguments_of fills in defaulted arguments, so zero arguments
        // implies an empty parameter pack (e.g. Pack<>).
        if (args.empty()) {
            return true;
        }
        auto acceptsExtra = [&](std::meta::info extra) -> bool {
            std::vector<std::meta::info> probe = args;
            probe.push_back(extra);
            return std::meta::can_substitute(kTmpl, probe);
        };
        if (acceptsExtra(args.back())) {
            return true;
        }
        // Cover fixed-head + empty trailing pack (e.g. HeadTail<int>,
        // TypeThenInts<int>) where the pack kind differs from args.back().
        return acceptsExtra(^^int) || acceptsExtra(std::meta::reflect_constant(0)) || acceptsExtra(std::meta::reflect_constant(0U)) ||
               acceptsExtra(std::meta::reflect_constant(std::size_t {0})) || acceptsExtra(std::meta::reflect_constant(false)) ||
               acceptsExtra(std::meta::reflect_constant(nullptr)) || acceptsExtra(^^VariadicProbeTemplate);
    }
}

template <std::meta::info Entity>
consteval auto EntityUnqualifiedName() noexcept -> std::pair<bool, std::string_view> {
    constexpr std::meta::info kInfo  = std::meta::dealias(Entity);
    constexpr std::meta::info kNamed = []() consteval -> std::meta::info {
        if constexpr (std::meta::is_type(kInfo) && std::meta::has_template_arguments(kInfo)) {
            return std::meta::template_of(kInfo);
        } else {
            return kInfo;
        }
    }();
    if constexpr (std::meta::has_identifier(kNamed)) {
        return {true, std::meta::identifier_of(kNamed)};
    } else {
        return {false, std::string_view {}};
    }
}

template <std::meta::info Entity>
consteval auto DescribeEntityInfo() noexcept -> EntityDescriptor {
    constexpr std::meta::info kInfo     = std::meta::dealias(Entity);
    constexpr EntityKind      kKind     = ClassifyEntity(kInfo);
    constexpr auto            kName     = EntityUnqualifiedName<kInfo>();
    constexpr bool            kGlobal   = (kInfo == ^^::);
    constexpr bool            kTopLevel = []() consteval -> bool {
        if constexpr (HasDeclaringScope(kInfo)) {
            return std::meta::dealias(std::meta::parent_of(kInfo)) == ^^::;
        } else {
            return false;
        }
    }();
    constexpr bool        kIsSpec     = std::meta::is_type(kInfo) && std::meta::has_template_arguments(kInfo);
    constexpr EntityToken kTemplateId = []() consteval -> EntityToken {
        if constexpr (kIsSpec) {
            return MakeEntityToken<std::meta::template_of(kInfo)>();
        } else {
            return EntityToken {};
        }
    }();

    return EntityDescriptor {
        .kind                     = kKind,
        .name                     = kName.second,
        .hasIdentifier            = kName.first,
        .isGlobal                 = kGlobal,
        .isTopLevel               = kTopLevel,
        .isFunctionLocal          = IsEntityFunctionLocal(kInfo),
        .isTemplateSpecialization = kIsSpec,
        .isVariadicSpecialization = IsVariadicSpecInfo<kInfo>(),
        .id                       = MakeEntityToken<kInfo>(),
        .templateId               = kTemplateId,
    };
}

template <std::meta::info Scope, typename Visitor>
constexpr void DispatchEnclosingScope(Visitor& visitor) {
    constexpr std::meta::info kScope = std::meta::dealias(Scope);
    constexpr ScopeDescriptor kDesc  = DescribeEntityInfo<kScope>();
    if constexpr (std::meta::is_type(kScope) && (std::meta::is_class_type(kScope) || std::meta::is_union_type(kScope) || std::meta::is_enum_type(kScope))) {
        using OwnerType = typename[:kScope:];
        if constexpr (requires { visitor.template OnTypeScope<OwnerType>(kDesc); }) {
            visitor.template OnTypeScope<OwnerType>(kDesc);
        } else {
            visitor(kDesc);
        }
    } else {
        visitor(kDesc);
    }
}

template <std::meta::info Entity, typename Visitor>
constexpr void WalkEnclosingScopes(Visitor& visitor) {
    constexpr std::meta::info kInfo = std::meta::dealias(Entity);
    if constexpr (HasDeclaringScope(kInfo)) {
        constexpr std::meta::info kParent = std::meta::dealias(std::meta::parent_of(kInfo));
        if constexpr (kParent != ^^::) {
            WalkEnclosingScopes<kParent>(visitor);
            DispatchEnclosingScope<kParent>(visitor);
        }
    }
}

// Public-facing tag passed to visitor.OnTemplate<TemplateTag>() for template
// template arguments. Wraps the reflected template without leaking meta::info.
template <std::meta::info Tmpl>
struct TemplateEntityTag {
    static constexpr EntityDescriptor descriptor      = DescribeEntityInfo<Tmpl>();
    static constexpr std::string_view name            = descriptor.name;
    static constexpr bool             hasIdentifier   = descriptor.hasIdentifier;
    static constexpr bool             isTopLevel      = descriptor.isTopLevel;
    static constexpr bool             isFunctionLocal = descriptor.isFunctionLocal;
    static constexpr EntityToken      id              = descriptor.id;

    template <typename Visitor>
    static constexpr void ForEachEnclosingScope(Visitor&& visitor) {
        WalkEnclosingScopes<Tmpl>(visitor);
    }
};

template <std::meta::info Spec>
consteval auto TemplateArgumentsArray() {
    return std::define_static_array(std::meta::template_arguments_of(Spec));
}

template <std::meta::info Arg, typename Visitor>
constexpr void DispatchTemplateArgument(Visitor& visitor) {
    if constexpr (std::meta::is_type(Arg)) {
        visitor.template OnType<typename[:std::meta::dealias(Arg):]>();
    } else if constexpr (std::meta::is_template(Arg)) {
        visitor.template OnTemplate<TemplateEntityTag<std::meta::dealias(Arg)>>();
    } else if constexpr (std::meta::is_value(Arg)) {
        using ValueType = std::remove_cv_t<typename[:std::meta::dealias(std::meta::type_of(Arg)):]>;
        if constexpr (std::is_integral_v<ValueType> || std::is_enum_v<ValueType> || std::is_null_pointer_v<ValueType> || std::is_floating_point_v<ValueType>) {
            constexpr ValueType kValue = std::meta::extract<ValueType>(Arg);
            visitor.template OnValue<kValue>();
        } else {
            visitor.template OnOtherValue<ValueType>();
        }
    } else {
        using ValueType = std::remove_cv_t<typename[:std::meta::dealias(std::meta::type_of(Arg)):]>;
        visitor.template OnOtherValue<ValueType>();
    }
}

} // namespace TemplatedDetail

// Exact token for T after alias resolution (cv-qualifiers, pointers, arrays,
// references, and function types each retain their own distinct token).
template <typename T>
[[nodiscard]] consteval auto EntityTokenOf() noexcept -> EntityToken {
    return EntityToken {&TemplatedDetail::TypeEntityTag<T>::kByte, true};
}

// Describes the unqualified type entity T (class, union, enum, or Other).
template <typename T>
[[nodiscard]] consteval auto DescribeEntity() noexcept -> EntityDescriptor {
    using Bare = std::remove_cv_t<T>;
    return TemplatedDetail::DescribeEntityInfo<^^Bare>();
}

template <typename T>
[[nodiscard]] consteval auto EntityKindOf() noexcept -> EntityKind {
    return DescribeEntity<T>().kind;
}

template <typename T>
[[nodiscard]] consteval auto HasIdentifier() noexcept -> bool {
    return DescribeEntity<T>().hasIdentifier;
}

template <typename T>
[[nodiscard]] consteval auto EntityName() noexcept -> std::string_view {
    return DescribeEntity<T>().name;
}

template <typename T>
[[nodiscard]] consteval auto IsFunctionLocal() noexcept -> bool {
    return DescribeEntity<T>().isFunctionLocal;
}

template <typename T>
[[nodiscard]] consteval auto IsTemplateSpecialization() noexcept -> bool {
    return DescribeEntity<T>().isTemplateSpecialization;
}

template <typename T>
[[nodiscard]] consteval auto IsVariadicSpecialization() noexcept -> bool {
    return DescribeEntity<T>().isVariadicSpecialization;
}

// Walks enclosing scopes of T from outermost (direct child of ::) to innermost
// (immediate parent of T). Calls `visitor.template OnTypeScope<OwnerT>(desc)`
// for class/union/enum owners when supported by `visitor`, otherwise `visitor(desc)`.
template <typename T, typename Visitor>
constexpr void ForEachEnclosingScope(Visitor&& visitor) {
    using Bare = std::remove_cv_t<T>;
    TemplatedDetail::WalkEnclosingScopes<^^Bare>(visitor);
}

// Visits each template argument of T in declaration order (including defaulted
// arguments). Dispatches to:
//   - visitor.template OnType<ArgT>()
//   - visitor.template OnTemplate<TemplateTag>()
//   - visitor.template OnValue<kVal>()       (integral, enum, nullptr_t, floating-point)
//   - visitor.template OnOtherValue<ValueT>() (object, pointer, reference, or class NTTP)
// No-op if T is not a template specialization.
template <typename T, typename Visitor>
constexpr void ForEachTemplateArgument(Visitor&& visitor) {
    using Bare                      = std::remove_cv_t<T>;
    constexpr std::meta::info kInfo = std::meta::dealias(^^Bare);
    if constexpr (std::meta::is_type(kInfo) && std::meta::has_template_arguments(kInfo)) {
        constexpr auto kArgs = TemplatedDetail::TemplateArgumentsArray<kInfo>();
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (TemplatedDetail::DispatchTemplateArgument<kArgs[Is]>(visitor), ...);
        }(std::make_index_sequence<kArgs.size()> {});
    }
}

#endif // ZHLN_REFLECTION_AVAILABLE

} // namespace ZHLN::Reflect
