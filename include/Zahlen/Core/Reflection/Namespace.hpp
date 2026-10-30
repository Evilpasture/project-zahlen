// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <Zahlen/Core/Reflection/Class.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#if ZHLN_REFLECTION_AVAILABLE
#include <meta>
#include <vector>
#endif

namespace ZHLN::Reflect {

inline constexpr auto AcceptAnyIdentifier = [](std::string_view) constexpr noexcept -> bool { return true; };

#if ZHLN_REFLECTION_AVAILABLE

namespace TemplatedDetail {

consteval auto NormalizePathSeparator(char c) noexcept -> char {
    return c == '\\' ? '/' : c;
}

consteval auto MatchesSourceLocationFile(std::string_view candidate, std::string_view wanted) noexcept -> bool {
    if (wanted.empty()) {
        return true;
    }
    const std::size_t candLen = candidate.size();
    const std::size_t wantLen = wanted.size();
    if (candLen < wantLen) {
        return false;
    }
    const std::size_t offset = candLen - wantLen;
    // Compare from the trailing character backwards so non-matching header files
    // (e.g. ".h"/".hpp" vs ".cpp") reject on the very first comparison without
    // burning constexpr evaluation steps across large global namespaces.
    for (std::size_t i = wantLen; i-- > 0;) {
        const char a = candidate[offset + i];
        const char b = wanted[i];
        if (a != b && NormalizePathSeparator(a) != NormalizePathSeparator(b)) {
            return false;
        }
    }
    if (offset == 0) {
        return true;
    }
    const char boundary = NormalizePathSeparator(candidate[offset - 1]);
    return NormalizePathSeparator(wanted.front()) != '/' && boundary == '/';
}

template <typename ScopeAnchor>
consteval auto ScopeNamespaceOf() -> std::meta::info {
    if constexpr (std::is_void_v<ScopeAnchor>) {
        return ^^::;
    } else {
        constexpr auto anchorInfo = std::meta::dealias(^^std::remove_cvref_t<ScopeAnchor>);
        constexpr auto scopeInfo  = std::meta::parent_of(anchorInfo);
        static_assert(std::meta::is_namespace(scopeInfo), "ScopeAnchor must be declared directly inside a namespace.");
        return scopeInfo;
    }
}

template <std::meta::info EntityInfo, typename StopScopeAnchor = void>
consteval auto EnclosingNamespacesOf() -> std::span<const std::meta::info> {
    std::vector<std::meta::info> chain;
    std::meta::info              cur = std::meta::parent_of(EntityInfo);
    while (cur != ^^::) {
        if (std::meta::is_namespace(cur)) {
            chain.push_back(cur);
            if constexpr (!std::is_void_v<StopScopeAnchor>) {
                if (cur == ScopeNamespaceOf<StopScopeAnchor>()) {
                    break;
                }
            }
        }
        cur = std::meta::parent_of(cur);
    }
    std::reverse(chain.begin(), chain.end());
    return std::define_static_array(chain);
}

template <auto IdentifierFilter>
consteval void WalkNamespaceFunctions(
    std::meta::info               ns,
    std::string_view              sourceFile,
    bool                          recurseNamedSubnamespaces,
    bool                          includeUnnamedSubnamespaces,
    std::vector<std::meta::info>& out
) {
    const auto ctx = std::meta::access_context::current();
    for (const std::meta::info m: std::meta::members_of(ns, ctx)) {
        if (std::meta::is_function(m)) {
            if (!std::meta::is_function_template(m) && !std::meta::is_deleted(m) &&
                MatchesSourceLocationFile(std::meta::source_location_of(m).file_name(), sourceFile) && std::meta::has_identifier(m) &&
                IdentifierFilter(std::meta::identifier_of(m))) {
                out.push_back(m);
            }
        } else if (std::meta::is_namespace(m) && !std::meta::is_namespace_alias(m)) {
            const bool hasId = std::meta::has_identifier(m);
            if ((hasId && recurseNamedSubnamespaces) || (!hasId && includeUnnamedSubnamespaces)) {
                WalkNamespaceFunctions<IdentifierFilter>(m, sourceFile, recurseNamedSubnamespaces, includeUnnamedSubnamespaces, out);
            }
        }
    }
}

template <auto IdentifierFilter>
consteval void WalkNamespaceClasses(
    std::meta::info               ns,
    std::string_view              sourceFile,
    bool                          recurseNamedSubnamespaces,
    bool                          includeUnnamedSubnamespaces,
    std::vector<std::meta::info>& out
) {
    const auto ctx = std::meta::access_context::current();
    for (const std::meta::info m: std::meta::members_of(ns, ctx)) {
        if (std::meta::is_type(m)) {
            if (!std::meta::is_type_alias(m) && MatchesSourceLocationFile(std::meta::source_location_of(m).file_name(), sourceFile) &&
                std::meta::is_class_type(m) && std::meta::is_complete_type(m) && std::meta::has_identifier(m) &&
                IdentifierFilter(std::meta::identifier_of(m))) {
                if (std::find(out.begin(), out.end(), m) == out.end()) {
                    out.push_back(m);
                }
            }
        } else if (std::meta::is_namespace(m) && !std::meta::is_namespace_alias(m)) {
            const bool hasId = std::meta::has_identifier(m);
            if ((hasId && recurseNamedSubnamespaces) || (!hasId && includeUnnamedSubnamespaces)) {
                WalkNamespaceClasses<IdentifierFilter>(m, sourceFile, recurseNamedSubnamespaces, includeUnnamedSubnamespaces, out);
            }
        }
    }
}

template <typename ScopeAnchor, ZHLN::StringLiteral SourceFile, auto IdentifierFilter, bool RecurseNamedSubnamespaces, bool IncludeUnnamedSubnamespaces>
consteval auto CollectNamespaceFunctions() -> std::span<const std::meta::info> {
    std::vector<std::meta::info> fns;
    WalkNamespaceFunctions<IdentifierFilter>(ScopeNamespaceOf<ScopeAnchor>(), SourceFile, RecurseNamedSubnamespaces, IncludeUnnamedSubnamespaces, fns);
    return std::define_static_array(fns);
}

template <typename ScopeAnchor, ZHLN::StringLiteral SourceFile, auto IdentifierFilter, bool RecurseNamedSubnamespaces, bool IncludeUnnamedSubnamespaces>
consteval auto CollectNamespaceClasses() -> std::span<const std::meta::info> {
    std::vector<std::meta::info> classes;
    WalkNamespaceClasses<IdentifierFilter>(ScopeNamespaceOf<ScopeAnchor>(), SourceFile, RecurseNamedSubnamespaces, IncludeUnnamedSubnamespaces, classes);
    return std::define_static_array(classes);
}

} // namespace TemplatedDetail

template <std::meta::info FnInfo>
struct ReflectedFreeFunction {
    static constexpr std::string_view name  = std::meta::identifier_of(FnInfo);
    static constexpr std::size_t      arity = std::meta::parameters_of(FnInfo).size();
    using ReturnType                        = typename[:std::meta::return_type_of(FnInfo):];

    template <std::size_t Index>
    using ParameterType = typename[:std::meta::type_of(std::meta::parameters_of(FnInfo)[Index]):];

    template <typename... Args>
    static constexpr decltype(auto) Invoke(Args&&... args) {
        return ([:FnInfo:])(std::forward<Args>(args)...);
    }

    template <typename F>
    static consteval void ForEachAnnotationType(F&& f) {
        Reflect::ForEachAnnotationType<FnInfo>(std::forward<F>(f));
    }

    template <typename StopScopeAnchor = void, typename F>
    static consteval void ForEachEnclosingNamespaceAnnotationType(F&& f) {
        [:Expand(TemplatedDetail::EnclosingNamespacesOf<FnInfo, StopScopeAnchor>()):] >> [&]<auto ns>() { Reflect::ForEachAnnotationType<ns>(f); };
    }

    static constexpr std::size_t annotationCount = TemplatedDetail::EntityAnnotationCount<FnInfo>();

    template <std::size_t Index>
    static consteval auto AnnotationConstantAt() {
        return TemplatedDetail::ExtractAnnotationConstantAt<FnInfo, Index>();
    }

    template <typename F>
    static constexpr void ForEachAnnotationConstant(F&& f) {
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (f.template operator()<AnnotationConstantAt<Is>()>(), ...);
        }(std::make_index_sequence<annotationCount> {});
    }

    template <typename StopScopeAnchor = void, typename F>
    static constexpr void ForEachEnclosingNamespaceAnnotationConstant(F&& f) {
        [:Expand(TemplatedDetail::EnclosingNamespacesOf<FnInfo, StopScopeAnchor>()):] >> [&]<auto ns>() {
            constexpr std::size_t nsAnnCount = TemplatedDetail::EntityAnnotationCount<ns>();
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                (f.template operator()<TemplatedDetail::ExtractAnnotationConstantAt<ns, Is>()>(), ...);
            }(std::make_index_sequence<nsAnnCount> {});
        };
    }
};

template <
    typename ScopeAnchor                            = void,
    ZHLN::StringLiteral SourceFile                  = "",
    auto                IdentifierFilter            = AcceptAnyIdentifier,
    bool                RecurseNamedSubnamespaces   = true,
    bool                IncludeUnnamedSubnamespaces = true,
    typename F>
constexpr void ForEachNamespaceFunction(F&& f) {
    [:Expand(
          TemplatedDetail::CollectNamespaceFunctions<ScopeAnchor, SourceFile, IdentifierFilter, RecurseNamedSubnamespaces, IncludeUnnamedSubnamespaces>()
      ):] >> [&]<auto fn>() { f.template operator()<ReflectedFreeFunction<fn>>(); };
}

template <
    typename ScopeAnchor                            = void,
    ZHLN::StringLiteral SourceFile                  = "",
    auto                IdentifierFilter            = AcceptAnyIdentifier,
    bool                RecurseNamedSubnamespaces   = true,
    bool                IncludeUnnamedSubnamespaces = true,
    typename F>
constexpr void ForEachNamespaceClass(F&& f) {
    [:Expand(
          TemplatedDetail::CollectNamespaceClasses<ScopeAnchor, SourceFile, IdentifierFilter, RecurseNamedSubnamespaces, IncludeUnnamedSubnamespaces>()
      ):] >> [&]<auto cls>() {
        using ClassType = typename[:cls:];
        f.template operator()<ClassType>();
    };
}

#else

template <
    typename ScopeAnchor                            = void,
    ZHLN::StringLiteral SourceFile                  = "",
    auto                IdentifierFilter            = AcceptAnyIdentifier,
    bool                RecurseNamedSubnamespaces   = true,
    bool                IncludeUnnamedSubnamespaces = true,
    typename F>
constexpr void ForEachNamespaceFunction(F&&) {
}

template <
    typename ScopeAnchor                            = void,
    ZHLN::StringLiteral SourceFile                  = "",
    auto                IdentifierFilter            = AcceptAnyIdentifier,
    bool                RecurseNamedSubnamespaces   = true,
    bool                IncludeUnnamedSubnamespaces = true,
    typename F>
constexpr void ForEachNamespaceClass(F&&) {
}

#endif

} // namespace ZHLN::Reflect
