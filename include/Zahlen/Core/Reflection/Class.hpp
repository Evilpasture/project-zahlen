// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/Reflection/Class.hpp
//
// Class-shaped reflection: bases (BaseClasses, HasBases, HasVirtualBases,
// ForEachBase), member functions (MemberFunctionCount, MemberFunctionNames,
// ForEachMemberFunction, ForEachMethodPointer, CollectMethodResults and the
// MethodCollector it drives) and nested types (ForEachNestedType).
//
// Scripting's binder is the main consumer: it walks a bound type's nested
// types, its member functions and its bases to build its tables.

#pragma once

#include <Zahlen/Core/Reflection/Core.hpp>
#include <array>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ZHLN::Reflect {

#if ZHLN_REFLECTION_AVAILABLE

namespace TemplatedDetail {

template <typename T>
consteval auto MembersOf() {
    return std::define_static_array(std::meta::members_of(std::meta::dealias(^^std::remove_cvref_t<T>), std::meta::access_context::current()));
}

template <typename T>
consteval auto BasesOf() {
    return std::define_static_array(std::meta::bases_of(^^std::remove_cvref_t<T>, std::meta::access_context::current()));
}

template <typename T>
constexpr auto ToTuple(T&& obj) {
    if constexpr (requires { std::tuple_size<std::decay_t<T>>::value; }) {
        return std::forward<T>(obj);
    } else {
        return std::make_tuple(std::forward<T>(obj));
    }
}

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

} // namespace TemplatedDetail

template <typename T, typename F>
constexpr void ForEachMemberFunction(F&& f) {
    [:Expand(TemplatedDetail::MembersOf<T>()):] >> [&]<auto member>() -> auto {
        if constexpr (std::meta::is_function(member) && std::meta::has_identifier(member)) {
            f.template operator()<member>();
        }
    };
}

template <typename T>
consteval auto BaseClasses() {
    return std::meta::bases_of(^^std::remove_cvref_t<T>, std::meta::access_context::current());
}

template <typename T>
consteval auto HasVirtualBases() -> bool {
    using U [[maybe_unused]] = std::remove_cvref_t<T>;
    // Plain loop, not std::ranges::any_of: no <ranges> in this header.
    for (auto base: TemplatedDetail::BasesOf<U>()) {
        if (std::meta::is_virtual(base)) {
            return true;
        }
    }
    return false;
}

template <typename T, typename F>
constexpr void ForEachBase(F&& f) {
    [:Expand(TemplatedDetail::BasesOf<T>()):] >> [&]<auto base>() -> auto { f.template operator()<typename[:std::meta::type_of(base):]>(); };
}

template <typename T>
consteval auto MemberFunctionCount() -> std::size_t {
    std::size_t count = 0;
    for (auto m: TemplatedDetail::MembersOf<T>()) {
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
                constexpr auto member = TemplatedDetail::MembersOf<T>()[Is];
                if constexpr (std::meta::is_function(member) && std::meta::has_identifier(member)) {
                    names[idx++] = std::meta::identifier_of(member);
                }
            }(),
            ...);
        return names;
    }(std::make_index_sequence<TemplatedDetail::MembersOf<T>().size()>());
}

template <typename T, typename F>
constexpr void ForEachNestedType(F&& f) {
    [:Expand(TemplatedDetail::MembersOf<T>()):] >> [&]<auto member>() -> auto {
        if constexpr (std::meta::is_type(member)) {
            using NestedType = typename[:member:];
            if constexpr (std::is_class_v<NestedType>) {
                f.template operator()<NestedType>();
            }
        }
    };
}

template <typename T, typename F>
constexpr void ForEachMethodPointer(F&& f) {
    [:Expand(TemplatedDetail::MembersOf<T>()):] >> [&]<auto member>() -> auto {
        if constexpr (std::meta::is_function(member) && std::meta::has_identifier(member)) {
            constexpr auto pmf = &[:member:];
            if constexpr (std::is_member_function_pointer_v<decltype(pmf)>) {
                constexpr std::string_view name = std::meta::identifier_of(member);
                f(name, pmf);
            }
        }
    };
}

template <typename T>
constexpr auto CollectMethodResults(const T& inst) {
    using Collector = TemplatedDetail::MethodCollector<std::remove_cvref_t<T>>;
    return [&]<size_t... Is>(std::index_sequence<Is...>) -> auto {
        return std::tuple_cat(TemplatedDetail::ToTuple((inst.[:Collector::method_handles[Is]:]()))...);
    }(std::make_index_sequence<Collector::count> {});
}

#else // No C++26 static reflection: this module's degraded stand-ins.

template <typename T, typename F>
constexpr void ForEachMemberFunction(F&& /*unused*/) {
}

template <typename T>
consteval auto BaseClasses() {
    return std::array<int, 0> {};
}

template <typename T>
consteval bool HasVirtualBases() {
    return false;
}

template <typename T, typename F>
constexpr void ForEachBase(F&& /*unused*/) {
}

template <typename T>
consteval std::size_t MemberFunctionCount() {
    return 0;
}

template <typename T>
consteval auto MemberFunctionNames() {
    return std::array<std::string_view, 0> {};
}

template <typename T, typename F>
constexpr void ForEachNestedType(F&& /*unused*/) {
}

template <typename T, typename F>
constexpr void ForEachMethodPointer(F&& /*unused*/) {
}

template <typename T>
constexpr auto CollectMethodResults(const T& /*inst*/) {
    return std::tuple {};
}

#endif

template <typename T>
constexpr auto HasBases() -> bool {
    return !BaseClasses<T>().empty();
}

} // namespace ZHLN::Reflect
