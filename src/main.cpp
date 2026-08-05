// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// File: src/main.cpp
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <span>

int main(int argc, char* argv[]) {
    return ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)))
        .transform_error([](const ZHLN::Error& err) -> int {
            ZHLN::Log("CommandLine Error: {}", err.Message());
            return EXIT_FAILURE;
        })
        .and_then([](const ZHLN::CommandLineOptions& options) -> std::expected<int, int> {
            if (options.helpRequested || options.versionRequested || options.printGraphRequested) {
                return 0; // Clean exit for CLI queries
            }

            ZHLN::SetLogLevel(options.logLevel);

            // Engine::Run handles starting the game OR the editor based on options.launchEditor
            return ZHLN::Engine::Run(options);
        })
        .or_else([](int errorCode) -> std::expected<int, int> { return errorCode; })
        .value();
}
