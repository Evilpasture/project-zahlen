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

namespace Ansi {
[[maybe_unused]] constexpr std::string_view Reset   = "\033[0m";
[[maybe_unused]] constexpr std::string_view Bold    = "\033[1m";
[[maybe_unused]] constexpr std::string_view Dim     = "\033[2m";
[[maybe_unused]] constexpr std::string_view Green   = "\033[32m";
[[maybe_unused]] constexpr std::string_view BGreen  = "\033[1;32m";
[[maybe_unused]] constexpr std::string_view Yellow  = "\033[33m";
[[maybe_unused]] constexpr std::string_view BYellow = "\033[1;33m";
[[maybe_unused]] constexpr std::string_view Cyan    = "\033[36m";
[[maybe_unused]] constexpr std::string_view BCyan   = "\033[1;36m";
[[maybe_unused]] constexpr std::string_view Gray    = "\033[90m";
} // namespace Ansi

auto GetPID() noexcept {
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

struct Token {
    std::string_view key;
    std::string_view value;
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
    using namespace Ansi;

    std::println(
        "\n{}Zahlen Engine{} - version {}{}.{}.{}{} ({}{}{})", BCyan, Reset, BYellow, ZHLN::EngineVersion.major, ZHLN::EngineVersion.minor,
        ZHLN::EngineVersion.patch, Reset, BGreen, ZHLN_GIT_COMMIT_HASH, Reset
    );
    std::println("  {}Target Triple:{}  {}", Gray, Reset, ZHLN_TARGET_TRIPLE);
    std::println("  {}Standard Lib:{}   {}", Gray, Reset, ZHLN::GetSTLVersion());
    std::println("  {}Active Linker:{}  {}", Gray, Reset, ZHLN_LINKER_NAME);
    std::println("  {}Built on:{}       {} (UTC)", Gray, Reset, __DATE__);
    std::println("  {}Build Profile:{}  {} | Sanitizers: {}", Gray, Reset, ZHLN::BuildType, ZHLN::Sanitizers);
    std::println("  {}Compiler:{}       {}", Gray, Reset, ZHLN::Compiler);
    std::println("  {}Compile Flags:{}  {}", Gray, Reset, ZHLN_COMPILER_FLAGS);
    std::println("  {}Metabuild:{}      {} | Build Tool: {}", Gray, Reset, ZHLN_META_BUILDER, ZHLN_BUILD_TOOL);
    std::println("  {}Copyright (C){}   2026 Evilpasture <evilpasture+github@proton.me>", Gray, Reset);

    std::println("\n{}License GPLv3+:{} GNU GPL version 3 or later {}<https://gnu.org/licenses/gpl.html>{}.", BCyan, Reset, Cyan, Reset);
    std::println("This is free software: you are free to change and redistribute it.");
    std::println("There is {}NO WARRANTY{}, to the extent permitted by law.\n", BYellow, Reset);
}

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
    std::string_view placeholder;
    std::string_view description;
    std::expected<void, ZHLN::Error> (*action)(ZHLN::CommandLineOptions&, std::string_view);
};

constexpr std::array Handlers = {
    CommandHandler {
        .key         = "--editor",
        .shortKey    = "",
        .placeholder = "",
        .description = "Launch the interactive world editor",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            opt.launchEditor = true;
            return {};
        }
    },
    CommandHandler {
        .key         = "--version",
        .shortKey    = "",
        .placeholder = "",
        .description = "Display engine version and build metadata",
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
        .description = "Configure Vulkan validation layers: on, off, or gpu (GPU-assisted)",
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
        .key         = "--headless",
        .shortKey    = "",
        .placeholder = "<on|off>",
        .description = "Run headlessly without creating an OS window (default: off)",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view v) -> std::expected<void, ZHLN::Error> {
            if (v.empty() || IsTrue(v)) {
                opt.headless = true;
            } else if (IsFalse(v)) {
                opt.headless = false;
            } else {
                std::println(stderr, "Error: Invalid value '{}' for --headless.", v);
                return std::unexpected(ZHLN::CommandLineError::InvalidValue);
            }
            return {};
        }
    },
    CommandHandler {
        .key         = "--verbose",
        .shortKey    = "",
        .placeholder = "",
        .description = "Enable detailed verbose logging output",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            opt.logLevel = ZHLN::LogLevel::Verbose;
            return {};
        }
    },
    CommandHandler {
        .key         = "--quiet",
        .shortKey    = "",
        .placeholder = "",
        .description = "Disable all console logging outputs (silent mode)",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            opt.logLevel = ZHLN::LogLevel::Quiet;
            return {};
        }
    },
    CommandHandler {
        .key         = "--fps-limit",
        .shortKey    = "",
        .placeholder = "<fps>",
        .description = "Cap framerate to integer target (0 = uncapped)",
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
        .description = "Enable RenderDoc API in-app capture injection (default: off)",
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
        .description = "Execute the automated performance benchmark suite",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            opt.benchmark = true;
            return {};
        }
    },
    CommandHandler {
        .key         = "--debug-attach",
        .shortKey    = "",
        .placeholder = "",
        .description = "Halt startup and wait for an external debugger attachment",
        .action      = [](ZHLN::CommandLineOptions& opt, std::string_view) -> std::expected<void, ZHLN::Error> {
            std::println("\n{}Waiting for debugger attachment. PID: {}{}{}\nPress ENTER to continue...", Ansi::BYellow, Ansi::BCyan, GetPID(), Ansi::Reset);
            std::cin.get();
            return {};
        }
    },
    CommandHandler {
        .key         = "--print-graph",
        .shortKey    = "",
        .placeholder = "[mode]",
        .description = "Print compile-time frame graph for AA mode (none, fxaa, mlaa, taa, smaa)",
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
                    std::println(stderr, "Error: Invalid AA mode '{}' for --print-graph. Valid modes: none, fxaa, mlaa, taa, smaa.", v);
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

void PrintHelp(std::string_view exeName) {
    using namespace Ansi;

    std::println("\n{}Usage:{} {}{}{} {}[options]{}\n", BCyan, Reset, BYellow, exeName, Reset, Gray, Reset);

    std::println("{}Options:{}", BCyan, Reset);

    for (const auto& handler: Handlers) {
        // Measure uncolored length to calculate exact column padding
        std::string rawOpt;
        if (!handler.shortKey.empty()) {
            rawOpt += handler.shortKey;
            rawOpt += ", ";
        }
        rawOpt += handler.key;
        if (!handler.placeholder.empty()) {
            rawOpt += " ";
            rawOpt += handler.placeholder;
        }

        constexpr size_t targetWidth = 32;
        size_t           padLen      = (rawOpt.length() < targetWidth) ? (targetWidth - rawOpt.length()) : 2;
        std::string      padding(padLen, ' ');

        // Format colored column
        std::string optCol = "  ";
        if (!handler.shortKey.empty()) {
            optCol += std::format("{}{}{}, ", Yellow, handler.shortKey, Reset);
        }
        optCol += std::format("{}{}{}", BGreen, handler.key, Reset);

        if (!handler.placeholder.empty()) {
            optCol += std::format(" {}{}{}", Cyan, handler.placeholder, Reset);
        }
        optCol += padding;

        std::println("{}{}", optCol, handler.description);
    }

    std::println("\n{}Environment Variables:{}", BCyan, Reset);

    auto printEnv = [](std::string_view name, std::string_view val, std::string_view desc) {
        std::string      rawEnv      = std::format("{}={}", name, val);
        constexpr size_t targetWidth = 32;
        size_t           padLen      = (rawEnv.length() < targetWidth) ? (targetWidth - rawEnv.length()) : 2;
        std::string      padding(padLen, ' ');

        std::println("  {}{}{}={}{}{}{}{}", BYellow, name, Reset, Cyan, val, Reset, padding, desc);
    };

    printEnv("ZHLN_VALIDATION", "off|on|gpu", "Configure Vulkan validation mode (default: on)");
    printEnv("ZHLN_HEADLESS", "off|on", "Run headlessly without creating an OS window");
    printEnv("ZHLN_NO_GPU_CULLING", "1", "Diagnostic: Force CPU frustum culling path");
    printEnv("ZHLN_DEBUG_INDIRECT", "1", "Diagnostic: Log GPU indirect draw telemetry");
    std::println("");
}

} // namespace

namespace ZHLN {

std::expected<CommandLineOptions, Error> HandleCommandLine(std::span<char* const> args) {
    CommandLineOptions options {.args = args, .validationMode = ValidationMode::On, .launchEditor = false};

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

    if (const char* envVal = std::getenv("ZHLN_HEADLESS")) {
        std::string_view val(envVal);
        if (IsTrue(val)) {
            options.headless = true;
        } else if (IsFalse(val)) {
            options.headless = false;
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
            std::println(stderr, "{}Error: Unknown argument: '{}'{}\n", Ansi::BYellow, tok.key, Ansi::Reset);

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
