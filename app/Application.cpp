// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Application composes the game and its frame plan. CMake generates the
// optional default-module install pass in DefaultModules.cpp.
#include <Zahlen/App.hpp>
#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IFramePipeline.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/HostRunner.hpp>
#include <Zahlen/Log.hpp>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

namespace ZHLN {

auto Application::RunFromCLI(EngineConfig config, std::span<char* const> args, const Update& update) const -> AppRunResult {
    auto parsed = ParseEngineConfig(args, config, _cli.Flags());
    if (!parsed) {
        std::println(stderr, "Error: {}", parsed.error());
        return std::unexpected(parsed.error());
    }
    if (parsed->helpRequested) {
        return AppExitReason::CliHelpRequested;
    }
    if (parsed->versionRequested) {
        return AppExitReason::CliVersionRequested;
    }
    if (parsed->graphRequested) {
        return AppExitReason::CliGraphRequested;
    }
    return RunConfigured(config, update);
}

auto Application::RunConfigured(const EngineConfig& config, const Update& update) const -> AppRunResult {
    SetLogLevel(config.logLevel);
    if (_prepare) {
        if (auto prepared = _prepare(); !prepared) {
            return std::unexpected(prepared.error());
        }
    }
    // Keep the Engine lifetime inside the shared process/task scope. Never
    // nest Engine::Run here: it creates another TaskSystem::Scope.
    ProcessScope process;

    auto created = COM::CreateEngineRef(COM::InChain(
        COM::EngineAppConfig {.appName = config.appName, .logLevel = config.logLevel, .maxFramesPerSecond = config.fpsLimit},
        COM::EngineWindowConfig {.width = config.width, .height = config.height, .fullscreen = config.fullscreen, .vsync = config.vsync, .headless = config.headless},
        COM::EngineRenderConfig {.validationMode = config.validationMode, .enableMeshShading = config.enableMeshShading,
            .initialPipelineBinary = config.initialPipelineBinaryData},
        config.physics, COM::EngineDiagnosticsInfo {.crashDumpPath = std::string_view(config.crashDumpPath)}));
    if (!created) {
        LogError("[Application] engine creation failed: {}", created.error());
        return std::unexpected(created.error()); // preserve the underlying init error
    }
    COM::Ref<COM::IEngine> engine = std::move(*created);
    auto                   pipeline = engine.As<COM::IFramePipeline>();
    if (!pipeline)
        return std::unexpected(COM::ComError::UnsupportedExtension);
    HostUI ui(*engine.Get()); // destroyed before the retained engine and ProcessScope
    engine->FocusWindow(engine->PrimaryWindow());

    if (_defaultModules) {
        InstallApplicationDefaults(*engine.Get());
    }
    for (const auto& install: _installers) {
        install(*engine.Get());
    }
    std::optional<ErrorCode> frameError;
    size_t                   enteredLayers = 0;
    for (const auto& layer: _layers) {
        ++enteredLayers; // failed Start may own partial resources needing Stop
        if (layer.start != nullptr) {
            const auto started = layer.start(layer.state.get(), *engine.Get(), ui);
            if (!started) {
                frameError = started.error();
                break;
            }
        }
        if (!engine->IsRunning())
            break;
    }
    if (!frameError && engine->IsRunning() && _start)
        _start(*engine.Get());

    FramePacer pacer(engine->MaxFramesPerSecond());
    Clock      runClock({.maxDeltaTime = 0.1f});
    while (!frameError && engine->IsRunning()) {
        const auto frameStart = FramePacer::BeginFrame();
        ProcessScope::PollCrashes(engine.Get());
        engine->ProcessEvents(); // includes input and renderer resize synchronization
        if (!engine->IsRunning()) {
            break;
        }

        const auto begun = ui.BeginFrame();
        if (!begun.HasValue()) { frameError = begun.error; break; }

        const float                rawDt   = runClock.Tick();
        const auto pacing = engine->Graphics()->PacedDeltaTime();
        const std::optional<float> pacedDt = pacing.HasValue() ? std::optional<float>(pacing.value) : std::nullopt;
        const float                dt      = pacer.SelectDelta(rawDt, pacedDt);
        for (const auto& layer: _layers) {
            if (layer.tick != nullptr) {
                const auto updated = layer.tick(layer.state.get(), *engine.Get(), dt);
                if (!updated) {
                    frameError = updated.error();
                    break;
                }
                if (!engine->IsRunning()) {
                    break;
                }
            }
        }
        if (frameError || !engine->IsRunning())
            break;
        for (const auto& layer: _layers) {
            if (layer.ui != nullptr) {
                const auto composed = layer.ui(layer.state.get(), *engine.Get(), ui);
                if (!composed) { frameError = composed.error(); break; }
            }
            if (!engine->IsRunning()) break;
        }
        if (frameError || !engine->IsRunning()) break;
        if (update) {
            auto updated = update(*engine.Get(), dt);
            if (!updated) { frameError = updated.error(); break; }
            if (!engine->IsRunning()) {
                break;
            }
        }
        if (auto advanced = pipeline->AdvanceFrame(dt); !advanced.HasValue()) {
            frameError = advanced.error;
            break;
        }
        for (const auto& layer: _layers) {
            if (layer.beforeRender != nullptr) {
                const auto result = layer.beforeRender(layer.state.get(), *engine.Get(), dt);
                if (!result) {
                    frameError = result.error();
                    break;
                }
            }
            if (!engine->IsRunning())
                break;
        }
        if (frameError || !engine->IsRunning()) {
            (void) pipeline->CancelFrame();
            break;
        }
        if (auto finished = pipeline->FinishFrame(); !finished.HasValue()) {
            LogError("[Application] engine frame failed: {}", finished.error);
            frameError = finished.error;
            break;
        }
        pacer.EndFrame(frameStart, pacedDt);
    }
    // Stop entered layers, including a partially failed Start, while the
    // engine and ProcessScope still live. Never touch an unvisited layer.
    for (const auto& _layer: std::ranges::reverse_view(std::span(_layers).first(enteredLayers))) {
        if (_layer.stop != nullptr) {
            _layer.stop(_layer.state.get(), *engine.Get());
        }
    }
    if (frameError) {
        return std::unexpected(*frameError);
    }
    return AppExitReason::UserClosed;
}

} // namespace ZHLN
