// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Opt-in local GPU test: whole application launches. Most stop before the
// first Tick; a failure probe runs a UI-only frame without GPU rendering.
#include "TestsFramework.hpp"
#include <Zahlen/App.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/FramePlan.hpp>
#include <Zahlen/GameModule.hpp>
#include <Zahlen/Kernel.hpp>
#include <Zahlen/KernelApp.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <algorithm>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace ZHLN::Test {

// Both the simulation and render graphs can resolve canonical engine configuration
// through their existing extension-service slots (without an Engine field).
static_assert(ECS::ResolvableFrom<SystemContext, ECS::Res<EngineConfig>>);
static_assert(ECS::ResolvableFrom<ECS::Carrier<RenderServices>, ECS::Res<EngineConfig>>);

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

    EngineConfig config {
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
        Expect(engine.Service<EngineConfig>().has_value()); // no-CLI uses the same canonical service
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

// Move-only state must survive the UI callback's type erasure and remain
// available to Stop while the Engine and its scene still exist.
struct UILifecycleLayer {
    std::unique_ptr<int>             state;
    std::vector<std::string_view>* calls = nullptr;
    void Start(Engine&) {
        ++*state;
        calls->push_back("layer-start");
    }
    void Tick(Engine&, float) {
        ++*state;
        calls->push_back("layer-tick");
    }
    void UI(Engine& engine) {
        Expect(*state == 3);
        Expect(engine.GetCurrentFrame() == 0);
        calls->push_back("layer-ui");
    }
    void Stop(Engine& engine) {
        Expect(*state == 3);
        Expect(engine.GetRegistry().GetSingleton<Components::CameraComponent>().has_value());
        calls->push_back("layer-stop");
    }
};

struct UIOnlyPhaseLayer {
    std::vector<std::string_view>* calls = nullptr;
    void UI(Engine&) { calls->push_back("ui-only"); }
};

struct StopOnlyPhaseLayer {
    std::vector<std::string_view>* calls = nullptr;
    void Stop(Engine&) { calls->push_back("stop-only"); }
};

struct HUDPhaseLayer {
    std::vector<std::string_view>* calls = nullptr;
    void UI(Engine&) { calls->push_back("hud-ui"); }
};

void RunHostUIPhaseForTest(Engine& engine) {
    // Exercise the actual Core UI phase without running GPU/render steps.
    const auto* uiStep = engine.GetFramePlan().Find("HostUICallback");
    Expect(uiStep != nullptr);
    if (uiStep != nullptr) {
        Expect(uiStep->phase == FramePhase::UI);
        Expect(uiStep->run != nullptr);
        if (uiStep->run != nullptr) {
            FrameContext frame;
            uiStep->run(engine, 0.0f, frame);
            Expect(frame.status == FrameStatus::OK);
        }
    }
}

void test_app_ui_only_layer_uses_engine_ui_phase() {
    std::vector<std::string_view> calls;
    auto app = App(EngineConfig {
        .appName        = "UIOnlyAppLayerTest",
        .validationMode = ValidationMode::Off,
        .headless       = true,
    });
    app.AddLayer(UIOnlyPhaseLayer {.calls = &calls});
    const auto result = app.Run([&](Engine& engine, float) {
        RunHostUIPhaseForTest(engine);
        engine.GetPlatformHost().Close();
    });
    ExpectUserClosed(result);
    Expect(calls == std::vector<std::string_view> {"ui-only"});
}

void test_app_layers_render_ui_in_order_and_stop_before_engine_teardown() {
    std::vector<std::string_view> calls;
    auto app = App(EngineConfig {
        .appName        = "AppLayerLifecycleTest",
        .validationMode = ValidationMode::Off,
        .headless       = true,
    });
    app.AddLayer(UILifecycleLayer {.state = std::make_unique<int>(1), .calls = &calls});
    app.AddLayer(UIOnlyPhaseLayer {.calls = &calls});
    app.AddLayer(StopOnlyPhaseLayer {.calls = &calls});
    app.AddLayer(HUDPhaseLayer {.calls = &calls});
    app.OnStart([&](Engine&) { calls.push_back("app-start"); });

    const auto result = app.Run([&](Engine& engine, float) {
        calls.push_back("update");
        RunHostUIPhaseForTest(engine);
        engine.GetPlatformHost().Close();
    });
    ExpectUserClosed(result);
    Expect(calls == std::vector<std::string_view> {
        "layer-start", "app-start", "layer-tick", "update",
        "layer-ui", "ui-only", "hud-ui", "stop-only", "layer-stop"
    });
}

void test_app_stops_layers_on_typed_frame_failure() {
    std::vector<std::string_view> calls;
    auto app = App(EngineConfig {
        .appName        = "AppLayerFrameFailureTest",
        .validationMode = ValidationMode::Off,
        .headless       = true,
    });
    app.AddLayer(UILifecycleLayer {.state = std::make_unique<int>(1), .calls = &calls});
    app.OnStart([&](Engine& engine) {
        // Replace the plan with a single deterministic failing UI step. This
        // tests the frame-error result and cleanup without a GPU frame.
        auto& plan = engine.GetFramePlan();
        plan.Clear();
        const auto placed = plan.Append(FramePhase::UI, "EmitUIAndFail", +[](Engine& active, float, FrameContext& frame) {
            if (const auto cb = active.GetUICallback()) {
                (*cb)(active);
            }
            frame.status = FrameStatus::Error;
        });
        Expect(placed.has_value());
    });
    const auto result = app.Run([&](Engine&, float) { calls.push_back("update"); });
    Expect(!result.has_value());
    if (!result) Expect(result.error().Is(AppError::FrameTickFailed));
    Expect(calls == std::vector<std::string_view> {"layer-start", "layer-tick", "update", "layer-ui", "layer-stop"});
}

void test_app_engine_config_service_is_available_before_install_and_scene_start() {
    EngineConfig config {
        .appName        = "ApplicationCliTest",
        .validationMode = ValidationMode::Off,
        .headless       = true,
    };
    bool             actionRan = false;
    bool             prepared  = false;
    bool             installed = false;
    bool             started   = false;
    bool             updated   = false;
    std::string_view region;

    auto app = App(config);
    app.AddCliOption(
        "--region", "<name>", "Initial region",
        [&](std::string_view value) -> std::expected<void, CommandLineError> {
            region    = value;
            actionRan = true;
            return {};
        }
    );
    app.BeforeEngineCreate([&]() -> std::expected<void, ErrorCode> {
        Expect(actionRan && region == "atrium");
        Expect(!installed && !started);
        prepared = true;
        return {};
    });
    app.Install([&](Engine& engine) {
        Expect(prepared);
        auto engineConfig = engine.Service<EngineConfig>();
        Expect(engineConfig.has_value());
        if (engineConfig) {
            Expect(!engineConfig->vsync);
            Expect(engineConfig->headless);
            Expect(engineConfig->appName == config.appName);
        }
        Expect(actionRan);
        Expect(region == "atrium");
        installed = true;
    });
    app.OnStart([&](Engine& engine) {
        Expect(installed);
        Expect(engine.Service<EngineConfig>().has_value());
        started = true;
    });

    char program[] = "test-application";
    char option[]  = "--region=atrium";
    char vsync[]   = "--vsync=off";
    char* args[]   = {program, option, vsync};
    const auto result = app.Run(std::span<char* const>(args), [&](Engine& engine) {
        Expect(started);
        Expect(engine.Service<EngineConfig>().has_value());
        updated = true;
        engine.GetPlatformHost().Close();
    });
    ExpectUserClosed(result);
    Expect(prepared && installed && started && updated);
}

void test_kernel_app_owns_process_input_and_shutdown_without_an_ecs_world() {
    bool started = false;
    bool inputReset = false;
    bool updated = false;
    bool rendered = false;
    bool shutdown = false;
    const auto result = KernelApp(EngineConfig {
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
