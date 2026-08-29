// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Core/Description.hpp
//
// The annotation vocabulary behind [[= ZHLN::Description<"..."> {}]].
//
// Split out of Reflection.hpp on purpose: an annotation site only needs
// StringLiteral and Description, so it no longer has to pay for <meta>,
// <format>, <ranges> and the rest of the reflection machinery.

#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace ZHLN {

// ============================================================================
// Compile-Time String (Structural Type for Non-Type Template Parameters)
// ============================================================================

template <std::size_t N>
struct StringLiteral {
    std::array<char, N> value {};
    constexpr StringLiteral(const char (&str)[N]) {
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = str[i];
        }
    }

    constexpr operator std::string_view() const {
        return {value.data(), N - 1};
    }
};

template <std::size_t N>
StringLiteral(const char (&)[N]) -> StringLiteral<N>;

// ============================================================================
// ZHLN::Description (Documentation Annotation)
// ============================================================================

template <StringLiteral Text>
struct Description {
    static constexpr std::string_view message = Text;
};

} // namespace ZHLN
