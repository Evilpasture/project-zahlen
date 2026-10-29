// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Application owns ordinary-game composition and its outer frame loop. Optional
// extension wiring stays here, not in the game or the engine's launch settings.
#include "engine/Platform.hpp"
#include <CommandLine/EngineCommandLine.hpp>
#include <Signal/CrashHandler.hpp>
#include <Zahlen/App.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Core/Platform.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/ecs/ECS.hpp>
#if defined(ZHLN_APP_HAS_FONTS)
#include <Fonts/Fonts.hpp>
#endif
#if defined(ZHLN_APP_HAS_CHARACTER_CONTROLLER)
#include <CharacterController/CharacterController.hpp>
#endif
#if defined(ZHLN_APP_HAS_CAMERA)
#include <Camera/TargetCamera.hpp>
#endif
#if defined(ZHLN_APP_HAS_INTERACTION)
#include <Interaction/InteractionSystem.hpp>
#endif
#if defined(ZHLN_APP_HAS_TERRAIN)
#include <Terrain/TerrainSystem.hpp>
#endif
#if defined(ZHLN_APP_HAS_ANIMATION)
#include <Animation/IK.hpp>
#endif
#if defined(ZHLN_APP_HAS_FREECAM)
#include <FreeCam/FreeCam.hpp>
#endif
#if defined(ZHLN_APP_HAS_RAGDOLL_AUTHORING)
#include <RagdollAuthoring/RagdollAuthoring.hpp>
#endif
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <thread>
#include <utility>

namespace ZHLN {

void InstallApplicationDefaults(Engine& engine) {
#if defined(ZHLN_APP_HAS_FONTS)
    // Prime the cache before InitializeDefaultScene uploads the font atlas.
    if (auto fontID = Fonts::LoadFontAsset(engine, Fonts::VendoredDefaultFontSource()); !fontID) {
        LogWarning("Font asset failed to load ({}), using embedded default.", fontID.error());
    }
#endif
    // Character declares MovementComponent writes; Interaction reads it.
#if defined(ZHLN_APP_HAS_CHARACTER_CONTROLLER)
    Character::Install(engine);
#endif
#if defined(ZHLN_APP_HAS_CAMERA)
    CameraRig::Install(engine);
#endif
#if defined(ZHLN_APP_HAS_INTERACTION)
    Interaction::Install(engine);
#endif
#if defined(ZHLN_APP_HAS_TERRAIN)
    Terrain::Install(engine);
#endif
#if defined(ZHLN_APP_HAS_ANIMATION)
    IK::Install(engine); // installs Animation before IK
#endif
#if defined(ZHLN_APP_HAS_FREECAM)
    FreeCam::Install(engine);
#endif
#if defined(ZHLN_APP_HAS_RAGDOLL_AUTHORING)
    RagdollAuthoring::Install(engine);
#endif
}

auto Application::RunFromCLI(EngineRunConfig config, int argc, char* argv[], const Update& update) const -> int {
    if (argc < 0 || (argc > 0 && argv == nullptr)) {
        LogError("[Application] invalid argv passed to Run");
        return EXIT_FAILURE;
    }
    std::span<char* const> args;
    if (argc > 0) {
        args = std::span<char* const>(argv, static_cast<size_t>(argc));
    }
    const CommandLineOptions defaults {
        .validationMode = config.validationMode,
        .vsync          = config.vsync,
        .fullscreen     = config.fullscreen,
        .headless       = config.headless,
        .fpsLimit       = config.fpsLimit,
        .driver         = config.driver,
    };
    auto parsed = HandleCommandLine(args, {}, defaults);
    if (!parsed) {
        LogError("[Application] command line: {}", parsed.error());
        return EXIT_FAILURE;
    }
    if (parsed->helpRequested || parsed->versionRequested || parsed->printGraphRequested) {
        return EXIT_SUCCESS;
    }
    if (parsed->launchEditor || parsed->benchmark) {
        LogError("[Application] use zahlen_editor or zahlen_benchmark instead of game mode flags");
        return EXIT_FAILURE;
    }

    SetLogLevel(parsed->logLevel);
    config.vsync          = parsed->vsync;
    config.fullscreen     = parsed->fullscreen;
    config.validationMode = parsed->validationMode;
    config.headless       = parsed->headless;
    config.fpsLimit       = parsed->fpsLimit;
    config.driver         = parsed->driver;
    return RunConfigured(config, update);
}

auto Application::RunConfigured(const EngineRunConfig& config, const Update& update) const -> int {
    // App owns both process-level crash reporting and the lifetime of the
    // platform, task workers, and engine. Never nest Engine::Run here.
    static CrashState crashState;
    SetupSignalHandler(crashState);
    Platform::Init();
    TaskSystem::Scope tasks; // Engine must be destroyed before its workers.

    EngineConfig engineConfig = Engine::MakeConfig(config);
    engineConfig.enableNativeGameplay = !static_cast<bool>(update);
    auto created = Engine::Create(engineConfig);
    if (!created) {
        LogError("[Application] engine creation failed: {}", created.error());
        return EXIT_FAILURE;
    }
    auto engine = std::move(*created);
    engine->GetPlatformHost().Focus();

    if (_defaultModules) {
        InstallApplicationDefaults(*engine);
    }
    for (const auto& install: _installers) {
        install(*engine);
    }
    if (!engine->InitializeDefaultScene()) {
        LogError("[Application] initial scene failed");
        return EXIT_FAILURE;
    }
    if (_ui) {
        engine->SetUICallback(_ui);
    }
    if (_start) {
        _start(*engine);
    }

    const double targetFrameTime = config.fpsLimit > 0 ? 1.0 / static_cast<double>(config.fpsLimit) : 0.0;
    Clock        runClock({.maxDeltaTime = 0.1f});
    while (engine->IsRunning()) {
        const auto frameStart = Clock::SteadyClock::now();
        CheckForCrashes(crashState, engine.get());
        engine->ProcessEvents();
        if (!engine->IsRunning()) {
            break;
        }

        const float rawDt = runClock.Tick();
        auto&       registry = engine->GetRegistry();
        if (auto input = registry.GetSingleton<Components::InputStateComponent>(); input && input->needsResize) {
            engine->GetRenderContext().SetResolution(input->newSize);
            input->needsResize = false;
            continue;
        }

        constexpr double           kFpsCapSlack = 1.05;
        const std::optional<float> pacedDt      = engine->GetRenderContext().GetPacedDeltaTime();
        const bool displayPaced = pacedDt.has_value() &&
                                  (config.fpsLimit == 0 || targetFrameTime <= static_cast<double>(*pacedDt) * kFpsCapSlack);
        const float dt = displayPaced ? *pacedDt : rawDt;
        if (update) {
            update(*engine, dt);
            if (!engine->IsRunning()) {
                break;
            }
        }
        const GameplayStatus status = engine->Tick(dt, config.driver);
        if (status == GameplayStatus::Error) {
            LogError("[Application] gameplay frame failed");
            return EXIT_FAILURE;
        }
        if (status == GameplayStatus::RequestQuit) {
            engine->GetPlatformHost().Close();
            break;
        }

        if (config.fpsLimit > 0 && !displayPaced) {
            const auto now = Clock::SteadyClock::now();
            const double elapsed = std::chrono::duration<double>(now - frameStart).count();
            if (elapsed < targetFrameTime) {
                const double sleepTime = targetFrameTime - elapsed;
                if (sleepTime > 0.002) {
                    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>((sleepTime - 0.001) * 1e6)));
                }
                while (std::chrono::duration<double>(Clock::SteadyClock::now() - frameStart).count() < targetFrameTime) {
                    CPURelax();
                }
            }
        }
    }
    return EXIT_SUCCESS;
}

} // namespace ZHLN
