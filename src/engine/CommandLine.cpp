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
#include <format>
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

// Forward declare PrintHelp so that command action lambdas can reference it.
void PrintHelp(std::string_view exeName);

constexpr bool IsTrue(std::string_view val) noexcept {
    return val == "on" || val == "true" || val == "1" || val == "yes";
}

constexpr bool IsFalse(std::string_view val) noexcept {
    return val == "off" || val == "false" || val == "0" || val == "no";
}

struct CommandHandler {
    std::string_view key;
    std::string_view shortKey;
    std::string_view placeholder; // e.g. "<on|off>" or "<value>"
    std::string_view description;
    std::expected<void, ZHLN::Error> (*action)(ZHLN::CommandLineOptions&, std::string_view);
};

constexpr std::array Handlers = {
    CommandHandler {
        .key         = "--editor",
        .shortKey    = "",
        .placeholder = "",
        .description = "Launch the world editor instead of the game",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            opt.launchEditor = true;
            return {};
        }
    },
    CommandHandler {
        .key         = "--version",
        .shortKey    = "",
        .placeholder = "",
        .description = "Display engine version information and exit",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            PrintVersion();
            opt.versionRequested = true;
            return {};
        }
    },
    CommandHandler {
        .key         = "--help",
        .shortKey    = "-h",
        .placeholder = "",
        .description = "Display this help menu and exit",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
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
        .key         = "--validation",
        .shortKey    = "",
        .placeholder = "[on|off|gpu]",
        .description = "Configure Vulkan validation layers: on (default), off, or gpu (GPU-assisted)",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::Error> {
            if (v.empty() || IsTrue(v) || v == "on") {
                opt.validationMode = ZHLN::ValidationMode::On;
            } else if (IsFalse(v) || v == "off") {
                opt.validationMode = ZHLN::ValidationMode::Off;
            } else if (v == "gpu" || v == "gpu_assisted") {
                opt.validationMode = ZHLN::ValidationMode::GPU;
            } else {
                std::println(stderr, "Error: Invalid value '{}' for --validation. Valid options: on, off, gpu.", v);
                return std::unexpected(ZHLN::CommandLineError::InvalidValue);
            }
            return {};
        }
    },
    CommandHandler {
        .key         = "--vsync",
        .shortKey    = "",
        .placeholder = "<on|off>",
        .description = "Enable or disable vertical synchronization (default: on)",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::Error> {
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
        .key         = "--fullscreen",
        .shortKey    = "",
        .placeholder = "<on|off>",
        .description = "Enable or disable fullscreen mode (default: off)",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::Error> {
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
        .key         = "--verbose",
        .shortKey    = "",
        .placeholder = "",
        .description = "Enable detailed verbose logging outputs",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            opt.logLevel = ZHLN::LogLevel::Verbose;
            return {};
        }
    },
    CommandHandler {
        .key         = "--quiet",
        .shortKey    = "",
        .placeholder = "",
        .description = "Disable all logging outputs (silent mode)",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            opt.logLevel = ZHLN::LogLevel::Quiet;
            return {};
        }
    },
    CommandHandler {
        .key         = "--fps-limit",
        .shortKey    = "",
        .placeholder = "<value>",
        .description = "Limit the framerate to the specified integer (0 = uncapped)",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::Error> {
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
        .key         = "--renderdoc",
        .shortKey    = "",
        .placeholder = "<on|off>",
        .description = "Load RenderDoc library at startup (default: off)",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::Error> {
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
        .key         = "--benchmark",
        .shortKey    = "",
        .placeholder = "",
        .description = "Run the benchmark suite",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            opt.benchmark = true;
            return {};
        }
    },
    CommandHandler {
        .key         = "--debug-attach",
        .shortKey    = "",
        .placeholder = "",
        .description = "Wait for a program to attach to the running engine process",
        .action      = [](ZHLN::CommandLineOptions&, std::string_view) -> std::expected<void, ZHLN::Error> {
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
        .key         = "--print-graph",
        .shortKey    = "",
        .placeholder = "[mode]",
        .description = "Print the compile-time generated render graph for the specified AA mode (none, fxaa, mlaa, taa, smaa) and exit (default: taa)",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::Error> {
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
    CommandHandler {
        .key         = "--driver",
        .shortKey    = "-d",
        .placeholder = "<driver>",
        .description = "Select the gameplay loop driver (fennel, cpp, hybrid)",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::Error> {
            if (v == "cpp" || v == "c++" || v == "native") {
                opt.driver = ZHLN::GameplayDriver::Cpp;
            } else if (v == "fennel" || v == "lua") {
                opt.driver = ZHLN::GameplayDriver::Fennel;
            } else if (v == "hybrid") {
                opt.driver = ZHLN::GameplayDriver::Hybrid;
            } else {
                std::println(stderr, "Error: Invalid driver '{}'. Valid options: cpp, fennel, hybrid.", v);
                return std::unexpected(ZHLN::CommandLineError::InvalidValue);
            }
            return {};
        }
    },
};

// Define PrintHelp here now that Handlers is fully declared and initialized.
void PrintHelp(std::string_view exeName) {
    std::println("Usage: {} [options]\n", exeName);
    std::println("Options:");

    for (const auto& handler: Handlers) {
        std::string optStr = handler.shortKey.empty() ? std::string(handler.key) : std::format("{}, {}", handler.shortKey, handler.key);

        if (!handler.placeholder.empty()) {
            optStr += " ";
            optStr += handler.placeholder;
        }

        std::println("  {:<24} {}", optStr, handler.description);
    }

    std::println(
        R"(
Environment Variables:
  ZHLN_VALIDATION=off|on|gpu  Configure Vulkan validation mode
)"
    );
}

} // namespace

namespace ZHLN {

std::expected<CommandLineOptions, Error> HandleCommandLine(std::span<char* const> args) {
    CommandLineOptions options {.args = args, .validationMode = ValidationMode::On, .launchEditor = false};

    // Check environment variables first (allows easy IDE configuration profiles)
    if (const char* envVal = std::getenv("ZHLN_VALIDATION")) {
        std::string_view val(envVal);
        if (val == "0" || val == "off" || val == "false" || val == "no") {
            options.validationMode = ValidationMode::Off;
        } else if (val == "gpu" || val == "gpu_assisted") {
            options.validationMode = ValidationMode::GPU;
        } else if (val == "1" || val == "on" || val == "true" || val == "yes") {
            options.validationMode = ValidationMode::On;
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
