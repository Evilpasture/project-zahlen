// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Core/ErrorDescribe.hpp
//
// Compile-time anchored error reflection: an ErrorCode's category hash is
// Hash32(Reflect::ScopedTypeName<E>()), and Reflect::kEnumEntries<E> already
// carries every enumerator's name and Description message at compile time.
// This turns that into a process-wide table keyed by category hash, held
// inside libzahlen_engine behind exported functions (an inline header
// table would be duplicated per DSO under `local: *;` symbol hiding).
// The ErrorCode(E) constructor anchors every enum it carries, so every
// category that can be described by an IErrorRegistry can also be
// described here with no registry instance at all. That is what lets std::formatter<ErrorCode> print
// "Category::Name: message" in a CLI tool, a unit test, or a log line
// without anyone allocating a COM registry first. IErrorRegistry remains
// the mechanism for dynamically loaded plugins whose enums are not visible
// at compile time in the host; the two describe paths agree because both
// are built from the same kEnumEntries table.
//
// Entries are inserted once (hash collisions rejected) and never removed:
// registration copies names and messages into engine-owned storage, so
// unloading a plugin DSO cannot leave a dangling pointer behind.

#include <Zahlen/Core/ErrorCode.hpp>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace ZHLN {

// Anchors enum E's compile-time reflection table to its ErrorCode
// category. Idempotent. Called automatically by the ErrorCode(E)
// constructor and by COM::RegisterEnum<E>; explicit calls are only needed
// for enums that are formatted before any ErrorCode is constructed.
template <typename E>
    requires std::is_enum_v<E>
inline auto RegisterStaticErrorCategory() noexcept -> bool {
    TemplatedDetail::AnchorEnumCategory<E>();
    return true;
}

// Human-readable rendering with no registry instance: "Category::Name:
// message" for an anchored category, raw "error 0x...:N" otherwise
// (never-anchored plugin categories still need IErrorRegistry::Describe).
// Implemented in libzahlen_engine over the single engine-owned table.
[[nodiscard]] ZHLN_API auto DescribeStaticError(ErrorCode code) -> std::string;

} // namespace ZHLN
