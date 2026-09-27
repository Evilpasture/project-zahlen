// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later


#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace ZHLN {


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


template <StringLiteral Text>
struct Description {
    static constexpr std::string_view message = Text;
};

}

#if defined(__cpp_impl_reflection) || (defined(__has_feature) && __has_feature(reflection))
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ZHLN_ANNOTATION(...) [[= __VA_ARGS__]]
#else
#define ZHLN_ANNOTATION(...)
#endif
