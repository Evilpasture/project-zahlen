// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
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
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
static_assert(false, "ItaniumMangle targets the Itanium C++ ABI");
#endif

namespace ZHLN::COM {

// Computes the NUL-terminated Itanium C++ ABI symbol name of free or static
// member function `Fn` (e.g. `COM::MangleFunction<&COM::CreateAppLayerRaw>()`).
// The entire mangler lives in this function body: templates and partial
// specializations cannot be declared at block scope, so type decomposition
// reuses Reflect::FunctionTypeTraits (Callable.hpp), and scope/template-
// argument dispatch uses the Reflect tag fallbacks (Entity.hpp) instead of
// named member templates on a local visitor struct ([temp.mem]/2).
template <auto Fn>
[[nodiscard]] consteval auto MangleFunction() -> std::string_view {
    using Traits = Reflect::FunctionTraits<Fn>;

    static_assert(!Traits::isSpecialMember, "ItaniumMangle: constructors, destructors, and special member functions are not supported.");
    static_assert(!Traits::isOperator, "ItaniumMangle: overloaded operator functions are not supported.");
    static_assert(!Traits::isConversion, "ItaniumMangle: conversion functions are not supported.");
    static_assert(!Traits::isTemplateSpecialization, "ItaniumMangle: function template specializations are not supported; use a non-template COM entry point.");
    static_assert(!Traits::isCVariadic, "ItaniumMangle: C variadic functions are not supported.");
    static_assert(!Traits::isFunctionLocal, "ItaniumMangle: local functions and members of function-local classes are not supported.");
    static_assert(Traits::hasExternalLinkage, "ItaniumMangle: function must have external linkage (not static or in an anonymous namespace).");
    static_assert(Traits::hasIdentifier && !Traits::name.empty(), "ItaniumMangle: function must have an ordinary C++ identifier.");

    // Walk-mode tags: the walk is dispatched directly (no walk lambda is
    // passed across generic-lambda boundaries -- Clang-P2996 cannot deduce
    // a generic walk lambda's placeholder from another generic lambda).
    struct ReflectWalk {};
    struct TagWalk {};
    struct TraitsWalk {};
    struct ScopeStatus {
        bool hasStd = false, hasAnon = false, hasUnnamed = false, hasVariadic = false, hasOther = false;
    };
    static constexpr auto scanScopes = []<typename Mode, typename Entity>(Mode) consteval noexcept {
        ScopeStatus status {};
        auto        visitor = [&](auto scope) noexcept {
            const auto& d = scope.desc;
            if (d.kind == Reflect::EntityKind::Namespace) {
                if (!d.hasIdentifier) {
                    status.hasAnon = true;
                } else if (d.isTopLevel && d.name == "std") {
                    status.hasStd = true;
                }
            } else if (d.kind == Reflect::EntityKind::Class || d.kind == Reflect::EntityKind::Union || d.kind == Reflect::EntityKind::Enum) {
                if (!d.hasIdentifier) {
                    status.hasUnnamed = true;
                }
                if (d.isVariadicSpecialization) {
                    status.hasVariadic = true;
                }
            } else {
                status.hasOther = true;
            }
        };
        if constexpr (std::is_same_v<Mode, TagWalk>) {
            Entity::ForEachEnclosingScope(visitor);
        } else if constexpr (std::is_same_v<Mode, TraitsWalk>) {
            Traits::ForEachEnclosingScope(visitor);
        } else {
            Reflect::ForEachEnclosingScope<Entity>(visitor);
        }
        return status;
    };
    static constexpr auto assertScopes = []<auto Status>() consteval noexcept {
        static_assert(!Status.hasAnon, "ItaniumMangle: entities in anonymous namespaces are not supported.");
        static_assert(!Status.hasStd, "ItaniumMangle: std:: entities (other than std::byte) are not supported across ABI boundaries.");
        static_assert(!Status.hasUnnamed, "ItaniumMangle: unnamed enclosing scopes are not supported.");
        static_assert(!Status.hasVariadic, "ItaniumMangle: variadic template enclosing scopes are not supported.");
        static_assert(!Status.hasOther, "ItaniumMangle: local or unsupported enclosing scopes are not supported.");
    };

    constexpr auto kFnPolicy = scanScopes.template operator()<TraitsWalk, void>(TraitsWalk {});
    assertScopes.template                          operator()<kFnPolicy>();

    // Return-type validation: compiler ABI tags on a return type (e.g.
    // libstdc++ B5cxx11 on std::string) propagate onto the function name.
    auto validateReturn = [&]<typename T>(this auto&& self) consteval -> void {
        using Bare = std::remove_cv_t<T>;
        if constexpr (std::is_void_v<Bare> || std::is_fundamental_v<Bare> || std::is_same_v<Bare, std::byte>) {
            return;
        } else if constexpr (std::is_pointer_v<Bare>) {
            self.template operator()<std::remove_pointer_t<Bare>>();
        } else if constexpr (std::is_reference_v<Bare>) {
            self.template operator()<std::remove_reference_t<Bare>>();
        } else if constexpr (std::is_array_v<Bare>) {
            self.template operator()<std::remove_extent_t<Bare>>();
        } else if constexpr (std::is_function_v<Bare>) {
            using FnInfo = Reflect::FunctionTypeTraits<Bare>;
            if constexpr (FnInfo::hasSignature || FnInfo::isCVariadic) {
                self.template operator()<typename FnInfo::ReturnType>();
                [&]<std::size_t... Is>(std::index_sequence<Is...>) consteval {
                    (self.template operator()<std::tuple_element_t<Is, typename FnInfo::ParameterTypes>>(), ...);
                }(std::make_index_sequence<std::tuple_size_v<typename FnInfo::ParameterTypes>> {});
            }
        } else if constexpr (std::is_class_v<Bare> || std::is_union_v<Bare> || std::is_enum_v<Bare>) {
            constexpr auto kDesc = Reflect::DescribeEntity<Bare>();
            static_assert(!kDesc.isFunctionLocal, "ItaniumMangle: functions returning function-local types are not supported.");
            constexpr auto kPolicy = scanScopes.template operator()<ReflectWalk, Bare>(ReflectWalk {});
            static_assert(
                !kPolicy.hasStd, "ItaniumMangle: functions returning std:: types are not supported across ABI boundaries (return-type ABI tags such as B5cxx11 "
                                 "affect symbol names)."
            );
            static_assert(!kPolicy.hasAnon, "ItaniumMangle: functions returning anonymous-namespace types are not supported.");
            auto checkArgs = [&]<typename Spec>() consteval {
                Reflect::ForEachTemplateArgument<Spec>([&](auto arg) consteval {
                    using A = decltype(arg);
                    if constexpr (requires { typename A::type; }) {
                        self.template operator()<typename A::type>();
                    } else if constexpr (requires { typename A::template_tag; }) {
                        constexpr auto kTP = scanScopes.template operator()<TagWalk, typename A::template_tag>(TagWalk {});
                        static_assert(
                            !kTP.hasStd, "ItaniumMangle: functions returning std:: types are not supported across ABI boundaries (return-type ABI tags such as "
                                         "B5cxx11 affect symbol names)."
                        );
                    } else if constexpr (requires { A::value; }) {
                        using V = std::remove_cv_t<decltype(A::value)>;
                        if constexpr (std::is_enum_v<V>) {
                            self.template operator()<V>();
                        }
                    } else {
                        self.template operator()<typename A::other_type>();
                    }
                });
            };
            if constexpr (kDesc.isTemplateSpecialization) {
                checkArgs.template operator()<Bare>();
            }
            Reflect::ForEachEnclosingScope<Bare>([&](auto scope) consteval {
                using OwnerT = typename decltype(scope)::owner_type;
                if constexpr (!std::is_void_v<OwnerT>) {
                    if (scope.desc.isTemplateSpecialization) {
                        checkArgs.template operator()<OwnerT>();
                    }
                }
            });
        }
    };
    validateReturn.template operator()<typename Traits::ReturnType>();

    // Mangler state: a plain non-templated local struct (legal at block scope).
    struct {
        std::string                       out;
        std::vector<Reflect::EntityToken> subs;
        consteval void                    AppendDecimal(uint64_t value) {
            std::array<char, 20> buf {};
            unsigned             n = 0;
            do {
                buf[n++] = static_cast<char>('0' + (value % 10U));
                value /= 10U;
            } while (value != 0U);
            while (n != 0U) {
                out += buf[--n];
            }
        }
        consteval void AppendBase36(std::size_t value) {
            constexpr std::string_view kDigits = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
            std::array<char, 16>       buf {};
            unsigned                   n = 0;
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
        [[nodiscard]] consteval auto Substitute(Reflect::EntityToken token) -> bool {
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
        consteval void AppendUnsignedLiteral(uint64_t value) {
            AppendDecimal(value);
        }
        consteval void AppendSignedLiteral(int64_t value) {
            if (value < 0) {
                out += 'n';
                AppendDecimal(uint64_t {0} - static_cast<uint64_t>(value));
            } else {
                AppendDecimal(static_cast<uint64_t>(value));
            }
        }
    } m {};

    struct DoType {};
    struct DoNamed {};
    struct DoTmplArgs {};
    struct DoTmplTmpl {};
    struct DoBareParams {};

    // Emits an Itanium <prefix>, substituting the deepest already-recorded
    // prefix (or enclosing <template-prefix>) first. `onTmplArgs` is a
    // templated lambda supplied by the caller (step, or MangleFunction
    // itself for the function-name prefix), so this is defined once.
    auto emitPrefix = [&]<typename Mode, typename Entity>(Mode, auto&& onTmplArgs) consteval {
        struct {
            std::size_t idx = 0, sub = 0;
            bool        hit = false, tmplOnly = false;
        } best {};
        std::size_t findCur = 0;
        auto        finder  = [&](auto scope) noexcept {
            const auto& d = scope.desc;
            if (auto [h, i] = m.FindSubstitution(d.id); h) {
                best = {findCur, i, true, false};
            } else if (d.isTemplateSpecialization) {
                if (auto [th, ti] = m.FindSubstitution(d.templateId); th) {
                    best = {findCur, ti, true, true};
                }
            }
            ++findCur;
        };
        if constexpr (std::is_same_v<Mode, TagWalk>) {
            Entity::ForEachEnclosingScope(finder);
        } else if constexpr (std::is_same_v<Mode, TraitsWalk>) {
            Traits::ForEachEnclosingScope(finder);
        } else {
            Reflect::ForEachEnclosingScope<Entity>(finder);
        }
        if (best.hit) {
            m.AppendSubstitution(best.sub);
        }
        std::size_t emitCur = 0;
        auto        emitter = [&](auto scope) consteval {
            const std::size_t idx = emitCur++;
            using OwnerT          = typename decltype(scope)::owner_type;
            const auto& d         = scope.desc;
            if constexpr (!std::is_void_v<OwnerT>) {
                if (best.hit && idx < best.idx) {
                    return;
                }
                if (best.hit && idx == best.idx) {
                    if (best.tmplOnly) {
                        onTmplArgs.template operator()<OwnerT>();
                        m.AddSubstitution(d.id);
                    }
                    return;
                }
                if (d.isTemplateSpecialization) {
                    m.AppendSourceName(d.name);
                    m.AddSubstitution(d.templateId);
                    onTmplArgs.template operator()<OwnerT>();
                    m.AddSubstitution(d.id);
                } else {
                    m.AppendSourceName(d.name);
                    m.AddSubstitution(d.id);
                }
            } else {
                if (best.hit && idx <= best.idx) {
                    return;
                }
                m.AppendSourceName(d.name);
                m.AddSubstitution(d.id);
            }
        };
        if constexpr (std::is_same_v<Mode, TagWalk>) {
            Entity::ForEachEnclosingScope(emitter);
        } else if constexpr (std::is_same_v<Mode, TraitsWalk>) {
            Traits::ForEachEnclosingScope(emitter);
        } else {
            Reflect::ForEachEnclosingScope<Entity>(emitter);
        }
    };

    auto step = [&]<typename Op, typename T>(this auto&& self) consteval -> void {
        if constexpr (std::is_same_v<Op, DoBareParams>) {
            if constexpr (std::tuple_size_v<T> == 0U) {
                m.out += 'v';
            } else {
                [&]<std::size_t... Is>(std::index_sequence<Is...>) consteval {
                    (self.template operator()<DoType, std::tuple_element_t<Is, T>>(), ...);
                }(std::make_index_sequence<std::tuple_size_v<T>> {});
            }
        } else if constexpr (std::is_same_v<Op, DoTmplArgs>) {
            m.out += 'I';
            Reflect::ForEachTemplateArgument<T>([&](auto arg) consteval {
                using A = decltype(arg);
                if constexpr (requires { typename A::type; }) {
                    self.template operator()<DoType, typename A::type>();
                } else if constexpr (requires { typename A::template_tag; }) {
                    self.template operator()<DoTmplTmpl, typename A::template_tag>();
                } else if constexpr (requires { A::value; }) {
                    constexpr auto Value = A::value;
                    using V              = std::remove_cv_t<decltype(Value)>;
                    if constexpr (std::is_null_pointer_v<V>) {
                        // TODO(Evilpasture): This affects crosscompiler ABI stuff so review when time is available.
#if defined(__GNUC__) && !defined(__clang__)
                        m.out += "LDnE";
#else
                        m.out += "LDn0E";
#endif
                    } else if constexpr (std::is_same_v<V, bool>) {
                        m.out += Value ? "Lb1E" : "Lb0E";
                    } else if constexpr (std::is_integral_v<V> && sizeof(V) <= sizeof(uint64_t)) {
                        m.out += 'L';
                        self.template operator()<DoType, V>();
                        if constexpr (std::is_signed_v<V>) {
                            m.AppendSignedLiteral(static_cast<int64_t>(Value));
                        } else {
                            m.AppendUnsignedLiteral(static_cast<uint64_t>(Value));
                        }
                        m.out += 'E';
                    } else if constexpr (std::is_enum_v<V> && sizeof(V) <= sizeof(uint64_t)) {
                        using Underlying = std::underlying_type_t<V>;
                        m.out += 'L';
                        self.template operator()<DoType, V>();
                        if constexpr (std::is_signed_v<Underlying>) {
                            m.AppendSignedLiteral(static_cast<int64_t>(static_cast<Underlying>(Value)));
                        } else {
                            m.AppendUnsignedLiteral(static_cast<uint64_t>(static_cast<Underlying>(Value)));
                        }
                        m.out += 'E';
                    } else if constexpr (std::is_floating_point_v<V>) {
                        static_assert(false, "ItaniumMangle: floating-point non-type template arguments are not supported in COM signatures.");
                    } else {
                        static_assert(false, "ItaniumMangle: unsupported non-type template argument width or type.");
                    }
                } else {
                    static_assert(false, "ItaniumMangle: pointer, reference, or class non-type template arguments are not supported in COM signatures.");
                }
            });
            m.out += 'E';
        } else if constexpr (std::is_same_v<Op, DoTmplTmpl>) {
            constexpr auto kDesc = T::descriptor;
            static_assert(!kDesc.isFunctionLocal, "ItaniumMangle: function-local templates are not supported.");
            static_assert(kDesc.hasIdentifier, "ItaniumMangle: unnamed template-template arguments are not supported.");
            constexpr auto kPolicy = scanScopes.template operator()<TagWalk, T>(TagWalk {});
            assertScopes.template                        operator()<kPolicy>();
            if (m.Substitute(kDesc.id)) {
                return;
            }
            if constexpr (kDesc.isTopLevel) {
                m.AppendSourceName(kDesc.name);
            } else {
                m.out += 'N';
                emitPrefix.template operator()<TagWalk, T>(TagWalk {}, [&]<typename OwnerT>() consteval { self.template operator()<DoTmplArgs, OwnerT>(); });
                m.AppendSourceName(kDesc.name);
                m.out += 'E';
            }
            m.AddSubstitution(kDesc.id);
        } else if constexpr (std::is_same_v<Op, DoNamed>) {
            constexpr auto kDesc = Reflect::DescribeEntity<T>();
            static_assert(!kDesc.isFunctionLocal, "ItaniumMangle: function-local types are not supported in COM signatures.");
            static_assert(kDesc.hasIdentifier, "ItaniumMangle: unnamed classes, unions, enums, and lambdas are not supported in COM signatures.");
            static_assert(!kDesc.isVariadicSpecialization, "ItaniumMangle: variadic class template specializations are not supported in COM signatures.");
            constexpr auto kPolicy = scanScopes.template operator()<ReflectWalk, T>(ReflectWalk {});
            assertScopes.template                        operator()<kPolicy>();
            if constexpr (kDesc.isTopLevel) {
                if constexpr (kDesc.isTemplateSpecialization) {
                    if (!m.Substitute(kDesc.templateId)) {
                        m.AppendSourceName(kDesc.name);
                        m.AddSubstitution(kDesc.templateId);
                    }
                    self.template operator()<DoTmplArgs, T>();
                } else {
                    m.AppendSourceName(kDesc.name);
                }
            } else {
                m.out += 'N';
                if constexpr (kDesc.isTemplateSpecialization) {
                    if (!m.Substitute(kDesc.templateId)) {
                        emitPrefix.template operator()<ReflectWalk, T>(ReflectWalk {}, [&]<typename OwnerT>() consteval {
                            self.template operator()<DoTmplArgs, OwnerT>();
                        });
                        m.AppendSourceName(kDesc.name);
                        m.AddSubstitution(kDesc.templateId);
                    }
                    self.template operator()<DoTmplArgs, T>();
                } else {
                    emitPrefix.template operator()<ReflectWalk, T>(ReflectWalk {}, [&]<typename OwnerT>() consteval {
                        self.template operator()<DoTmplArgs, OwnerT>();
                    });
                    m.AppendSourceName(kDesc.name);
                }
                m.out += 'E';
            }
            m.AddSubstitution(Reflect::EntityTokenOf<T>());
        } else { // DoType
            // Builtin codes inlined: calling a generic lambda from inside
            // `step` is not instantiated by Clang-P2996 in consteval
            // ("undefined function operator()<T>"), so no builtinCode lambda.
            if constexpr (std::is_same_v<T, void>) {
                m.out += 'v';
            } else if constexpr (std::is_same_v<T, wchar_t>) {
                m.out += 'w';
            } else if constexpr (std::is_same_v<T, bool>) {
                m.out += 'b';
            } else if constexpr (std::is_same_v<T, char>) {
                m.out += 'c';
            } else if constexpr (std::is_same_v<T, signed char>) {
                m.out += 'a';
            } else if constexpr (std::is_same_v<T, unsigned char>) {
                m.out += 'h';
            } else if constexpr (std::is_same_v<T, short>) {
                m.out += 's';
            } else if constexpr (std::is_same_v<T, unsigned short>) {
                m.out += 't';
            } else if constexpr (std::is_same_v<T, int>) {
                m.out += 'i';
            } else if constexpr (std::is_same_v<T, unsigned int>) {
                m.out += 'j';
            } else if constexpr (std::is_same_v<T, long>) {
                m.out += 'l';
            } else if constexpr (std::is_same_v<T, unsigned long>) {
                m.out += 'm';
            } else if constexpr (std::is_same_v<T, long long>) {
                m.out += 'x';
            } else if constexpr (std::is_same_v<T, unsigned long long>) {
                m.out += 'y';
            } else if constexpr (std::is_same_v<T, float>) {
                m.out += 'f';
            } else if constexpr (std::is_same_v<T, double>) {
                m.out += 'd';
            } else if constexpr (std::is_same_v<T, long double>) {
                m.out += 'e';
            } else if constexpr (std::is_same_v<T, char8_t>) {
                m.out += "Du";
            } else if constexpr (std::is_same_v<T, char16_t>) {
                m.out += "Ds";
            } else if constexpr (std::is_same_v<T, char32_t>) {
                m.out += "Di";
            } else if constexpr (std::is_same_v<T, std::nullptr_t>) {
                m.out += "Dn";
            } else {
                constexpr auto kToken = Reflect::EntityTokenOf<T>();
                if (m.Substitute(kToken)) {
                    return;
                }
                if constexpr (std::is_bounded_array_v<T>) {
                    m.out += 'A';
                    m.AppendDecimal(std::extent_v<T>);
                    m.out += '_';
                    self.template operator()<DoType, std::remove_extent_t<T>>();
                    m.AddSubstitution(kToken);
                } else if constexpr (std::is_unbounded_array_v<T>) {
                    m.out += "A_";
                    self.template operator()<DoType, std::remove_extent_t<T>>();
                    m.AddSubstitution(kToken);
                } else if constexpr (std::is_const_v<T> || std::is_volatile_v<T>) {
                    if constexpr (std::is_volatile_v<T>) {
                        m.out += 'V';
                    }
                    if constexpr (std::is_const_v<T>) {
                        m.out += 'K';
                    }
                    self.template operator()<DoType, std::remove_cv_t<T>>();
                    m.AddSubstitution(kToken);
                } else if constexpr (std::is_lvalue_reference_v<T>) {
                    m.out += 'R';
                    self.template operator()<DoType, std::remove_reference_t<T>>();
                    m.AddSubstitution(kToken);
                } else if constexpr (std::is_rvalue_reference_v<T>) {
                    m.out += 'O';
                    self.template operator()<DoType, std::remove_reference_t<T>>();
                    m.AddSubstitution(kToken);
                } else if constexpr (std::is_pointer_v<T>) {
                    m.out += 'P';
                    self.template operator()<DoType, std::remove_pointer_t<T>>();
                    m.AddSubstitution(kToken);
                } else if constexpr (std::is_function_v<T>) {
                    using FnInfo = Reflect::FunctionTypeTraits<T>;
                    static_assert(!FnInfo::isCVariadic, "ItaniumMangle: C variadic function types are not supported in COM signatures.");
                    static_assert(FnInfo::hasSignature, "ItaniumMangle: cv/ref-qualified function types are not supported in COM signatures.");
                    if constexpr (FnInfo::isNoexcept) {
                        m.out += "Do";
                    }
                    m.out += 'F';
                    self.template operator()<DoType, typename FnInfo::ReturnType>();
                    self.template operator()<DoBareParams, typename FnInfo::ParameterTypes>();
                    m.out += 'E';
                    m.AddSubstitution(kToken);
                } else if constexpr (std::is_same_v<T, std::byte>) {
                    m.out += "St4byte";
                    m.AddSubstitution(kToken);
                } else if constexpr (std::is_member_pointer_v<T>) {
                    static_assert(false, "ItaniumMangle: pointers to members are not supported in COM signatures.");
                } else if constexpr (std::is_class_v<T> || std::is_union_v<T> || std::is_enum_v<T>) {
                    self.template operator()<DoNamed, T>();
                } else {
                    static_assert(false, "ItaniumMangle: unsupported type in function signature.");
                }
            }
        }
    };

    m.out.reserve(64);
    m.out += "_Z";

    if constexpr (Traits::isGlobalScope) {
        m.AppendSourceName(Traits::name);
    } else {
        m.out += 'N';
        emitPrefix.template operator()<TraitsWalk, void>(TraitsWalk {}, [&]<typename OwnerT>() consteval { step.template operator()<DoTmplArgs, OwnerT>(); });
        // The final function <unqualified-name> itself is NOT added to the
        // substitution table (Itanium C++ ABI §5.1.8).
        m.AppendSourceName(Traits::name);
        m.out += 'E';
    }

    step.template operator()<DoBareParams, typename Traits::ParameterTypes>();

    // Trailing NUL so `.data()` is directly usable with dlsym while
    // `.size()` reports the exact symbol length.
    m.out.push_back('\0');
    const std::string_view stored = Reflect::DefineStaticString(m.out);
    return stored.substr(0, stored.size() - 1U);
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
