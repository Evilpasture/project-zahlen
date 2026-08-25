// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// samples/GlTFInspectorSample.cpp
//
// Minimal host for the glTF inspector module. Builds the engine, hands control to
// ZHLN::glTF::Initialize (which opens the "drop a glTF" screen and wires up the
// file-drop + orbit viewer), then pumps the engine's standard frame loop.
//
// Drop a .glb / .gltf file onto the window to load and orbit it.

#include <Zahlen/Clock.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>

import ZHLN.glTF;

#include <algorithm>

auto main(int argc, char* argv[]) -> int {
    auto optionsRes = ZHLN::HandleCommandLine(std::span(argv, static_cast<size_t>(argc)));
    if (!optionsRes) {
        return EXIT_FAILURE;
    }
    const auto& options = optionsRes.value();

    if (options.helpRequested || options.versionRequested) {
        return EXIT_SUCCESS;
    }

    ZHLN::SetLogLevel(options.logLevel);
    ZHLN::SetupSignalHandler();
    ZHLN::TaskSystem::Init();

    auto engineRes = ZHLN::Engine::Create(
        {.physics = {.maxBodies = 2048, .maxBodyPairs = 4096, .maxContactConstraints = 4096},
         .render  = {.appName = "Zahlen :: glTF Inspector", .vsync = options.vsync, .fullscreen = options.fullscreen}}
    );

    if (!engineRes) {
        ZHLN::Log("FATAL: Failed to initialize Engine: {}", engineRes.error().Message());
        return EXIT_FAILURE;
    }

    auto engine = std::move(engineRes.value());
    engine->GetWindow().Focus();

    // Boots the default scene, the "drop a glTF" GUI, the file-drop handler and
    // the orbit camera. The inspector owns the frame loop from here on.
    ZHLN::glTF::Initialize(*engine);

    ZHLN::Clock clock;
    while (engine->IsRunning()) {
        const float dt = std::min(clock.GetDeltaTime(), 0.05f);
        engine->ProcessEvents();

        const auto status = engine->Tick(dt, ZHLN::GameplayDriver::Cpp);
        if (status == ZHLN::GameplayStatus::RequestQuit) {
            engine->GetWindow().Close();
            break;
        }
    }

    ZHLN::TaskSystem::Shutdown();
    return EXIT_SUCCESS;
}
