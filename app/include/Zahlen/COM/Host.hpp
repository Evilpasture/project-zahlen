// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Thin CLI and PNG-capture host harness around ZHLN::COM::App. Keeps COM::App
// itself free of CLI and Screenshot dependencies while letting standalone COM
// executables link only zahlen_engine, zahlen_command_line, and
// zahlen_screenshot (no Application, HostRunner, or HostUI).
#include <Screenshot/Screenshot.hpp>
#include <Zahlen/COM/App.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <concepts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace ZHLN::COM {

struct HostOptions {
    bool                headless                                                                                          = false;
    uint32_t frames     ZHLN_ANNOTATION(Description<"Request exit after this many completed frames (0: interactive)"> {}) = 0;
    uint32_t width      ZHLN_ANNOTATION(CliRange<1, 16384> {})                                                            = 1280;
    uint32_t height     ZHLN_ANNOTATION(CliRange<1, 16384> {})                                                            = 720;
    std::string capture ZHLN_ANNOTATION(Description<"Write a PNG after --frames completed frames (default: 1)"> {});
};

namespace TemplatedDetail {

template <typename Runner>
[[nodiscard]] auto ExecuteHostApp(std::span<char* const> args, std::string_view programTitle, App app, Runner&& runner) -> int {
    HostOptions options {
        .headless = app.Headless(),
        .frames   = app.MaxFrames(),
        .width    = app.Width(),
        .height   = app.Height(),
    };
    ConfigCli cli;
    cli.WithConfig(options);
    const std::string_view title  = programTitle.empty() ? app.Title() : programTitle;
    const auto             parsed = cli.Parse(args, {.programTitle = title});
    if (!parsed) {
        return EXIT_FAILURE;
    }
    if (parsed->helpRequested || parsed->versionRequested) {
        return EXIT_SUCCESS;
    }

    const uint32_t limit = options.frames ? options.frames : (options.capture.empty() ? 0u : 1u);
    app.Resolution(options.width, options.height).Headless(options.headless).MaxFrames(limit);

    Screenshot::CaptureOutcome capture;
    if (!options.capture.empty()) {
        app.AddLayer(Screenshot::CaptureLayer(options.capture, limit, capture));
    }

    const auto result = std::forward<Runner>(runner)(app);
    if (!result) {
        return EXIT_FAILURE;
    }
    if (!options.capture.empty() && !capture.Succeeded()) {
        std::fputs(capture.Attempted() ? "COM host: frame capture/write failed\n" : "COM host: closed before capture frame\n", stderr);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

} // namespace TemplatedDetail

template <typename F>
concept EngineUpdate = std::invocable<F&, IEngine&> || std::invocable<F&, IEngine&, float>;

// --- The two real implementations -------------------------------------------

[[nodiscard]] inline auto RunApp(std::span<char* const> args, std::string_view programTitle, App app) -> int {
    return TemplatedDetail::ExecuteHostApp(args, programTitle, std::move(app), [](const App& configured) { return configured.Run(); });
}

[[nodiscard]] auto RunApp(std::span<char* const> args, std::string_view programTitle, App app, EngineUpdate auto&& update) -> int {
    return TemplatedDetail::ExecuteHostApp(args, programTitle, std::move(app), [&update](const App& configured) {
        return configured.Run(std::forward<decltype(update)>(update));
    });
}

// --- No title: forward with an empty title ----------------------------------

[[nodiscard]] inline auto RunApp(std::span<char* const> args, App app) -> int {
    return RunApp(args, {}, std::move(app));
}

[[nodiscard]] auto RunApp(std::span<char* const> args, App app, EngineUpdate auto&& update) -> int {
    return RunApp(args, {}, std::move(app), std::forward<decltype(update)>(update));
}

// --- argc/argv: convert to span and forward ---------------------------------

[[nodiscard]] inline auto RunApp(int argc, char* argv[], App app) -> int {
    return RunApp(std::span(argv, static_cast<size_t>(argc)), std::move(app));
}

[[nodiscard]] inline auto RunApp(int argc, char* argv[], std::string_view programTitle, App app) -> int {
    return RunApp(std::span(argv, static_cast<size_t>(argc)), programTitle, std::move(app));
}

[[nodiscard]] auto RunApp(int argc, char* argv[], App app, EngineUpdate auto&& update) -> int {
    return RunApp(std::span(argv, static_cast<size_t>(argc)), std::move(app), std::forward<decltype(update)>(update));
}

[[nodiscard]] auto RunApp(int argc, char* argv[], std::string_view programTitle, App app, EngineUpdate auto&& update) -> int {
    return RunApp(std::span(argv, static_cast<size_t>(argc)), programTitle, std::move(app), std::forward<decltype(update)>(update));
}

} // namespace ZHLN::COM
