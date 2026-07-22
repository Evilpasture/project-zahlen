// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/game_main.cpp
#include "Zahlen/Audio.hpp"
#include "Zahlen/CommandLine.hpp"
#include "Zahlen/Input.hpp"
#include "ecs/ECS.hpp"
#include "ecs/EntityCommandBuffer.hpp"
#include "ecs/SystemGraph.hpp"
#include "engine/FileWatcher.hpp"
#include "engine/Platform.hpp"
#include "engine/system/CameraSystem.hpp"
#include "engine/system/InputSystem.hpp"
#include "engine/system/LightingSystem.hpp"
#include "engine/system/PhysicsStateSystem.hpp"
#include "engine/system/PhysicsSystem.hpp"
#include "engine/system/RenderSystem.hpp"
#include "engine/system/TargetCameraSystem.hpp"
#include "engine/system/TransformSystem.hpp"
#include "engine/system/UIRenderSystem.hpp"
#include "imgui.h"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Clock.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Format.hpp>
#include <Zahlen/GUI.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Scripting.h>
#include <Zahlen/Scripting.hpp>
#include <algorithm>
#include <chrono>
#include <detail/ControlFlow.hpp>
#include <engine/system/AnimationSystem.hpp>
#include <engine/system/ArticulationSystem.hpp>
#include <engine/system/CullingSystem.hpp>
#include <engine/system/InteractionSystem.hpp>
#include <engine/system/UIInteractionSystem.hpp>
#include <expected>
#include <physics/PhysicsWorld.hpp>
#include <print>
#include <threading/Mutex.hpp>
#include <threading/TaskSystem.hpp>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

using namespace ZHLN;
using namespace ZHLN::ECS;

namespace ZHLN {
void UISystem(Engine& engine, ScriptRunner& scriptRunner);
} // namespace ZHLN

namespace {

void WriteBenchmarkLog(std::vector<double> frameTimes) {
    if (frameTimes.empty()) {
        return;
    }

    double totalTime = 0.0;
    for (double t: frameTimes) {
        totalTime += t;
    }
    double avgFrameTime = totalTime / frameTimes.size();
    double avgFps       = 1.0 / avgFrameTime;

    std::ranges::sort(frameTimes);

    size_t count1Percent = std::max<size_t>(1, frameTimes.size() / 100);
    double sum1Percent   = 0.0;
    for (size_t i = frameTimes.size() - count1Percent; i < frameTimes.size(); ++i) {
        sum1Percent += frameTimes[i];
    }
    double low1PercentFps = 1.0 / (sum1Percent / count1Percent);

    size_t count01Percent = std::max<size_t>(1, frameTimes.size() / 1000);
    double sum01Percent   = 0.0;
    for (size_t i = frameTimes.size() - count01Percent; i < frameTimes.size(); ++i) {
        sum01Percent += frameTimes[i];
    }
    double low01PercentFps = 1.0 / (sum01Percent / count01Percent);

    double p99  = frameTimes[static_cast<size_t>(frameTimes.size() * 0.99)] * 1000.0;
    double p999 = frameTimes[static_cast<size_t>(frameTimes.size() * 0.999)] * 1000.0;

    FILE* f = std::fopen("benchmark.log", "w");
    if (f != nullptr) {
        std::println(f, "=========================================");
        std::println(f, "         ZAHLEN BENCHMARK REPORT         ");
        std::println(f, "=========================================");
        std::println(f, "Total Frames:       {}", frameTimes.size());
        std::println(f, "Total Time (s):     {:.3f}", totalTime);
        std::println(f, "Average FPS:        {:.2f}", avgFps);
        std::println(f, "Average Frametime:  {:.2f} ms", avgFrameTime * 1000.0);
        std::println(f, "1% Low FPS:         {:.2f}", low1PercentFps);
        std::println(f, "0.1% Low FPS:       {:.2f}", low01PercentFps);
        std::println(f, "99.0% Percentile:   {:.2f} ms", p99);
        std::println(f, "99.9% Percentile:   {:.2f} ms", p999);
        std::println(f, "=========================================");
        std::fclose(f);
        ZHLN::Log("Benchmark report written to benchmark.log");
    } else {
        ZHLN::Log("Error: Failed to write benchmark.log");
    }
}

std::string s_GameplayFile = "scripts/gameplay_snow.fnl";

// ============================================================================
// Flattened System Wrappers (For 100% Predictable Function Pointers)
// ============================================================================

void Sys_VisualInterpolation(Engine& engine, float /*dt*/) {
    VisualInterpolationSystem::Update(engine, engine.GetCurrentAlpha());
}

void Sys_Animation(Engine& engine, float dt) {
    static AnimationSystem sys;
    sys.UpdateAnimations(engine.GetRenderContext(), engine.GetRegistry(), dt);
}

void Sys_Articulation(Engine& engine, float dt) {
    static ArticulationSystem sys;
    sys.Update(engine, dt);
}

void Sys_Transform(Engine& engine, float /*dt*/) {
    static TransformSystem sys;
    sys.ResolveTransforms(engine.GetRegistry());
}

void Sys_Audio(Engine& engine, float dt) {
    AudioSystem(engine, dt);
}

void Sys_TargetCamera(Engine& engine, float dt) {
    static TargetCameraSystem sys;
    sys.Update(engine, dt, engine.GetCurrentAlpha());
}

void Sys_Camera(Engine& engine, float dt) {
    static CameraSystem sys;
    sys.Update(engine, dt, engine.GetCurrentAlpha());
}

void Sys_Culling(Engine& engine, float /*dt*/) {
    engine.GetCullingSystem().Update<false>(engine, engine.GetVisibleEntities(), engine.GetVisibleShadowEntities());
}

void Sys_Lighting(Engine& engine, float dt) {
    static LightingSystem sys;
    sys.Update(engine, dt);
}

void Sys_ParticleSpawner(Engine& engine, float /*dt*/) {
}

void Sys_PostProcess(Engine& engine, float /*dt*/) {
    auto& reg = engine.GetRegistry();
    auto& rc  = engine.GetRenderContext();

    for (Entity e: reg.GetEntitiesWith<Components::PostProcessSettingsComponent>()) {
        if (auto* pp = reg.Get<Components::PostProcessSettingsComponent>(e)) {
            Renderer::SetGISettings(
                rc, {
                        .mode              = pp->giMode,
                        .aoRadius          = pp->aoRadius,
                        .aoBias            = pp->aoBias,
                        .aoPower           = pp->aoPower,
                        .giIntensity       = pp->giIntensity,
                        .giSamples         = pp->giSamples,
                        .vignetteIntensity = pp->vignetteIntensity,
                        .vignettePower     = pp->vignettePower,
                        .enableSSR         = pp->enableSSR ? 1 : 0,
                        .enableRTR         = pp->enableRTR ? 1 : 0,
                    }
            );
        }
    }
}

void BuildSystemGraphs(Engine& engine) {
    auto& updateGraph = engine.GetUpdateGraph();
    auto& renderGraph = engine.GetRenderGraph();

    updateGraph.AddSystem({
        .update_func    = Sys_VisualInterpolation,
        .name           = "VisualInterpolationSystem",
        .access_pattern = {Read<Components::PhysicsStateComponent>(), Write<Components::TransformComponent>()},
        .enabled        = true,
    });

    updateGraph.AddSystem({
        .update_func    = Sys_Animation,
        .name           = "AnimationSystem",
        .access_pattern = {Read<Components::MovementComponent>(), Write<Components::MeshComponent>()},
        .enabled        = true,
    });

    updateGraph.AddSystem({
        .update_func = Sys_Articulation,
        .name        = "ArticulationSystem",
        .access_pattern =
            {
                Read<Components::PhysicsComponent>(),
                Read<Components::MeshComponent>(),
                Write<Components::RagdollComponent>(),
                Write<Components::TransformComponent>(),
            },
        .enabled = true,
    });

    updateGraph.AddSystem({
        .update_func    = Sys_Transform,
        .name           = "TransformSystem",
        .access_pattern = {Read<Components::HierarchyComponent>(), Read<Components::TransformComponent>(), Write<Components::MeshComponent>()},
        .enabled        = true,
    });

    updateGraph.AddSystem(
        {.update_func = Sys_PostProcess, .name = "PostProcessSystem", .access_pattern = {Read<Components::PostProcessSettingsComponent>()}, .enabled = true}
    );

    updateGraph.AddSystem({
        .update_func    = Sys_Audio,
        .name           = "AudioSystem",
        .access_pattern = {Read<Components::PhysicsComponent>(), Read<Components::ALifeComponent>(), Write<Components::AudioSourceComponent>()},
        .enabled        = true,
    });

    updateGraph.AddSystem({.update_func = Sys_ParticleSpawner, .name = "ParticleSpawnerExample", .access_pattern = {}, .enabled = true});

    updateGraph.AddSystem({
        .update_func =
            [](Engine& eng, float dt) {
                static InteractionSystem sys;
                sys.Update(eng, dt);
            },
        .name = "InteractionSystem",
        .access_pattern =
            {
                Write<Components::TriggerComponent>(),
                Write<Components::ContainerComponent>(),
                Write<Components::PickupComponent>(),
                Read<Components::ItemBaseComponent>(),
                Read<Components::UsableComponent>(),
                Read<Components::MovementComponent>(),
            },
        .enabled = true,
    });

    updateGraph.Compile();

    renderGraph.AddSystem({
        .update_func    = Sys_Culling,
        .name           = "CullingSystem",
        .access_pattern = {Read<Components::MeshComponent>(), Read<Components::CameraComponent>()},
        .enabled        = true,
    });

    renderGraph.AddSystem({
        .update_func = Sys_Lighting,
        .name        = "LightingSystem",
        .access_pattern =
            {
                Read<Components::LightComponent>(),
                Read<Components::TransformComponent>(),
                Read<Components::NameComponent>(),
                Write<Components::MeshComponent>(),
            },
        .enabled = true,
    });

    renderGraph.Compile();
}

bool InitializeGame(Engine& engine) {
    auto& rc  = engine.GetRenderContext();
    auto& reg = engine.GetRegistry();
    auto& pc  = engine.GetPhysicsContext();

    Mesh lineMesh = CreativeWorksFactory::CreateBox(rc, {0.01f, 0.01f, 0.5f}, {0.0f, 1.0f, 1.0f, 1.0f});

    auto lineMat_res = CreativeWorksFactory::CreateBasicMaterial(rc);
    if (!lineMat_res) {
        ZHLN::Log("ERROR: Failed to compile basic material during initialization: {}", lineMat_res.error().Message());
        return false;
    }
    Material lineMat = lineMat_res.value();

    reg.RegisterAllComponentsIn<ZHLN::Components>();

    // Spawn a blank, static camera just to render the main menu
    Entity cameraEntity = reg.Create();
    reg.Add(cameraEntity, Components::MainCameraTagComponent {});
    reg.Add(cameraEntity, Components::CameraComponent {});
    reg.Add(cameraEntity, Components::AASettingsComponent {.state = {.mode = AAMode::TAA, .taaFeedback = 0.95f}});

    Entity settingsEntity = reg.Create();
    reg.Add(settingsEntity, Components::GlobalSettingsTagComponent {});
    reg.Add(settingsEntity, Components::PostProcessSettingsComponent {});
    reg.Add(settingsEntity, Components::ShadowSettingsComponent {});
    reg.Add(
        settingsEntity, Components::DebugSettingsComponent {
                            .debugLineVbo      = lineMesh.posBuffer,
                            .debugLinePipeline = lineMat.pipeline,
                            .debugLineAlbedo   = lineMat.albedoIndex,
                            .physicsDrawMode   = 0,
                        }
    );

    Entity uiSettings = reg.Create();
    reg.Add(uiSettings, Components::UISettingsComponent {});
    CreativeWorksFactory::CreateFontAtlasTexture(rc);

    BuildSystemGraphs(engine);

    return true;
}

void UpdateGame(Engine& engine, float dt, ScriptRunner& scriptRunner, FileWatcher& gameplayWatcher) {
    static InputSystem inputSystem;
    inputSystem.Update(engine);
    UIInteractionSystem::Update(engine);
    UISystem(engine, scriptRunner);

    if (gameplayWatcher.CheckModified()) {
        scriptRunner.ReloadFile(s_GameplayFile);
    }
    engine.GetRenderContext().CheckShaderReload();

    static TargetCameraSystem targetCamSys;
    static CameraSystem       camSys;
    targetCamSys.Update(engine, dt, engine.GetCurrentAlpha());
    camSys.Update(engine, dt, engine.GetCurrentAlpha());

    inputSystem.PlayerInputTranslate(engine, engine.GetCamera());

    // Consolidate and execute the physical simulation loop
    static PhysicsSystem physicsSystem;
    physicsSystem.Update(engine, dt);

    {
        ZHLN_PROFILE_SCOPE("ECS System: Script/Lua Update");
        scriptRunner.CallUpdate(&engine, dt);
    }

    engine.GetUpdateGraph().Execute(engine, dt);
    engine.GetMainECB().Playback();
}

std::expected<void, RenderFrameResult> RenderGame(Engine& engine, float frameTime) {
    engine.GetRenderGraph().Execute(engine, frameTime);

    auto render_res = RenderSystem::Update(engine);
    if (!render_res) {
        if (render_res.error().Is<RenderFrameResult>()) {
            return std::unexpected(render_res.error().As<RenderFrameResult>());
        }
        return std::unexpected(RenderFrameResult::Error);
    }

    {
        ZHLN_PROFILE_SCOPE("ECS System: Update Transform History");
        static TransformSystem ts;
        ts.UpdateTransformHistory(engine.GetRegistry());
    }

    return {};
}

std::expected<std::unique_ptr<Engine>, EngineError> InitializeEngine(CommandLineOptions options) {
    Platform::Init();
    ZHLN::SetupSignalHandler();
    TaskSystem::Init();

    uint32_t w = options.fullscreen ? 0 : 1280;
    uint32_t h = options.fullscreen ? 0 : 720;

    EngineConfig config {
        .physics = {.maxBodies = 5000, .maxBodyPairs = 10000, .maxContactConstraints = 10000, .tempAllocatorSize = 64 * 1024 * 1024},
        .render  = {
            .appName          = "Zahlen Engine",
            .width            = w,
            .height           = h,
            .vsync            = options.vsync,
            .fullscreen       = options.fullscreen,
            .enableValidation = options.enableValidation,
        },
    };

    // Call the refactored factory method
    auto engine_res = Engine::Create(config);

    if (!engine_res) {
        return std::unexpected(EngineError {.msg = std::string(engine_res.error().Message()), .code = EXIT_FAILURE});
    }

    auto engine = std::move(engine_res.value());
    engine->GetWindow().Focus();
    return engine;
}

std::expected<int, EngineError> RunEngineLoop(std::unique_ptr<Engine> engine, const CommandLineOptions& options) {
    ScriptRunner scriptRunner;
    FileWatcher  gameplayWatcher(s_GameplayFile);

    if (!InitializeGame(*engine)) {
        return std::unexpected(EngineError {.msg = "Game failed to initialize.", .code = EXIT_FAILURE});
    }

    for (int i = 0; i < 3; ++i) {
        engine->ProcessEvents();

        auto res = RenderGame(*engine, 0.016f);
        if (!res) {
            if (res.error() == RenderFrameResult::DeviceLost) {
                engine->HandleDeviceLost();
            }
        }
    }

    ZHLN::Log("Window active and presenting. Loading scene assets... ");
    scriptRunner.CallUpdate(engine.get(), 0.0f);

    // float physicsAccumulator = 0.0f; <--- REMOVED (Now managed inside PhysicsSystem)
    const double targetFrameTime = options.fpsLimit > 0 ? 1.0 / static_cast<double>(options.fpsLimit) : 0.0;

    std::vector<double> frameTimes;
    if (options.benchmark) {
        frameTimes.reserve(10000);
    }

    auto frameStart = std::chrono::high_resolution_clock::now();

    while (engine->IsRunning()) {
        engine->ProcessEvents();

        if (engine->GetInput().IsKeyDown(KeyCode::Escape)) {
            engine->GetWindow().Close();
            break;
        }

        auto   frameEnd = std::chrono::high_resolution_clock::now();
        double elapsed  = std::chrono::duration<double>(frameEnd - frameStart).count();
        frameStart      = std::chrono::high_resolution_clock::now();

        if (options.benchmark) {
            frameTimes.push_back(elapsed);
        }

        float rawDt = std::min(static_cast<float>(elapsed), 0.1f);

        double               target      = elapsed;
        constexpr std::array snapTargets = {1.0 / 60.0, 1.0 / 75.0, 1.0 / 90.0, 1.0 / 120.0, 1.0 / 144.0, 1.0 / 240.0, 1.0 / 360.0};
        for (double t: snapTargets) {
            if (std::abs(elapsed - t) < 0.001) {
                target = t;
                break;
            }
        }

        static double smoothedElapsed = 0.0166667;
        smoothedElapsed               = (smoothedElapsed * 0.9) + (target * 0.1);
        float frameTime               = std::min(static_cast<float>(smoothedElapsed), 0.1f);

        if (engine->GetInput().NeedsResize()) {
            engine->GetRenderContext().SetResolution(engine->GetInput().GetNewSize());
            engine->GetInput().ClearResizeFlag();
            if (!engine->GetWindow().IsTTY()) {
                ImGui::EndFrame();
            }
            continue;
        }

        // Simplified game update loop signature
        UpdateGame(*engine, rawDt, scriptRunner, gameplayWatcher);

        auto render_res = RenderGame(*engine, rawDt);
        if (!render_res) {
            if (render_res.error() == RenderFrameResult::DeviceLost) {
                engine->HandleDeviceLost();
            }
        }

        if (options.fpsLimit > 0) {
            auto   now          = std::chrono::high_resolution_clock::now();
            double frameElapsed = std::chrono::duration<double>(now - frameStart).count();
            if (frameElapsed < targetFrameTime) {
                double sleepTime = targetFrameTime - frameElapsed;
                if (sleepTime > 0.002) {
                    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>((sleepTime - 0.001) * 1e6)));
                }
                while (std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - frameStart).count() < targetFrameTime) {
                    CPURelax();
                }
            }
        }
    }

    TaskSystem::Shutdown();

    if (options.benchmark && !frameTimes.empty()) {
        WriteBenchmarkLog(frameTimes);
    }

    return EXIT_SUCCESS;
}

} // namespace

extern std::expected<int, int> RunGame(const ZHLN::CommandLineOptions& options) {
    return InitializeEngine(options).and_then(
                                        [&options](std::unique_ptr<Engine> engine) { return RunEngineLoop(std::move(engine), options); }
    ).transform_error([](const EngineError& err) -> int {
        if (!err.msg.empty() && !err.silent) {
            ZHLN::Log("Error: {}", err.msg);
        }
        return err.code;
    });
}
