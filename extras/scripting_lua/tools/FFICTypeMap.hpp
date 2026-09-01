/*
 * Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// extras/scripting_lua/tools/FFICTypeMap.hpp
//
// The C++ type -> C declaration mapping used to generate ffi_cdef's struct
// bodies. Deliberately free of any reflection: it is ordinary template code
// over a complete type, so it compiles and can be tested on a compiler with no
// static-reflection support. The reflection layer's only job is to hand this
// each field's type, name and offset.
//
// The rule this file exists to enforce: never guess a layout. Every spelling
// below is either a scalar whose C counterpart has the same size and alignment
// by definition, or it carries the C++ type's measured sizeof/alignof so the
// caller can check the result and fall back to an opaque blob. A mapping that
// cannot be justified is not emitted as a guess -- it is emitted as bytes.

#pragma once

#include <Jolt/Jolt.h>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

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
        static constexpr bool      available = true;
        static constexpr std::size_t extent  = N;
        using element                        = E;
    };

    template <typename T> struct FixedStringTraits {
        static constexpr bool available = false;
    };
    template <std::size_t N> struct FixedStringTraits<ZHLN::FixedString<N>> {
        static constexpr bool      available = true;
        static constexpr std::size_t capacity = N;
    };

    /// The C spelling of a scalar, or nullptr if it is not one this file knows.
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
/// Returns an opaque CDecl (base == nullptr, size/align still populated) for
/// anything without a C counterpart -- std::bitset, HashMap, intrusive Refs.
/// Callers must render that as a correctly sized and aligned byte blob rather
/// than inventing a layout.
template <typename T>
consteval auto MapCType() -> CDecl {
    using U = std::remove_cvref_t<T>;

    CDecl d;
    d.size  = sizeof(U);
    d.align = alignof(U);

    if constexpr (std::is_enum_v<U>) {
        // An enum occupies exactly its underlying type.
        d.base = detail::ScalarName<std::underlying_type_t<U>>();
    } else if constexpr (detail::ScalarName<U>() != nullptr) {
        d.base = detail::ScalarName<U>();
    } else if constexpr (std::is_same_v<U, ZHLN::Entity>) {
        // { uint32_t index; uint32_t generation; } -- emitted as a named struct
        // so scripts can write .index and .generation. Size 8, align 4, which
        // no scalar spelling reproduces.
        d.base = "ZHLN_Entity";
    } else if constexpr (std::is_same_v<U, JPH::Vec3> || std::is_same_v<U, JPH::Vec4> ||
                         std::is_same_v<U, JPH::Quat>) {
        // Measured 16 bytes at 16-byte alignment. Vec3 stores three floats in a
        // 16-byte SIMD slot, so float[3] would be wrong on both counts.
        d.base      = "float";
        d.count     = 4;
        d.aligned16 = true;
    } else if constexpr (std::is_same_v<U, JPH::Mat44>) {
        d.base      = "float";
        d.count     = 16;
        d.aligned16 = true;
    } else if constexpr (detail::FixedStringTraits<U>::available) {
        // char data[N]; size_t len;  -- emitted as a named struct per capacity.
        d.base = "ZHLN_FixedString";
    } else if constexpr (std::is_array_v<U>) {
        // A raw C array member, e.g. `char _pad[3]`. remove_cvref_t does not
        // strip array-ness, so without this branch it falls through to the
        // opaque case -- correct bytes, but needlessly unnamed.
        constexpr auto element = MapCType<std::remove_extent_t<U>>();
        d.base                 = element.base;
        d.count                = element.count * static_cast<int>(std::extent_v<U>);
        d.aligned16            = element.aligned16;
    } else if constexpr (detail::ArrayTraits<U>::available) {
        constexpr auto element = MapCType<typename detail::ArrayTraits<U>::element>();
        d.base                 = element.base;
        d.count                = element.count * static_cast<int>(detail::ArrayTraits<U>::extent);
        d.aligned16            = element.aligned16;
    } else {
        d.base = nullptr;
    }

    return d;
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
