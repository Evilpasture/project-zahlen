// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SystemWiring.hpp"

#include "LODSystem.hpp"
#include "NativeScriptModule.hpp"
#include "AnimationSystem.hpp"
#include "ArticulationSystem.hpp"
#include "AudioSystem.hpp"
#include "CameraSystem.hpp"
#include "CullingSystem.hpp"
#include "DecalSystem.hpp"
#include "EnvironmentSunSystem.hpp"
#include "LightingSystem.hpp"
#include "PhysicsStateSystem.hpp"
#include "PhysicsSystem.hpp"
#include "RenderSystem.hpp"
#include "TransformSystem.hpp"
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/FrameScheduler.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/Frame.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <algorithm>
#include <filesystem>
#include <format>
#include <string_view>

namespace ZHLN {
namespace {

namespace Steps {


void HostUICallback(Engine& engine, float , FrameContext& ) {
    if (const auto cb = engine.GetUICallback(); cb && static_cast<bool>(*cb)) {
        (*cb)(engine);
    }
}

void HotReload(Engine& engine, float , FrameContext& ) {
    engine.GetFileSystemWatcher().DispatchEvents();
}

void Physics(Engine& engine, float dt, FrameContext& ) {
    PhysicsSystem::Update(engine, dt, engine.GetPhysicsAccumulator());
}

void Gameplay(Engine& engine, float dt, FrameContext& ctx) {
    switch (ctx.driver) {
        using enum GameplayDriver;
        case Cpp: {
            ZHLN::ScopedTimer profTimer("ECS System: Native C++ Gameplay Update");
            ctx.status = engine.UpdateNativeGameplay(dt);
            break;
        }
        case Scripted: {
            ZHLN::ScopedTimer profTimer("ECS System: Scripted Update");
            engine.GetScriptRunner().CallUpdate(&engine, dt);
            break;
        }
        case Hybrid: {
            {
                ZHLN::ScopedTimer profTimer("ECS System: Native C++ Gameplay Update");
                ctx.status = engine.UpdateNativeGameplay(dt);
            }
            {
                ZHLN::ScopedTimer profTimer("ECS System: Scripted Update");
                engine.GetScriptRunner().CallUpdate(&engine, dt);
            }
            break;
        }
    }
}

void UpdateGraph(Engine& engine, float dt, FrameContext& ) {
    // Before Execute, and on this thread: the scratch arenas are per-worker and
    // this is the last point at which no worker holds a pointer into one.
    engine.ResetWorkerScratch();
    const Frame frame = engine.MakeFrame(dt);
    engine.GetUpdateGraph().Execute(frame);
}

void CommandPlayback(Engine& engine, float , FrameContext& ) {
    engine.GetMainECB().Playback();
}

void CleanupScene(Engine& engine, float , FrameContext& ) {
    engine.ProcessPendingDestroy();
}

void Camera(Engine& engine, float dt, FrameContext& ) {
    // No instance and no function-local static: the pass is stateless, and the
    // camera it projects is component data.
    CameraSystem::Update(engine, dt, engine.GetCurrentAlpha());
}

void LOD(Engine& engine, float , FrameContext& ) {
    LODSystem::Update(engine);
}

void RenderGraph(Engine& engine, float dt, FrameContext& ) {
    engine.ResetWorkerScratch();
    const Frame frame = engine.MakeFrame(dt);
    engine.GetRenderGraph().Execute(frame);
}

void Present(Engine& engine, float dt, FrameContext& ctx) {
    auto render_res = RenderSystem::Update(engine, dt);
    if (!render_res) {
        if (render_res.error().Is(FrameResult::DeviceLost)) {
            if (auto lost_res = engine.HandleDeviceLost(); !lost_res) {
                ZHLN::Log("[Engine] Fatal: GPU device recovery failed: {}", lost_res.error());
                ctx.status = GameplayStatus::Error;
                engine.GetPlatformHost().Close();
            }
            ctx.deviceLost = true;
        }
    }
}


void TransformHistory(Engine& engine, float , FrameContext& ) {
    ZHLN::ScopedTimer      profTimer("ECS System: Update Transform History");
    TransformSystem::UpdateTransformHistory(engine.GetRegistry());
}

}

}

void BuildFrameScheduler(Engine& engine) {
    using Phase     = FramePhase;
    auto& scheduler = engine.GetFrameScheduler();

    scheduler.Clear();
    scheduler.Add(Phase::UI, "HostUICallback", Steps::HostUICallback);
    scheduler.Add(Phase::HotReload, "ScriptAndShaderReload", Steps::HotReload);
    scheduler.Add(Phase::Physics, "PhysicsSystem", Steps::Physics);
    scheduler.Add(Phase::Gameplay, "GameplayModule", Steps::Gameplay);
    scheduler.Add(Phase::Simulation, "UpdateGraph", Steps::UpdateGraph);
    scheduler.Add(Phase::Simulation, "MainECBPlayback", Steps::CommandPlayback);
    scheduler.Add(Phase::Simulation, "SceneCleanup", Steps::CleanupScene);
    scheduler.Add(Phase::Camera, "CameraSystems", Steps::Camera);
    scheduler.Add(Phase::Camera, "LODSystem", Steps::LOD);
    scheduler.Add(Phase::Visibility, "RenderGraph", Steps::RenderGraph);
    scheduler.Add(Phase::Present, "RenderSystem", Steps::Present);
    scheduler.Add(Phase::History, "TransformHistory", Steps::TransformHistory);

    engine.ApplyFrameSchedulerExtensions(scheduler);
}

void BuildSystemGraphs(Engine& engine) {
    auto& updateGraph = engine.GetUpdateGraph();
    auto& renderGraph = engine.GetRenderGraph();

    updateGraph.Clear();
    renderGraph.Clear();

    // Every graph node derives its name, thunk and component hazards from its
    // callable's signature. The camera is modified in the earlier frame phase.
    updateGraph.AddSystem<&VisualInterpolationSystem::Update>();
    updateGraph.AddSystem<&AnimationSystem::Update>();
    updateGraph.AddSystem<&ArticulationSystem::Update>();
    updateGraph.AddSystem<&TransformSystem::Update>();
    updateGraph.AddSystem<&AudioSystem>();

    renderGraph.DeclareExternalWrites("ExternalPreRenderWrites", {ECS::Write<Components::CameraComponent>()});
    renderGraph.AddSystem<&EnvironmentSunSystem::Update>();
    renderGraph.AddSystem<&CullingSystem::GraphUpdate>();
    renderGraph.AddSystem<&DecalSystem::Update>();
    renderGraph.AddSystem<&LightingSystem::Update>();

    engine.ApplySystemGraphsExtensions(updateGraph, renderGraph);

    updateGraph.Compile();
    renderGraph.Compile();
}

auto InitializeDefaultScene(Engine& engine) -> bool {
    auto& reg = engine.GetRegistry();

    reg.RegisterAllComponentsIn<ZHLN::Components>();

    reg.Create(
        Components::MainCameraTagComponent {}, Components::CameraComponent {},
        Components::AASettingsComponent {}, Components::FreeCamTagComponent {}
    );

    // A scene reset clears the registry, so the engine-level singletons World::Create()
    // seeds are re-seeded here alongside the rest of the scene's settings.
    reg.Create(
        Components::GlobalSettingsTagComponent {}, Components::PostProcessSettingsComponent {}, Components::ShadowSettingsComponent {},
        Components::CullingStatsComponent {}, Components::DebugSettingsComponent {.physicsDrawMode = 0}
    );

    reg.Create(GUI::UISettingsComponent {});

    engine.SeedSceneFontAtlas(reg);

    BuildSystemGraphs(engine);
    BuildFrameScheduler(engine);
    return true;
}

}
