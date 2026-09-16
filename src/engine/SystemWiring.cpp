// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/SystemWiring.cpp
#include "SystemWiring.hpp"

#include "DefaultPreset.hpp"
#include "LODSystem.hpp"
#include "NativeScriptModule.hpp"
#include "AnimationSystem.hpp"
#include "ArticulationSystem.hpp"
#include "CameraSystem.hpp"
#include "CullingSystem.hpp"
#include "DecalSystem.hpp"
#include "LightingSystem.hpp"
#include "ParticleSystem.hpp"
#include "PhysicsStateSystem.hpp"
#include "PhysicsSystem.hpp"
#include "RenderSystem.hpp"
#include "TargetCameraSystem.hpp"
#include "TextureSystem.hpp"
#include "TransformSystem.hpp"
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/FileSystemWatcher.hpp>
#include <Zahlen/FrameScheduler.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/SystemContext.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <algorithm>
#include <filesystem>
#include <format>
#include <string_view>

namespace ZHLN {
namespace {

void Sys_VisualInterpolation(SystemContext& ctx) {
    VisualInterpolationSystem::Update(ctx);
}

void Sys_Animation(SystemContext& ctx) {
    static AnimationSystem sys;
    sys.UpdateAnimations(*ctx.render, ctx.registry, ctx.dt);
}

void Sys_Articulation(SystemContext& ctx) {
    // Must run on the World's instance, not a node-local one: its tracking
    // ledger is the shared state DespawnEntity's Release() drains.
    ctx.articulation->Update(ctx, ctx.dt);
}

void Sys_Transform(SystemContext& ctx) {
    static TransformSystem sys;
    sys.ResolveTransforms(ctx.registry);
}

void Sys_Audio(SystemContext& ctx) {
    AudioSystem(ctx, ctx.dt);
}

void Sys_Culling(SystemContext& ctx) {
    ctx.culling->Update<false>(ctx, *ctx.visibleEntities, *ctx.visibleShadowEntities);
}

void Sys_Lighting(SystemContext& ctx) {
    static LightingSystem sys;
    sys.Update(ctx, ctx.dt);
}

void Sys_Particle(SystemContext& ctx) {
    static ParticleSystem sys;
    sys.Update(ctx, ctx.dt);
}

// ============================================================================
// FRAME PHASE STEPS
//
// Each function is one ordered unit of work in the frame. The two SystemGraphs
// are steps like any other, so hazard analysis only ever orders systems *inside*
// a graph -- never the phases around them, which run in fixed registration
// order. Adding a system means adding a step here, not editing Engine::Tick.
// ============================================================================

namespace Steps {

// The Input phase step (raw device state -> per-entity InputComponent) and
// the PlayerIntent step (camera-relative intent -> MovementComponent) moved
// to extras/CharacterController with the components they translate; that
// module re-inserts both through the FrameSchedulerExtension seam at their
// original positions.

void HostUICallback(Engine& engine, float /*dt*/, FrameContext& /*ctx*/) {
    if (const auto* cb = engine.GetUICallback(); cb != nullptr && static_cast<bool>(*cb)) {
        (*cb)(engine);
    }
}

void HotReload(Engine& engine, float /*dt*/, FrameContext& /*ctx*/) {
    // All background discovery has already settled into the service queue.
    // This is the sole callback dispatch point, before gameplay and rendering.
    engine.GetFileSystemWatcher().DispatchEvents();
}

void Physics(Engine& engine, float dt, FrameContext& /*ctx*/) {
    static PhysicsSystem physicsSystem;
    physicsSystem.Update(engine, dt);
}

void Gameplay(Engine& engine, float dt, FrameContext& ctx) {
    switch (ctx.driver) {
        using enum GameplayDriver;
        case Cpp: {
            ZHLN::ScopedTimer profTimer("ECS System: Native C++ Gameplay Update");
            ctx.status = engine.UpdateNativeGameplay(dt);
            break;
        }
        case Fennel: {
            ZHLN::ScopedTimer profTimer("ECS System: Script/Lua Update");
            engine.GetScriptRunner().CallUpdate(&engine, dt);
            break;
        }
        case Hybrid: {
            {
                ZHLN::ScopedTimer profTimer("ECS System: Native C++ Gameplay Update");
                ctx.status = engine.UpdateNativeGameplay(dt);
            }
            {
                ZHLN::ScopedTimer profTimer("ECS System: Script/Lua Update");
                engine.GetScriptRunner().CallUpdate(&engine, dt);
            }
            break;
        }
    }
}

void UpdateGraph(Engine& engine, float dt, FrameContext& /*ctx*/) {
    SystemContext sysCtx = engine.MakeSystemContext(dt);
    engine.GetUpdateGraph().Execute(sysCtx);
}

void CommandPlayback(Engine& engine, float /*dt*/, FrameContext& /*ctx*/) {
    engine.GetMainECB().Playback();
}

/// Resolve target cameras and camera matrices from current physics and
/// procedural rig poses immediately before visibility/render work.
void Camera(Engine& engine, float dt, FrameContext& /*ctx*/) {
    static TargetCameraSystem targetCamSys;
    static CameraSystem       camSys;
    targetCamSys.Update(engine, dt, engine.GetCurrentAlpha());
    camSys.Update(engine, dt, engine.GetCurrentAlpha());
}

void LOD(Engine& engine, float /*dt*/, FrameContext& /*ctx*/) {
    LODSystem::Update(engine);
}

void RenderGraph(Engine& engine, float dt, FrameContext& /*ctx*/) {
    SystemContext sysCtx = engine.MakeSystemContext(dt);
    engine.GetRenderGraph().Execute(sysCtx);
}

void Present(Engine& engine, float dt, FrameContext& ctx) {
    auto render_res = RenderSystem::Update(engine, dt);
    if (!render_res) {
        if (render_res.error().Is<RenderFrameResult>() && render_res.error().As<RenderFrameResult>() == RenderFrameResult::DeviceLost) {
            // HandleDeviceLost tears the RenderContext down before rebuilding
            // it. If the rebuild fails the engine has no context at all, and
            // the next Present would dereference null; report it as a fatal
            // frame status and close the window so the host loop exits.
            if (auto lost_res = engine.HandleDeviceLost(); !lost_res) {
                ZHLN::Log("[Engine] Fatal: GPU device recovery failed: {}", lost_res.error().Message());
                ctx.status = GameplayStatus::Error;
                engine.GetWindow().Close();
            }
            ctx.deviceLost = true;
        }
    }
}

/// Auto-detect missing gameplay scripts / modules and engage the Fallback Preset.
void Fallback(Engine& engine, float dt, FrameContext& ctx) {
    if (!engine.FallbackSceneEnabled()) {
        return;
    }

    if (!DefaultPreset::IsActive()) {
        // The runtime declares its own boot entry points; core only asks whether
        // any of them exist, so no scripting language is named here.
        //
        // An empty list means no runtime is installed, which is not a reason to
        // stand down: the Fennel driver still has nothing to run, and the
        // fallback scene is the only thing that puts anything on screen. Without
        // it a plain `zahlen` with no flags renders an empty world -- the camera
        // and system graphs from InitializeDefaultScene have no geometry.
        const auto bootPaths       = engine.GetScriptRunner().BootScriptPaths();
        const bool scriptingDriver = ctx.driver == GameplayDriver::Fennel || ctx.driver == GameplayDriver::Hybrid;
        const bool hasBootScript   = std::ranges::any_of(bootPaths, [](const std::string_view p) { return std::filesystem::exists(std::filesystem::path(p)); });
        if (scriptingDriver && !hasBootScript) {
            if (bootPaths.empty()) {
                DefaultPreset::BuildFallbackScene(
                    engine, FallbackReason::MissingBootScript, "No scripting runtime is installed, so no boot script could run."
                );
            } else {
                DefaultPreset::BuildFallbackScene(
                    engine, FallbackReason::MissingBootScript, std::format("Script '{}' was not found in working directory.", bootPaths.front())
                );
            }
        } else if (ctx.driver == GameplayDriver::Cpp && !engine.IsNativeGameplayLoaded()) {
            DefaultPreset::BuildFallbackScene(
                engine, FallbackReason::MissingNativeModule, "Native gameplay module (libgameplay.so / gameplay.dll) was not found."
            );
        }
    }

    if (DefaultPreset::IsActive()) {
        DefaultPreset::Update(engine, dt);
    }
}

void TransformHistory(Engine& engine, float /*dt*/, FrameContext& /*ctx*/) {
    ZHLN::ScopedTimer      profTimer("ECS System: Update Transform History");
    static TransformSystem transformSystem;
    transformSystem.UpdateTransformHistory(engine.GetRegistry());
}

} // namespace Steps

} // namespace

void BuildFrameScheduler(Engine& engine) {
    using Phase     = FramePhase;
    auto& scheduler = engine.GetFrameScheduler();

    scheduler.Clear();
    // The Input and PlayerIntent phase steps moved to extras/CharacterController
    // with the components they translate; that module re-inserts them through
    // the FrameSchedulerExtension seam (before HostUICallback and after
    // ScriptAndShaderReload, their original positions).
    scheduler.Add(Phase::UI, "HostUICallback", Steps::HostUICallback);
    scheduler.Add(Phase::HotReload, "ScriptAndShaderReload", Steps::HotReload);
    scheduler.Add(Phase::Physics, "PhysicsSystem", Steps::Physics);
    scheduler.Add(Phase::Gameplay, "GameplayModule", Steps::Gameplay);
    scheduler.Add(Phase::Fallback, "DefaultPreset", Steps::Fallback);
    scheduler.Add(Phase::Simulation, "UpdateGraph", Steps::UpdateGraph);
    scheduler.Add(Phase::Simulation, "MainECBPlayback", Steps::CommandPlayback);
    scheduler.Add(Phase::Camera, "CameraSystems", Steps::Camera);
    scheduler.Add(Phase::Camera, "LODSystem", Steps::LOD);
    scheduler.Add(Phase::Visibility, "RenderGraph", Steps::RenderGraph);
    scheduler.Add(Phase::Present, "RenderSystem", Steps::Present);
    scheduler.Add(Phase::History, "TransformHistory", Steps::TransformHistory);

    // Optional layers contribute their phase steps on every (re)build, so a
    // scene reset never strands a host that installed an extras module.
    // InsertAfter positioning is the extension's own business; the core steps
    // above are the anchors.
    engine.ApplyFrameSchedulerExtensions(scheduler);
}

void BuildSystemGraphs(Engine& engine) {
    auto& updateGraph = engine.GetUpdateGraph();
    auto& renderGraph = engine.GetRenderGraph();

    // Rebuild, never append. InitializeDefaultScene is called again whenever a
    // scene is reset on a live engine (the GPU test pool does exactly that),
    // and without this the graphs accumulate a second, third, ... copy of every
    // system. Duplicates are not merely slow: Compile() only orders nodes that
    // conflict, so a system with a read-only or empty access pattern --
    // TextureSystem, CullingSystem, DecalSystem -- has no edge to its own
    // duplicate and the copies are dispatched to run *concurrently* over the
    // same engine state. That is a data race on whatever they fill in, and it
    // shows up much later as a corrupted allocator heap.
    // BuildFrameScheduler has always cleared for the same reason.
    updateGraph.Clear();
    renderGraph.Clear();

    using namespace ZHLN::ECS;

    // Character locomotion (MovementComponent) used to be anchored here as an
    // external write for VisualInterpolationSystem's yaw read; both moved to
    // extras/CharacterController, which contributes its own external-writes
    // anchor and systems through the engine's SystemGraphsExtension seam.
    // Pose interpolation reads PhysicsWorld SoA under one lock; there is no
    // PhysicsStateComponent to declare. Authored scene data with no per-frame
    // writer (HierarchyComponent, SkeletalMeshComponent, PhysicsComponent, ...)
    // is deliberately not declared.

    updateGraph.AddSystem({
        .update_func    = [](SystemContext& ctx) -> void { TextureSystem::Update(ctx, ctx.dt); },
        .name           = "TextureSystem",
        .access_pattern = {},
        .enabled        = true,
    });

    updateGraph.AddSystem({
        .update_func    = Sys_VisualInterpolation,
        .name           = "VisualInterpolationSystem",
        .access_pattern = {Read<Components::PhysicsComponent>(), Write<Components::TransformComponent>()},
        .enabled        = true,
    });

    updateGraph.AddSystem({
        .update_func = Sys_Animation,
        .name        = "AnimationSystem",
        // MovementComponent left this pattern when character locomotion moved
        // to extras/CharacterController; AnimationSystem never read it in its
        // body (the entry was an ordering anchor only).
        .access_pattern = {Read<Components::SkeletalMeshComponent>(), Write<Components::TransformComponent>(), Write<Components::MorphTargetComponent>()},
        .enabled        = true,
    });

    updateGraph.AddSystem({
        .update_func = Sys_Articulation,
        .name        = "ArticulationSystem",
        .access_pattern =
            {
                Read<Components::PhysicsComponent>(),
                Read<Components::MeshComponent>(),
                Read<Components::KinematicPoseOverrideComponent>(),
                Write<Components::RagdollComponent>(),
                Write<Components::TransformComponent>(),
            },
        .enabled = true,
    });

    updateGraph.AddSystem({
        .update_func    = Sys_Transform,
        .name           = "TransformSystem",
        .access_pattern = {Read<Components::HierarchyComponent>(), Read<Components::TransformComponent>(), Write<Components::WorldTransformComponent>()},
        .enabled        = true,
    });

    // NOTE: the former PostProcessSystem bridge (ECS → SetGISettings) was
    // removed: RenderSystem::RenderMain now performs the single
    // ECS → GraphicsSettings → RenderContext::ApplySettings sync each frame
    // (see system/GraphicsSettingsSync.hpp).

    updateGraph.AddSystem({
        .update_func    = Sys_Audio,
        .name           = "AudioSystem",
        .access_pattern = {Read<Components::PhysicsComponent>(), Write<Components::AudioSourceComponent>()},
        .enabled        = true,
    });

    // InteractionSystem (trigger/pickup/container/usable) moved to
    // extras/Interaction. It re-registers itself through the engine's
    // SystemGraphsExtension seam (see Interaction::Install), which replays on
    // every graph rebuild, so it survives scene resets just like this wiring.

    updateGraph.AddSystem({
        .update_func    = Sys_Particle,
        .name           = "ParticleSystem",
        .access_pattern = {Write<Components::ParticleEmitterComponent>()},
        .enabled        = true,
    });

    // Terrain moved to extras/Terrain: its update-graph node is contributed
    // through the SystemGraphsExtension seam and appended here as well,
    // preserving its end-of-graph position.

    // Compilation is deferred until after the render graph's core systems and
    // the optional-layer extensions below are registered, so contributed nodes
    // take part in hazard analysis and AddSystemBefore anchoring for BOTH
    // graphs. Compile() only builds edges from earlier nodes to later ones --
    // an extension added after Compile() could never anchor before a core
    // system, which is exactly what e.g. an animation modifier needs.

    // CameraSystem (Camera phase) writes CameraComponent::prevUnjitteredViewProj
    // before this graph runs; CullingSystem reads CameraComponent. Same anchor
    // rationale as updateGraph above.
    //   CameraComponent      <- CameraSystem::Update (Camera phase).
    // TransformComponent / WorldTransformComponent are written by updateGraph,
    // not by an imperative phase, so they are cross-graph ordering rather than an
    // undeclared external write -- left to the phase order on purpose.
    renderGraph.DeclareExternalWrites(
        "ExternalPreRenderWrites", {
                                       Write<Components::CameraComponent>(),
                                   }
    );

    renderGraph.AddSystem({
        .update_func    = Sys_Culling,
        .name           = "CullingSystem",
        .access_pattern = {Read<Components::MeshComponent>(), Read<Components::WorldTransformComponent>(), Read<Components::CameraComponent>()},
        .enabled        = true,
    });

    renderGraph.AddSystem({
        .update_func    = [](SystemContext& ctx) -> void { DecalSystem::Update(ctx); },
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

    // Optional layers contribute graph nodes on every (re)build, before either
    // graph is compiled. See the note where updateGraph.Compile() was deferred.
    engine.ApplySystemGraphsExtensions(updateGraph, renderGraph);

    updateGraph.Compile();
    renderGraph.Compile();
}

} // namespace ZHLN
