// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Core/Reflection/Core.hpp>
#include <cstdint>
#include <string>
#include <type_traits>

namespace ZHLN::Reflect {
namespace TemplatedDetail {

// Canonical grammar v1: named scopes use C++ identifiers and ::. Template
// arguments are tagged and delimited; qualifiers, pointers and arrays are
// structural. No compiler-generated display spelling participates.
template <std::meta::info Info>
consteval void AppendCanonical(std::string& out);
template <std::meta::info Info>
consteval void AppendNamed(std::string& out);

consteval void AppendUnsigned(std::string& out, uint64_t value) {
    char digits[20];
    unsigned n = 0;
    do { digits[n++] = static_cast<char>('0' + value % 10); value /= 10; } while (value);
    while (n) out += digits[--n];
}

template <typename T, bool = std::is_enum_v<T>>
struct CanonicalSigned : std::bool_constant<std::is_signed_v<T>> {};
template <typename T>
struct CanonicalSigned<T, true> : std::bool_constant<std::is_signed_v<std::underlying_type_t<T>>> {};

template <std::meta::info Arg>
consteval void AppendArgument(std::string& out) {
    if constexpr (std::meta::is_type(Arg)) {
        out += "T{"; AppendCanonical<Arg>(out); out += '}';
    } else if constexpr (std::meta::is_template(Arg)) {
        out += "M{"; AppendCanonical<Arg>(out); out += '}';
    } else if constexpr (std::meta::is_value(Arg)) {
        using V = typename[:std::meta::type_of(Arg):];
        if constexpr ((std::is_integral_v<V> || std::is_enum_v<V>) && sizeof(V) <= sizeof(uint64_t)) {
            constexpr V value = std::meta::extract<V>(Arg);
            out += "V{"; AppendCanonical<^^V>(out); out += ':';
            if constexpr (CanonicalSigned<V>::value) {
                if (value < static_cast<V>(0)) {
                    out += '-'; AppendUnsigned(out, uint64_t(0) - static_cast<uint64_t>(value));
                } else AppendUnsigned(out, static_cast<uint64_t>(value));
            } else AppendUnsigned(out, static_cast<uint64_t>(value));
            out += '}';
        } else static_assert((std::is_integral_v<V> || std::is_enum_v<V>) && sizeof(V) <= sizeof(uint64_t), "canonical type name: unsupported non-type template argument");
    } else static_assert(std::meta::is_value(Arg), "canonical type name: unsupported template argument");
}

template <std::meta::info Info>
consteval void AppendCanonical(std::string& out) {
    constexpr auto info = std::meta::dealias(Info);
    if constexpr (std::meta::is_type(info)) {
        if constexpr (std::meta::is_lvalue_reference_type(info)) { out += "L{"; AppendCanonical<std::meta::remove_reference(info)>(out); out += '}'; return; }
        else if constexpr (std::meta::is_rvalue_reference_type(info)) { out += "R{"; AppendCanonical<std::meta::remove_reference(info)>(out); out += '}'; return; }
        else if constexpr (std::meta::is_const_type(info)) { out += "C{"; AppendCanonical<std::meta::remove_const(info)>(out); out += '}'; return; }
        else if constexpr (std::meta::is_volatile_type(info)) { out += "Q{"; AppendCanonical<std::meta::remove_volatile(info)>(out); out += '}'; return; }
        else if constexpr (std::meta::is_pointer_type(info)) { out += "P{"; AppendCanonical<std::meta::remove_pointer(info)>(out); out += '}'; return; }
        else if constexpr (std::meta::is_array_type(info)) {
            out += "A{";
            if constexpr (std::meta::is_bounded_array_type(info)) AppendUnsigned(out, std::meta::extent(info));
            out += ':'; AppendCanonical<std::meta::remove_extent(info)>(out); out += '}'; return;
        } else {
            // Fundamental types have no declaring scope; their stable spellings are
            // specified here rather than obtained from a presentation API.
            if constexpr (info == ^^void) { out += "void"; return; }
            else if constexpr (info == ^^bool) { out += "bool"; return; }
            else if constexpr (info == ^^char) { out += "char"; return; }
            else if constexpr (info == ^^signed char) { out += "signed char"; return; }
            else if constexpr (info == ^^unsigned char) { out += "unsigned char"; return; }
            else if constexpr (info == ^^wchar_t) { out += "wchar_t"; return; }
            else if constexpr (info == ^^char8_t) { out += "char8_t"; return; }
            else if constexpr (info == ^^char16_t) { out += "char16_t"; return; }
            else if constexpr (info == ^^char32_t) { out += "char32_t"; return; }
            else if constexpr (info == ^^short) { out += "short"; return; }
            else if constexpr (info == ^^unsigned short) { out += "unsigned short"; return; }
            else if constexpr (info == ^^int) { out += "int"; return; }
            else if constexpr (info == ^^unsigned int) { out += "unsigned int"; return; }
            else if constexpr (info == ^^long) { out += "long"; return; }
            else if constexpr (info == ^^unsigned long) { out += "unsigned long"; return; }
            else if constexpr (info == ^^long long) { out += "long long"; return; }
            else if constexpr (info == ^^unsigned long long) { out += "unsigned long long"; return; }
            else if constexpr (info == ^^float) { out += "float"; return; }
            else if constexpr (info == ^^double) { out += "double"; return; }
            else if constexpr (info == ^^long double) { out += "long double"; return; }
            else if constexpr (info == ^^std::nullptr_t) { out += "decltype(nullptr)"; return; }
            else { AppendNamed<info>(out); }
        }
    } else {
        AppendNamed<info>(out);
    }
}

template <std::meta::info Info>
consteval void AppendNamed(std::string& out) {
    constexpr auto info = Info;
    if constexpr (std::meta::is_namespace(info) || std::meta::is_template(info) ||
                  (std::meta::is_type(info) && (std::meta::is_class_type(info) || std::meta::is_union_type(info) || std::meta::is_enum_type(info)))) {
        constexpr auto entity = [] {
            if constexpr (std::meta::has_template_arguments(info)) return std::meta::template_of(info);
            else return info;
        }();
        if constexpr (!std::meta::has_identifier(entity)) static_assert(std::meta::has_identifier(entity), "canonical type name: unnamed entity or namespace");
        else {
            // Named declarations have an enclosing scope. The global
            // namespace is the recursion terminator and is never passed here.
            constexpr auto parent = std::meta::parent_of(info);
            if constexpr (parent != ^^::) { AppendCanonical<parent>(out); out += "::"; }
            out += std::meta::identifier_of(entity);
            if constexpr (std::meta::has_template_arguments(info)) {
                out += '<';
                [:Expand(std::meta::template_arguments_of(info)):] >> [&]<auto Arg>() { AppendArgument<Arg>(out); };
                out += '>';
            }
        }
    } else {
        static_assert(std::meta::is_namespace(info), "canonical type name: unsupported type or local scope");
    }
}

} // namespace TemplatedDetail

template <typename T>
consteval auto CanonicalTypeName() -> std::string_view {
    std::string name;
    TemplatedDetail::AppendCanonical<^^std::remove_cvref_t<T>>(name);
    return DefineStaticString(name);
}
} // namespace ZHLN::Reflect
