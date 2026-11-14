// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Zahlen/Engine/Capabilities/DebugDraw.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
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
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/Engine/Frame.hpp>
#include <Zahlen/Engine/FramePlan.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Window/PlatformHost.hpp>
#include <Zahlen/Core/Profiler.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Window/Window.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <string_view>

namespace ZHLN {
namespace { namespace Steps {

// The host's UI callback. What runs immediately after it in the same phase --
// releasing a capture the host did not ask for, then re-evaluating the action
// maps -- reaches the engine-private input router, which this translation unit
// cannot see. That half is `uiInputStep`, defined in Engine.cpp and placed here
// by pointer, exactly like presentStep: a FrameStepFn carries no userdata, so a
// step that needs engine-private state has to be written where that state is
// visible.
void HostUICallback(Engine& engine, float, FrameContext&) {
    if (const auto cb = engine.GetUICallback(); cb && static_cast<bool>(*cb)) {
        (*cb)(engine);
    }
}

void HotReload(Engine& engine, float, FrameContext&) {
    engine.GetFileSystemWatcher().DispatchEvents();
}

void Physics(Engine& engine, float dt, FrameContext& ctx) {
    const auto& character = engine.GetPhysicsSubstepHooks();
    ZHLN::ScopedTimer profTimer("ECS System: Physics & Movement");
    for (uint32_t step = 0; step < ctx.fixedSteps; ++step) {
        if (character.preStep != nullptr) {
            character.preStep(engine, kFixedStepDt);
        }
        // A private buffer is played here, not at simulation-graph or main
        // buffer playback: hooks in the next substep must see consumed commands.
        engine.GetUpdateGraph().InvokeSequentialAndPlayback<&PhysicsSystem::CommitImpulses>(engine.MakeFrame(dt, ctx.alpha));
        engine.Physics().Step(kFixedStepDt);
        if (character.postStep != nullptr) {
            character.postStep(engine);
        }
    }
}

void UpdateGraph(Engine& engine, float dt, FrameContext& ctx) {
    // Before Execute, and on this thread: the scratch arenas are per-worker and
    // this is the last point at which no worker holds a pointer into one.
    engine.ResetWorkerScratch();
    const Frame frame = engine.MakeFrame(dt, ctx.alpha);
    engine.GetUpdateGraph().Execute(frame);
}

void CommandPlayback(Engine& engine, float, FrameContext&) {
    engine.Commands().Playback();
}

void CleanupScene(Engine& engine, float, FrameContext&) {
    engine.ProcessPendingDestroy();
}

void Camera(Engine& engine, float dt, FrameContext& ctx) {
    // No instance and no function-local static: the pass is stateless, and the
    // camera it projects is component data.
    engine.GetRenderGraph().InvokeSequential<&CameraSystem::Update>(engine.MakeFrame(dt, ctx.alpha));
}

void LOD(Engine&, float, FrameContext&) {
    // Preserved as a named Phase::Camera anchor for external modules; actual LOD
    // selection runs inside RenderGraph (LODSystem::GraphUpdate) in parallel with
    // DecalSystem and LightingSystem ahead of CullingSystem.
}

void RenderGraph(Engine& engine, float dt, FrameContext& ctx) {
    engine.Debug().Clear();
    engine.ResetWorkerScratch();
    const Frame frame = engine.MakeFrame(dt, ctx.alpha);
    engine.GetRenderGraph().Execute(frame);
}

void TransformHistory(Engine& engine, float dt, FrameContext& ctx) {
    ZHLN::ScopedTimer profTimer("ECS System: Update Transform History");
    engine.GetUpdateGraph().InvokeSequential<&TransformSystem::UpdateTransformHistory>(engine.MakeFrame(dt, ctx.alpha));
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
void BuildCoreFramePlan(FramePlan& plan, FrameStepFn presentStep, FrameStepFn uiInputStep) {
    using Phase = FramePhase;

    // A core step that cannot be placed is a programming error in this function,
    // not a composition problem to return to a caller: the names are literals,
    // duplicated only by a careless edit here.
    const auto require = [](std::expected<void, FramePlanError> placed) -> void {
        if (!placed) {
            ZHLN::LogError("[Engine] core frame plan rejected a step: {}", ToString(placed.error()));
        }
    };

    // HostUICallback is the step's name and stays the UI callback, because
    // modules anchor to it. The input half of the same phase runs beside it as
    // its own step, so the name keeps meaning what it always did and the input
    // evaluation has a name a module can anchor to as well.
    require(plan.Append(Phase::UI, "HostUICallback", Steps::HostUICallback));
    require(plan.Append(Phase::UI, "EvaluateInput", uiInputStep));
    require(plan.Append(Phase::HotReload, "StateAndShaderReload", Steps::HotReload));
    require(plan.Append(Phase::Physics, "PhysicsSystem", Steps::Physics));
    require(plan.Append(Phase::Simulation, "UpdateGraph", Steps::UpdateGraph));
    require(plan.Append(Phase::Simulation, "MainECBPlayback", Steps::CommandPlayback));
    require(plan.Append(Phase::Simulation, "SceneCleanup", Steps::CleanupScene));
    require(plan.Append(Phase::Camera, "CameraSystems", Steps::Camera));
    require(plan.Append(Phase::Camera, "LODSystem", Steps::LOD));
    require(plan.Append(Phase::Visibility, "RenderGraph", Steps::RenderGraph));
    require(plan.Append(Phase::Present, "RenderSystem", presentStep));
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

void SeedBaselineScene(ECS::Registry& reg) {
    if (reg.SingletonEntity<Components::MainCameraTagComponent>() == Entity::Null()) {
        reg.Create(Components::MainCameraTagComponent {}, Components::CameraComponent {});
    }
}

} // namespace ZHLN
