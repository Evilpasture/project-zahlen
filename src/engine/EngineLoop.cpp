// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/EngineLoop.cpp
#include "Zahlen/Audio.hpp"
#include "Zahlen/CommandLine.hpp"
#include "Zahlen/Components.hpp"
#include "Zahlen/CreativeWorksFactory.hpp"
#include "Zahlen/Engine.hpp"
#include "Zahlen/Input.hpp"
#include "Zahlen/Log.hpp"
#include "Zahlen/Render.hpp"
#include "Zahlen/Scripting.hpp"
#include "Zahlen/Window.hpp"
#include "Zahlen/ecs/ECS.hpp"
#include "ecs/EntityCommandBuffer.hpp"
#include "ecs/SystemGraph.hpp"
#include "engine/FileWatcher.hpp"
#include "engine/NativeScriptModule.hpp"
#include "engine/Platform.hpp"
#include "engine/system/AnimationSystem.hpp"
#include "engine/system/ArticulationSystem.hpp"
#include "engine/system/CameraSystem.hpp"
#include "engine/system/CullingSystem.hpp"
#include "engine/system/DecalSystem.hpp"
#include "engine/system/InputSystem.hpp"
#include "engine/system/InteractionSystem.hpp"
#include "engine/system/LightingSystem.hpp"
#include "engine/system/ParticleSystem.hpp"
#include "engine/system/PhysicsStateSystem.hpp"
#include "engine/system/PhysicsSystem.hpp"
#include "engine/system/RenderSystem.hpp"
#include "engine/system/TargetCameraSystem.hpp"
#include "engine/system/TerrainSystem.hpp"
#include "engine/system/TransformSystem.hpp"
#include "engine/system/UIInteractionSystem.hpp"
#include "engine/system/UIRenderSystem.hpp"
#include "imgui.h"
#include <Zahlen/Editor.hpp>
#include <Zahlen/Profiler.hpp>
#include <algorithm>
#include <chrono>
#include <thread>

namespace ZHLN {

void UISystem(Engine& engine, ScriptRunner& scriptRunner);

namespace {

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

void Sys_Culling(Engine& engine, float /*dt*/) {
    engine.GetCullingSystem().Update<false>(engine, engine.GetVisibleEntities(), engine.GetVisibleShadowEntities());
}

void Sys_Lighting(Engine& engine, float dt) {
    static LightingSystem sys;
    sys.Update(engine, dt);
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

void Sys_Particle(Engine& engine, float dt) {
    static ParticleSystem sys;
    sys.Update(engine, dt);
}

void Sys_Terrain(Engine& engine, float dt) {
    static TerrainSystem sys;
    sys.Update(engine, dt);
}

void BuildSystemGraphs(Engine& engine) {
    auto& updateGraph = engine.GetUpdateGraph();
    auto& renderGraph = engine.GetRenderGraph();

    using namespace ZHLN::ECS;

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

    updateGraph.AddSystem({
        .update_func    = Sys_PostProcess,
        .name           = "PostProcessSystem",
        .access_pattern = {Read<Components::PostProcessSettingsComponent>()},
        .enabled        = true,
    });

    updateGraph.AddSystem({
        .update_func    = Sys_Audio,
        .name           = "AudioSystem",
        .access_pattern = {Read<Components::PhysicsComponent>(), Read<Components::ALifeComponent>(), Write<Components::AudioSourceComponent>()},
        .enabled        = true,
    });

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

    updateGraph.AddSystem({
        .update_func    = Sys_Particle,
        .name           = "ParticleSystem",
        .access_pattern = {Write<Components::ParticleEmitterComponent>()},
        .enabled        = true,
    });

    updateGraph.AddSystem({
        .update_func    = Sys_Terrain,
        .name           = "TerrainSystem",
        .access_pattern = {Write<Components::TerrainComponent>(), Write<Components::MeshComponent>()},
        .enabled        = true,
    });

    updateGraph.Compile();

    renderGraph.AddSystem({
        .update_func    = Sys_Culling,
        .name           = "CullingSystem",
        .access_pattern = {Read<Components::MeshComponent>(), Read<Components::CameraComponent>()},
        .enabled        = true,
    });

    renderGraph.AddSystem({
        .update_func    = [](Engine& eng, float /*dt*/) { DecalSystem::Update(eng); },
        .name           = "DecalSystem",
        .access_pattern = {Read<Components::DecalComponent>(), Read<Components::TransformComponent>()},
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

} // namespace

bool Engine::InitializeDefaultScene() {
    auto& rc  = GetRenderContext();
    auto& reg = GetRegistry();

    reg.RegisterAllComponentsIn<ZHLN::Components>();

    Entity cameraEntity = reg.Create();
    reg.Add(cameraEntity, Components::MainCameraTagComponent {});
    reg.Add(cameraEntity, Components::CameraComponent {});
    reg.Add(cameraEntity, Components::AASettingsComponent {.state = {.mode = AAMode::TAA, .taaFeedback = 0.95f}});

    Entity settingsEntity = reg.Create();
    reg.Add(settingsEntity, Components::GlobalSettingsTagComponent {});
    reg.Add(settingsEntity, Components::PostProcessSettingsComponent {});
    reg.Add(settingsEntity, Components::ShadowSettingsComponent {});
    reg.Add(settingsEntity, Components::DebugSettingsComponent {.physicsDrawMode = 0});

    Entity uiSettings = reg.Create();
    reg.Add(uiSettings, Components::UISettingsComponent {});
    CreativeWorksFactory::CreateFontAtlasTexture(rc);

    BuildSystemGraphs(*this);
    return true;
}

GameplayStatus Engine::Tick(float dt, GameplayDriver driver) {
    static ScriptRunner       scriptRunner;
    static FileWatcher        gameplayWatcher("scripts/boot.lua");
    static NativeScriptModule nativeModule("scripts/gameplay");
    static InputSystem        inputSystem;
    static TargetCameraSystem targetCamSys;
    static CameraSystem       camSys;
    static PhysicsSystem      physicsSystem;

    // 1. Process Input & UI Systems
    inputSystem.Update(*this);
    UIInteractionSystem::Update(*this, dt);
    UISystem(*this, scriptRunner);

    if (driver != GameplayDriver::Cpp && gameplayWatcher.CheckModified()) {
        scriptRunner.ReloadFile("scripts/boot.lua");
    }
    GetRenderContext().CheckShaderReload();

    // 2. Camera Systems
    targetCamSys.Update(*this, dt, GetCurrentAlpha());
    camSys.Update(*this, dt, GetCurrentAlpha());
    inputSystem.PlayerInputTranslate(*this, GetCamera());

    // 3. Physics Fixed Step & WriteBack
    physicsSystem.Update(*this, dt);

    // 4. Gameplay Module Update
    GameplayStatus status = GameplayStatus::OK;
    switch (driver) {
        using enum GameplayDriver;
        case Cpp: {
            ZHLN_PROFILE_SCOPE("ECS System: Native C++ Gameplay Update");
            status = nativeModule.Update(this, dt);
            break;
        }
        case Fennel: {
            ZHLN_PROFILE_SCOPE("ECS System: Script/Lua Update");
            scriptRunner.CallUpdate(this, dt);
            break;
        }
        case Hybrid: {
            {
                ZHLN_PROFILE_SCOPE("ECS System: Native C++ Gameplay Update");
                status = nativeModule.Update(this, dt);
            }
            {
                ZHLN_PROFILE_SCOPE("ECS System: Script/Lua Update");
                scriptRunner.CallUpdate(this, dt);
            }
            break;
        }
    }

    // 5. Update Graph & Command Buffer Playback
    GetUpdateGraph().Execute(*this, dt);
    GetMainECB().Playback();

    // 6. Render Graph & Frame Submission
    GetRenderGraph().Execute(*this, dt);
    auto render_res = RenderSystem::Update(*this, dt);
    if (!render_res) {
        if (render_res.error().Is<RenderFrameResult>() && render_res.error().As<RenderFrameResult>() == RenderFrameResult::DeviceLost) {
            HandleDeviceLost();
        }
    }

    // 7. Motion Vectors & Transform History
    {
        ZHLN_PROFILE_SCOPE("ECS System: Update Transform History");
        static TransformSystem ts;
        ts.UpdateTransformHistory(GetRegistry());
    }

    return status;
}

int Engine::Run(const CommandLineOptions& options) {
    Platform::Init();
    ZHLN::SetupSignalHandler();
    TaskSystem::Init();

    uint32_t w = options.fullscreen ? 0 : 1280;
    uint32_t h = options.fullscreen ? 0 : 720;

    EngineConfig config {
        .physics = {.maxBodies = 5000, .maxBodyPairs = 10000, .maxContactConstraints = 10000, .tempAllocatorSize = 64 * 1024 * 1024},
        .render  = {
            .appName          = options.launchEditor ? "Zahlen World Editor" : "Zahlen Engine",
            .width            = w,
            .height           = h,
            .vsync            = options.vsync,
            .fullscreen       = options.fullscreen,
            .enableValidation = options.enableValidation,
        },
    };

    auto engine_res = Engine::Create(config);
    if (!engine_res) {
        ZHLN::Log("Error initializing Engine: {}", engine_res.error().Message());
        return EXIT_FAILURE;
    }

    auto engine = std::move(engine_res.value());
    engine->GetWindow().Focus();
    engine->InitializeDefaultScene();

    // --- LAUNCH EDITOR OR GAME LOOP ---
    if (options.launchEditor) {
        return WorldEditor::Run(*engine, options); // Launches full editor
    }

    const double targetFrameTime = options.fpsLimit > 0 ? 1.0 / static_cast<double>(options.fpsLimit) : 0.0;
    auto         frameStart      = std::chrono::high_resolution_clock::now();

    while (engine->IsRunning()) {
        engine->ProcessEvents();

        if (engine->GetInput().IsKeyDown(KeyCode::Escape)) {
            engine->GetWindow().Close();
            break;
        }

        auto   frameEnd = std::chrono::high_resolution_clock::now();
        double elapsed  = std::chrono::duration<double>(frameEnd - frameStart).count();
        frameStart      = std::chrono::high_resolution_clock::now();

        float rawDt = std::min(static_cast<float>(elapsed), 0.1f);

        if (engine->GetInput().NeedsResize()) {
            engine->GetRenderContext().SetResolution(engine->GetInput().GetNewSize());
            engine->GetInput().ClearResizeFlag();
            if (!engine->GetWindow().IsTTY()) {
                ImGui::EndFrame();
            }
            continue;
        }

        // Single synchronized engine tick
        GameplayStatus status = engine->Tick(rawDt, options.driver);
        if (status == GameplayStatus::RequestQuit) {
            engine->GetWindow().Close();
            break;
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
    return EXIT_SUCCESS;
}

} // namespace ZHLN
