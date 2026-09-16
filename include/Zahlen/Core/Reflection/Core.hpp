// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/Reflection/Core.hpp
//
// The floor of the stack: the P2996 primers, the feature check, and the two
// things every other module here is built on -- the splice replicator that
// turns a define_static_array of handles into a pack, and TypeName.
//
// What it costs a translation unit: <meta> and <vector> (Expand's argument
// list), and nothing else. No <format>, no <ranges>, no <string>, no member
// queries. Enums.hpp is this header plus the enum vocabulary, and it is what
// Zahlen/Error.hpp and Zahlen/ErrorCode.hpp include, so a translation unit that
// only carries error codes never sees the rest of the file set.

#pragma once

#include <string_view>
#include <type_traits>

// One macro test, in one place. The sibling headers switch on
// ZHLN_REFLECTION_AVAILABLE instead of repeating the idiom, so it cannot drift
// into a second copy; code that wants the capability as a constant asks
// ZHLN::Reflect::ReflectionAvailable, which is this macro's bool.
//
// The test sits above the namespace because it guards the includes below, and
// an include can never sit inside namespace ZHLN::Reflect: an include in a
// namespace declares the included header's names there, so libc++ would define
// ZHLN::Reflect::std instead of ::std and every std::-qualified lookup inside it
// resolves to the wrong namespace -- the failure is "no member named 'invoke' in
// namespace 'ZHLN::Reflect::std'; did you mean '::std::invoke'?", reported from
// inside libc++'s own headers. Macro text is namespace-agnostic, so the test is
// what moves out with the includes.
#if defined(__cpp_impl_reflection) || (defined(__has_feature) && __has_feature(reflection))
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ZHLN_REFLECTION_AVAILABLE 1
#else
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ZHLN_REFLECTION_AVAILABLE 0
#endif

#if ZHLN_REFLECTION_AVAILABLE
#include <meta>
#include <vector>
#endif

namespace ZHLN::Reflect {

template <typename T>
constexpr auto IsBracesConstructible() -> bool {
    return std::is_aggregate_v<std::remove_cvref_t<T>>;
}

inline constexpr bool ReflectionAvailable = ZHLN_REFLECTION_AVAILABLE != 0;

#if ZHLN_REFLECTION_AVAILABLE

// Every reflection handle in the directory is an NTTP of type
// std::meta::info, spelled explicitly rather than `auto`: GCC's module merger
// compares template declarations streamed out of a module interface against
// the importer's textually-included copy of the same header (Wire.cppm's GMF
// vs Network.cppm's GMF), and placeholder `auto` parameter types stream
// inconsistently across module contexts -> "conflicting imported
// declaration" (cf. GCC PR 118049 / 120644). An explicitly-typed
// std::meta::info parameter merges cleanly. The rule applies to every module
// here, not just this one.

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
            // Builtin types (`unsigned int`) and template specializations
            // (`ZHLN::FixedString<64>`) have no identifier; render their full
            // display spelling so TypeName's rename predicate sees something
            // it can actually match instead of a placeholder.
            return std::meta::display_string_of(info);
        }
    }
};

} // namespace TemplatedDetail

// Unconstrained by design. The constraint was std::ranges::range, and <ranges>
// is the single most expensive include this directory exists to keep away from
// the error path; the body calls size() and iterates, so a non-range fails to
// compile with a clear error anyway.
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

/// TypeName with an optional rename predicate.
///
/// `rename` is a compile-time callable invoked with the type's reflected
/// spelling; a non-null return replaces the name with the returned string,
/// nullptr keeps the type's own spelling. This is a naming hook only: the
/// predicate can never change what reflection reports about the type, and the
/// no-argument form above remains the canonical spelling used everywhere else.
///
/// Typical use is project-specific spellings without forking this file, e.g.
/// `TypeName<uint32_t>([](std::string_view s) -> const char* {
///     return s == "unsigned int" ? "uint32_t" : nullptr;
/// })`.
template <typename T, typename NameOverride>
consteval auto TypeName(NameOverride rename) -> std::string_view {
    const std::string_view spelling   = TypeName<T>();
    const char*            overridden = rename(spelling);
    if (overridden != nullptr) {
        return overridden;
    }
    return spelling;
}

#else // No C++26 static reflection: the prose above says why that is fatal.

// ---------------------------------------------------------------------------
// No C++26 static reflection: the degraded stand-ins for this module.
//
// This is not a supported build configuration, and the hard stop below is why.
//
// Several of these feed struct layout. InputStateComponent::keys is a
// std::bitset<Reflect::EnumCount<KeyCode>()>, and EnumCount is 72 with
// reflection and 0 here. A target compiled without the flag therefore lays
// shared structs out differently from every target compiled with it: mouseX at
// offset 4 instead of 16, sizeof 44 instead of 56. That is an ODR violation
// with no diagnostic at any level -- it compiled, linked, ran, and made
// Context::Button read the key bitset as the mouse position (mouse=(0,3e-45),
// 3e-45 being 0x00000002, i.e. bit 65, KeyCode::LButton). The target missing
// from the zahlen_enable_reflection list in the root CMakeLists.txt was the
// whole bug.
//
// clangd is exempt because it has no P2996 either and would otherwise mark the
// entire tree as broken; __CLANGD__ is defined by the language server itself, so
// nothing has to pass it. ZHLN_ALLOW_REFLECTION_STUBS is the explicit opt-out
// for host-only tooling that is deliberately built on a compiler without
// reflection and pins the values it needs (see tests/gui_harness).
//
// The stubs live next to the real definition they stand in for, in the #else of
// the same module: one header, one home per symbol, whichever configuration is
// being compiled. A stub kept anywhere else would be invisible to a translation
// unit that includes only the module it needs.
// ---------------------------------------------------------------------------

#if !defined(__CLANGD__) && !defined(ZHLN_ALLOW_REFLECTION_STUBS)
static_assert(ReflectionAvailable,
              "ZHLN requires C++26 static reflection (-freflection); "
              "give the CMake target zahlen_enable_reflection(<target>)");
#endif

// Unconstrained for the same reason as the real Expand in Core.hpp's guarded
// branch: no <ranges> on the error path.
template <typename R>
consteval int Expand(R&& /*unused*/) {
    return 0;
}

namespace TemplatedDetail {

template <typename T>
consteval auto ExtractTypeName() noexcept -> std::string_view {
#if defined(__clang__)
    std::string_view p     = __PRETTY_FUNCTION__;
    auto             start = p.find("[T = ");
    if (start != std::string_view::npos) {
        start += 5;
        auto end = p.find(']', start);
        if (end != std::string_view::npos) {
            std::string_view raw = p.substr(start, end - start);
            for (std::string_view prefix: {"enum class ", "enum ", "struct ", "class "}) {
                if (raw.starts_with(prefix)) {
                    raw.remove_prefix(prefix.size());
                    break;
                }
            }
            return raw;
        }
    }
#elif defined(__GNUC__)
    std::string_view p     = __PRETTY_FUNCTION__;
    auto             start = p.find("[with T = ");
    if (start != std::string_view::npos) {
        start += 10;
        auto end = p.find(';', start);
        if (end == std::string_view::npos)
            end = p.find(']', start);
        if (end != std::string_view::npos) {
            std::string_view raw = p.substr(start, end - start);
            for (std::string_view prefix: {"enum class ", "enum ", "struct ", "class "}) {
                if (raw.starts_with(prefix)) {
                    raw.remove_prefix(prefix.size());
                    break;
                }
            }
            return raw;
        }
    }
#elif defined(_MSC_VER)
    std::string_view p     = __FUNCSIG__;
    auto             start = p.find("ExtractTypeName<");
    if (start != std::string_view::npos) {
        start += 16;
        auto end = p.rfind(">(void)");
        if (end != std::string_view::npos && end > start) {
            std::string_view raw = p.substr(start, end - start);
            for (std::string_view prefix: {"enum class ", "enum ", "struct ", "class "}) {
                if (raw.starts_with(prefix)) {
                    raw.remove_prefix(prefix.size());
                    break;
                }
            }
            return raw;
        }
    }
#endif
    return "";
}

} // namespace TemplatedDetail

template <typename T>
consteval std::string_view TypeName() {
    return TemplatedDetail::ExtractTypeName<std::remove_cvref_t<T>>();
}

/// TypeName with an optional rename predicate (fallback build). The compiler
/// has no reflection, so the predicate receives an empty spelling and may
/// still supply a name; returning nullptr yields the same empty spelling as
/// the no-argument form above.
template <typename T, typename NameOverride>
consteval auto TypeName(NameOverride rename) -> std::string_view {
    const std::string_view spelling   = TypeName<T>();
    const char*            overridden = rename(spelling);
    if (overridden != nullptr) {
        return overridden;
    }
    return spelling;
}

#endif

} // namespace ZHLN::Reflect
