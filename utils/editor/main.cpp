// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
// Dedicated host-owned paused editor loop. No native Engine or registry bridge.
#include "EditorSession.hpp"
#include <FallbackScene/FallbackScene.hpp>
#include <SceneDocument.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Zahlen/HostRunner.hpp>
#include <Zahlen/Log.hpp>
#include <array>
#include <charconv>
#include <cstdlib>
#include <span>
namespace {
auto Run(const ZHLN::EngineConfig& config, const std::filesystem::path& scenePath, uint32_t frames, bool rasterOnly) -> std::expected<void, ZHLN::ErrorCode> {
    using namespace ZHLN;
    ProcessScope process;
    auto         created = COM::CreateEngineRef(
        COM::InChain(
            COM::EngineAppConfig {.appName = config.appName, .logLevel = config.logLevel, .maxFramesPerSecond = config.fpsLimit},
            COM::EngineWindowConfig {
                .width = config.width, .height = config.height, .fullscreen = config.fullscreen, .vsync = config.vsync, .headless = config.headless
            },
            COM::EngineRenderConfig {
                .validationMode        = config.validationMode,
                .enableMeshShading     = config.enableMeshShading,
                .initialPipelineBinary = config.initialPipelineBinaryData
            },
            config.physics, COM::EngineDiagnosticsInfo {.crashDumpPath = std::string_view(config.crashDumpPath)}
        )
    );
    if (!created)
        return std::unexpected(created.error());
    auto   engine = std::move(*created);
    HostUI ui(*engine.Get());
    if (scenePath.empty()) {
        auto description                  = FallbackScene::DefaultPreset::FallbackScene();
        description.camera.position       = {0, 20, 40};
        description.camera.pitch          = -20;
        description.environment.enableRTR = !rasterOnly;
        if (auto scene = Scene::Instantiate(*engine.Get(), description, {.buildBLAS = !rasterOnly}); !scene)
            return std::unexpected(scene.error());
    } else {
        auto description = Tools::ReadSceneDocument(scenePath);
        if (!description)
            return std::unexpected(description.error());
        if (auto valid = Scene::Validate(*engine->Assets(), *description); !valid)
            return std::unexpected(valid.error());
        engine->ClearScene();
        if (auto scene = Scene::Instantiate(*engine.Get(), *description, {.buildBLAS = !rasterOnly}); !scene) {
            engine->ClearScene();
            return std::unexpected(scene.error());
        }
    }
    Editor::Session editor;
    if (auto started = editor.Start(*engine.Get(), !rasterOnly); !started)
        return started;
    engine->FocusWindow(engine->PrimaryWindow());
    Clock                          clock({.maxDeltaTime = .1f});
    FramePacer                     pacer(config.fpsLimit);
    std::expected<void, ErrorCode> result;
    while (engine->IsRunning() && (!frames || engine->CurrentFrame() < frames)) {
        auto begin = FramePacer::BeginFrame();
        ProcessScope::PollCrashes(engine.Get());
        engine->ProcessEvents();
        if (!engine->IsRunning())
            break;
        auto paced  = engine->Graphics()->PacedDeltaTime();
        auto pacing = paced.HasValue() ? std::optional<float>(paced.value) : std::nullopt;
        result      = editor.Frame(*engine.Get(), ui, pacer.SelectDelta(clock.Tick(), pacing));
        if (!result)
            break;
        pacer.EndFrame(begin, pacing);
    }
    editor.Stop(*engine.Get());
    return result;
}
} // namespace
int main(int argc, char** argv) {
    using namespace ZHLN;
    EngineConfig          config {.appName = "Zahlen World Editor"};
    std::filesystem::path scenePath;
    uint32_t              frames     = 0;
    bool                  rasterOnly = false;
    const std::array      flags {
        CliFlag {
            .key         = "--scene",
            .placeholder = "<file.toml|file.lua>",
            .description = "Open a scene document",
            .action      = [&](std::string_view value) -> std::expected<void, CommandLineError> {
                if (value.empty())
                    return std::unexpected(CommandLineError::MissingValue);
                scenePath = value;
                return {};
            }
        },
        CliFlag {
            .key         = "--frames",
            .placeholder = "<count>",
            .description = "Close after a bounded number of rendered frames",
            .action      = [&](std::string_view value) -> std::expected<void, CommandLineError> {
                uint32_t parsed   = 0;
                auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
                if (error != std::errc {} || end != value.data() + value.size() || !parsed)
                    return std::unexpected(CommandLineError::MissingValue);
                frames = parsed;
                return {};
            }
        },
        CliFlag {
            .key         = "--raster-only",
            .description = "Do not build ray tracing acceleration structures",
            .action      = [&](std::string_view) -> std::expected<void, CommandLineError> {
                rasterOnly = true;
                return {};
            }
        }
    };
    auto parsed = ParseEngineConfig(std::span(argv, size_t(argc)), config, flags);
    if (!parsed) {
        LogError("[WorldEditor] command line: {}", parsed.error());
        return EXIT_FAILURE;
    }
    if (parsed->helpRequested || parsed->versionRequested || parsed->graphRequested)
        return EXIT_SUCCESS;
    if (config.headless && !frames) {
        LogError("[WorldEditor] headless use requires bounded --frames");
        return EXIT_FAILURE;
    }
    auto result = Run(config, scenePath, frames, rasterOnly);
    if (!result)
        LogError("[WorldEditor] failed: {}", result.error());
    return result ? EXIT_SUCCESS : EXIT_FAILURE;
}
