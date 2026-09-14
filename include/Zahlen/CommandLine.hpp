// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <span>
#include <string>

namespace ZHLN {

enum class LogLevel : uint8_t { Quiet, Moderate, Verbose };

enum class CommandLineError : uint8_t { InvalidValue = 1, MissingValue, UnknownArgument };

enum class GameplayDriver : uint8_t {
    Fennel, // Fennel/LuaJIT owns the game loop & logic (Default)
    Cpp,    // Native C++ (.so / .dll) owns the game loop
    Hybrid  // Native C++ handles core loop/physics; Fennel handles scripted UI
};

/// What the gameplay driver asked the host to do after a tick. Lives beside
/// GameplayDriver rather than in Types.hpp so naming a tick's outcome does not
/// pull in the renderer/math header.
enum class GameplayStatus : int8_t { OK = 0, RequestQuit = 1, RequestReload = 2, Error = -1 };

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

    // Configurable Game Loop Driver
    GameplayDriver driver = GameplayDriver::Fennel;

    // User requests
    bool helpRequested       = false;
    bool versionRequested    = false;
    bool printGraphRequested = false;
};

struct EngineError {
    std::string msg;
    int         code   = EXIT_FAILURE;
    bool        silent = false;
};

ZHLN_API auto HandleCommandLine(std::span<char* const> args) -> std::expected<CommandLineOptions, Error>;

} // namespace ZHLN
