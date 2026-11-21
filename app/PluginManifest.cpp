// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Manifest grammar and reader. See Zahlen/PluginManifest.hpp for the format and
// for why it is parsed here instead of through plugins/toml.

#include <Zahlen/PluginManifest.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN::Plugin {

namespace {

constexpr std::string_view kWhitespace = " \t\r\n\f\v";

[[nodiscard]] constexpr auto Trim(std::string_view text) noexcept -> std::string_view {
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

// Strips a `#` comment unless the `#` sits inside a double-quoted string. A
// manifest is one string per line, so a single forward scan is enough: there is
// no state to carry between lines.
[[nodiscard]] auto StripComment(std::string_view line) noexcept -> std::string_view {
    bool quoted = false;
    bool escape = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (quoted && c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && c == '#') {
            return line.substr(0, i);
        }
    }
    return line;
}

// A quoted string value, including the surrounding quotes. Returns the decoded
// text, or false when the closing quote is missing or an escape is unknown.
[[nodiscard]] auto DecodeQuoted(std::string_view token, std::string& out, std::string& detail) -> bool {
    if (token.size() < 2 || token.front() != '"') {
        detail = std::string(token);
        return false;
    }
    out.clear();
    for (size_t i = 1; i < token.size(); ++i) {
        const char c = token[i];
        if (c == '\\') {
            if (i + 1 >= token.size()) {
                detail = std::string(token);
                return false;
            }
            const char escaped = token[++i];
            switch (escaped) {
                case '\\': out.push_back('\\'); break;
                case '"': out.push_back('"'); break;
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                default: detail = std::string(token); return false;
            }
            continue;
        }
        if (c == '"') {
            // The closing quote must end the token; trailing text is a syntax
            // error rather than something to ignore.
            return Trim(token.substr(i + 1)).empty();
        }
        out.push_back(c);
    }
    detail = std::string(token);
    return false;
}

// `["a", "b"]`, `[]`, or a string array with trailing comma. Nested arrays and
// non-string elements are rejected: this subset has no use for them.
[[nodiscard]] auto DecodeStringArray(std::string_view token, std::vector<std::string>& out, std::string& detail) -> bool {
    if (token.size() < 2 || token.front() != '[' || token.back() != ']') {
        detail = std::string(token);
        return false;
    }
    out.clear();
    const std::string_view body = Trim(token.substr(1, token.size() - 2));
    if (body.empty()) {
        return true;
    }
    size_t start = 0;
    while (start <= body.size()) {
        const size_t comma = body.find(',', start);
        const auto   piece = Trim(body.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (!piece.empty()) {
            std::string element;
            if (!DecodeQuoted(piece, element, detail) || piece.front() != '"') {
                detail = std::string(piece);
                return false;
            }
            out.push_back(std::move(element));
        } else if (comma == std::string_view::npos) {
            break;
        }
        if (comma == std::string_view::npos) {
            break;
        }
        start = comma + 1;
    }
    return true;
}

[[nodiscard]] auto DecodeBool(std::string_view token, bool& out) noexcept -> bool {
    if (token == "true") {
        out = true;
        return true;
    }
    if (token == "false") {
        out = false;
        return true;
    }
    return false;
}

// Unsigned decimal integer, no sign, no separators, bounded to what api fits.
[[nodiscard]] auto DecodeUint32(std::string_view token, uint32_t& out) noexcept -> bool {
    if (token.empty() || token.size() > 10) {
        return false;
    }
    uint64_t value = 0;
    for (const char c: token) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<uint64_t>(c - '0');
        if (value > 0xFFFFFFFFull) {
            return false;
        }
    }
    out = static_cast<uint32_t>(value);
    return true;
}

struct KeyState {
    bool name        = false;
    bool library     = false;
    bool api         = false;
    bool description = false;
    bool required    = false;
    bool optional    = false;
    bool autoLoad    = false;
    bool hotReload   = false;
};

} // namespace

auto IsValidPluginName(std::string_view name) noexcept -> bool {
    if (name.empty() || name.size() > 96) {
        return false;
    }
    if (name.front() == '-' || name.front() == '.' || name.back() == '-' || name.back() == '.') {
        return false;
    }
    for (const char c: name) {
        const bool lower = c >= 'a' && c <= 'z';
        const bool digit = c >= '0' && c <= '9';
        if (!lower && !digit && c != '_' && c != '-' && c != '.') {
            return false;
        }
    }
    return true;
}

auto ManifestFileName(std::string_view libraryStem) -> std::string {
    std::string name(libraryStem);
    name.append(kManifestSuffix);
    return name;
}

auto ParsePluginManifest(std::string_view text) -> std::expected<PluginManifest, ManifestIssue> {
    if (text.size() > kMaxManifestBytes) {
        return std::unexpected(ManifestIssue {.code = ManifestIssueCode::TooLarge, .line = 0, .detail = {}});
    }

    PluginManifest manifest;
    KeyState       seen;
    bool           inPluginTable = false;
    uint32_t       lineNumber    = 0;

    size_t cursor = 0;
    while (cursor <= text.size()) {
        const size_t newline = text.find('\n', cursor);
        const auto   rawLine = text.substr(cursor, newline == std::string_view::npos ? std::string_view::npos : newline - cursor);
        cursor               = newline == std::string_view::npos ? text.size() + 1 : newline + 1;
        ++lineNumber;

        const std::string_view line = Trim(StripComment(rawLine));
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[') {
            if (line.back() != ']') {
                return std::unexpected(ManifestIssue {.code = ManifestIssueCode::UnknownSection, .line = lineNumber, .detail = std::string(line)});
            }
            const std::string_view section = Trim(line.substr(1, line.size() - 2));
            if (section != "plugin") {
                return std::unexpected(ManifestIssue {.code = ManifestIssueCode::UnknownSection, .line = lineNumber, .detail = std::string(section)});
            }
            inPluginTable = true;
            continue;
        }

        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return std::unexpected(ManifestIssue {.code = ManifestIssueCode::InvalidValue, .line = lineNumber, .detail = std::string(line)});
        }
        const std::string_view key   = Trim(line.substr(0, equals));
        const std::string_view value = Trim(line.substr(equals + 1));
        if (key.empty() || value.empty()) {
            return std::unexpected(ManifestIssue {.code = ManifestIssueCode::InvalidValue, .line = lineNumber, .detail = std::string(line)});
        }
        // Keys outside the table are a syntax error in real TOML; here they are
        // almost always a missing `[plugin]` line, so say so.
        if (!inPluginTable) {
            return std::unexpected(ManifestIssue {.code = ManifestIssueCode::UnknownSection, .line = lineNumber, .detail = std::string(key)});
        }

        // Reports the second occurrence of a key and marks the first as seen.
        // Duplicates are an error because the alternative -- last one wins --
        // silently changes start order when a file is edited by splicing.
        const auto duplicate = [&](bool& flag) -> std::optional<ManifestIssue> {
            if (flag) {
                return ManifestIssue {.code = ManifestIssueCode::DuplicateKey, .line = lineNumber, .detail = std::string(key)};
            }
            flag = true;
            return std::nullopt;
        };

        if (key == "name") {
            if (const auto issue = duplicate(seen.name)) {
                return std::unexpected(*issue);
            }
            std::string detail;
            if (!DecodeQuoted(value, manifest.name, detail) || !IsValidPluginName(manifest.name)) {
                return std::unexpected(ManifestIssue {.code = ManifestIssueCode::InvalidName, .line = lineNumber, .detail = std::string(value)});
            }
        } else if (key == "library") {
            if (const auto issue = duplicate(seen.library)) {
                return std::unexpected(*issue);
            }
            std::string detail;
            if (!DecodeQuoted(value, manifest.library, detail) || manifest.library.empty()) {
                return std::unexpected(ManifestIssue {.code = ManifestIssueCode::InvalidLibrary, .line = lineNumber, .detail = std::string(value)});
            }
            // A library is a file stem or a file name; a path separator here
            // would let a manifest point outside every configured search dir.
            if (manifest.library.find('/') != std::string::npos || manifest.library.find('\\') != std::string::npos) {
                return std::unexpected(ManifestIssue {.code = ManifestIssueCode::InvalidLibrary, .line = lineNumber, .detail = manifest.library});
            }
        } else if (key == "api") {
            if (const auto issue = duplicate(seen.api)) {
                return std::unexpected(*issue);
            }
            uint32_t parsed = 0;
            if (!DecodeUint32(value, parsed)) {
                return std::unexpected(ManifestIssue {.code = ManifestIssueCode::InvalidValue, .line = lineNumber, .detail = std::string(value)});
            }
            manifest.api = parsed;
        } else if (key == "description") {
            if (const auto issue = duplicate(seen.description)) {
                return std::unexpected(*issue);
            }
            std::string detail;
            if (!DecodeQuoted(value, manifest.description, detail)) {
                return std::unexpected(ManifestIssue {.code = ManifestIssueCode::UnterminatedString, .line = lineNumber, .detail = std::string(value)});
            }
        } else if (key == "requires" || key == "optional_requires") {
            const bool isRequired = key == "requires";
            if (const auto issue = duplicate(isRequired ? seen.required : seen.optional)) {
                return std::unexpected(*issue);
            }
            std::vector<std::string> parsed;
            std::string              detail;
            if (!DecodeStringArray(value, parsed, detail)) {
                return std::unexpected(ManifestIssue {.code = ManifestIssueCode::UnterminatedArray, .line = lineNumber, .detail = std::string(value)});
            }
            for (const auto& dependency: parsed) {
                if (!IsValidPluginName(dependency)) {
                    return std::unexpected(ManifestIssue {.code = ManifestIssueCode::InvalidName, .line = lineNumber, .detail = dependency});
                }
                if (dependency == manifest.name) {
                    return std::unexpected(ManifestIssue {.code = ManifestIssueCode::SelfDependency, .line = lineNumber, .detail = dependency});
                }
            }
            for (size_t i = 0; i < parsed.size(); ++i) {
                for (size_t j = i + 1; j < parsed.size(); ++j) {
                    if (parsed[i] == parsed[j]) {
                        return std::unexpected(ManifestIssue {.code = ManifestIssueCode::DuplicateDependency, .line = lineNumber, .detail = parsed[i]});
                    }
                }
            }
            (isRequired ? manifest.required : manifest.optional) = std::move(parsed);
        } else if (key == "auto_load" || key == "hot_reload") {
            const bool isAutoLoad = key == "auto_load";
            if (const auto issue = duplicate(isAutoLoad ? seen.autoLoad : seen.hotReload)) {
                return std::unexpected(*issue);
            }
            bool parsed = false;
            if (!DecodeBool(value, parsed)) {
                return std::unexpected(ManifestIssue {.code = ManifestIssueCode::InvalidValue, .line = lineNumber, .detail = std::string(value)});
            }
            (isAutoLoad ? manifest.autoLoad : manifest.hotReload) = parsed;
        } else {
            return std::unexpected(ManifestIssue {.code = ManifestIssueCode::UnknownKey, .line = lineNumber, .detail = std::string(key)});
        }
    }

    if (!seen.name) {
        return std::unexpected(ManifestIssue {.code = ManifestIssueCode::MissingName, .line = 0, .detail = {}});
    }
    if (!seen.library) {
        return std::unexpected(ManifestIssue {.code = ManifestIssueCode::MissingLibrary, .line = 0, .detail = manifest.name});
    }
    if (manifest.api != kManifestApi) {
        return std::unexpected(ManifestIssue {
            .code   = ManifestIssueCode::UnsupportedApi,
            .line   = 0,
            .detail = std::to_string(manifest.api) + " (host speaks " + std::to_string(kManifestApi) + ")",
        });
    }
    if (!manifest.name.empty()) {
        for (const auto& dependency: manifest.required) {
            for (const auto& optionalDependency: manifest.optional) {
                if (dependency == optionalDependency) {
                    return std::unexpected(ManifestIssue {.code = ManifestIssueCode::DuplicateDependency, .line = 0, .detail = dependency});
                }
            }
        }
    }
    return manifest;
}

} // namespace ZHLN::Plugin
