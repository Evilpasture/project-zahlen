// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include <Zahlen/Error.hpp>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <span>
#include <string>

namespace ZHLN {

enum class LogLevel : uint8_t { Quiet, Moderate, Verbose };

enum class CommandLineError : uint8_t { Success = 0, InvalidValue, MissingValue, UnknownArgument };

struct CommandLineOptions {
    std::span<char* const> args;
    bool                   enableValidation = true;
    bool                   launchEditor     = false;
    bool                   vsync            = true;
    bool                   fullscreen       = false;
    LogLevel               logLevel         = LogLevel::Moderate;
    uint32_t               fpsLimit         = 0;
    bool                   enableRenderDoc  = false;
    bool                   benchmark        = false;

    // User requests (successful early-exit paths)
    bool helpRequested       = false;
    bool versionRequested    = false;
    bool printGraphRequested = false;
};

struct EngineError {
    std::string msg;
    int         code   = EXIT_FAILURE;
    bool        silent = false;
};

std::expected<CommandLineOptions, Error> HandleCommandLine(std::span<char* const> args);

} // namespace ZHLN
