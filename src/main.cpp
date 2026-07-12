// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/main.cpp
#include "Zahlen/CommandLine.hpp"
#include "Zahlen/Log.hpp"
#include <expected>
#include <span>

using namespace ZHLN;

int RunGame(const CommandLineOptions& options);
int RunEditor(const CommandLineOptions& options);

namespace ZHLN {
bool LoadRenderDocLibrary() noexcept;
} // namespace ZHLN

int main(int argc, char* argv[]) {
    auto options_res = HandleCommandLine(std::span(argv, static_cast<size_t>(argc)));
    if (!options_res) {
        ZHLN::Log("Error: {}", options_res.error().Message());
        return EXIT_FAILURE;
    }

    const auto& options = options_res.value();

    // Clean exit for non-error user requests
    if (options.helpRequested || options.versionRequested || options.printGraphRequested) {
        return 0;
    }

    ZHLN::SetLogLevel(options.logLevel);

    // Load RenderDoc before starting any windowing or rendering modules
    if (options.enableRenderDoc) {
        ZHLN::LoadRenderDocLibrary();
    }

    return options.launchEditor ? RunEditor(options) : RunGame(options);
}
