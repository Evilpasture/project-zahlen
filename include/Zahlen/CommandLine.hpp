// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <span>
#include <string>

namespace ZHLN {

enum class LogLevel : uint8_t { Quiet, Moderate, Verbose };

enum class CommandLineError : uint8_t { InvalidValue = 1, MissingValue, UnknownArgument };

enum class GameplayDriver : uint8_t { Scripted, Cpp, Hybrid };

enum class GameplayStatus : int8_t { OK = 0, RequestQuit = 1, RequestReload = 2, Error = -1 };

struct CommandLineOptions;

// One engine or application flag: key, optional short key, help placeholder,
// help description, and the action applying a parsed value. Applications pass
// their own handlers to HandleCommandLine so --help shows a single menu and
// no caller filters argv first. Actions run synchronously during the parse,
// so capturing lambdas may borrow app-local config.
struct CommandHandler {
    std::string_view                                                                     key;
    std::string_view                                                                     shortKey    = "";
    std::string_view                                                                     placeholder = "";
    std::string_view                                                                     description = "";
    std::function<std::expected<void, ErrorCode>(CommandLineOptions&, std::string_view)> action;
};

struct CommandLineOptions {
    std::span<char* const> args;
    ValidationMode         validationMode  = ValidationMode::On;
    bool                   launchEditor    = false;
    bool                   vsync           = true;
    bool                   fullscreen      = false;
    bool                   headless        = false;
    LogLevel               logLevel        = LogLevel::Moderate;
    uint32_t               fpsLimit        = 0;
    bool                   enableRenderDoc = false;
    bool                   benchmark       = false;

    GameplayDriver driver = GameplayDriver::Scripted;

    bool helpRequested       = false;
    bool versionRequested    = false;
    bool printGraphRequested = false;

    // Application handlers accepted by this parse; the --help action prints
    // them alongside the engine flags. Borrowed, like args.
    std::span<const CommandHandler> appHandlers;
};

struct EngineError {
    std::string msg;
    int         code   = EXIT_FAILURE;
    bool        silent = false;
};

// Engine handlers win on key collisions: an application handler sharing an
// engine key is unreachable.
ZHLN_API auto HandleCommandLine(std::span<char* const> args, std::span<const CommandHandler> appHandlers = {}) -> std::expected<CommandLineOptions, ErrorCode>;

} // namespace ZHLN
