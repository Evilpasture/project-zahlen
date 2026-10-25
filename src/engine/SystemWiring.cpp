// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SystemWiring.hpp"
#include "ArticulationSystem.hpp"
#include "AudioSystem.hpp"
#include "CameraSystem.hpp"
#include "CullingSystem.hpp"
#include "DecalSystem.hpp"
#include "EnvironmentSunSystem.hpp"
#include "LODSystem.hpp"
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
#include <Zahlen/Frame.hpp>
#include <Zahlen/FramePlan.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <string_view>

namespace ZHLN {
namespace { namespace Steps {

void HostUICallback(Engine& engine, float, FrameContext&) {
    if (const auto cb = engine.GetUICallback(); cb && static_cast<bool>(*cb)) {
        (*cb)(engine);
    }
}

void HotReload(Engine& engine, float, FrameContext&) {
    engine.GetFileSystemWatcher().DispatchEvents();
}

void Physics(Engine& engine, float dt, FrameContext&) {
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

void UpdateGraph(Engine& engine, float dt, FrameContext&) {
    // Before Execute, and on this thread: the scratch arenas are per-worker and
    // this is the last point at which no worker holds a pointer into one.
    engine.ResetWorkerScratch();
    const Frame frame = engine.MakeFrame(dt);
    engine.GetUpdateGraph().Execute(frame);
}

void CommandPlayback(Engine& engine, float, FrameContext&) {
    engine.GetMainECB().Playback();
}

void CleanupScene(Engine& engine, float, FrameContext&) {
    engine.ProcessPendingDestroy();
}

void Camera(Engine& engine, float dt, FrameContext&) {
    // No instance and no function-local static: the pass is stateless, and the
    // camera it projects is component data.
    CameraSystem::Update(engine, dt, engine.GetCurrentAlpha());
}

void LOD(Engine&, float, FrameContext&) {
    // Preserved as a named Phase::Camera anchor for external modules; actual LOD
    // selection runs inside RenderGraph (LODSystem::GraphUpdate) in parallel with
    // DecalSystem and LightingSystem ahead of CullingSystem.
}

void RenderGraph(Engine& engine, float dt, FrameContext&) {
    engine.GetSceneData().Clear();
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

void TransformHistory(Engine& engine, float, FrameContext&) {
    ZHLN::ScopedTimer profTimer("ECS System: Update Transform History");
    TransformSystem::UpdateTransformHistory(engine.GetRegistry());
}

}} // namespace ::Steps

// Core's own steps, and the names modules anchor to.
//
// This runs once per engine, not once per scene: the plan a module composed into
// has to survive a scene rebuild, and clearing it here is what used to leave a
// module with no anchor and append its steps where they did not belong. The
// names below are the plan's public vocabulary -- a module placing itself
// relative to one of these is depending on this function, so a rename here is a
// breaking change to every module that anchored to it, reported as
// FramePlanError::AnchorMissing rather than absorbed.
void BuildCoreFramePlan(Engine& engine) {
    using Phase = FramePhase;
    auto& plan  = engine.GetFramePlan();

    // A core step that cannot be placed is a programming error in this function,
    // not a composition problem to return to a caller: the names are literals,
    // duplicated only by a careless edit here.
    const auto require = [](std::expected<void, FramePlanError> placed) -> void {
        if (!placed) {
            ZHLN::LogError("[Engine] core frame plan rejected a step: {}", ToString(placed.error()));
        }
    };

    require(plan.Append(Phase::UI, "HostUICallback", Steps::HostUICallback));
    require(plan.Append(Phase::HotReload, "ScriptAndShaderReload", Steps::HotReload));
    require(plan.Append(Phase::Physics, "PhysicsSystem", Steps::Physics));
    require(plan.Append(Phase::Gameplay, "GameplayModule", Steps::Gameplay));
    require(plan.Append(Phase::Simulation, "UpdateGraph", Steps::UpdateGraph));
    require(plan.Append(Phase::Simulation, "MainECBPlayback", Steps::CommandPlayback));
    require(plan.Append(Phase::Simulation, "SceneCleanup", Steps::CleanupScene));
    require(plan.Append(Phase::Camera, "CameraSystems", Steps::Camera));
    require(plan.Append(Phase::Camera, "LODSystem", Steps::LOD));
    require(plan.Append(Phase::Visibility, "RenderGraph", Steps::RenderGraph));
    require(plan.Append(Phase::Present, "RenderSystem", Steps::Present));
    require(plan.Append(Phase::History, "TransformHistory", Steps::TransformHistory));
}

void BuildSystemGraphs(Engine& engine) {
    auto& updateGraph = engine.GetUpdateGraph();
    auto& renderGraph = engine.GetRenderGraph();

    updateGraph.Clear();
    renderGraph.Clear();

    // Every graph node derives its name, thunk and component hazards from its
    // callable's signature. The camera is modified in the earlier frame phase.
    updateGraph.AddSystem<&VisualInterpolationSystem::Update>();
    updateGraph.AddSystem<&ArticulationSystem::Update>();
    updateGraph.AddSystem<&TransformSystem::Update>();
    updateGraph.AddSystem<&AudioSystem>();
    updateGraph.AddSystem<&EnvironmentSunSystem::Update>();

    renderGraph.DeclareExternalWrites("ExternalPreRenderWrites", {ECS::Write<Components::CameraComponent>()});
    renderGraph.AddSystem<&LODSystem::GraphUpdate>();
    renderGraph.AddSystem<&DecalSystem::Update>();
    renderGraph.AddSystem<&LightingSystem::Update>();
    renderGraph.AddSystem<&CullingSystem::GraphUpdate>();

    // Every module the application installed contributes its nodes here, in
    // install order, and each one's signature-derived hazards are what order
    // them against core's. Nothing a module declared survives into this loop as
    // a callback the engine did not ask for: the modules themselves are the
    // record, and GetInstalledModules is what this reads.
    for (const Module& module: engine.GetInstalledModules()) {
        if (module.addGraphSystems != nullptr) {
            module.addGraphSystems(updateGraph, renderGraph);
        }
    }

    updateGraph.Compile();
    renderGraph.Compile();
}

auto InitializeDefaultScene(Engine& engine) -> bool {
    auto& reg = engine.GetRegistry();

    reg.RegisterAllComponentsIn<ZHLN::Components>();

    reg.Create(Components::MainCameraTagComponent {}, Components::CameraComponent {}, Components::AASettingsComponent {});

    // A scene reset clears the registry, so the engine-level singletons World::Create()
    // seeds are re-seeded here alongside the rest of the scene's settings.
    reg.Create(
        Components::GlobalSettingsTagComponent {}, Components::PostProcessSettingsComponent {}, Components::ShadowSettingsComponent {},
        Components::CullingStatsComponent {}, Components::DebugSettingsComponent {.physicsDrawMode = 0}
    );

    reg.Create(GUI::UISettingsComponent {});

    engine.SeedSceneFontAtlas(reg);

    // The graphs are rebuilt per scene; the frame plan is not. A scene reset
    // clears the registry and rebuilds the graphs, and both are scene state --
    // the timeline is composition state, installed once by the application, and
    // rebuilding it per scene was how a replayed module step could land twice.
    BuildSystemGraphs(engine);
    return true;
}

} // namespace ZHLN
