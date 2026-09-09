// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/SystemWiring.cpp
#include "SystemWiring.hpp"

#include "DefaultPreset.hpp"
#include "EngineAccess.hpp"
#include "LODSystem.hpp"
#include "NativeScriptModule.hpp"
#include "AnimationSystem.hpp"
#include "ArticulationSystem.hpp"
#include "CameraSystem.hpp"
#include "CullingSystem.hpp"
#include "DecalSystem.hpp"
#include "InputSystem.hpp"
#include "InteractionSystem.hpp"
#include "LightingSystem.hpp"
#include "ParticleSystem.hpp"
#include "PhysicsStateSystem.hpp"
#include "PhysicsSystem.hpp"
#include "RenderSystem.hpp"
#include "TargetCameraSystem.hpp"
#include "TerrainSystem.hpp"
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
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <filesystem>

namespace ZHLN {
namespace {

void Sys_VisualInterpolation(Engine& engine, float /*dt*/) {
    VisualInterpolationSystem::Update(engine, engine.GetCurrentAlpha());
}

void Sys_Animation(Engine& engine, float dt) {
    static AnimationSystem sys;
    sys.UpdateAnimations(engine.GetRenderContext(), engine.GetRegistry(), dt);
}

void Sys_Articulation(Engine& engine, float dt) {
    engine.GetArticulationSystem().Update(engine, dt);
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

void Sys_Particle(Engine& engine, float dt) {
    static ParticleSystem sys;
    sys.Update(engine, dt);
}

void Sys_Terrain(Engine& engine, float dt) {
    static TerrainSystem sys;
    sys.Update(engine, dt);
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

void Input(Engine& engine, float /*dt*/, FrameContext& /*ctx*/) {
    static InputSystem inputSystem;
    inputSystem.Update(engine);
}

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

/// Translate gameplay input using the previous resolved camera. Camera
/// transforms are finalized after physics and the update graph so rig-driven
/// first-person views cannot lag one simulation frame behind their body.
void PlayerIntent(Engine& engine, float /*dt*/, FrameContext& /*ctx*/) {
    static InputSystem inputSystem;
    inputSystem.PlayerInputTranslate(engine, engine.GetCamera());
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
            ctx.status = EngineFrameStepAccess::NativeGameplayModule(engine).Update(&engine, dt);
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
                ctx.status = EngineFrameStepAccess::NativeGameplayModule(engine).Update(&engine, dt);
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
    engine.GetUpdateGraph().Execute(engine, dt);
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
    engine.GetRenderGraph().Execute(engine, dt);
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
    if (!EngineFrameStepAccess::Config(engine).enableFallbackScene) {
        return;
    }

    if (!DefaultPreset::IsActive()) {
        if ((ctx.driver == GameplayDriver::Fennel || ctx.driver == GameplayDriver::Hybrid) && !std::filesystem::exists("scripts/boot.lua") &&
            !std::filesystem::exists("scripts/boot.fnl")) {
            DefaultPreset::BuildFallbackScene(engine, FallbackReason::MissingBootScript, "Script 'scripts/boot.lua' was not found in working directory.");
        } else if (ctx.driver == GameplayDriver::Cpp && !EngineFrameStepAccess::NativeGameplayModule(engine).IsLoaded()) {
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
    scheduler.Add(Phase::Input, "InputSystem", Steps::Input);
    scheduler.Add(Phase::UI, "HostUICallback", Steps::HostUICallback);
    scheduler.Add(Phase::HotReload, "ScriptAndShaderReload", Steps::HotReload);
    scheduler.Add(Phase::PlayerIntent, "PlayerInputTranslate", Steps::PlayerIntent);
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

    // Components written by imperative frame phases that run before this graph
    // executes. No node inside the graph performs these writes, so without this
    // anchor hazard analysis would see VisualInterpolationSystem reading
    // PhysicsStateComponent and AnimationSystem/InteractionSystem reading
    // MovementComponent with no writer to order against, and build no edge.
    //   PhysicsStateComponent <- PhysicsStateSystem::WriteBack, called from the
    //                            Physics phase's fixed-step accumulator.
    //   MovementComponent     <- InputSystem::PlayerInputTranslate (PlayerIntent
    //                            phase) and MovementSystem (Physics phase).
    // Authored scene data with no per-frame writer (HierarchyComponent,
    // SkeletalMeshComponent, PhysicsComponent, ItemBaseComponent, UsableComponent,
    // KinematicPoseOverrideComponent) is deliberately not declared: there is no
    // write to anchor, and claiming one would misdescribe the frame.
    updateGraph.DeclareExternalWrites(
        "ExternalPreUpdateWrites", {
                                       Write<Components::PhysicsStateComponent>(),
                                       Write<Components::MovementComponent>(),
                                   }
    );

    updateGraph.AddSystem({
        .update_func    = [](Engine& eng, float dt) -> void { TextureSystem::Update(eng, dt); },
        .name           = "TextureSystem",
        .access_pattern = {},
        .enabled        = true,
    });

    updateGraph.AddSystem({
        .update_func    = Sys_VisualInterpolation,
        .name           = "VisualInterpolationSystem",
        .access_pattern = {Read<Components::PhysicsStateComponent>(), Write<Components::TransformComponent>(), Write<Components::WorldTransformComponent>()},
        .enabled        = true,
    });

    updateGraph.AddSystem({
        .update_func = Sys_Animation,
        .name        = "AnimationSystem",
        .access_pattern =
            {Read<Components::MovementComponent>(), Read<Components::SkeletalMeshComponent>(), Write<Components::TransformComponent>(),
             Write<Components::MorphTargetComponent>()},
        .enabled = true,
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

    updateGraph.AddSystem({
        .update_func = [](Engine& eng, float dt) -> void {
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
        .update_func    = [](Engine& eng, float /*dt*/) -> void { DecalSystem::Update(eng); },
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

} // namespace ZHLN
