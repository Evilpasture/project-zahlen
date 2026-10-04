// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <ProjectLightClient/ClientConfig.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace ZHLN::ProjectLight {
namespace {

using Variables = std::unordered_map<std::string, std::string>;

struct ConfigError {
    std::string message;
};

constexpr std::array<std::string_view, 7> kSupportedKeys = {
    "PROJECT_LIGHT_SCHEME",   "PROJECT_LIGHT_SERVER", "PROJECT_LIGHT_PORT",  "PROJECT_LIGHT_USER_ID",
    "PROJECT_LIGHT_USERNAME", "PROJECT_LIGHT_TOKEN",  "PROJECT_LIGHT_PLACE",
};

auto Trim(std::string_view text) noexcept -> std::string_view {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

auto IsKeyCharacter(char value, bool first) noexcept -> bool {
    const auto c = static_cast<unsigned char>(value);
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || value == '_' || (!first && c >= '0' && c <= '9');
}

auto ParseValue(std::string_view raw, size_t lineNumber) -> std::expected<std::string, ConfigError> {
    raw = Trim(raw);
    if (raw.empty()) {
        return std::string {};
    }

    const char quote = raw.front();
    if (quote == '\'' || quote == '"') {
        std::string output;
        output.reserve(raw.size());
        bool   closed = false;
        size_t i      = 1;
        for (; i < raw.size(); ++i) {
            const char current = raw[i];
            if (current == quote) {
                closed = true;
                ++i;
                break;
            }
            if (current == '\\' && quote == '"' && i + 1 < raw.size()) {
                const char escaped = raw[++i];
                switch (escaped) {
                    case 'n':
                        output.push_back('\n');
                        break;
                    case 'r':
                        output.push_back('\r');
                        break;
                    case 't':
                        output.push_back('\t');
                        break;
                    default:
                        output.push_back(escaped);
                        break;
                }
            } else {
                output.push_back(current);
            }
        }
        if (!closed) {
            return std::unexpected(ConfigError {"dotenv line " + std::to_string(lineNumber) + " has an unterminated quoted value"});
        }
        const std::string_view suffix = Trim(raw.substr(i));
        if (!suffix.empty() && !suffix.starts_with('#')) {
            return std::unexpected(ConfigError {"dotenv line " + std::to_string(lineNumber) + " has text after a quoted value"});
        }
        return output;
    }

    // In unquoted values, a # starts a comment. Quote values that contain #.
    const size_t comment = raw.find('#');
    if (comment != std::string_view::npos) {
        raw = Trim(raw.substr(0, comment));
    }
    return std::string(raw);
}

auto ParseVariables(std::string_view text) -> std::expected<Variables, std::string> {
    Variables variables;
    size_t    lineNumber = 0;
    while (!text.empty()) {
        ++lineNumber;
        const size_t     newline = text.find('\n');
        std::string_view line    = Trim(text.substr(0, newline));
        if (newline == std::string_view::npos) {
            text = {};
        } else {
            text.remove_prefix(newline + 1);
        }

        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        if (line.starts_with("export ")) {
            line = Trim(line.substr(7));
        }
        const size_t equals = line.find('=');
        if (equals == std::string_view::npos) {
            return std::unexpected("dotenv line " + std::to_string(lineNumber) + " is missing '='");
        }

        const std::string_view key = Trim(line.substr(0, equals));
        if (key.empty() || !IsKeyCharacter(key.front(), true) ||
            !std::all_of(key.begin() + 1, key.end(), [](char value) { return IsKeyCharacter(value, false); })) {
            return std::unexpected("dotenv line " + std::to_string(lineNumber) + " has an invalid key");
        }
        auto value = ParseValue(line.substr(equals + 1), lineNumber);
        if (!value) {
            return std::unexpected(value.error().message);
        }
        variables.insert_or_assign(std::string(key), std::move(*value));
    }
    return variables;
}

auto NormalizeScheme(std::string_view value) -> std::expected<std::string, ConfigError> {
    value = Trim(value);
    if (value.ends_with("://")) {
        value.remove_suffix(3);
    }
    if (value.empty()) {
        return std::string {};
    }
    const auto isAsciiAlpha = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); };
    if (!isAsciiAlpha(value.front())) {
        return std::unexpected(ConfigError {"PROJECT_LIGHT_SCHEME must begin with an ASCII letter"});
    }

    std::string scheme;
    scheme.reserve(value.size());
    for (const char c: value) {
        const bool isAlpha = isAsciiAlpha(c);
        const bool isDigit = c >= '0' && c <= '9';
        if (!isAlpha && !isDigit && c != '+' && c != '.' && c != '-') {
            return std::unexpected(ConfigError {"PROJECT_LIGHT_SCHEME contains an invalid URI-scheme character"});
        }
        scheme.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c);
    }
    return scheme;
}

template <typename T>
auto ParseUnsigned(std::string_view text, T& output) noexcept -> bool {
    if (text.empty()) {
        return false;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), output, 10);
    return error == std::errc {} && end == text.data() + text.size();
}

auto BuildSettings(const Variables& variables, std::filesystem::path loadedFrom) -> std::expected<ClientSettings, std::string> {
    auto valueFor = [&variables](std::string_view key) -> std::string_view {
        const auto found = variables.find(std::string(key));
        return found == variables.end() ? std::string_view {} : std::string_view(found->second);
    };

    ClientSettings settings;
    settings.loadedFrom = std::move(loadedFrom);
    auto scheme         = NormalizeScheme(valueFor("PROJECT_LIGHT_SCHEME"));
    if (!scheme) {
        return std::unexpected(scheme.error().message);
    }
    settings.launchScheme = std::move(*scheme);

    const std::string_view server = valueFor("PROJECT_LIGHT_SERVER");
    if (!server.empty()) {
        std::string host;
        uint16_t    port = 0;
        if (!ParseHostPort(server, host, port)) {
            return std::unexpected("PROJECT_LIGHT_SERVER must be a host or host:port endpoint");
        }
        if (host == "0.0.0.0") {
            host = "127.0.0.1";
        }
        settings.launch.host = std::move(host);
        if (port != 0) {
            settings.launch.port = port;
        }
    }

    const std::string_view portText = valueFor("PROJECT_LIGHT_PORT");
    if (!portText.empty()) {
        uint32_t port = 0;
        if (!ParseUnsigned(portText, port) || port == 0 || port > std::numeric_limits<uint16_t>::max()) {
            return std::unexpected("PROJECT_LIGHT_PORT must be an integer from 1 to 65535");
        }
        settings.launch.port = static_cast<uint16_t>(port);
    }

    const std::string_view userIdText = valueFor("PROJECT_LIGHT_USER_ID");
    if (!userIdText.empty() && !ParseUnsigned(userIdText, settings.launch.userId)) {
        return std::unexpected("PROJECT_LIGHT_USER_ID must be a non-negative 64-bit integer");
    }

    settings.launch.username = std::string(valueFor("PROJECT_LIGHT_USERNAME"));
    settings.launch.token    = std::string(valueFor("PROJECT_LIGHT_TOKEN"));
    settings.launch.place    = std::string(valueFor("PROJECT_LIGHT_PLACE"));

    settings.launch.enabled = !server.empty() || !portText.empty() || !userIdText.empty() || !valueFor("PROJECT_LIGHT_USERNAME").empty() ||
                              !valueFor("PROJECT_LIGHT_TOKEN").empty() || !valueFor("PROJECT_LIGHT_PLACE").empty();
    return settings;
}

auto ReadFile(const std::filesystem::path& path) -> std::expected<std::string, ConfigError> {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(ConfigError {"could not open project-light config file '" + path.string() + "'"});
    }
    std::string contents {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (input.bad()) {
        return std::unexpected(ConfigError {"could not read project-light config file '" + path.string() + "'"});
    }
    return contents;
}

auto FindImplicitConfig(const std::filesystem::path& start) -> std::optional<std::filesystem::path> {
    std::filesystem::path directory = start;
    for (size_t depth = 0; depth < 9 && !directory.empty(); ++depth) {
        for (const std::string_view filename: {"project-light.env", ".env"}) {
            auto            candidate = directory / filename;
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error) && !error) {
                return candidate;
            }
        }
        const auto parent = directory.parent_path();
        if (parent == directory) {
            break;
        }
        directory = parent;
    }

    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        auto            candidate = std::filesystem::path(home) / ".config" / "project-light" / "client.env";
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate;
        }
    }
    return std::nullopt;
}

} // namespace

auto ParseClientSettings(std::string_view dotenv, std::string source) -> std::expected<ClientSettings, std::string> {
    auto variables = ParseVariables(dotenv);
    if (!variables) {
        return std::unexpected(variables.error());
    }
    return BuildSettings(*variables, std::filesystem::path(std::move(source)));
}

auto LoadClientSettings(const std::filesystem::path& workingDirectory) -> std::expected<ClientSettings, std::string> {
    std::error_code       pathError;
    std::filesystem::path start = workingDirectory;
    if (start.empty()) {
        start = std::filesystem::current_path(pathError);
    } else if (start.is_relative()) {
        start = std::filesystem::absolute(start, pathError);
    }
    if (pathError) {
        return std::unexpected("could not resolve the working directory for project-light config");
    }

    Variables             variables;
    std::filesystem::path configPath;
    if (const char* configuredPath = std::getenv("PROJECT_LIGHT_ENV_FILE"); configuredPath != nullptr && configuredPath[0] != '\0') {
        configPath = std::filesystem::path(configuredPath);
        if (configPath.is_relative()) {
            configPath = start / configPath;
        }
    } else if (auto discovered = FindImplicitConfig(start)) {
        configPath = std::move(*discovered);
    }

    if (!configPath.empty()) {
        auto contents = ReadFile(configPath);
        if (!contents) {
            return std::unexpected(contents.error().message);
        }
        auto parsed = ParseVariables(*contents);
        if (!parsed) {
            return std::unexpected(configPath.string() + ": " + parsed.error());
        }
        variables = std::move(*parsed);
    }

    // Real process environment wins over the dotenv file. Only the documented
    // keys are copied; unrelated environment variables are left untouched.
    for (const std::string_view key: kSupportedKeys) {
        if (const char* environmentValue = std::getenv(std::string(key).c_str()); environmentValue != nullptr) {
            variables.insert_or_assign(std::string(key), environmentValue);
        }
    }

    return BuildSettings(variables, configPath);
}

} // namespace ZHLN::ProjectLight
