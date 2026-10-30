// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// samples/GlTFInspectorSample.cpp
//
// Minimal host for the glTF inspector module. Builds the engine, hands control to
// ZHLN::glTF::Initialize (which opens the "drop a glTF" screen and wires up the
// file-drop + orbit viewer), then pumps the engine's standard frame loop.
//
// Drop a .glb / .gltf file onto the window to load and orbit it.

#include <Animation/AnimationSystem.hpp>
#include <Zahlen/ConfigCli.hpp>
#include <Signal/CrashHandler.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/FramePlan.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>

#if defined(ZHLN_HAS_FONTS)
#include <Fonts/Fonts.hpp>
#endif

import ZHLN.glTF;

#include <algorithm>
#include <span>

auto main(int argc, char* argv[]) -> int {
    ZHLN::EngineConfig engineConfig {
        .physics = {.maxBodies = 2048, .maxBodyPairs = 4096, .maxContactConstraints = 4096},
        .appName = "Zahlen :: glTF Inspector",
        .enableFallbackScene = false,
    };

    auto optionsRes = ZHLN::ParseEngineConfig(std::span(argv, static_cast<size_t>(argc)), engineConfig);
    if (!optionsRes) {
        ZHLN::LogError("[Sample] command line: {}", optionsRes.error());
        return EXIT_FAILURE;
    }

    if (optionsRes->helpRequested || optionsRes->versionRequested || optionsRes->graphRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(engineConfig.logLevel);

    // Crash diagnostics keep their state in a caller-owned struct; static
    // storage duration is required because its address is copied into the
    // signal handler slots. See <Signal/CrashHandler.hpp>.
    static ZHLN::CrashState crashState;
    ZHLN::SetupSignalHandler(crashState);
    ZHLN::TaskSystem::Scope taskScope;

    auto engineRes = ZHLN::Engine::Create(engineConfig);

    if (!engineRes) {
        ZHLN::LogError("Failed to initialize Engine: {}", engineRes.error());
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    if (!engineConfig.headless) {
        engine->GetPlatformHost().Focus();
    }
    ZHLN::Animation::Install(*engine);

#if defined(ZHLN_HAS_FONTS)
    // The composition root's default bake (see app/main.cpp): the vendored
    // JetBrains Mono NF. Installed before glTF::Initialize boots the scene, so
    // the atlas that builds the inspector's UI is that font.
    if (auto fontID = ZHLN::Fonts::LoadFontAsset(*engine, ZHLN::Fonts::VendoredDefaultFontSource()); !fontID) {
        ZHLN::LogWarning("Font asset failed to load ({}), using embedded default.", fontID.error());
    }
#endif

    // Boots the default scene, the "drop a glTF" GUI, the file-drop handler and
    // the orbit camera. The inspector owns the frame loop from here on.
    ZHLN::glTF::Initialize(*engine);

    ZHLN::Clock clock({.maxDeltaTime = 0.05f});
    while (engine->IsRunning()) {
        const float dt = clock.Tick();
        ZHLN::CheckForCrashes(crashState, engine.get());
        engine->ProcessEvents();

        const auto status = engine->Tick(dt);
        if (status == ZHLN::FrameStatus::Error) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
