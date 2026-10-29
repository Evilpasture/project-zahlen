// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Opt-in local GPU test: a whole application launch, stopped before the first
// Tick so we can pin the ordering of composition, scene, input and gameplay.
#include "TestsFramework.hpp"
#include <Zahlen/App.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/GameModule.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <vector>

namespace ZHLN::Test {

void test_app_composes_before_scene_and_runs_gameplay_after_events() {
    std::vector<std::string_view> calls;
    int                           updates = 0;

    EngineRunConfig config {
        .appName        = "ApplicationTest",
        .validationMode = ValidationMode::Off,
        .headless       = true,
    };
    auto application = App(config);
    application.Install([&](Engine& engine) {
        calls.push_back("install");
        Expect(!engine.GetRegistry().GetSingleton<Components::CameraComponent>());
        const auto installed = engine.InstallModule(Module {.name = "AppTestModule"});
        Expect(installed.has_value());
    });
    application.OnStart([&](Engine& engine) {
        calls.push_back("start");
        Expect(engine.GetRegistry().GetSingleton<Components::CameraComponent>().has_value());
        Expect(!engine.IsNativeGameplayEnabled());
        Expect(!engine.IsNativeGameplayLoaded());
        // ProcessEvents begins an input frame. If update runs before it, the
        // deliberately stale mouse delta below is still visible to gameplay.
        engine.GetRegistry().GetSingleton<Components::InputStateComponent>()->mouseDeltaX = 123.0f;
        const auto modules = engine.GetInstalledModules();
        Expect(std::ranges::any_of(modules, [](const Module& module) { return module.name == "AppTestModule"; }));
        Expect(!std::ranges::any_of(modules, [](const Module& module) { return module.name == "FallbackScene"; }));
#if defined(ZHLN_TEST_APP_HAS_CHARACTER)
        Expect(modules.front().name == "CharacterController");
#endif
    });

    // A move-only callback proves Run does not copy its user's game logic.
    auto state = std::make_unique<int>(42);
    const int code = application.Run([&, state = std::move(state)](Engine& engine, float dt) {
        ++updates;
        Expect(*state == 42);
        Expect(dt >= 0.0f);
        const auto input = engine.GetRegistry().GetSingleton<Components::InputStateComponent>();
        Expect(input.has_value());
        Expect(input->mouseDeltaX == 0.0f);
        Expect(engine.GetCurrentFrame() == 0);
        calls.push_back("update");
        engine.GetPlatformHost().Close();
    });

    Expect(code == EXIT_SUCCESS);
    Expect(updates == 1);
    Expect(calls == std::vector<std::string_view> {"install", "start", "update"});
}

} // namespace ZHLN::Test
