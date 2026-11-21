// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Compile-time Itanium C++ ABI (§5.1) symbol mangler and typed DSO symbol
// lookup for COM-compliant free and static member functions.
//
// All C++ AST introspection comes from Core/Reflection/{Callable,Entity}.hpp
// and <type_traits>; this header contains zero raw reflection tokens and owns
// every cross-DSO ABI rule:
//   - Builtin primitives via canonical C++ types (aliases already resolved by
//     template instantiation: uint32_t -> j, size_t -> m/y, std::nullptr_t -> Dn)
//   - CV-qualifiers ([V][K]), pointers (P), lvalue/rvalue references (R/O),
//     bounded/unbounded arrays (A<N>_ / A_), and function pointers (PF...E /
//     PDoF...E)
//   - Nested namespaces, classes, unions, enums, and fixed-arity class template
//     specializations (type, template-template, and integral/bool/enum/nullptr
//     non-type arguments), plus std::byte (St4byte)
//   - Left-to-right base-36 substitution table (S_, S0_..S9_, SA_..SZ_, S10_..)
//   - Compile-time rejection of non-COM / ABI-unstable forms: internal linkage,
//     anonymous namespaces, function-local or unnamed types, function templates,
//     variadic class templates, operators/conversions/special members, C
//     variadics, pointers to members, floating-point/object/class NTTPs, and
//     std:: parameter or return types (preventing inline-ABI-namespace drift
//     and B5cxx11-style return-type ABI tag propagation).
//   - Note: extern "C" functions cannot be rejected automatically yet because
//     std::meta::has_c_language_linkage awaits bloomberg/clang-p2996#340; keep
//     MangleFunction / ResolveSymbol strictly on C++-linkage declarations.
//
// On Darwin/Mach-O, MangleFunction returns "_Z..." — the exact spelling dlsym
// expects (dyld prepends Mach-O's extra leading underscore itself, whereas
// linker flags like -exported_symbol take "__Z...").

#include <Zahlen/Core/Reflection/Callable.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <Zahlen/Core/Reflection/Entity.hpp>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::COM {

namespace TemplatedDetail {

template <typename T>
[[nodiscard]] consteval auto ItaniumBuiltinCode() noexcept -> std::string_view {
    if constexpr (std::is_same_v<T, void>) {
        return "v";
    } else if constexpr (std::is_same_v<T, wchar_t>) {
        return "w";
    } else if constexpr (std::is_same_v<T, bool>) {
        return "b";
    } else if constexpr (std::is_same_v<T, char>) {
        return "c";
    } else if constexpr (std::is_same_v<T, signed char>) {
        return "a";
    } else if constexpr (std::is_same_v<T, unsigned char>) {
        return "h";
    } else if constexpr (std::is_same_v<T, short>) {
        return "s";
    } else if constexpr (std::is_same_v<T, unsigned short>) {
        return "t";
    } else if constexpr (std::is_same_v<T, int>) {
        return "i";
    } else if constexpr (std::is_same_v<T, unsigned int>) {
        return "j";
    } else if constexpr (std::is_same_v<T, long>) {
        return "l";
    } else if constexpr (std::is_same_v<T, unsigned long>) {
        return "m";
    } else if constexpr (std::is_same_v<T, long long>) {
        return "x";
    } else if constexpr (std::is_same_v<T, unsigned long long>) {
        return "y";
    } else if constexpr (std::is_same_v<T, float>) {
        return "f";
    } else if constexpr (std::is_same_v<T, double>) {
        return "d";
    } else if constexpr (std::is_same_v<T, long double>) {
        return "e";
    } else if constexpr (std::is_same_v<T, char8_t>) {
        return "Du";
    } else if constexpr (std::is_same_v<T, char16_t>) {
        return "Ds";
    } else if constexpr (std::is_same_v<T, char32_t>) {
        return "Di";
    } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
        return "Dn";
    } else {
        return {};
    }
}

template <typename FnType>
struct ItaniumFunctionType {
    static constexpr bool kSupported  = false;
    static constexpr bool kCVariadic  = false;
    static constexpr bool kIsNoexcept = false;
};

template <typename R, typename... Args>
struct ItaniumFunctionType<R(Args...)> {
    static constexpr bool kSupported  = true;
    static constexpr bool kCVariadic  = false;
    static constexpr bool kIsNoexcept = false;
    using ReturnType                  = R;
    using ParameterTypes              = std::tuple<Args...>;
};

template <typename R, typename... Args>
struct ItaniumFunctionType<R(Args...) noexcept> {
    static constexpr bool kSupported  = true;
    static constexpr bool kCVariadic  = false;
    static constexpr bool kIsNoexcept = true;
    using ReturnType                  = R;
    using ParameterTypes              = std::tuple<Args...>;
};

template <typename R, typename... Args>
struct ItaniumFunctionType<R(Args..., ...)> {
    static constexpr bool kSupported  = false;
    static constexpr bool kCVariadic  = true;
    static constexpr bool kIsNoexcept = false;
    using ReturnType                  = R;
    using ParameterTypes              = std::tuple<Args...>;
};

template <typename R, typename... Args>
struct ItaniumFunctionType<R(Args..., ...) noexcept> {
    static constexpr bool kSupported  = false;
    static constexpr bool kCVariadic  = true;
    static constexpr bool kIsNoexcept = true;
    using ReturnType                  = R;
    using ParameterTypes              = std::tuple<Args...>;
};

[[nodiscard]] consteval auto IsTopLevelStdNamespace(const Reflect::ScopeDescriptor& desc) noexcept -> bool {
    return desc.kind == Reflect::EntityKind::Namespace && desc.isTopLevel && desc.name == "std";
}

struct ScopePolicyStatus {
    bool hasStdNamespace       = false;
    bool hasAnonymousNamespace = false;
    bool hasUnnamedScope       = false;
    bool hasVariadicScope      = false;
    bool hasUnsupportedScope   = false;
};

struct ScopePolicyScanner {
    ScopePolicyStatus status {};

    consteval void operator()(const Reflect::ScopeDescriptor& desc) noexcept {
        if (desc.kind == Reflect::EntityKind::Namespace) {
            if (!desc.hasIdentifier) {
                status.hasAnonymousNamespace = true;
            } else if (IsTopLevelStdNamespace(desc)) {
                status.hasStdNamespace = true;
            }
        } else if (desc.kind == Reflect::EntityKind::Class || desc.kind == Reflect::EntityKind::Union || desc.kind == Reflect::EntityKind::Enum) {
            if (!desc.hasIdentifier) {
                status.hasUnnamedScope = true;
            }
            if (desc.isVariadicSpecialization) {
                status.hasVariadicScope = true;
            }
        } else {
            status.hasUnsupportedScope = true;
        }
    }
};

template <typename WalkFn>
[[nodiscard]] consteval auto ScanEnclosingScopes(WalkFn walk) noexcept -> ScopePolicyStatus {
    ScopePolicyScanner scanner {};
    walk(scanner);
    return scanner.status;
}

template <ScopePolicyStatus Status>
consteval void AssertValidScopes() {
    static_assert(!Status.hasAnonymousNamespace, "ItaniumMangle: entities in anonymous namespaces are not supported.");
    static_assert(!Status.hasStdNamespace, "ItaniumMangle: std:: entities (other than std::byte) are not supported across ABI boundaries.");
    static_assert(!Status.hasUnnamedScope, "ItaniumMangle: unnamed enclosing scopes are not supported.");
    static_assert(!Status.hasVariadicScope, "ItaniumMangle: variadic template enclosing scopes are not supported.");
    static_assert(!Status.hasUnsupportedScope, "ItaniumMangle: local or unsupported enclosing scopes are not supported.");
}

struct ItaniumMangler {
    std::string                       out {};
    std::vector<Reflect::EntityToken> subs {};

    consteval void AppendDecimal(uint64_t value) {
        char     buf[20] {};
        unsigned n = 0;
        do {
            buf[n++] = static_cast<char>('0' + (value % 10U));
            value /= 10U;
        } while (value != 0U);
        while (n != 0U) {
            out += buf[--n];
        }
    }

    consteval void AppendBase36(std::size_t value) {
        constexpr char kDigits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        char           buf[16] {};
        unsigned       n = 0;
        do {
            buf[n++] = kDigits[value % 36U];
            value /= 36U;
        } while (value != 0U);
        while (n != 0U) {
            out += buf[--n];
        }
    }

    consteval void AppendSourceName(std::string_view name) {
        AppendDecimal(name.size());
        out += name;
    }

    consteval void AppendSubstitution(std::size_t index) {
        out += 'S';
        if (index > 0U) {
            AppendBase36(index - 1U);
        }
        out += '_';
    }

    [[nodiscard]] consteval auto FindSubstitution(Reflect::EntityToken token) const noexcept -> std::pair<bool, std::size_t> {
        if (!token) {
            return {false, 0U};
        }
        for (std::size_t i = 0; i < subs.size(); ++i) {
            if (subs[i] == token) {
                return {true, i};
            }
        }
        return {false, 0U};
    }

    [[nodiscard]] consteval auto TrySubstitute(Reflect::EntityToken token) -> bool {
        const auto [found, idx] = FindSubstitution(token);
        if (found) {
            AppendSubstitution(idx);
        }
        return found;
    }

    consteval void AddSubstitution(Reflect::EntityToken token) {
        if (!token) {
            return;
        }
        if (!FindSubstitution(token).first) {
            subs.push_back(token);
        }
    }

    template <typename IntT>
    consteval void AppendIntegralLiteral(IntT value) {
        if constexpr (std::is_signed_v<IntT>) {
            if (value < IntT {0}) {
                out += 'n';
                AppendDecimal(uint64_t {0} - static_cast<uint64_t>(value));
            } else {
                AppendDecimal(static_cast<uint64_t>(value));
            }
        } else {
            AppendDecimal(static_cast<uint64_t>(value));
        }
    }
};

template <typename T>
consteval void EncodeType(ItaniumMangler& m);

template <typename T>
consteval void EncodeTemplateArguments(ItaniumMangler& m);

template <typename Tuple>
consteval void EncodeBareFunctionParameters(ItaniumMangler& m) {
    constexpr std::size_t kCount = std::tuple_size_v<Tuple>;
    if constexpr (kCount == 0U) {
        m.out += 'v';
    } else {
        [&]<std::size_t... Is>(std::index_sequence<Is...>) { (EncodeType<std::tuple_element_t<Is, Tuple>>(m), ...); }(std::make_index_sequence<kCount> {});
    }
}

struct PrefixMatch {
    std::size_t index           = 0;
    std::size_t subIndex        = 0;
    bool        matched         = false;
    bool        templateOnlyHit = false;
};

struct PrefixFinder {
    const ItaniumMangler& m;
    std::size_t           cursor = 0;
    PrefixMatch           best {};

    consteval void operator()(const Reflect::ScopeDescriptor& desc) noexcept {
        if (const auto [hit, idx] = m.FindSubstitution(desc.id); hit) {
            best = PrefixMatch {.index = cursor, .subIndex = idx, .matched = true, .templateOnlyHit = false};
        } else if (desc.isTemplateSpecialization) {
            if (const auto [tHit, tIdx] = m.FindSubstitution(desc.templateId); tHit) {
                best = PrefixMatch {.index = cursor, .subIndex = tIdx, .matched = true, .templateOnlyHit = true};
            }
        }
        ++cursor;
    }
};

struct PrefixEmitter {
    ItaniumMangler& m;
    PrefixMatch     match {};
    std::size_t     cursor = 0;

    consteval void operator()(const Reflect::ScopeDescriptor& desc) {
        const std::size_t idx = cursor++;
        if (match.matched && idx <= match.index) {
            return;
        }
        m.AppendSourceName(desc.name);
        m.AddSubstitution(desc.id);
    }

    template <typename OwnerT>
    consteval void OnTypeScope(const Reflect::ScopeDescriptor& desc) {
        const std::size_t idx = cursor++;
        if (match.matched && idx < match.index) {
            return;
        }
        if (match.matched && idx == match.index) {
            if (match.templateOnlyHit) {
                EncodeTemplateArguments<OwnerT>(m);
                m.AddSubstitution(desc.id);
            }
            return;
        }
        if (desc.isTemplateSpecialization) {
            m.AppendSourceName(desc.name);
            m.AddSubstitution(desc.templateId);
            EncodeTemplateArguments<OwnerT>(m);
            m.AddSubstitution(desc.id);
        } else {
            m.AppendSourceName(desc.name);
            m.AddSubstitution(desc.id);
        }
    }
};

// Emits an Itanium <prefix> for an enclosing scope chain, substituting the
// deepest already-recorded prefix (or enclosing <template-prefix>) first so
// `A::B::Bar` after `A::B::Foo` emits `S0_3Bar` rather than `S_1B3Bar`.
template <typename WalkFn>
consteval void EmitEnclosingPrefix(ItaniumMangler& m, WalkFn walk) {
    PrefixFinder finder {.m = m};
    walk(finder);
    if (finder.best.matched) {
        m.AppendSubstitution(finder.best.subIndex);
    }
    PrefixEmitter emitter {.m = m, .match = finder.best};
    walk(emitter);
}

template <typename TemplateTag>
consteval void EncodeTemplateTemplateArgument(ItaniumMangler& m) {
    constexpr Reflect::EntityDescriptor kDesc = TemplateTag::descriptor;
    static_assert(!kDesc.isFunctionLocal, "ItaniumMangle: function-local templates are not supported.");
    static_assert(kDesc.hasIdentifier, "ItaniumMangle: unnamed template-template arguments are not supported.");

    constexpr auto kWalk   = [](auto& v) consteval { TemplateTag::ForEachEnclosingScope(v); };
    constexpr auto kPolicy = ScanEnclosingScopes(kWalk);
    AssertValidScopes<kPolicy>();

    if (m.TrySubstitute(kDesc.id)) {
        return;
    }
    if constexpr (kDesc.isTopLevel) {
        m.AppendSourceName(kDesc.name);
    } else {
        m.out += 'N';
        EmitEnclosingPrefix(m, kWalk);
        m.AppendSourceName(kDesc.name);
        m.out += 'E';
    }
    m.AddSubstitution(kDesc.id);
}

struct TemplateArgumentEncoder {
    ItaniumMangler& m;

    template <typename ArgT>
    consteval void OnType() {
        EncodeType<ArgT>(m);
    }

    template <typename TemplateTag>
    consteval void OnTemplate() {
        EncodeTemplateTemplateArgument<TemplateTag>(m);
    }

    template <auto Value>
    consteval void OnValue() {
        using V = std::remove_cv_t<decltype(Value)>;
        if constexpr (std::is_null_pointer_v<V>) {
            /*
             * TODO(Evilpasture): Temporary for now.
             * https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangling.literal
             * The pointer literal expression nullptr is encoded as "LDnE". In contrast, a template argument which happens to be a null pointer (an extension
             * made standard in C++11) is mangled as if it were a literal 0 of the appropriate pointer type; for example, "LPi0E" or "LDn0E". This inconsistency
             * is an unfortunate accident.
             * */
#if defined(__GNUC__) && !defined(__clang__)
            m.out += "LDnE";
#else
            m.out += "LDn0E";
#endif
        } else if constexpr (std::is_same_v<V, bool>) {
            m.out += Value ? "Lb1E" : "Lb0E";
        } else if constexpr (std::is_integral_v<V> && sizeof(V) <= sizeof(uint64_t)) {
            m.out += 'L';
            EncodeType<V>(m);
            m.AppendIntegralLiteral<V>(Value);
            m.out += 'E';
        } else if constexpr (std::is_enum_v<V> && sizeof(V) <= sizeof(uint64_t)) {
            using Underlying = std::underlying_type_t<V>;
            m.out += 'L';
            EncodeType<V>(m);
            m.AppendIntegralLiteral<Underlying>(static_cast<Underlying>(Value));
            m.out += 'E';
        } else if constexpr (std::is_floating_point_v<V>) {
            static_assert(false, "ItaniumMangle: floating-point non-type template arguments are not supported in COM signatures.");
        } else {
            static_assert(false, "ItaniumMangle: unsupported non-type template argument width or type.");
        }
    }

    template <typename ValueT>
    consteval void OnOtherValue() {
        static_assert(false, "ItaniumMangle: pointer, reference, or class non-type template arguments are not supported in COM signatures.");
    }
};

template <typename T>
consteval void EncodeTemplateArguments(ItaniumMangler& m) {
    m.out += 'I';
    Reflect::ForEachTemplateArgument<T>(TemplateArgumentEncoder {.m = m});
    m.out += 'E';
}

template <typename T>
consteval void EncodeNamedType(ItaniumMangler& m, Reflect::EntityToken typeToken) {
    constexpr Reflect::EntityDescriptor kDesc = Reflect::DescribeEntity<T>();
    static_assert(!kDesc.isFunctionLocal, "ItaniumMangle: function-local types are not supported in COM signatures.");
    static_assert(kDesc.hasIdentifier, "ItaniumMangle: unnamed classes, unions, enums, and lambdas are not supported in COM signatures.");
    static_assert(!kDesc.isVariadicSpecialization, "ItaniumMangle: variadic class template specializations are not supported in COM signatures.");

    constexpr auto kWalk   = [](auto& v) consteval { Reflect::ForEachEnclosingScope<T>(v); };
    constexpr auto kPolicy = ScanEnclosingScopes(kWalk);
    AssertValidScopes<kPolicy>();

    if constexpr (kDesc.isTopLevel) {
        if constexpr (kDesc.isTemplateSpecialization) {
            if (!m.TrySubstitute(kDesc.templateId)) {
                m.AppendSourceName(kDesc.name);
                m.AddSubstitution(kDesc.templateId);
            }
            EncodeTemplateArguments<T>(m);
        } else {
            m.AppendSourceName(kDesc.name);
        }
    } else {
        m.out += 'N';
        if constexpr (kDesc.isTemplateSpecialization) {
            if (!m.TrySubstitute(kDesc.templateId)) {
                EmitEnclosingPrefix(m, kWalk);
                m.AppendSourceName(kDesc.name);
                m.AddSubstitution(kDesc.templateId);
            }
            EncodeTemplateArguments<T>(m);
        } else {
            EmitEnclosingPrefix(m, kWalk);
            m.AppendSourceName(kDesc.name);
        }
        m.out += 'E';
    }
    m.AddSubstitution(typeToken);
}

template <typename T>
consteval void EncodeType(ItaniumMangler& m) {
    constexpr std::string_view kBuiltin = ItaniumBuiltinCode<T>();
    if constexpr (!kBuiltin.empty()) {
        // Unqualified builtins (including multi-char Dn/Du/Ds/Di) never enter
        // the substitution table (Itanium C++ ABI §5.1.8).
        m.out += kBuiltin;
    } else {
        constexpr Reflect::EntityToken kToken = Reflect::EntityTokenOf<T>();
        if (m.TrySubstitute(kToken)) {
            return;
        }
        // Check arrays BEFORE cv-qualifiers: in C++, `const U[N]` satisfies
        // both std::is_array_v and std::is_const_v, and Itanium encodes the
        // array dimension outside the element qualifier (`A<N>_K<U>`).
        if constexpr (std::is_bounded_array_v<T>) {
            m.out += 'A';
            m.AppendDecimal(std::extent_v<T>);
            m.out += '_';
            EncodeType<std::remove_extent_t<T>>(m);
            m.AddSubstitution(kToken);
        } else if constexpr (std::is_unbounded_array_v<T>) {
            m.out += "A_";
            EncodeType<std::remove_extent_t<T>>(m);
            m.AddSubstitution(kToken);
        } else if constexpr (std::is_const_v<T> || std::is_volatile_v<T>) {
            // Itanium §5.1.5: `<CV-qualifiers> ::= [r] [V] [K]` — combined
            // `const volatile U` forms a single `VK<U>` substitution candidate.
            if constexpr (std::is_volatile_v<T>) {
                m.out += 'V';
            }
            if constexpr (std::is_const_v<T>) {
                m.out += 'K';
            }
            EncodeType<std::remove_cv_t<T>>(m);
            m.AddSubstitution(kToken);
        } else if constexpr (std::is_lvalue_reference_v<T>) {
            m.out += 'R';
            EncodeType<std::remove_reference_t<T>>(m);
            m.AddSubstitution(kToken);
        } else if constexpr (std::is_rvalue_reference_v<T>) {
            m.out += 'O';
            EncodeType<std::remove_reference_t<T>>(m);
            m.AddSubstitution(kToken);
        } else if constexpr (std::is_pointer_v<T>) {
            m.out += 'P';
            EncodeType<std::remove_pointer_t<T>>(m);
            m.AddSubstitution(kToken);
        } else if constexpr (std::is_function_v<T>) {
            using FnInfo = ItaniumFunctionType<T>;
            static_assert(!FnInfo::kCVariadic, "ItaniumMangle: C variadic function types are not supported in COM signatures.");
            static_assert(FnInfo::kSupported, "ItaniumMangle: cv/ref-qualified function types are not supported in COM signatures.");
            // `DoF<ret><params>E` is a single substitution unit (`F...E` is not
            // recorded separately inside a noexcept function type).
            if constexpr (FnInfo::kIsNoexcept) {
                m.out += "Do";
            }
            m.out += 'F';
            EncodeType<typename FnInfo::ReturnType>(m);
            EncodeBareFunctionParameters<typename FnInfo::ParameterTypes>(m);
            m.out += 'E';
            m.AddSubstitution(kToken);
        } else if constexpr (std::is_same_v<T, std::byte>) {
            // Itanium §5.1.8: `St` as a 2-char prefix is not recorded; the full
            // `St4byte` type is.
            m.out += "St4byte";
            m.AddSubstitution(kToken);
        } else if constexpr (std::is_member_pointer_v<T>) {
            static_assert(false, "ItaniumMangle: pointers to members are not supported in COM signatures.");
        } else if constexpr (std::is_class_v<T> || std::is_union_v<T> || std::is_enum_v<T>) {
            EncodeNamedType<T>(m, kToken);
        } else {
            static_assert(false, "ItaniumMangle: unsupported type in function signature.");
        }
    }
}

// Plain symbol lookup cannot verify a non-template function's return type.
// Use ResolveVerifiedSymbol (Contract.hpp) for a contract-checked DSO boundary.

// Even though non-template function symbols omit the return type from
// <bare-function-type>, compiler ABI tags on a return type (e.g. libstdc++'s
// `B5cxx11` on std::string) propagate onto the function name itself. Reject
// std:: types (other than std::byte / std::nullptr_t) anywhere in the return
// type so MangleFunction never silently omits an ABI tag.
template <typename T>
consteval void ValidateReturnType();

struct ReturnTemplateArgValidator {
    template <typename ArgT>
    consteval void OnType() {
        ValidateReturnType<ArgT>();
    }

    template <typename TemplateTag>
    consteval void OnTemplate() {
        constexpr auto kPolicy = ScanEnclosingScopes([](auto& v) consteval { TemplateTag::ForEachEnclosingScope(v); });
        static_assert(
            !kPolicy.hasStdNamespace,
            "ItaniumMangle: functions returning std:: types are not supported across ABI boundaries (return-type ABI tags such as B5cxx11 affect symbol names)."
        );
    }

    template <auto Value>
    consteval void OnValue() {
        using V = std::remove_cv_t<decltype(Value)>;
        if constexpr (std::is_enum_v<V>) {
            ValidateReturnType<V>();
        }
    }

    template <typename ValueT>
    consteval void OnOtherValue() {
        ValidateReturnType<ValueT>();
    }
};

struct OwnerReturnValidator {
    consteval void operator()(const Reflect::ScopeDescriptor&) const noexcept {
    }

    template <typename OwnerT>
    consteval void OnTypeScope(const Reflect::ScopeDescriptor& desc) const {
        if (desc.isTemplateSpecialization) {
            Reflect::ForEachTemplateArgument<OwnerT>(ReturnTemplateArgValidator {});
        }
    }
};

template <typename T>
consteval void ValidateReturnType() {
    using Bare = std::remove_cv_t<T>;
    if constexpr (std::is_void_v<Bare> || std::is_fundamental_v<Bare> || std::is_same_v<Bare, std::byte>) {
        return;
    } else if constexpr (std::is_pointer_v<Bare>) {
        ValidateReturnType<std::remove_pointer_t<Bare>>();
    } else if constexpr (std::is_reference_v<Bare>) {
        ValidateReturnType<std::remove_reference_t<Bare>>();
    } else if constexpr (std::is_array_v<Bare>) {
        ValidateReturnType<std::remove_extent_t<Bare>>();
    } else if constexpr (std::is_function_v<Bare>) {
        using FnInfo = ItaniumFunctionType<Bare>;
        if constexpr (FnInfo::kSupported || FnInfo::kCVariadic) {
            ValidateReturnType<typename FnInfo::ReturnType>();
            []<std::size_t... Is>(std::index_sequence<Is...>) consteval {
                (ValidateReturnType<std::tuple_element_t<Is, typename FnInfo::ParameterTypes>>(), ...);
            }(std::make_index_sequence<std::tuple_size_v<typename FnInfo::ParameterTypes>> {});
        }
    } else if constexpr (std::is_class_v<Bare> || std::is_union_v<Bare> || std::is_enum_v<Bare>) {
        constexpr Reflect::EntityDescriptor kDesc = Reflect::DescribeEntity<Bare>();
        static_assert(!kDesc.isFunctionLocal, "ItaniumMangle: functions returning function-local types are not supported.");
        constexpr auto kWalk   = [](auto& v) consteval { Reflect::ForEachEnclosingScope<Bare>(v); };
        constexpr auto kPolicy = ScanEnclosingScopes(kWalk);
        static_assert(
            !kPolicy.hasStdNamespace,
            "ItaniumMangle: functions returning std:: types are not supported across ABI boundaries (return-type ABI tags such as B5cxx11 affect symbol names)."
        );
        static_assert(!kPolicy.hasAnonymousNamespace, "ItaniumMangle: functions returning anonymous-namespace types are not supported.");
        if constexpr (kDesc.isTemplateSpecialization) {
            Reflect::ForEachTemplateArgument<Bare>(ReturnTemplateArgValidator {});
        }
        // Check enclosing template owners too (e.g. Box<std::string>::Handle).
        Reflect::ForEachEnclosingScope<Bare>(OwnerReturnValidator {});
    }
}

template <auto Fn>
[[nodiscard]] consteval auto MangleFunctionImpl() -> std::string_view {
#if defined(_MSC_VER)
    static_assert(false, "ItaniumMangle targets the Itanium C++ ABI; MSVC name decoration is not implemented.");
#endif
    using Traits = Reflect::FunctionTraits<Fn>;

    static_assert(!Traits::isSpecialMember, "ItaniumMangle: constructors, destructors, and special member functions are not supported.");
    static_assert(!Traits::isOperator, "ItaniumMangle: overloaded operator functions are not supported.");
    static_assert(!Traits::isConversion, "ItaniumMangle: conversion functions are not supported.");
    static_assert(!Traits::isTemplateSpecialization, "ItaniumMangle: function template specializations are not supported; use a non-template COM entry point.");
    static_assert(!Traits::isCVariadic, "ItaniumMangle: C variadic functions are not supported.");
    static_assert(!Traits::isFunctionLocal, "ItaniumMangle: local functions and members of function-local classes are not supported.");
    static_assert(Traits::hasExternalLinkage, "ItaniumMangle: function must have external linkage (not static or in an anonymous namespace).");
    static_assert(Traits::hasIdentifier && !Traits::name.empty(), "ItaniumMangle: function must have an ordinary C++ identifier.");

    constexpr auto kWalk   = [](auto& v) consteval { Traits::ForEachEnclosingScope(v); };
    constexpr auto kPolicy = ScanEnclosingScopes(kWalk);
    AssertValidScopes<kPolicy>();

    ValidateReturnType<typename Traits::ReturnType>();

    ItaniumMangler m {};
    m.out.reserve(64);
    m.out += "_Z";

    if constexpr (Traits::isGlobalScope) {
        m.AppendSourceName(Traits::name);
    } else {
        m.out += 'N';
        EmitEnclosingPrefix(m, kWalk);
        // The final function <unqualified-name> itself is NOT added to the
        // substitution table (Itanium C++ ABI §5.1.8).
        m.AppendSourceName(Traits::name);
        m.out += 'E';
    }

    EncodeBareFunctionParameters<typename Traits::ParameterTypes>(m);

    // Append a trailing NUL so `.data()` is directly usable with dlsym /
    // SharedLibrary::GetSymbol while `.size()` reports the exact symbol length.
    m.out.push_back('\0');
    const std::string_view stored = Reflect::DefineStaticString(m.out);
    return stored.substr(0, stored.size() - 1U);
}

} // namespace TemplatedDetail

// Computes the NUL-terminated Itanium C++ ABI symbol name of free or static
// member function `Fn` (e.g. `COM::MangleFunction<&COM::CreateAppLayer>()`).
template <auto Fn>
[[nodiscard]] consteval auto MangleFunction() -> std::string_view {
    return TemplatedDetail::MangleFunctionImpl<Fn>();
}

// Resolves C++-linkage function `Fn` from a ZHLN::SharedLibrary (or compatible
// loader providing `GetSymbol(const char*) -> void*`) via its consteval-mangled
// Itanium C++ ABI symbol name, keeping SharedLibrary free of reflection headers.
template <auto Fn, typename Library>
    requires requires(const Library& library, const char* symbolName) {
        { library.GetSymbol(symbolName) } -> std::convertible_to<void*>;
    }
[[nodiscard]] auto ResolveSymbol(const Library& library) noexcept -> decltype(Fn) {
    constexpr std::string_view kSymbol = MangleFunction<Fn>();
    return reinterpret_cast<decltype(Fn)>(library.GetSymbol(kSymbol.data()));
}

} // namespace ZHLN::COM
