// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <string_view>
#include <type_traits>

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

#else


#if !defined(__CLANGD__) && !defined(ZHLN_ALLOW_REFLECTION_STUBS)
static_assert(ReflectionAvailable,
              "ZHLN requires C++26 static reflection (-freflection); "
              "give the CMake target zahlen_enable_reflection(<target>)");
#endif

template <typename R>
consteval int Expand(R&& ) {
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

}

template <typename T>
consteval std::string_view TypeName() {
    return TemplatedDetail::ExtractTypeName<std::remove_cvref_t<T>>();
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

#endif

}
