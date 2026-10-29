// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Opt-in local GPU test: a whole application launch, stopped before the first
// Tick so we can pin the ordering of composition, scene, input and gameplay.
#include "TestsFramework.hpp"
#include <Zahlen/App.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/GameModule.hpp>
#include <Zahlen/Kernel.hpp>
#include <Zahlen/KernelApp.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <algorithm>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ZHLN::Test {

// Both the simulation and render graphs can resolve application CLI options
// through their existing extension-service slots (without an Engine field).
static_assert(ECS::ResolvableFrom<SystemContext, ECS::Res<CommandLineOptions>>);
static_assert(ECS::ResolvableFrom<ECS::Carrier<RenderServices>, ECS::Res<CommandLineOptions>>);

void ExpectUserClosed(const AppRunResult& result) {
    Expect(result.has_value());
    if (result) Expect(*result == AppExitReason::UserClosed);
}

struct StartPhaseLayer {
    std::vector<std::string_view>* calls = nullptr;
    void Start(Engine&) { calls->push_back("start-only"); }
};

struct TickPhaseLayer {
    std::vector<std::string_view>* calls = nullptr;
    void Tick(Engine&, float) { calls->push_back("tick-only"); }
};

struct RecordingLayer {
    std::vector<std::string_view>* calls = nullptr;
    void Start(Engine& engine) {
        calls->push_back("layer-start");
        Expect(engine.GetRegistry().GetSingleton<Components::CameraComponent>().has_value());
    }
    void Tick(Engine& engine, float dt) {
        calls->push_back("layer-tick");
        Expect(engine.GetCurrentFrame() == 0);
        Expect(dt >= 0.0f);
    }
};

void test_app_composes_before_scene_and_runs_gameplay_after_events() {
    std::vector<std::string_view> calls;
    int                           updates = 0;

    EngineRunConfig config {
        .appName        = "ApplicationTest",
        .validationMode = ValidationMode::Off,
        .headless       = true,
    };
    auto application = App(config);
    application.AddLayer(StartPhaseLayer {.calls = &calls});
    application.AddLayer(RecordingLayer {.calls = &calls});
    application.AddLayer(TickPhaseLayer {.calls = &calls});
    application.Install([&](Engine& engine) {
        calls.push_back("install");
        Expect(!engine.Service<CommandLineOptions>()); // no-CLI Run stays service-free
        Expect(!engine.GetRegistry().GetSingleton<Components::CameraComponent>());
        const auto installed = engine.InstallModule(Module {.name = "AppTestModule"});
        Expect(installed.has_value());
    });
    application.OnStart([&](Engine& engine) {
        calls.push_back("start");
        Expect(engine.GetRegistry().GetSingleton<Components::CameraComponent>().has_value());
        const auto& plan = engine.GetFramePlan();
        Expect(plan.Find("PhysicsSystem") != nullptr);
        Expect(plan.Find("UpdateGraph") != nullptr);
        Expect(plan.Find("GameplayModule") == nullptr);
        // ProcessEvents begins an input frame. If update runs before it, the
        // deliberately stale mouse delta below is still visible to gameplay.
        auto input = engine.GetRegistry().GetSingleton<Components::InputStateComponent>();
        input->mouseDeltaX = 123.0f;
        // A resize is consumed during Engine::ProcessEvents, not by App's loop.
        input->ApplyResize({.width = 800, .height = 600});
        const auto modules = engine.GetInstalledModules();
        Expect(std::ranges::any_of(modules, [](const Module& module) { return module.name == "AppTestModule"; }));
        Expect(!std::ranges::any_of(modules, [](const Module& module) { return module.name == "FallbackScene"; }));
#if defined(ZHLN_TEST_APP_HAS_CHARACTER)
        Expect(modules.front().name == "CharacterController");
#endif
    });

    // A move-only callback proves Run does not copy its user's game logic.
    auto state = std::make_unique<int>(42);
    const auto result = application.Run([&, state = std::move(state)](Engine& engine, float dt) {
        ++updates;
        Expect(*state == 42);
        Expect(dt >= 0.0f);
        const auto input = engine.GetRegistry().GetSingleton<Components::InputStateComponent>();
        Expect(input.has_value());
        Expect(input->mouseDeltaX == 0.0f);
        Expect(!input->needsResize);
        Expect(input->newSize.width == 800u);
        Expect(engine.GetCurrentFrame() == 0);
        calls.push_back("update");
        engine.GetPlatformHost().Close();
    });

    ExpectUserClosed(result);
    Expect(updates == 1);
    Expect(calls == std::vector<std::string_view> {"install", "start-only", "layer-start", "start", "layer-tick", "tick-only", "update"});
}

void test_app_cli_options_service_is_available_before_install_and_scene_start() {
    EngineRunConfig config {
        .appName        = "ApplicationCliTest",
        .validationMode = ValidationMode::Off,
        .headless       = true,
    };
    bool             actionRan = false;
    bool             installed = false;
    bool             started   = false;
    bool             updated   = false;
    std::string_view region;

    auto app = App(config);
    app.AddCliOption(
        "--region", "<name>", "Initial region",
        [&](CommandLineOptions&, std::string_view value) -> std::expected<void, ErrorCode> {
            region    = value;
            actionRan = true;
            return {};
        }
    );
    app.Install([&](Engine& engine) {
        auto parsed = engine.Service<CommandLineOptions>();
        Expect(parsed.has_value());
        if (parsed) {
            Expect(parsed->appHandlers.size() == 1);
            Expect(parsed->args.size() == 3);
            Expect(!parsed->vsync);
        }
        Expect(actionRan);
        Expect(region == "atrium");
        installed = true;
    });
    app.OnStart([&](Engine& engine) {
        Expect(installed);
        Expect(engine.Service<CommandLineOptions>().has_value());
        started = true;
    });

    char program[] = "test-application";
    char option[]  = "--region=atrium";
    char vsync[]   = "--vsync=off";
    char* args[]   = {program, option, vsync};
    const auto result = app.Run(std::span<char* const>(args), [&](Engine& engine) {
        Expect(started);
        Expect(engine.Service<CommandLineOptions>().has_value());
        updated = true;
        engine.GetPlatformHost().Close();
    });
    ExpectUserClosed(result);
    Expect(installed && started && updated);
}

void test_kernel_app_owns_process_input_and_shutdown_without_an_ecs_world() {
    bool started = false;
    bool inputReset = false;
    bool updated = false;
    bool rendered = false;
    bool shutdown = false;
    const auto result = KernelApp(RenderConfig {
        .appName        = "KernelApplicationTest",
        .width          = 320,
        .height         = 180,
        .validationMode = ValidationMode::Off,
        .headless       = true,
    })
        .OnStart([&](Kernel&) { started = true; })
        .OnInputFrame([&](Kernel&, float) { inputReset = true; })
        .OnUpdate([&](Kernel& kernel, float dt) {
            Expect(started && inputReset);
            Expect(dt >= 0.0f);
            updated = true;
            kernel.GetPlatformHost().Close(); // no GPU frame or Window required
        })
        .OnRender([&](Kernel&, FrameScope&) { rendered = true; })
        .OnShutdown([&](Kernel&) { shutdown = true; })
        .Run();
    ExpectUserClosed(result);
    Expect(started && inputReset && updated && shutdown);
    Expect(!rendered);
}

} // namespace ZHLN::Test
