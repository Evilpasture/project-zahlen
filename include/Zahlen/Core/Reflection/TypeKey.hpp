// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Core/Reflection/TypeKey.hpp
//
// Unified 64-bit compile-time type identity for ECS storage, system scheduling,
// event channels, extension services, and COM component publication.
//
// - Public / named types (every enclosing scope and template argument has a
//   canonical AST spelling): Hash64(CanonicalTypeName<Bare>()), matching
//   COM::ChainTypeID<Bare>() and Hash64(COM::IEntities::ComponentName<Bare>())
//   across every TU and DSO.
// - TU-local / anonymous / function-local / unnamed types: domain-separated
//   hash combining ScopedTypeName with std::meta::source_location_of
//   (file_name, line, column) and constituent type arguments so distinct local
//   types never collide across tests or anonymous namespaces.

#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Core/Reflection/Callable.hpp>
#include <Zahlen/Core/Reflection/CanonicalName.hpp>
#include <Zahlen/Core/Reflection/Core.hpp>
#include <cstdint>
#include <source_location>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace ZHLN::Reflect {

#if ZHLN_REFLECTION_AVAILABLE

namespace TemplatedDetail {

// True iff CanonicalTypeName can spell Info without hitting a static_assert:
// all constituent scopes, template arguments, and underlying types must be
// named non-local declarations or supported fundamental/compound types.
template <std::meta::info Info>
consteval auto CanCanonicalize() noexcept -> bool {
    constexpr auto info = std::meta::dealias(Info);
    if constexpr (std::meta::is_type(info)) {
        if constexpr (std::meta::is_lvalue_reference_type(info) || std::meta::is_rvalue_reference_type(info)) {
            return CanCanonicalize<std::meta::remove_reference(info)>();
        } else if constexpr (std::meta::is_const_type(info)) {
            return CanCanonicalize<std::meta::remove_const(info)>();
        } else if constexpr (std::meta::is_volatile_type(info)) {
            return CanCanonicalize<std::meta::remove_volatile(info)>();
        } else if constexpr (std::meta::is_pointer_type(info)) {
            return CanCanonicalize<std::meta::remove_pointer(info)>();
        } else if constexpr (std::meta::is_array_type(info)) {
            return CanCanonicalize<std::meta::remove_extent(info)>();
        } else if constexpr (std::meta::is_function_type(info)) {
            using Traits = FunctionTypeTraits<typename[:info:]>;
            if constexpr (!Traits::hasSignature) {
                return false;
            } else {
                using Ret = typename Traits::ReturnType;
                if constexpr (!CanCanonicalize<^^Ret>()) {
                    return false;
                } else {
                    return []<typename... Args>(std::type_identity<std::tuple<Args...>>) consteval -> bool {
                        return (CanCanonicalize<^^Args>() && ...);
                    }(std::type_identity<typename Traits::ParameterTypes> {});
                }
            }
        } else if constexpr (info == ^^void || info == ^^bool || info == ^^char || info == ^^signed char || info == ^^unsigned char ||
                             info == ^^wchar_t || info == ^^char8_t || info == ^^char16_t || info == ^^char32_t ||
                             info == ^^short || info == ^^unsigned short || info == ^^int || info == ^^unsigned int ||
                             info == ^^long || info == ^^unsigned long || info == ^^long long || info == ^^unsigned long long ||
                             info == ^^float || info == ^^double || info == ^^long double || info == ^^std::nullptr_t) {
            return true;
        } else if constexpr (std::meta::is_class_type(info) || std::meta::is_union_type(info) || std::meta::is_enum_type(info)) {
            constexpr auto entity = []() consteval -> std::meta::info {
                if constexpr (std::meta::has_template_arguments(info)) {
                    return std::meta::template_of(info);
                } else {
                    return info;
                }
            }();
            if constexpr (!std::meta::has_identifier(entity) || !HasParent(info)) {
                return false;
            } else {
                constexpr auto parent = std::meta::parent_of(info);
                if constexpr (parent != ^^::) {
                    if constexpr (!CanCanonicalize<parent>()) {
                        return false;
                    }
                }
                if constexpr (std::meta::has_template_arguments(info)) {
                    bool ok = true;
                    [:Expand(std::meta::template_arguments_of(info)):] >> [&]<auto Arg>() {
                        if constexpr (std::meta::is_type(Arg) || std::meta::is_template(Arg)) {
                            ok = ok && CanCanonicalize<Arg>();
                        } else if constexpr (std::meta::is_value(Arg)) {
                            using V = typename[:std::meta::type_of(Arg):];
                            if constexpr ((std::is_integral_v<V> || std::is_enum_v<V>) && sizeof(V) <= sizeof(uint64_t)) {
                                ok = ok && CanCanonicalize<^^V>();
                            } else {
                                ok = false;
                            }
                        } else {
                            ok = false;
                        }
                    };
                    return ok;
                } else {
                    return true;
                }
            }
        } else {
            return false;
        }
    } else if constexpr (std::meta::is_namespace(info) || std::meta::is_template(info)) {
        if constexpr (!std::meta::has_identifier(info) || !HasParent(info)) {
            return false;
        } else {
            constexpr auto parent = std::meta::parent_of(info);
            if constexpr (parent != ^^::) {
                return CanCanonicalize<parent>();
            } else {
                return true;
            }
        }
    } else {
        return false;
    }
}

constexpr void HashFoldU64(uint64_t& h, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (i * 8)) & 0xFFull;
        h *= kFnvPrime64;
    }
}

constexpr void HashFoldString(uint64_t& h, std::string_view sv) noexcept {
    HashFoldU64(h, sv.size());
    for (char c: sv) {
        h ^= static_cast<uint8_t>(c);
        h *= kFnvPrime64;
    }
}

consteval void HashSourceLocation(uint64_t& h, std::source_location loc) noexcept {
    const char* file = loc.file_name();
    if (file != nullptr) {
        uint64_t len = 0;
        while (file[len] != '\0') {
            ++len;
        }
        HashFoldU64(h, len);
        for (uint64_t i = 0; i < len; ++i) {
            h ^= static_cast<uint8_t>(file[i]);
            h *= kFnvPrime64;
        }
    } else {
        HashFoldU64(h, 0);
    }
    HashFoldU64(h, loc.line());
    HashFoldU64(h, loc.column());
}

template <std::meta::info Info>
consteval void HashLocalInfo(uint64_t& h) noexcept {
    constexpr auto info = std::meta::dealias(Info);
    if constexpr (std::meta::is_type(info)) {
        if constexpr (std::meta::is_lvalue_reference_type(info)) {
            HashFoldString(h, "L");
            HashLocalInfo<std::meta::remove_reference(info)>(h);
        } else if constexpr (std::meta::is_rvalue_reference_type(info)) {
            HashFoldString(h, "R");
            HashLocalInfo<std::meta::remove_reference(info)>(h);
        } else if constexpr (std::meta::is_const_type(info)) {
            HashFoldString(h, "C");
            HashLocalInfo<std::meta::remove_const(info)>(h);
        } else if constexpr (std::meta::is_volatile_type(info)) {
            HashFoldString(h, "Q");
            HashLocalInfo<std::meta::remove_volatile(info)>(h);
        } else if constexpr (std::meta::is_pointer_type(info)) {
            HashFoldString(h, "P");
            HashLocalInfo<std::meta::remove_pointer(info)>(h);
        } else if constexpr (std::meta::is_array_type(info)) {
            HashFoldString(h, "A");
            if constexpr (std::meta::is_bounded_array_type(info)) {
                HashFoldU64(h, std::meta::extent(info));
            }
            HashLocalInfo<std::meta::remove_extent(info)>(h);
        } else if constexpr (std::meta::is_function_type(info)) {
            HashFoldString(h, "F");
            using Traits = FunctionTypeTraits<typename[:info:]>;
            if constexpr (Traits::hasSignature) {
                using Ret = typename Traits::ReturnType;
                HashLocalInfo<^^Ret>(h);
                HashFoldU64(h, Traits::isNoexcept ? 1ull : 0ull);
                HashFoldU64(h, Traits::isCVariadic ? 1ull : 0ull);
                [&]<typename... Args>(std::type_identity<std::tuple<Args...>>) consteval {
                    (HashLocalInfo<^^Args>(h), ...);
                }(std::type_identity<typename Traits::ParameterTypes> {});
            } else {
                HashFoldString(h, std::meta::display_string_of(info));
            }
        } else if constexpr (std::meta::is_class_type(info) || std::meta::is_union_type(info) || std::meta::is_enum_type(info)) {
            HashFoldString(h, QualifiedTypeSpelling(info));
            HashSourceLocation(h, std::meta::source_location_of(info));
            if constexpr (std::meta::has_template_arguments(info)) {
                [:Expand(std::meta::template_arguments_of(info)):] >> [&]<auto Arg>() {
                    if constexpr (std::meta::is_type(Arg)) {
                        HashFoldString(h, "T");
                        HashLocalInfo<Arg>(h);
                    } else if constexpr (std::meta::is_template(Arg)) {
                        HashFoldString(h, "M");
                        HashFoldString(h, std::meta::display_string_of(Arg));
                        HashSourceLocation(h, std::meta::source_location_of(Arg));
                    } else if constexpr (std::meta::is_value(Arg)) {
                        HashFoldString(h, "V");
                        using V = typename[:std::meta::type_of(Arg):];
                        if constexpr ((std::is_integral_v<V> || std::is_enum_v<V>) && sizeof(V) <= sizeof(uint64_t)) {
                            HashFoldU64(h, static_cast<uint64_t>(std::meta::extract<V>(Arg)));
                        } else {
                            HashFoldString(h, std::meta::display_string_of(Arg));
                        }
                        HashLocalInfo<std::meta::type_of(Arg)>(h);
                    }
                };
            }
        } else {
            HashFoldString(h, std::meta::display_string_of(info));
        }
    } else {
        HashFoldString(h, std::meta::display_string_of(info));
    }
}

} // namespace TemplatedDetail

template <typename T>
[[nodiscard]] consteval auto TypeKey() noexcept -> uint64_t {
    using Bare = std::remove_cvref_t<T>;
    if constexpr (TemplatedDetail::CanCanonicalize<^^Bare>()) {
        return Hash64(CanonicalTypeName<Bare>());
    } else {
        uint64_t h = Hash64("ZHLN::LocalTypeKey::v1");
        TemplatedDetail::HashLocalInfo<^^Bare>(h);
        return h;
    }
}

#endif // ZHLN_REFLECTION_AVAILABLE

} // namespace ZHLN::Reflect
