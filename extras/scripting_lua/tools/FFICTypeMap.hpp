/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// extras/scripting_lua/tools/FFICTypeMap.hpp
//
// The C++ type -> C declaration mapping used to generate ffi_cdef's struct
// bodies. Two distinct concerns:
//
//  1. Layout is ordinary template code over the complete type: sizeof/alignof,
//     array extents and Jolt's 16-byte SIMD alignment. It never needs
//     reflection, so the mapper still compiles and can be tested on a compiler
//     with no static-reflection support.
//
//  2. Names are obtained through the project's reflection header
//     (Zahlen/Core/Reflection.hpp, ZHLN::Reflect::TypeName) when the
//     compiler has static reflection: the type's own spelling is handed to a
//     caller-supplied compile-time predicate -- a lambda -- which returns the
//     C name to emit or nullptr to keep the built-in mapping. That is what
//     lets a previously opaque field (std::bitset, HashMap, an intrusive Ref,
//     ...) be named by the generator without editing this file, and what lets
//     any emitted spelling be renamed, e.g. "unsigned int" -> "uint32_t" or
//     "Entity" -> "ZHLN_Entity".
//
// The rule this file exists to enforce: never guess a layout. A name never
// decides size or alignment -- sizeof/alignof always do -- and the caller
// verifies each produced declaration against the measured values. A mapping
// that cannot be justified is not emitted as a guess; it is emitted as bytes.

#pragma once

#include <Jolt/Jolt.h>
#include <Zahlen/Core/Reflection.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

// Reflection is optional. All name lookups go through Zahlen/Core/Reflection.hpp:
// with a reflection-capable compiler ZHLN::Reflect::TypeName returns the type's
// own spelling (std::meta::identifier_of / display_string_of via TypeReflector),
// and without one it returns an empty spelling, which makes the predicate's
// "no opinion" path fall back to the built-in vocabulary below. No <meta> is
// touched here.

namespace ZHLN::FFI {

/// How a single C++ field is spelled in the generated C.
struct CDecl {
    /// C type name, or nullptr when the type has no C counterpart we can name.
    const char* base = nullptr;

    /// Array extent. 1 for a scalar, N for a `base name[N]`.
    int count = 1;

    /// The C++ type demands 16-byte alignment (Jolt's SIMD types). Plain
    /// `float[4]` only guarantees 4, which would silently shift every
    /// following field, so the declaration needs an explicit aligned attribute.
    bool aligned16 = false;

    /// sizeof/alignof of the C++ type. The driver uses these to insert padding
    /// and to verify the declaration it emitted actually agrees.
    std::size_t size  = 0;
    std::size_t align = 0;

    [[nodiscard]] constexpr bool isOpaque() const noexcept { return base == nullptr; }
};

namespace detail {

    template <typename T> struct ArrayTraits {
        static constexpr bool available = false;
    };
    template <typename E, std::size_t N> struct ArrayTraits<std::array<E, N>> {
        static constexpr bool        available = true;
        static constexpr std::size_t extent    = N;
        using element                          = E;
    };

    template <typename T> struct FixedStringTraits {
        static constexpr bool available = false;
    };
    template <std::size_t N> struct FixedStringTraits<ZHLN::FixedString<N>> {
        static constexpr bool        available = true;
        static constexpr std::size_t capacity  = N;
    };

    /// The source spelling of T, obtained through static reflection.
    ///
    /// Delegates to ZHLN::Reflect::TypeName (Zahlen/Core/Reflection.hpp), which
    /// dealises typedefs first: a class or enum with an identifier is its bare
    /// identifier -- "Vec3", "Entity", "TextAlignment" -- a builtin type is its
    /// canonical spelling -- "float", "unsigned int" -- and a template
    /// specialization, having no identifier, is reported as
    /// "TemplateSpecialization" (callers that need the rendered spelling can
    /// match that and use std::meta::display_string_of themselves). Without
    /// static reflection the result is empty and the mapper falls back to the
    /// type-based vocabulary below, so this header still compiles and is
    /// testable on a compiler with no reflection support.
    template <typename T>
    consteval auto ReflectedName() -> std::string_view {
        return ZHLN::Reflect::TypeName<std::remove_cvref_t<T>>();
    }

    /// The default vocabulary: spellings whose C name is the C++ name itself.
    /// This is what the no-argument MapCType<T>() uses. Return nullptr to
    /// leave the decision to the built-in (type-based) mapping below.
    struct DefaultNameOverride {
        consteval auto operator()(std::string_view name) const -> const char* {
            if (name == "bool") return "bool";
            if (name == "float") return "float";
            if (name == "double") return "double";
            if (name == "char") return "char";
            if (name == "int8_t") return "int8_t";
            if (name == "uint8_t") return "uint8_t";
            if (name == "int16_t") return "int16_t";
            if (name == "uint16_t") return "uint16_t";
            if (name == "int32_t") return "int32_t";
            if (name == "uint32_t") return "uint32_t";
            if (name == "int64_t") return "int64_t";
            if (name == "uint64_t") return "uint64_t";
            return nullptr;
        }
    };

    /// The C spelling of a scalar by type, or nullptr if it is not one this
    /// file knows. This is the fallback used when the compiler has no static
    /// reflection (or the reflected spelling matched no vocabulary entry), and
    /// the guaranteed mapping for enum underlying types in either build.
    template <typename T>
    consteval auto ScalarName() -> const char* {
        using U = std::remove_cvref_t<T>;
        if constexpr (std::is_same_v<U, bool>) {
            return "bool";
        } else if constexpr (std::is_same_v<U, float>) {
            return "float";
        } else if constexpr (std::is_same_v<U, double>) {
            return "double";
        } else if constexpr (std::is_same_v<U, std::int8_t>) {
            return "int8_t";
        } else if constexpr (std::is_same_v<U, std::uint8_t>) {
            return "uint8_t";
        } else if constexpr (std::is_same_v<U, std::int16_t>) {
            return "int16_t";
        } else if constexpr (std::is_same_v<U, std::uint16_t>) {
            return "uint16_t";
        } else if constexpr (std::is_same_v<U, std::int32_t>) {
            return "int32_t";
        } else if constexpr (std::is_same_v<U, std::uint32_t>) {
            return "uint32_t";
        } else if constexpr (std::is_same_v<U, std::int64_t>) {
            return "int64_t";
        } else if constexpr (std::is_same_v<U, std::uint64_t>) {
            return "uint64_t";
        } else if constexpr (std::is_same_v<U, char>) {
            return "char";
        } else {
            return nullptr;
        }
    }
} // namespace detail

/// Map a C++ field type to its C declaration.
///
/// `rename` is a compile-time predicate called with the type's reflected
/// spelling. Returning a non-null string overrides the C base name; returning
/// nullptr falls through to the built-in mapping. Layout is always derived
/// from the C++ type itself (sizeof/alignof, extents, alignment), never from
/// the name -- the predicate can only change how the type is spelled.
///
/// Returns an opaque CDecl (base == nullptr, size/align still populated) for
/// anything the predicate and the built-in mapping both refuse to name --
/// std::bitset, HashMap, intrusive Refs. Callers must render that as a
/// correctly sized and aligned byte blob rather than inventing a layout.
template <typename T, typename NameOverride>
consteval auto MapCType(NameOverride rename) -> CDecl {
    using U = std::remove_cvref_t<T>;

    CDecl d;
    d.size  = sizeof(U);
    d.align = alignof(U);

    const std::string_view spelling = detail::ReflectedName<U>();
    const char*            name     = rename(spelling);

    if constexpr (std::is_enum_v<U>) {
        // An enum occupies exactly its underlying type.
        d.base = name != nullptr ? name : detail::ScalarName<std::underlying_type_t<U>>();
    } else if constexpr (detail::ScalarName<U>() != nullptr) {
        d.base = name != nullptr ? name : detail::ScalarName<U>();
    } else if constexpr (std::is_same_v<U, ZHLN::Entity>) {
        // { uint32_t index; uint32_t generation; } -- emitted as a named struct
        // so scripts can write .index and .generation. Size 8, align 4, which
        // no scalar spelling reproduces.
        d.base = name != nullptr ? name : "ZHLN_Entity";
    } else if constexpr (std::is_same_v<U, JPH::Vec3> || std::is_same_v<U, JPH::Vec4> ||
                         std::is_same_v<U, JPH::Quat>) {
        // Measured 16 bytes at 16-byte alignment. Vec3 stores three floats in a
        // 16-byte SIMD slot, so float[3] would be wrong on both counts.
        d.base      = name != nullptr ? name : "float";
        d.count     = 4;
        d.aligned16 = true;
    } else if constexpr (std::is_same_v<U, JPH::Mat44>) {
        d.base      = name != nullptr ? name : "float";
        d.count     = 16;
        d.aligned16 = true;
    } else if constexpr (detail::FixedStringTraits<U>::available) {
        // char data[N]; size_t len;  -- emitted as a named struct per capacity.
        d.base = name != nullptr ? name : "ZHLN_FixedString";
    } else if constexpr (std::is_array_v<U>) {
        // A raw C array member, e.g. `char _pad[3]`. remove_cvref_t does not
        // strip array-ness, so without this branch it falls through to the
        // opaque case -- correct bytes, but needlessly unnamed. The element's
        // mapping (with the same predicate) supplies extent and alignment.
        const auto element = MapCType<std::remove_extent_t<U>>(rename);
        d.base             = name != nullptr ? name : element.base;
        d.count            = element.count * static_cast<int>(std::extent_v<U>);
        d.aligned16        = element.aligned16;
    } else if constexpr (detail::ArrayTraits<U>::available) {
        const auto element = MapCType<typename detail::ArrayTraits<U>::element>(rename);
        d.base             = name != nullptr ? name : element.base;
        d.count            = element.count * static_cast<int>(detail::ArrayTraits<U>::extent);
        d.aligned16        = element.aligned16;
    } else {
        // No built-in layout rule and no name from the predicate: opaque.
        d.base = name;
    }

    return d;
}

/// Map a C++ field type to its C declaration using the default vocabulary.
template <typename T>
consteval auto MapCType() -> CDecl {
    return MapCType<T>(detail::DefaultNameOverride {});
}

/// C spelling of the struct a FixedString<N> becomes.
template <std::size_t N>
consteval auto FixedStringStructName() -> const char* {
    // The driver emits one typedef per capacity it encounters.
    if constexpr (N == 64) {
        return "ZHLN_FixedString64";
    } else if constexpr (N == 128) {
        return "ZHLN_FixedString128";
    } else if constexpr (N == 256) {
        return "ZHLN_FixedString256";
    } else {
        return nullptr; // driver must synthesise a typedef for this capacity
    }
}

} // namespace ZHLN::FFI
