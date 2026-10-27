// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <CommandLine/CommandLine.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Print.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <format>
#include <iterator>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#ifndef ZHLN_GIT_COMMIT_HASH
    #define ZHLN_GIT_COMMIT_HASH "unknown"
#endif

#ifndef ZHLN_COMPILER_FLAGS
    #define ZHLN_COMPILER_FLAGS "unknown"
#endif

#ifndef ZHLN_META_BUILDER
    #define ZHLN_META_BUILDER "unknown"
#endif

#ifndef ZHLN_BUILD_TOOL
    #define ZHLN_BUILD_TOOL "unknown"
#endif

#ifndef ZHLN_LINKER_NAME
    #define ZHLN_LINKER_NAME "unknown"
#endif

namespace ZHLN {
namespace {

constexpr auto MatchesFlag(const CliFlag& flag, std::string_view key) noexcept -> bool {
    return flag.key == key || (!flag.shortKey.empty() && flag.shortKey == key);
}

void PrintFlagRow(const CliFlagDesc& flag) {
    const size_t shortPart = flag.shortKey.empty() ? 0 : (flag.shortKey.size() + 2);
    const size_t placePart = flag.placeholder.empty() ? 0 : (1 + flag.placeholder.size());
    const size_t rawLen    = shortPart + flag.key.size() + placePart;

    constexpr size_t kTargetWidth = 32;
    const size_t     padLen       = (rawLen < kTargetWidth) ? (kTargetWidth - rawLen) : 2;

    std::string line;
    line.reserve(rawLen + padLen + flag.description.size() + 48);
    auto out = std::back_inserter(line);
    std::format_to(out, "  ");
    if (!flag.shortKey.empty()) {
        std::format_to(out, "{}{}{}, ", Color::Yellow, flag.shortKey, Color::Reset);
    }
    std::format_to(out, "{}{}{}", Color::BGreen, flag.key, Color::Reset);
    if (!flag.placeholder.empty()) {
        std::format_to(out, " {}{}{}", Color::Cyan, flag.placeholder, Color::Reset);
    }
    line.append(padLen, ' ');
    std::format_to(out, "{}", flag.description);

    std::println("{}", line);
}

auto ToFlagDescs(std::span<const CliFlag> flags, bool includeHelp, bool includeVersion) -> std::vector<CliFlagDesc> {
    std::vector<CliFlagDesc> descs;
    descs.reserve(flags.size() + 2);
    if (includeHelp) {
        descs.push_back(CliFlagDesc {
            .key         = "--help",
            .shortKey    = "-h",
            .placeholder = "",
            .description = "Display this help menu and exit",
        });
    }
    if (includeVersion) {
        descs.push_back(CliFlagDesc {
            .key         = "--version",
            .shortKey    = "",
            .placeholder = "",
            .description = "Display build version and compiler metadata",
        });
    }
    for (const auto& flag: flags) {
        descs.push_back(CliFlagDesc {
            .key         = flag.key,
            .shortKey    = flag.shortKey,
            .placeholder = flag.placeholder,
            .description = flag.description,
        });
    }
    return descs;
}

} // namespace

auto TokenizeCliArgs(std::span<char* const> args) -> std::vector<CliToken> {
    std::vector<CliToken> tokens;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == nullptr) {
            continue;
        }
        const std::string_view arg = args[i];
        if (const auto pos = arg.find('='); pos != std::string_view::npos) {
            tokens.push_back({.key = arg.substr(0, pos), .value = arg.substr(pos + 1)});
        } else if (arg.starts_with('-') && (i + 1 < args.size()) && args[i + 1] != nullptr) {
            const std::string_view next = args[i + 1];
            if (!next.starts_with('-')) {
                tokens.push_back({.key = arg, .value = next});
                ++i;
            } else {
                tokens.push_back({.key = arg, .value = ""});
            }
        } else {
            tokens.push_back({.key = arg, .value = ""});
        }
    }
    return tokens;
}

auto ExtractExecutableName(std::span<char* const> args, std::string_view fallback) -> std::string_view {
    if (args.empty() || args[0] == nullptr) {
        return fallback;
    }
    const std::string_view raw = args[0];
    if (raw.empty()) {
        return fallback;
    }
    const size_t slash = raw.find_last_of("/\\");
    return slash == std::string_view::npos ? raw : raw.substr(slash + 1);
}

void PrintBuildVersion(std::string_view programTitle) {
    std::println(
        "\n{}{}{} - version {}{}.{}.{}{} ({}{}{})",
        Color::BCyan,
        programTitle,
        Color::Reset,
        Color::BYellow,
        EngineVersion.major,
        EngineVersion.minor,
        EngineVersion.patch,
        Color::Reset,
        Color::BGreen,
        ZHLN_GIT_COMMIT_HASH,
        Color::Reset
    );
    std::println("  {}Target Triple:{}  {}", Color::Gray, Color::Reset, ZHLN_TARGET_TRIPLE);
    std::println("  {}Standard Lib:{}   {}", Color::Gray, Color::Reset, GetSTLVersion());
    std::println("  {}Active Linker:{}  {}", Color::Gray, Color::Reset, ZHLN_LINKER_NAME);
    std::println("  {}Built on:{}       {} (UTC)", Color::Gray, Color::Reset, __DATE__);
    std::println("  {}Build Profile:{}  {} | Sanitizers: {}", Color::Gray, Color::Reset, BuildType, Sanitizers);
    std::println("  {}Compiler:{}       {}", Color::Gray, Color::Reset, Compiler);
    std::println("  {}Compile Flags:{}  {}", Color::Gray, Color::Reset, ZHLN_COMPILER_FLAGS);
    std::println("  {}Metabuild:{}      {} | Build Tool: {}", Color::Gray, Color::Reset, ZHLN_META_BUILDER, ZHLN_BUILD_TOOL);
    std::println("  {}Copyright (C){}   2026 Evilpasture <evilpasture+github@proton.me>", Color::Gray, Color::Reset);

    std::println(
        "\n{}License GPLv3+:{} GNU GPL version 3 or later {}<https://gnu.org/licenses/gpl.html>{}.",
        Color::BCyan,
        Color::Reset,
        Color::Cyan,
        Color::Reset
    );
    std::println("This is free software: you are free to change and redistribute it.");
    std::println("There is {}NO WARRANTY{}, to the extent permitted by law.\n", Color::BYellow, Color::Reset);
}

void PrintCliHelp(
    std::string_view             exeName,
    std::span<const CliFlagDesc> primaryFlags,
    std::span<const CliFlagDesc> secondaryFlags,
    const CliParseConfig&        config
) {
    std::println(
        "\n{}Usage:{} {}{}{} {}{}{}\n",
        Color::BCyan,
        Color::Reset,
        Color::BYellow,
        exeName,
        Color::Reset,
        Color::Gray,
        config.usageSuffix,
        Color::Reset
    );

    if (!primaryFlags.empty()) {
        std::println("{}{}:{}", Color::BCyan, config.primaryHeading, Color::Reset);
        for (const auto& flag: primaryFlags) {
            PrintFlagRow(flag);
        }
    }

    if (!secondaryFlags.empty()) {
        std::println("\n{}{}:{}", Color::BCyan, config.secondaryHeading, Color::Reset);
        for (const auto& flag: secondaryFlags) {
            PrintFlagRow(flag);
        }
    }

    if (!config.environmentDocs.empty()) {
        std::println("\n{}Environment Variables:{}", Color::BCyan, Color::Reset);
        for (const auto& env: config.environmentDocs) {
            const size_t     rawLen       = env.name.size() + 1 + env.value.size();
            constexpr size_t kTargetWidth = 32;
            const size_t     padLen       = (rawLen < kTargetWidth) ? (kTargetWidth - rawLen) : 2;
            const std::string padding(padLen, ' ');
            std::println(
                "  {}{}{}={}{}{}{}{}",
                Color::BYellow,
                env.name,
                Color::Reset,
                Color::Cyan,
                env.value,
                Color::Reset,
                padding,
                env.description
            );
        }
    }
    std::println("");
}

auto ParseCli(
    std::span<char* const>   args,
    std::span<const CliFlag> flags,
    const CliParseConfig&    config
) -> std::expected<CliParseResult, CommandLineError> {
    CliParseResult             result {.args = args};
    const std::string_view     exeName = ExtractExecutableName(args, "zahlen");
    const std::vector<CliToken> tokens = TokenizeCliArgs(args);

    const bool userDefinesHelp = std::ranges::any_of(flags, [](const CliFlag& f) {
        return MatchesFlag(f, "--help") || MatchesFlag(f, "-h");
    });
    const bool userDefinesVersion = std::ranges::any_of(flags, [](const CliFlag& f) {
        return MatchesFlag(f, "--version");
    });

    for (const auto& tok: tokens) {
        if (config.addHelpFlag && !userDefinesHelp && (tok.key == "--help" || tok.key == "-h")) {
            const auto descs = ToFlagDescs(flags, true, config.addVersionFlag && !userDefinesVersion);
            PrintCliHelp(exeName, descs, {}, config);
            result.helpRequested = true;
            continue;
        }
        if (config.addVersionFlag && !userDefinesVersion && tok.key == "--version") {
            PrintBuildVersion(config.programTitle);
            result.versionRequested = true;
            continue;
        }

        const auto it = std::ranges::find_if(flags, [&](const CliFlag& f) { return MatchesFlag(f, tok.key); });
        if (it != flags.end()) {
            const auto step = it->action(tok.value);
            if (!step) {
                return std::unexpected(step.error());
            }
        } else {
            if (config.printHelpOnError) {
                std::println(stderr, "{}Error: Unknown argument: '{}'{}\n", Color::BYellow, tok.key, Color::Reset);
                const auto descs = ToFlagDescs(
                    flags,
                    config.addHelpFlag && !userDefinesHelp,
                    config.addVersionFlag && !userDefinesVersion
                );
                PrintCliHelp(exeName, descs, {}, config);
            }
            return std::unexpected(CommandLineError::UnknownArgument);
        }
    }

    return result;
}

} // namespace ZHLN
