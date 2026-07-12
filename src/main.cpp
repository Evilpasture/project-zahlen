// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/main.cpp
#include "Zahlen/CommandLine.hpp"
#include "Zahlen/Log.hpp"
#include <expected>
#include <span>

using namespace ZHLN;

std::expected<int, int> RunGame(const CommandLineOptions& options);
std::expected<int, int> RunEditor(const CommandLineOptions& options);

namespace ZHLN {
bool LoadRenderDocLibrary() noexcept;
} // namespace ZHLN

int main(int argc, char* argv[]) {
    return HandleCommandLine(std::span(argv, static_cast<size_t>(argc)))
        .transform_error([](const Error& err) -> int {
            ZHLN::Log("Error: {}", err.Message());
            return EXIT_FAILURE;
        })
        .and_then([](const CommandLineOptions& options) -> std::expected<int, int> {
            if (options.helpRequested || options.versionRequested || options.printGraphRequested) {
                return 0; // Clean exit
            }

            ZHLN::SetLogLevel(options.logLevel);

            if (options.enableRenderDoc) {
                ZHLN::LoadRenderDocLibrary();
            }

            return options.launchEditor ? RunEditor(options) : RunGame(options);
        })
        .or_else([](int errorCode) -> std::expected<int, int> {
            return errorCode; // Normalize error path into a valid expected payload
        })
        .value();
}
