// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// src/gltf/CsgExtras.hpp
//
// Reads the CSG modifiers a Blender export hangs off a glTF node:
//
//     "extras": { "csg_data": "[{\"operation\":\"Difference\",\"operand_name\":\"Hole\"}]" }
//
// -- note the value is a JSON document inside a JSON string, which is what the
// exporter writes so the payload survives round-tripping through tools that
// only understand string custom properties.
//
// This used to go through the reflection-driven JSON layer. That pulled a
// general-purpose parser (and with it simdjson) into zahlen_gltf and, through
// it, into the engine -- to pull two fields out of one optional member of one
// optional node. The scanner below does exactly that job and nothing else, so
// the importer has no JSON dependency at all and the JSON extra stays optional.
//
// It is deliberately small and specific: it understands the two members above,
// string escaping, and the brace/bracket nesting needed to find them. It is not
// a JSON parser and should not grow into one -- if a second caller needs
// structured JSON, that caller belongs on extras/json.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN::GLTF {

/// One modifier as the extras text spells it. `operation` is the name of a
/// CSGOperation enumerator; turning it into the enum is the caller's job, so
/// this header stays free of engine types and compiles on its own.
struct CsgExtrasEntry {
    std::string operation;
    std::string operand_name;
};

namespace detail {

    [[nodiscard]] inline auto IsJsonSpace(const char c) noexcept -> bool {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    /// Returns one past the closing quote of the string starting at
    /// `text[from] == '"'`, or npos if it is unterminated.
    [[nodiscard]] inline auto EndOfJsonString(const std::string_view text, const size_t from) noexcept -> size_t {
        for (size_t i = from + 1; i < text.size(); ++i) {
            if (text[i] == '\\') {
                ++i; // The escaped character never ends the string.
            } else if (text[i] == '"') {
                return i + 1;
            }
        }
        return std::string_view::npos;
    }

    inline void AppendUtf8(std::string& out, uint32_t codePoint) {
        if (codePoint < 0x80u) {
            out += static_cast<char>(codePoint);
        } else if (codePoint < 0x800u) {
            out += static_cast<char>(0xC0u | (codePoint >> 6));
            out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
        } else if (codePoint < 0x10000u) {
            out += static_cast<char>(0xE0u | (codePoint >> 12));
            out += static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
        } else {
            out += static_cast<char>(0xF0u | (codePoint >> 18));
            out += static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu));
            out += static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu));
            out += static_cast<char>(0x80u | (codePoint & 0x3Fu));
        }
    }

    [[nodiscard]] inline auto HexDigit(const char c) noexcept -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }

    /// Undoes JSON string escaping. `token` includes the surrounding quotes, so
    /// a quoted member value can be passed straight from FindMemberValue.
    /// Returns false on a malformed escape, leaving `out` untouched.
    [[nodiscard]] inline auto UnescapeJsonString(const std::string_view token, std::string& out) -> bool {
        if (token.size() < 2 || token.front() != '"' || token.back() != '"') {
            return false;
        }

        std::string decoded;
        decoded.reserve(token.size());

        const std::string_view body = token.substr(1, token.size() - 2);
        for (size_t i = 0; i < body.size(); ++i) {
            if (body[i] != '\\') {
                decoded += body[i];
                continue;
            }
            ++i;
            if (i >= body.size()) {
                return false;
            }
            switch (body[i]) {
                case '"':  decoded += '"';  break;
                case '\\': decoded += '\\'; break;
                case '/':  decoded += '/';  break;
                case 'b':  decoded += '\b'; break;
                case 'f':  decoded += '\f'; break;
                case 'n':  decoded += '\n'; break;
                case 'r':  decoded += '\r'; break;
                case 't':  decoded += '\t'; break;
                case 'u': {
                    if (i + 4 >= body.size()) {
                        return false;
                    }
                    uint32_t codePoint = 0;
                    for (size_t d = 1; d <= 4; ++d) {
                        const int hex = HexDigit(body[i + d]);
                        if (hex < 0) {
                            return false;
                        }
                        codePoint = (codePoint << 4) | static_cast<uint32_t>(hex);
                    }
                    i += 4;

                    // A high surrogate is only half a code point; pair it with
                    // the \uDC00-\uDFFF escape that has to follow it.
                    if (codePoint >= 0xD800u && codePoint <= 0xDBFFu && i + 6 < body.size() && body[i + 1] == '\\' && body[i + 2] == 'u') {
                        uint32_t low = 0;
                        bool     ok  = true;
                        for (size_t d = 3; d <= 6; ++d) {
                            const int hex = HexDigit(body[i + d]);
                            if (hex < 0) {
                                ok = false;
                                break;
                            }
                            low = (low << 4) | static_cast<uint32_t>(hex);
                        }
                        if (ok && low >= 0xDC00u && low <= 0xDFFFu) {
                            codePoint = 0x10000u + ((codePoint - 0xD800u) << 10) + (low - 0xDC00u);
                            i += 6;
                        }
                    }
                    AppendUtf8(decoded, codePoint);
                    break;
                }
                default: return false;
            }
        }

        out = std::move(decoded);
        return true;
    }

    /// The raw text of the value of the first member named `key` at the top
    /// level of `text`. A string value comes back quotes included, so it can go
    /// straight to UnescapeJsonString; anything else comes back as the token up
    /// to the next separator. Nesting and string contents are skipped, so a key
    /// inside a sub-object or inside another member's string is not a match.
    [[nodiscard]] inline auto FindMemberValue(const std::string_view text, const std::string_view key) -> std::optional<std::string_view> {
        size_t depth = 0;
        size_t i     = 0;

        while (i < text.size()) {
            switch (text[i]) {
                case '{':
                case '[':
                    ++depth;
                    ++i;
                    continue;
                case '}':
                case ']':
                    if (depth > 0) {
                        --depth;
                    }
                    ++i;
                    continue;
                case '"': {
                    const size_t end = EndOfJsonString(text, i);
                    if (end == std::string_view::npos) {
                        return std::nullopt; // Unterminated string: give up rather than guess.
                    }
                    const std::string_view raw = text.substr(i + 1, end - i - 2);

                    // A key is a string followed by ':'. Only the top level of
                    // this object is searched.
                    if (depth == 1 && raw == key) {
                        size_t at = end;
                        while (at < text.size() && IsJsonSpace(text[at])) {
                            ++at;
                        }
                        if (at < text.size() && text[at] == ':') {
                            ++at;
                            while (at < text.size() && IsJsonSpace(text[at])) {
                                ++at;
                            }
                            if (at >= text.size()) {
                                return std::nullopt;
                            }
                            if (text[at] == '"') {
                                const size_t valueEnd = EndOfJsonString(text, at);
                                if (valueEnd == std::string_view::npos) {
                                    return std::nullopt;
                                }
                                return text.substr(at, valueEnd - at);
                            }
                            size_t valueEnd = at;
                            while (valueEnd < text.size() && text[valueEnd] != ',' && text[valueEnd] != '}' && text[valueEnd] != ']' &&
                                   !IsJsonSpace(text[valueEnd])) {
                                ++valueEnd;
                            }
                            return text.substr(at, valueEnd - at);
                        }
                    }
                    i = end;
                    continue;
                }
                default:
                    ++i;
                    continue;
            }
        }
        return std::nullopt;
    }

    /// The top-level `{...}` objects of a JSON array, in order. Nested braces
    /// are tracked so an object containing an object is one element, not two.
    [[nodiscard]] inline auto SplitTopLevelObjects(const std::string_view text) -> std::vector<std::string_view> {
        std::vector<std::string_view> objects;
        size_t                        depth = 0;
        size_t                        start = 0;

        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '"') {
                const size_t end = EndOfJsonString(text, i);
                if (end == std::string_view::npos) {
                    break;
                }
                i = end - 1;
                continue;
            }
            if (text[i] == '{') {
                if (depth == 0) {
                    start = i;
                }
                ++depth;
            } else if (text[i] == '}') {
                if (depth > 0) {
                    --depth;
                }
                if (depth == 0) {
                    objects.push_back(text.substr(start, i + 1 - start));
                }
            }
        }
        return objects;
    }

} // namespace detail

/// Pulls every `{"operation": ..., "operand_name": ...}` pair out of a glTF
/// node's `extras` text. An entry missing either member is skipped rather than
/// failing the lot, so one bad modifier does not silently cost a model all of
/// its CSG. Returns an empty vector when the node has no `csg_data`, which is
/// the overwhelmingly common case and costs one scan of a short string.
[[nodiscard]] inline auto ScanCsgExtras(const std::string_view nodeExtras) -> std::vector<CsgExtrasEntry> {
    std::vector<CsgExtrasEntry> entries;

    const auto csgData = detail::FindMemberValue(nodeExtras, "csg_data");
    if (!csgData) {
        return entries;
    }

    std::string payload;
    if (!detail::UnescapeJsonString(*csgData, payload)) {
        return entries;
    }

    for (const std::string_view object: detail::SplitTopLevelObjects(payload)) {
        CsgExtrasEntry entry;
        const auto     operation = detail::FindMemberValue(object, "operation");
        const auto     operand   = detail::FindMemberValue(object, "operand_name");
        if (!operation || !operand || !detail::UnescapeJsonString(*operation, entry.operation) ||
            !detail::UnescapeJsonString(*operand, entry.operand_name)) {
            continue;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace ZHLN::GLTF
