// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Application composes the game and its frame plan. CMake generates the
// optional default-module install pass in DefaultModules.cpp.
#include <Zahlen/App.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/HostRunner.hpp>
#include <Zahlen/Kernel.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Render/Render.hpp>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ZHLN {

auto Application::RunFromCLI(EngineConfig config, std::span<char* const> args, const Update& update) const -> AppRunResult {
    auto parsed = ParseEngineConfig(args, config, _cli.Flags());
    if (!parsed) {
        LogError("[Application] command line: {}", parsed.error());
        return std::unexpected(parsed.error());
    }
    if (parsed->helpRequested) return AppExitReason::CliHelpRequested;
    if (parsed->versionRequested) return AppExitReason::CliVersionRequested;
    if (parsed->graphRequested) return AppExitReason::CliGraphRequested;
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

    auto created = Engine::Create(config);
    if (!created) {
        LogError("[Application] engine creation failed: {}", created.error());
        return std::unexpected(created.error()); // preserve the underlying init error
    }
    auto engine = std::move(*created);
    engine->GetPlatformHost().Focus();

    // Publish the effective kernel-owned value before installers and graph
    // construction. Its pipeline byte span now points into Kernel-owned storage
    // (including on device recovery), never a caller-owned temporary. No argv
    // or handler borrows enter services, including on no-CLI Run.
    engine->EmplaceService<EngineConfig>(engine->GetKernel().GetConfig());

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
    // Core has one host UI step; dispatch the UI-capable layers there in
    // registration order. Snapshot their slots so the Engine's callback owns
    // their (possibly move-only) state for this Run.
    std::vector<LayerSlot> uiLayers;
    for (const auto& layer: _layers) {
        if (layer.ui != nullptr) {
            uiLayers.push_back(layer);
        }
    }
    if (!uiLayers.empty()) {
        engine->SetUICallback([layers = std::move(uiLayers)](Engine& active) {
            for (const auto& layer: layers) {
                layer.ui(layer.state.get(), active);
            }
        });
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
    bool       frameFailed = false;
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
            frameFailed = true;
            break;
        }
        pacer.EndFrame(frameStart, pacedDt);
    }
    // Every successfully initialized layer gets Stop while the Engine is still
    // alive, even if events, a layer/update, or a failed frame ended the loop.
    for (auto it = _layers.rbegin(); it != _layers.rend(); ++it) {
        if (it->stop != nullptr) {
            it->stop(it->state.get(), *engine);
        }
    }
    if (frameFailed) {
        return std::unexpected(AppError::FrameTickFailed);
    }
    return AppExitReason::UserClosed;
}

} // namespace ZHLN
