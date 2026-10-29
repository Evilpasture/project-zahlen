// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <CommandLine/EngineCommandLine.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Core/Print.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Render/RenderGraphDump.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN {
namespace {

constexpr auto MatchesHandler(const CommandHandler& handler, std::string_view key) noexcept -> bool {
    return handler.key == key || (!handler.shortKey.empty() && handler.shortKey == key);
}

constexpr auto IsTrue(std::string_view val) noexcept -> bool {
    return val == "on" || val == "true" || val == "1" || val == "yes";
}

constexpr auto IsFalse(std::string_view val) noexcept -> bool {
    return val == "off" || val == "false" || val == "0" || val == "no";
}

constexpr std::array kEngineEnvDocs = {
    CliEnvDoc {
        .name        = "ZHLN_VALIDATION",
        .value       = "off|on|gpu",
        .description = "Configure Vulkan validation mode (default: on)",
    },
    CliEnvDoc {
        .name        = "ZHLN_HEADLESS",
        .value       = "off|on",
        .description = "Run headlessly without creating an OS window",
    },
    CliEnvDoc {
        .name        = "ZHLN_NO_GPU_CULLING",
        .value       = "1",
        .description = "Diagnostic: Force CPU frustum culling path",
    },
};

void PrintEngineHelp(
    std::string_view                exeName,
    std::span<const CommandHandler> engineHandlers,
    std::span<const CommandHandler> appHandlers
);

[[nodiscard]] auto EngineHandlers() noexcept -> std::span<const CommandHandler>;

const std::array kEngineHandlers = {
    CommandHandler {
        .key         = "--version",
        .shortKey    = "",
        .placeholder = "",
        .description = "Display engine version and build metadata",
        .action      = [](CommandLineOptions& opt, std::string_view) -> std::expected<void, ErrorCode> {
            PrintBuildVersion("Zahlen Engine");
            opt.versionRequested = true;
            return {};
        },
    },
    CommandHandler {
        .key         = "--help",
        .shortKey    = "-h",
        .placeholder = "",
        .description = "Display this help menu and exit",
        .action      = [](CommandLineOptions& opt, std::string_view) -> std::expected<void, ErrorCode> {
            const std::string_view exeName = ExtractExecutableName(opt.args, "zahlen");
            PrintEngineHelp(exeName, EngineHandlers(), opt.appHandlers);
            opt.helpRequested = true;
            return {};
        },
    },
    CommandHandler {
        .key         = "--validation",
        .shortKey    = "",
        .placeholder = "[on|off|gpu]",
        .description = "Configure Vulkan validation layers: on, off, or gpu (GPU-assisted)",
        .action      = [](CommandLineOptions& opt, std::string_view v) -> std::expected<void, ErrorCode> {
            if (v.empty() || IsTrue(v) || v == "on") {
                opt.validationMode = ValidationMode::On;
            } else if (IsFalse(v) || v == "off") {
                opt.validationMode = ValidationMode::Off;
            } else if (v == "gpu" || v == "gpu_assisted") {
                opt.validationMode = ValidationMode::GPU;
            } else {
                std::println(stderr, "Error: Invalid value '{}' for --validation. Valid options: on, off, gpu.", v);
                return std::unexpected(CommandLineError::InvalidValue);
            }
            return {};
        },
    },
    CommandHandler {
        .key         = "--vsync",
        .shortKey    = "",
        .placeholder = "<on|off>",
        .description = "Enable or disable vertical synchronization (default: on)",
        .action      = [](CommandLineOptions& opt, std::string_view v) -> std::expected<void, ErrorCode> {
            if (v.empty()) {
                opt.vsync = true;
            } else if (IsTrue(v) || IsFalse(v)) {
                opt.vsync = IsTrue(v);
            } else {
                std::println(stderr, "Error: Invalid value '{}' for --vsync.", v);
                return std::unexpected(CommandLineError::InvalidValue);
            }
            return {};
        },
    },
    CommandHandler {
        .key         = "--fullscreen",
        .shortKey    = "",
        .placeholder = "<on|off>",
        .description = "Enable or disable fullscreen mode (default: off)",
        .action      = [](CommandLineOptions& opt, std::string_view v) -> std::expected<void, ErrorCode> {
            if (v.empty()) {
                opt.fullscreen = true;
            } else if (IsTrue(v) || IsFalse(v)) {
                opt.fullscreen = IsTrue(v);
            } else {
                std::println(stderr, "Error: Invalid value '{}' for --fullscreen.", v);
                return std::unexpected(CommandLineError::InvalidValue);
            }
            return {};
        },
    },
    CommandHandler {
        .key         = "--headless",
        .shortKey    = "",
        .placeholder = "<on|off>",
        .description = "Run headlessly without creating an OS window (default: off)",
        .action      = [](CommandLineOptions& opt, std::string_view v) -> std::expected<void, ErrorCode> {
            if (v.empty() || IsTrue(v)) {
                opt.headless = true;
            } else if (IsFalse(v)) {
                opt.headless = false;
            } else {
                std::println(stderr, "Error: Invalid value '{}' for --headless.", v);
                return std::unexpected(CommandLineError::InvalidValue);
            }
            return {};
        },
    },
    CommandHandler {
        .key         = "--verbose",
        .shortKey    = "",
        .placeholder = "",
        .description = "Enable detailed verbose logging output",
        .action      = [](CommandLineOptions& opt, std::string_view) -> std::expected<void, ErrorCode> {
            opt.logLevel = LogLevel::Verbose;
            return {};
        },
    },
    CommandHandler {
        .key         = "--quiet",
        .shortKey    = "",
        .placeholder = "",
        .description = "Disable all console logging outputs (silent mode)",
        .action      = [](CommandLineOptions& opt, std::string_view) -> std::expected<void, ErrorCode> {
            opt.logLevel = LogLevel::Quiet;
            return {};
        },
    },
    CommandHandler {
        .key         = "--fps-limit",
        .shortKey    = "",
        .placeholder = "<fps>",
        .description = "Cap framerate to integer target (0 = uncapped)",
        .action      = [](CommandLineOptions& opt, std::string_view v) -> std::expected<void, ErrorCode> {
            if (v.empty()) {
                std::println(stderr, "Error: --fps-limit requires an integer value.");
                return std::unexpected(CommandLineError::MissingValue);
            }
            uint32_t       val    = 0;
            const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), val);
            if (ec != std::errc {} || ptr != v.data() + v.size()) {
                std::println(stderr, "Error: Invalid value '{}' for --fps-limit.", v);
                return std::unexpected(CommandLineError::InvalidValue);
            }
            opt.fpsLimit = val;
            return {};
        },
    },
    CommandHandler {
        .key         = "--renderdoc",
        .shortKey    = "",
        .placeholder = "<on|off>",
        .description = "Enable RenderDoc API in-app capture injection (default: off)",
        .action      = [](CommandLineOptions& opt, std::string_view v) -> std::expected<void, ErrorCode> {
            if (v.empty() || IsTrue(v)) {
                opt.enableRenderDoc = true;
            } else if (IsFalse(v)) {
                opt.enableRenderDoc = false;
            } else {
                std::println(stderr, "Error: Invalid value '{}' for --renderdoc.", v);
                return std::unexpected(CommandLineError::InvalidValue);
            }
            return {};
        },
    },
    CommandHandler {
        .key         = "--debug-attach",
        .shortKey    = "",
        .placeholder = "",
        .description = "Halt startup and wait for an external debugger attachment",
        .action      = [](CommandLineOptions&, std::string_view) -> std::expected<void, ErrorCode> {
            std::println(
                "\n{}Waiting for debugger attachment. PID: {}{}{}\nPress ENTER to continue...",
                Color::BYellow,
                Color::BCyan,
                GetPID(),
                Color::Reset
            );
            std::fgetc(stdin);
            return {};
        },
    },
    CommandHandler {
        .key         = "--print-graph",
        .shortKey    = "",
        .placeholder = "[mode]",
        .description = "Print compile-time frame graph for AA mode (none, fxaa, mlaa, taa, smaa)",
        .action      = [](CommandLineOptions& opt, std::string_view v) -> std::expected<void, ErrorCode> {
            AAMode mode = AAMode::TAA;
            if (!v.empty()) {
                std::string modeStr(v);
                std::ranges::transform(modeStr, modeStr.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (modeStr == "none") {
                    mode = AAMode::None;
                } else if (modeStr == "fxaa") {
                    mode = AAMode::FXAA;
                } else if (modeStr == "mlaa") {
                    mode = AAMode::MLAA;
                } else if (modeStr == "taa") {
                    mode = AAMode::TAA;
                } else if (modeStr == "smaa") {
                    mode = AAMode::SMAA;
                } else {
                    std::println(stderr, "Error: Invalid AA mode '{}' for --print-graph. Valid modes: none, fxaa, mlaa, taa, smaa.", v);
                    return std::unexpected(CommandLineError::InvalidValue);
                }
            }
            std::println("{}", GetRenderGraphDump(mode));
            opt.printGraphRequested = true;
            return {};
        },
    },
};

auto EngineHandlers() noexcept -> std::span<const CommandHandler> {
    return kEngineHandlers;
}

auto ToHandlerDescs(std::span<const CommandHandler> handlers) -> std::vector<CliFlagDesc> {
    std::vector<CliFlagDesc> out;
    out.reserve(handlers.size());
    for (const auto& h: handlers) {
        out.push_back(CliFlagDesc {
            .key         = h.key,
            .shortKey    = h.shortKey,
            .placeholder = h.placeholder,
            .description = h.description,
        });
    }
    return out;
}

void PrintEngineHelp(
    std::string_view                exeName,
    std::span<const CommandHandler> engineHandlers,
    std::span<const CommandHandler> appHandlers
) {
    const auto primary   = ToHandlerDescs(engineHandlers);
    const auto secondary = ToHandlerDescs(appHandlers);
    PrintCliHelp(
        exeName,
        primary,
        secondary,
        CliParseConfig {
            .programTitle     = "Zahlen Engine",
            .usageSuffix      = "[options]",
            .primaryHeading   = "Options",
            .secondaryHeading = "Application Options",
            .environmentDocs  = kEngineEnvDocs,
        }
    );
}

} // namespace

auto HandleCommandLine(
    std::span<char* const>          args,
    std::span<const CommandHandler> appHandlers,
    CommandLineOptions             defaults
) -> std::expected<CommandLineOptions, ErrorCode> {
    CommandLineOptions options = defaults;
    options.args        = args;
    options.appHandlers = appHandlers;

    if (const char* envVal = std::getenv("ZHLN_VALIDATION")) {
        const std::string_view val(envVal);
        if (val == "0" || val == "off" || val == "false" || val == "no") {
            options.validationMode = ValidationMode::Off;
        } else if (val == "gpu" || val == "gpu_assisted") {
            options.validationMode = ValidationMode::GPU;
        } else if (val == "1" || val == "on" || val == "true" || val == "yes") {
            options.validationMode = ValidationMode::On;
        }
    }

    if (const char* envVal = std::getenv("ZHLN_HEADLESS")) {
        const std::string_view val(envVal);
        if (IsTrue(val)) {
            options.headless = true;
        } else if (IsFalse(val)) {
            options.headless = false;
        }
    }

    const std::vector<CliToken> tokens = TokenizeCliArgs(args);

    for (const auto& tok: tokens) {
        const auto engineIt = std::ranges::find_if(kEngineHandlers, [&](const auto& h) { return MatchesHandler(h, tok.key); });
        if (engineIt != kEngineHandlers.end()) {
            if (auto result = engineIt->action(options, tok.value); !result) {
                return std::unexpected(result.error());
            }
            // Only opted-in reflected fields see a built-in token. Search all
            // matches: an earlier ordinary colliding handler must not hide one.
            for (const auto& appHandler: appHandlers) {
                if (appHandler.mirrorEngineFlag && MatchesHandler(appHandler, tok.key)) {
                    if (auto mirrored = appHandler.action(options, tok.value); !mirrored) {
                        return std::unexpected(mirrored.error());
                    }
                }
            }
            continue;
        }

        const auto appIt = std::ranges::find_if(appHandlers, [&](const auto& h) { return MatchesHandler(h, tok.key); });
        if (appIt != appHandlers.end()) {
            if (auto result = appIt->action(options, tok.value); !result) {
                return std::unexpected(result.error());
            }
        } else {
            std::println(stderr, "{}Error: Unknown argument: '{}'{}\n", Color::BYellow, tok.key, Color::Reset);
            PrintEngineHelp(ExtractExecutableName(args, "zahlen"), kEngineHandlers, options.appHandlers);
            return std::unexpected(CommandLineError::UnknownArgument);
        }
    }

    return options;
}

} // namespace ZHLN
