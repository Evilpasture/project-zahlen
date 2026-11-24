// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
// The composition root owns every GUI frame and submission, not the plugin.
#include <Benchmark/Viewer/BenchmarkViewer.hpp>
#include <cstdio>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/HostRunner.hpp>
#include <UI/HostUI.hpp>
#include <array>
#include <charconv>
#include <cstdlib>
#include <print>

namespace {
auto Run(const ZHLN::EngineConfig& config, uint32_t frames) -> std::expected<void, ZHLN::ErrorCode> {
    using namespace ZHLN;
    ProcessScope process;
    auto         created = COM::CreateEngine(
        COM::InChain(
            COM::AppConfig {.appName = config.appName, .logLevel = config.logLevel, .maxFramesPerSecond = config.fpsLimit},
            COM::WindowConfig {
                .width = config.width, .height = config.height, .fullscreen = config.fullscreen, .vsync = config.vsync, .headless = config.headless
            },
            COM::RenderConfig {.validationMode = config.validationMode, .enableMeshShading = config.enableMeshShading}, config.physics
        )
    );
    if (!created)
        return std::unexpected(created.error());
    auto                   engine = std::move(*created);
    HostUI                 host(*engine.Get());
    Bench::Viewer viewer;
    viewer.Attach(*engine.Get());
    viewer.SetVisible(true);
    viewer.SetTelemetryVisible(true);
    Clock      clock;
    FramePacer pacer(config.fpsLimit);
    while (engine->IsRunning() && (!frames || engine->CurrentFrame() < frames)) {
        const auto start = FramePacer::BeginFrame();
        ProcessScope::PollCrashes(engine.Get());
        engine->ProcessEvents();
        if (!engine->IsRunning())
            break;
        auto        paced  = engine->Graphics()->PacedDeltaTime();
        auto        pacing = paced.HasValue() ? std::optional<float>(paced.value) : std::nullopt;
        const float dt     = pacer.SelectDelta(clock.Tick(), pacing);
        host.BeginFrame();
        auto& gui = host.Context();
        viewer.SetFontAtlas(&host.GetFontAtlas());
        auto ui = gui.BeginFrame(host.ConsumeInput(dt));
        viewer.DrawPanels(ui, *engine.Get());
        if (auto sent = host.Submit(std::move(ui).End()); !sent.HasValue())
            return std::unexpected(sent.error);
        if (auto tick = engine->Tick(dt); !tick.HasValue())
            return std::unexpected(tick.error);
        pacer.EndFrame(start, pacing);
    }
    return {};
}
} // namespace
int main(int argc, char** argv) {
    using namespace ZHLN;
    EngineConfig     config {.appName = "Zahlen Benchmark Viewer"};
    uint32_t         frames = 0;
    const std::array flags {CliFlag {
        .key         = "--frames",
        .placeholder = "<count>",
        .description = "Close after a bounded number of frames",
        .action      = [&](std::string_view text) -> std::expected<void, CommandLineError> {
            auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), frames);
            if (error != std::errc {} || end != text.data() + text.size() || !frames)
                return std::unexpected(CommandLineError::MissingValue);
            return {};
        }
    }};
    auto             parsed = ParseEngineConfig(std::span(argv, size_t(argc)), config, flags);
    if (!parsed)
        return EXIT_FAILURE;
    if (parsed->helpRequested || parsed->versionRequested)
        return EXIT_SUCCESS;
    if (config.headless && !frames) {
        std::println(stderr, "--headless requires --frames");
        return EXIT_FAILURE;
    }
    auto ran = Run(config, frames);
    if (!ran) {
        std::println(stderr, "Benchmark viewer failed: {}", ran.error());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
