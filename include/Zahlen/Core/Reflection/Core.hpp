// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <string_view>
#include <type_traits>

// GCC without Clang's __has_feature cannot parse even a short-circuited
// __has_feature(...) expression in #if; probe it only inside its own guard.
#if defined(__cpp_impl_reflection)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ZHLN_REFLECTION_AVAILABLE 1
#elif defined(__has_feature)
#if __has_feature(reflection)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ZHLN_REFLECTION_AVAILABLE 1
#else
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ZHLN_REFLECTION_AVAILABLE 0
#endif
#else
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ZHLN_REFLECTION_AVAILABLE 0
#endif

#if ZHLN_REFLECTION_AVAILABLE
#include <meta>
#include <string>
#include <vector>
#endif

namespace ZHLN::Reflect {

template <typename T>
constexpr auto IsBracesConstructible() -> bool {
    return std::is_aggregate_v<std::remove_cvref_t<T>>;
}

inline constexpr bool ReflectionAvailable = ZHLN_REFLECTION_AVAILABLE != 0;

#if ZHLN_REFLECTION_AVAILABLE


namespace TemplatedDetail {

template <std::meta::info... vals>
struct ReplicatorType {
    template <typename F>
    constexpr void operator>>([[maybe_unused]] F body) const {
        (body.template operator()<vals>(), ...);
    }
};

template <std::meta::info... vals>
ReplicatorType<vals...> Replicator {};

template <typename T>
struct TypeReflector {
    static consteval auto name() -> std::string_view {
        constexpr auto info = std::meta::dealias(^^T);
        if constexpr (std::meta::has_identifier(info)) {
            return std::meta::identifier_of(info);
        } else {
            return std::meta::display_string_of(info);
        }
    }
};

// display_string_of is implementation-defined: some compilers include scope,
// others return only the final identifier. Walk parents to obtain the scope
// instead. Specializations have no identifier, so retain their display spelling
// but remove only scope separators outside template argument lists.
consteval auto LocalTypeSpelling(std::meta::info info) -> std::string_view {
    if (std::meta::has_identifier(info)) {
        return std::meta::identifier_of(info);
    }
    if (std::meta::is_namespace(info)) {
        return "(anonymous namespace)";
    }
    const std::string_view display = std::meta::display_string_of(info);
    size_t start = 0;
    unsigned depth = 0;
    for (size_t i = 0; i + 1 < display.size(); ++i) {
        if (display[i] == '<') ++depth;
        else if (display[i] == '>' && depth != 0) --depth;
        else if (depth == 0 && display[i] == ':' && display[i + 1] == ':') {
            start = i + 2;
            ++i;
        }
    }
    return display.substr(start);
}

consteval auto QualifiedTypeSpelling(std::meta::info info) -> std::string {
    std::vector<std::string_view> parts;
    for (auto current = info; current != ^^::; current = std::meta::parent_of(current)) {
        parts.push_back(LocalTypeSpelling(current));
    }
    std::string name;
    for (size_t i = parts.size(); i != 0; --i) {
        if (!name.empty()) name += "::";
        name += parts[i - 1];
    }
    return name;
}

}

template <typename R>
consteval auto Expand(R&& range) {
    std::vector<std::meta::info> args;
    args.reserve(range.size());
    for (auto r: range) {
        args.push_back(std::meta::reflect_constant(r));
    }
    return std::meta::substitute(^^TemplatedDetail::Replicator, args);
}

template <typename T>
consteval auto TypeName() -> std::string_view {
    return TemplatedDetail::TypeReflector<std::remove_cvref_t<T>>::name();
}

template <typename T, typename NameOverride>
consteval auto TypeName(NameOverride rename) -> std::string_view {
    const std::string_view spelling   = TypeName<T>();
    const char*            overridden = rename(spelling);
    if (overridden != nullptr) {
        return overridden;
    }
    return spelling;
}

consteval auto DefineStaticString(std::string_view sv) -> std::string_view {
    const auto span = std::define_static_array(sv);
    return std::string_view(span.data(), span.size());
}

// Presentation-only compiler spelling. Never use this for canonical IDs.
template <typename T>
consteval auto QualifiedTypeName() -> std::string_view {
    return DefineStaticString(std::meta::display_string_of(std::meta::dealias(^^std::remove_cvref_t<T>)));
}

// Component names need actual declaring scopes even when a compiler's display
// spelling omits them. The returned view has static storage.
template <typename T>
consteval auto ScopedTypeName() -> std::string_view {
    constexpr auto info = std::meta::dealias(^^std::remove_cvref_t<T>);
    if constexpr (std::meta::is_class_type(info) || std::meta::is_enum_type(info)) {
        return DefineStaticString(TemplatedDetail::QualifiedTypeSpelling(info));
    } else {
        return DefineStaticString(std::meta::display_string_of(info));
    }
}

#else

// The stub era is over: no ZHLN_ALLOW_REFLECTION_STUBS exemption, no
// __PRETTY_FUNCTION__ name extraction. A translation unit that reaches
// here lacks -freflection and says so.
static_assert(ReflectionAvailable,
              "ZHLN requires C++26 static reflection (-freflection); "
              "give the CMake target zahlen_enable_reflection(<target>)");

#endif

}
