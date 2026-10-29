// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// CommandLine/EngineCommandLine.hpp
//
// Engine-host command-line preset built on <CommandLine/CommandLine.hpp>.
// Provides windowing, validation, framerate, RenderDoc, frame-graph dump, and
// interactive/headless engine flags for applications (`app/`
// and `samples/`), keeping standalone CLI tools free of renderer/editor flags.

#include <CommandLine/CommandLine.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/LogLevel.hpp>
#include <Zahlen/Error.hpp>

#include <cstdint>
#include <cstdlib>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace ZHLN {

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
// engine key is unreachable. Defaults are applied first, then the environment,
// then CLI flags. Hosts with nonstandard launch settings can pass their own
// defaults without losing them when no matching flag was supplied.
[[nodiscard]] auto HandleCommandLine(
    std::span<char* const>          args,
    std::span<const CommandHandler> appHandlers = {},
    CommandLineOptions             defaults    = {}
) -> std::expected<CommandLineOptions, ErrorCode>;

} // namespace ZHLN
