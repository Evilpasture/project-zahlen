// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// A real executable consuming only the COM engine boundary. CLI binding and
// PNG encoding are host libraries; no Application/HostRunner is linked.
#include <Zahlen/COM/App.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Screenshot/Screenshot.hpp>
#include <Zahlen/Render/CapturedFrame.hpp>
#include <Zahlen/Core/Reflection/Annotations.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>

namespace {
struct CubeOptions {
    bool headless = false;
    bool emptyScene ZHLN_ANNOTATION(ZHLN::Description<"Skip mesh creation (capture/host smoke test)"> {}) = false;
    uint32_t frames ZHLN_ANNOTATION(ZHLN::Description<"Request exit after this many completed frames (0: interactive)"> {}) = 0;
    uint32_t width ZHLN_ANNOTATION(ZHLN::CliRange<1, 16384> {}) = 1280;
    uint32_t height ZHLN_ANNOTATION(ZHLN::CliRange<1, 16384> {}) = 720;
    std::string capture ZHLN_ANNOTATION(ZHLN::Description<"Write a PNG after --frames completed frames (default: 1)"> {}) = {};
};
}

int main(int argc, char* argv[]) {
    CubeOptions options;
    ZHLN::ConfigCli cli;
    cli.WithConfig(options);
    const auto parsed = cli.Parse(std::span<char* const>(argv, static_cast<size_t>(argc)), {.programTitle = "Zahlen COM Cube"});
    if (!parsed) return EXIT_FAILURE;
    if (parsed->helpRequested || parsed->versionRequested) return EXIT_SUCCESS;
    const uint32_t limit = options.frames ? options.frames : (options.capture.empty() ? 0 : 1);
    bool failed = false;
    bool captured = false;
    uint32_t completed = 0;
    ZHLN::Screenshot::CaptureBuffer capture;
    const auto result = ZHLN::COM::App()
        .Title("Zahlen COM Cube").Resolution(options.width, options.height)
        .Headless(options.headless)
        .OnStart([&](ZHLN::COM::IEngine& engine) {
            if (options.emptyScene) return;
            const auto cube = engine.Assets()->SpawnBox({1, 1, 1});
            if (!cube.HasValue()) {
                std::fputs("COM cube: scene creation failed\n", stderr);
                failed = true;
                engine.RequestClose();
            }
        })
        .Run([&](ZHLN::COM::IEngine& engine) {
            // Updates precede Tick. 'completed' counts prior successful ticks;
            // the first update cannot capture an unrendered frame.
            if (!limit || completed++ < limit) return;
            if (!options.capture.empty()) {
                const auto frame = capture.Read(*engine.Graphics(), engine.WindowExtent(engine.PrimaryWindow()));
                if (!frame || !ZHLN::Screenshot::WritePNG(options.capture, **frame)) {
                    std::fputs("COM cube: frame capture/write failed\n", stderr);
                    failed = true;
                } else captured = true;
            }
            engine.RequestClose();
        });
    if (!result || failed || (!options.capture.empty() && !captured)) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
