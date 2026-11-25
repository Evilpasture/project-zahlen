// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Error.hpp
//
// Error and ErrorCode used to be two 8-byte wrappers around the same words,
// hashing the category through different type names. They are one type now:
// ZHLN::ErrorCode (Zahlen/Core/ErrorCode.hpp). Human-readable text for a
// statically anchored category comes from Zahlen/Core/ErrorDescribe.hpp:
// RegisterEnum<E> anchors E's compile-time reflection entries into a static
// table, so the std::formatter below prints "Category::Name: message" with
// no IErrorRegistry instance. Call sites with a registry can still Describe
// for the structured form; dynamically loaded plugin categories that were
// never statically anchored fall back to the raw "error 0x...:N" words.

#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Core/ErrorDescribe.hpp>
#include <format>

namespace std {
template <>
struct formatter<ZHLN::ErrorCode, char> {
    constexpr auto parse(format_parse_context& ctx) -> format_parse_context::iterator {
        return ctx.begin();
    }

    auto format(const ZHLN::ErrorCode& code, format_context& ctx) const -> format_context::iterator {
        return std::format_to(ctx.out(), "{}", ZHLN::DescribeStaticError(code));
    }
};
} // namespace std
