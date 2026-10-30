// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// CommandLine/CommandLine.hpp
//
// Generic, engine-independent command-line tokenizer, flag dispatcher, and
// formatted `--help` / `--version` printer. Links zero engine runtime symbols,
// allowing CLI tools (`ztest`, test workers, offline utilities) to expose only
// their own flags while host layers bind their own configuration through reflection.

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace ZHLN {

enum class CommandLineError : uint8_t {
    InvalidValue = 1,
    MissingValue,
    UnknownArgument,
    DuplicateFlag,
};

struct CliToken {
    std::string_view key;
    std::string_view value;
};

struct CliEnvDoc {
    std::string_view name;
    std::string_view value;
    std::string_view description;
};

struct CliFlagDesc {
    std::string_view key;
    std::string_view shortKey    = "";
    std::string_view placeholder = "";
    std::string_view description = "";
};

struct CliFlag {
    std::string_view                                                       key;
    std::string_view                                                       shortKey    = "";
    std::string_view                                                       placeholder = "";
    std::string_view                                                       description = "";
    std::function<std::expected<void, CommandLineError>(std::string_view)> action;
};

struct CliParseConfig {
    std::string_view           programTitle     = "Zahlen Engine";
    std::string_view           usageSuffix      = "[options]";
    std::string_view           primaryHeading   = "Options";
    std::string_view           secondaryHeading = "Application Options";
    std::span<const CliEnvDoc> environmentDocs  = {};
    bool                       addHelpFlag      = true;
    bool                       addVersionFlag   = true;
    bool                       printHelpOnError = true;
};

struct CliParseResult {
    std::span<char* const> args;
    bool                   helpRequested    = false;
    bool                   versionRequested = false;
};

[[nodiscard]] auto TokenizeCliArgs(std::span<char* const> args) -> std::vector<CliToken>;

[[nodiscard]] auto ExtractExecutableName(std::span<char* const> args, std::string_view fallback = "zahlen") -> std::string_view;

void PrintBuildVersion(std::string_view programTitle = "Zahlen Engine");

void PrintCliHelp(
    std::string_view            exeName,
    std::span<const CliFlagDesc> primaryFlags,
    std::span<const CliFlagDesc> secondaryFlags = {},
    const CliParseConfig&       config         = {}
);

[[nodiscard]] auto ParseCli(
    std::span<char* const>   args,
    std::span<const CliFlag> flags,
    const CliParseConfig&    config = {}
) -> std::expected<CliParseResult, CommandLineError>;

} // namespace ZHLN
