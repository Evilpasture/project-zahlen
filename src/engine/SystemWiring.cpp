// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SystemWiring.hpp"

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
#include "TextureSystem.hpp"
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
#include <Zahlen/SystemContext.hpp>
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

void Sys_VisualInterpolation(SystemContext& ctx) {
    VisualInterpolationSystem::Update(ctx);
}

void Sys_Animation(SystemContext& ctx) {
    static AnimationSystem sys;
    sys.UpdateAnimations(*ctx.render, ctx.registry, ctx.dt, ctx.bonePosePostProcessor);
}

void Sys_Articulation(SystemContext& ctx) {
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


namespace Steps {


void HostUICallback(Engine& engine, float , FrameContext& ) {
    if (const auto* cb = engine.GetUICallback(); cb != nullptr && static_cast<bool>(*cb)) {
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
    SystemContext sysCtx = engine.MakeSystemContext(dt);
    engine.GetUpdateGraph().Execute(sysCtx);
}

void CommandPlayback(Engine& engine, float , FrameContext& ) {
    engine.GetMainECB().Playback();
}

void Camera(Engine& engine, float dt, FrameContext& ) {
    static CameraSystem camSys;
    camSys.Update(engine, dt, engine.GetCurrentAlpha());
}

void LOD(Engine& engine, float , FrameContext& ) {
    LODSystem::Update(engine);
}

void RenderGraph(Engine& engine, float dt, FrameContext& ) {
    SystemContext sysCtx = engine.MakeSystemContext(dt);
    engine.GetRenderGraph().Execute(sysCtx);
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
    static TransformSystem transformSystem;
    transformSystem.UpdateTransformHistory(engine.GetRegistry());
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

    using namespace ZHLN::ECS;


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


    updateGraph.AddSystem({
        .update_func    = Sys_Audio,
        .name           = "AudioSystem",
        .access_pattern = {Read<Components::PhysicsComponent>(), Write<Components::AudioSourceComponent>()},
        .enabled        = true,
    });


    updateGraph.AddSystem({
        .update_func    = Sys_Particle,
        .name           = "ParticleSystem",
        .access_pattern = {Write<Components::ParticleEmitterComponent>()},
        .enabled        = true,
    });



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

    engine.ApplySystemGraphsExtensions(updateGraph, renderGraph);

    updateGraph.Compile();
    renderGraph.Compile();
}

auto InitializeDefaultScene(Engine& engine) -> bool {
    auto& reg = engine.GetRegistry();

    reg.RegisterAllComponentsIn<ZHLN::Components>();

    reg.Create(
        Components::MainCameraTagComponent {}, Components::CameraComponent {},
        Components::AASettingsComponent {.state = {.mode = AAMode::TAA, .taaFeedback = 0.95f}}, Components::FreeCamTagComponent {}
    );

    reg.Create(
        Components::GlobalSettingsTagComponent {}, Components::PostProcessSettingsComponent {}, Components::ShadowSettingsComponent {},
        Components::DebugSettingsComponent {.physicsDrawMode = 0}
    );

    reg.Create(GUI::UISettingsComponent {});

    engine.SeedSceneFontAtlas(reg);

    BuildSystemGraphs(engine);
    BuildFrameScheduler(engine);
    return true;
}

}
