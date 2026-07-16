// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Zahlen/CommandLine.hpp"
#include "Zahlen/Config.hpp"
#include "Zahlen/Types.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <expected>
#include <filesystem>
#include <iostream>
#include <print>
#include <span>
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

#ifndef ZHLN_TARGET_TRIPLE
#define ZHLN_TARGET_TRIPLE "unknown"
#endif

#ifndef ZHLN_LINKER_NAME
#define ZHLN_LINKER_NAME "unknown"
#endif

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace ZHLN {
extern std::string_view GetRenderGraphDump(AAMode currentMode) noexcept;
}

namespace {

auto GetPID() noexcept {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

struct Token {
    std::string_view key;
    std::string_view value; // Empty if no value was provided
};

std::vector<Token> Tokenize(std::span<char* const> args) {
    std::vector<Token> tokens;
    for (size_t i = 1; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (auto pos = arg.find('='); pos != std::string_view::npos) {
            tokens.push_back({.key = arg.substr(0, pos), .value = arg.substr(pos + 1)});
        } else if (arg.starts_with("-") && (i + 1 < args.size())) {
            std::string_view next = args[i + 1];
            if (!next.starts_with("-")) {
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

void PrintVersion() {
    std::println(
        "Zahlen Engine - version {}.{}.{} ({})", ZHLN::EngineVersion.major, ZHLN::EngineVersion.minor, ZHLN::EngineVersion.patch, ZHLN_GIT_COMMIT_HASH
    );
    std::println("Target Triple:  {}", ZHLN_TARGET_TRIPLE);
    std::println("Standard Lib:   {}", ZHLN::GetSTLVersion());
    std::println("Active Linker:  {}", ZHLN_LINKER_NAME);
    std::println("Built on:      {} (UTC)", __DATE__);
    std::println("Build Profile: {} | Sanitizers: {}", ZHLN::BuildType, ZHLN::Sanitizers);
    std::println("Compiler:      {}", ZHLN::Compiler);
    std::println("Compile Flags: {}", ZHLN_COMPILER_FLAGS);
    std::println("Generator/Metabuild Tool:  {}", ZHLN_META_BUILDER);
    std::println("Build Tool:    {}", ZHLN_BUILD_TOOL);
    std::println("Copyright (C) {}, {}, Contact: {}", "Evilpasture", 2026, "evilpasture+github@proton.me");

    std::println(
        "\nLicense GPLv3+: GNU GPL version 3 or later "
        "<https://gnu.org/licenses/gpl.html>."
    );
    std::println("This is free software: you are free to change and redistribute it.");
    std::println("There is NO WARRANTY, to the extent permitted by law.");
}

void PrintHelp(std::string_view exeName) {
    static constexpr std::string_view HelpMenu =
        R"(
Usage: {} [options]

Options:
  -h, --help               Display this help menu and exit
  --version                Display engine version information and exit
  --editor                 Launch the world editor instead of the game
  --no-validation          Disable Vulkan validation layers completely
  --vsync <on|off>         Enable or disable vertical synchronization (default: on)
  --fullscreen <on|off>    Enable or disable fullscreen mode (default: off)
  --fps-limit <value>      Limit the framerate to the specified integer (0 = uncapped)
  --verbose                Enable detailed verbose logging outputs
  --quiet                  Disable all logging outputs (silent mode)
  --renderdoc <on|off>     Load RenderDoc library at startup (default: off)
  --benchmark              Run the benchmark suite
  --print-graph [mode]     Print the compile-time generated render graph for the specified AA mode (none, fxaa, mlaa, taa, smaa) and exit (default: taa)
  --debug-attach           Wait for a program to attach to the running engine process

Environment Variables:
  ZHLN_VALIDATION=0    Disable Vulkan validation layers

)";
    std::print(HelpMenu, exeName);
}

constexpr bool IsTrue(std::string_view val) noexcept {
    return val == "on" || val == "true" || val == "1" || val == "yes";
}

constexpr bool IsFalse(std::string_view val) noexcept {
    return val == "off" || val == "false" || val == "0" || val == "no";
}

struct CommandHandler {
    std::string_view key;
    std::string_view shortKey;
    std::expected<void, ZHLN::Error> (*action)(ZHLN::CommandLineOptions&, std::string_view);
};

constexpr std::array Handlers = {
    CommandHandler {
        .key      = "--editor",
        .shortKey = "",
        .action   = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            opt.launchEditor = true;
            return {};
        }
    },
    CommandHandler {
        .key      = "--version",
        .shortKey = "",
        .action   = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            PrintVersion();
            opt.versionRequested = true;
            return {};
        }
    },
    CommandHandler {
        .key      = "--help",
        .shortKey = "-h",
        .action   = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            std::string exeName = "zahlen";
            if (!opt.args.empty() && opt.args[0] != nullptr) {
                exeName = std::filesystem::path(opt.args[0]).filename().string();
            }
            PrintHelp(exeName);
            opt.helpRequested = true;
            return {};
        }
    },
    CommandHandler {
        .key      = "--no-validation",
        .shortKey = "",
        .action   = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            opt.enableValidation = false;
            return {};
        }
    },
    CommandHandler {
        .key      = "--vsync",
        .shortKey = "",
        .action   = [](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::Error> {
            if (v.empty()) {
                opt.vsync = true;
            } else if (IsTrue(v) || IsFalse(v)) {
                opt.vsync = IsTrue(v);
            } else {
                std::println(stderr, "Error: Invalid value '{}' for --vsync.", v);
                return std::unexpected(ZHLN::CommandLineError::InvalidValue);
            }
            return {};
        }
    },
    CommandHandler {
        .key      = "--fullscreen",
        .shortKey = "",
        .action   = [](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::Error> {
            if (v.empty()) {
                opt.fullscreen = true;
            } else if (IsTrue(v) || IsFalse(v)) {
                opt.fullscreen = IsTrue(v);
            } else {
                std::println(stderr, "Error: Invalid value '{}' for --fullscreen.", v);
                return std::unexpected(ZHLN::CommandLineError::InvalidValue);
            }
            return {};
        }
    },
    CommandHandler {
        .key      = "--verbose",
        .shortKey = "",
        .action   = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            opt.logLevel = ZHLN::LogLevel::Verbose;
            return {};
        }
    },
    CommandHandler {
        .key      = "--quiet",
        .shortKey = "",
        .action   = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            opt.logLevel = ZHLN::LogLevel::Quiet;
            return {};
        }
    },
    CommandHandler {
        .key      = "--fps-limit",
        .shortKey = "",
        .action   = [](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::Error> {
            if (v.empty()) {
                std::println(stderr, "Error: --fps-limit requires an integer value.");
                return std::unexpected(ZHLN::CommandLineError::MissingValue);
            }
            uint32_t val   = 0;
            auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), val);
            if (ec != std::errc {}) {
                std::println(stderr, "Error: Invalid value '{}' for --fps-limit.", v);
                return std::unexpected(ZHLN::CommandLineError::InvalidValue);
            }
            opt.fpsLimit = val;
            return {};
        }
    },

    CommandHandler {
        .key      = "--renderdoc",
        .shortKey = "",
        .action   = [](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::Error> {
            if (v.empty() || IsTrue(v)) {
                opt.enableRenderDoc = true;
            } else if (IsFalse(v)) {
                opt.enableRenderDoc = false;
            } else {
                std::println(stderr, "Error: Invalid value '{}' for --renderdoc.", v);
                return std::unexpected(ZHLN::CommandLineError::InvalidValue);
            }
            return {};
        }
    },
    CommandHandler {
        .key      = "--benchmark",
        .shortKey = "",
        .action   = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            opt.benchmark = true;
            return {};
        }
    },
    CommandHandler {
        .key      = "--debug-attach",
        .shortKey = "",
        .action   = [](ZHLN::CommandLineOptions&, std::string_view) -> std::expected<void, ZHLN::Error> {
            std::println(
                R"(
Waiting for attachment. PID: {}
Press ENTER to continue.
)",
                GetPID()
            );

            std::cin.get();
            return {};
        }
    },
    CommandHandler {
        .key      = "--print-graph",
        .shortKey = "",
        .action   = [](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::Error> {
            ZHLN::AAMode mode = ZHLN::AAMode::TAA;
            if (!v.empty()) {
                std::string modeStr(v);
                std::ranges::transform(modeStr, modeStr.begin(), [](unsigned char c) { return std::tolower(c); });
                if (modeStr == "none") {
                    mode = ZHLN::AAMode::None;
                } else if (modeStr == "fxaa") {
                    mode = ZHLN::AAMode::FXAA;
                } else if (modeStr == "mlaa") {
                    mode = ZHLN::AAMode::MLAA;
                } else if (modeStr == "taa") {
                    mode = ZHLN::AAMode::TAA;
                } else if (modeStr == "smaa") {
                    mode = ZHLN::AAMode::SMAA;
                } else {
                    std::println(stderr, "Error: Invalid AA mode '{}' for --print-graph. Valid modes are: none, fxaa, mlaa, taa, smaa.", v);
                    return std::unexpected(ZHLN::CommandLineError::InvalidValue);
                }
            }
            std::println("{}", ZHLN::GetRenderGraphDump(mode));
            opt.printGraphRequested = true;
            return {};
        }
    },
};

} // namespace

namespace ZHLN {

std::expected<CommandLineOptions, Error> HandleCommandLine(std::span<char* const> args) {
    CommandLineOptions options {.args = args, .enableValidation = true, .launchEditor = false};

    // Check environment variables first (allows easy IDE configuration profiles)
    if (const char* envVal = std::getenv("ZHLN_VALIDATION")) {
        if (std::string_view(envVal) == "0" || std::string_view(envVal) == "false") {
            options.enableValidation = false;
        }
    }

    std::vector<Token> tokens = Tokenize(args);

    for (const auto& tok: tokens) {
        const auto* it = std::ranges::find_if(Handlers, [&](const auto& handler) {
            return handler.key == tok.key || (!handler.shortKey.empty() && handler.shortKey == tok.key);
        });

        if (it != Handlers.end()) {
            auto result = it->action(options, tok.value);
            if (!result) {
                return std::unexpected(result.error());
            }
        } else {
            std::println(stderr, "Error: Unknown argument: '{}'", tok.key);

            std::string exeName = "zahlen";
            if (!args.empty() && args[0] != nullptr) {
                exeName = std::filesystem::path(args[0]).filename().string();
            }
            PrintHelp(exeName);

            return std::unexpected(CommandLineError::UnknownArgument);
        }
    }
    return options;
}
} // namespace ZHLN
