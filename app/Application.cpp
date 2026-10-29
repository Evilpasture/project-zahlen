// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Application composes the game and its frame plan. CMake generates the
// optional default-module install pass in DefaultModules.cpp.
#include <CommandLine/EngineCommandLine.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/HostRunner.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Render/Render.hpp>
#include <optional>
#include <span>
#include <utility>

namespace ZHLN {

auto Application::RunFromCLI(EngineRunConfig config, std::span<char* const> args, const Update& update) const -> AppRunResult {
    const CommandLineOptions defaults {
        .validationMode = config.validationMode,
        .vsync          = config.vsync,
        .fullscreen     = config.fullscreen,
        .headless       = config.headless,
        .fpsLimit       = config.fpsLimit,
    };
    auto parsed = HandleCommandLine(args, _cliHandlers, defaults);
    if (!parsed) {
        LogError("[Application] command line: {}", parsed.error());
        return std::unexpected(parsed.error()); // keep the original CLI error category
    }
    if (parsed->helpRequested) {
        return AppExitReason::CliHelpRequested;
    }
    if (parsed->versionRequested) {
        return AppExitReason::CliVersionRequested;
    }
    if (parsed->printGraphRequested) {
        return AppExitReason::CliGraphRequested;
    }
    SetLogLevel(parsed->logLevel);
    config.vsync          = parsed->vsync;
    config.fullscreen     = parsed->fullscreen;
    config.validationMode = parsed->validationMode;
    config.headless       = parsed->headless;
    config.fpsLimit       = parsed->fpsLimit;
    return RunConfigured(config, update, &*parsed);
}

auto Application::RunConfigured(const EngineRunConfig& config, const Update& update, const CommandLineOptions* options) const -> AppRunResult {
    // Keep the Engine lifetime inside the shared process/task scope. Never
    // nest Engine::Run here: it creates another TaskSystem::Scope.
    ProcessScope process;

    auto created = Engine::Create(Engine::MakeConfig(config));
    if (!created) {
        LogError("[Application] engine creation failed: {}", created.error());
        return std::unexpected(created.error()); // preserve the underlying init error
    }
    auto engine = std::move(*created);
    engine->GetPlatformHost().Focus();

    // The copy owns the parsed flags; its spans borrow args and _cliHandlers.
    // Both outlive this Engine because RunConfigured completes synchronously.
    if (options != nullptr) {
        engine->EmplaceService<CommandLineOptions>(*options);
    }

    if (_defaultModules) {
        InstallApplicationDefaults(*engine);
    }
    for (const auto& install: _installers) {
        install(*engine);
    }
    if (!engine->InitializeDefaultScene()) {
        LogError("[Application] initial scene failed");
        return std::unexpected(AppError::SceneInitFailed);
    }
    if (_ui) {
        engine->SetUICallback(_ui);
    }
    for (const auto& layer: _layers) {
        if (layer.start != nullptr) {
            layer.start(layer.state.get(), *engine);
        }
    }
    if (_start) {
        _start(*engine);
    }

    FramePacer pacer(config.fpsLimit);
    Clock      runClock({.maxDeltaTime = 0.1f});
    while (engine->IsRunning()) {
        const auto frameStart = FramePacer::BeginFrame();
        process.PollCrashes(engine.get());
        engine->ProcessEvents(); // includes input and renderer resize synchronization
        if (!engine->IsRunning()) {
            break;
        }

        const float rawDt = runClock.Tick();
        const std::optional<float> pacedDt = engine->GetRenderContext().GetPacedDeltaTime();
        const float dt = pacer.SelectDelta(rawDt, pacedDt);
        for (const auto& layer: _layers) {
            if (layer.tick != nullptr) {
                layer.tick(layer.state.get(), *engine, dt);
                if (!engine->IsRunning()) {
                    break;
                }
            }
        }
        if (!engine->IsRunning()) {
            break;
        }
        if (update) {
            update(*engine, dt);
            if (!engine->IsRunning()) {
                break;
            }
        }
        if (engine->Tick(dt) == FrameStatus::Error) {
            LogError("[Application] engine frame failed");
            return std::unexpected(AppError::FrameTickFailed);
        }
        pacer.EndFrame(frameStart, pacedDt);
    }
    return AppExitReason::UserClosed;
}

} // namespace ZHLN
