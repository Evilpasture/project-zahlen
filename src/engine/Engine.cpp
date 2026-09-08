// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Engine.cpp
#include <GLFW/glfw3.h>
#include <algorithm>
#include <iterator>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
// clang-format on
#include "tty/TTYBackend.hpp"
#include "LODSystem.hpp"
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/FrameScheduler.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Profiler.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <Zahlen/FileSystemWatcher.hpp>
#include "NativeScriptModule.hpp"
#include "Platform.hpp"
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
#include <filesystem>
#include <renderdoc_app.h>
#ifdef __linux__
#include <dlfcn.h>
#endif

#include <Zahlen/Threading/Thread.hpp>

namespace ZHLN {

static RENDERDOC_API_1_5_0* s_RDocAPI = nullptr;

static void InitRenderDocAPI() {
#if defined(_WIN32)
    if (HMODULE mod = GetModuleHandleA("renderdoc.dll")) {
        pRENDERDOC_GetAPI R_GetAPI = (pRENDERDOC_GetAPI) GetProcAddress(mod, "RENDERDOC_GetAPI");
        if (R_GetAPI) {
            R_GetAPI(eRENDERDOC_API_Version_1_5_0, (void**) &s_RDocAPI);
        }
    }
#elif defined(__linux__)
    if (void* mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD)) {
        auto R_GetAPI = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(mod, "RENDERDOC_GetAPI"));
        if (R_GetAPI != nullptr) {
            R_GetAPI(eRENDERDOC_API_Version_1_5_0, reinterpret_cast<void**>(&s_RDocAPI));
        }
    }
#endif
    if (s_RDocAPI != nullptr) {
        ZHLN::Log("[RenderDoc] In-App API successfully bound.");
    }
}

namespace CreativeWorksFactory {

}

struct EngineImpl {
    // Declared first so it outlives every callback-owning client during normal
    // and partial-initialization teardown.
    std::unique_ptr<FileSystemWatcher>    fileSystemWatcher;
    std::unique_ptr<Window>               window;
    std::unique_ptr<RenderContext>        renderContext;
    std::unique_ptr<PhysicsContext>       physicsContext;
    std::unique_ptr<AudioContext>         audioContext;
    std::unique_ptr<CreativeWorksManager> assetManager;
    std::unique_ptr<ScriptRunner>         scriptRunner;
    std::unique_ptr<NativeScriptModule>   nativeScriptModule;
    FileWatchHandle                        bootLuaWatch = 0;
    FileWatchHandle                        bootFennelWatch = 0;
    GameplayDriver                         activeGameplayDriver = GameplayDriver::Cpp;

    Engine::UICallback                      uiCallback = nullptr;
    std::vector<Engine::DeviceLostCallback> deviceLostCallbacks;

    Camera        mainCamera;
    ECS::Registry registry;

    FrameScheduler                            scheduler;
    std::unique_ptr<ECS::SystemGraph>         updateGraph;
    std::unique_ptr<ECS::SystemGraph>         renderGraph;
    std::unique_ptr<ECS::EntityCommandBuffer> mainECB;
    std::unique_ptr<CullingSystem>            cullingSystem;
    std::unique_ptr<ArticulationSystem>        articulationSystem;
    JPH::Array<Entity>                        visibleEntities;
    JPH::Array<Entity>                        visibleShadowEntities;
    float                                     currentAlpha = 0.0f;

    // Built once per engine, not once per scene: the glyph packing costs
    // 96 SDF rasterisations, and the upload burns a 1024x1024 bindless texture
    // that nothing ever releases. The scene owns a *copy* in UISettingsComponent,
    // which Registry::Clear() throws away, so the engine keeps the authoritative
    // one and re-seeds each new scene from it. See InitializeDefaultScene.
    std::optional<FontAtlas> fontAtlas;

    void*    gameState    = nullptr;
    uint64_t frameCounter = 0;
    bool     joltAcquired = false;
    EngineConfig config;
};

// Frame steps are free functions to keep FrameScheduler's ABI simple. This
// narrow friend keeps the engine-owned NativeScriptModule private while letting
// only those steps access its lifecycle-owned instance.
class EngineFrameStepAccess {
  public:
    [[nodiscard]] static auto NativeGameplayModule(Engine& engine) -> NativeScriptModule& {
        return *engine._impl->nativeScriptModule;
    }
};

// --- SYSTEM GRAPH HELPERS ---
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

/// The frame, in order. Phase names are documentation: steps run strictly in
/// registration order regardless of the phase they are tagged with.
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

} // namespace

Engine::Engine(): _impl(nullptr) {
}

Engine::Engine(const EngineConfig& cfg): _impl(nullptr) {
    auto res = InitInternal(cfg);
    if (!res) {
        ZHLN::Panic("FATAL: Failed to initialize Engine via legacy constructor: {}", res.error().Message());
    }
}

auto Engine::HandleDeviceLost() noexcept -> std::expected<void, Error> {
    _impl->renderContext->OnDeviceLost();
    _impl->renderContext.reset();

    auto rc_res = RenderContext::Create(*_impl->window, _impl->config.render, _impl->fileSystemWatcher.get());
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());
    CreativeWorksFactory::RebuildVulkanResources(*_impl->renderContext, _impl->registry);

    // Core has rebuilt everything it owns. Owners outside the engine now
    // re-upload against the new context, in the order they registered.
    for (const auto& callback: _impl->deviceLostCallbacks) {
        if (callback) {
            callback(*this);
        }
    }
    return {};
}

Engine::Engine(const EngineConfig& cfg, bool& outSuccess): _impl(nullptr) {
    auto res   = InitInternal(cfg);
    outSuccess = res.has_value();
    if (!res) {
        ZHLN::Log("Engine initialization failed: {}", res.error().Message());
    }
}

auto Engine::Create(const EngineConfig& cfg) -> std::expected<std::unique_ptr<Engine>, Error> {
    auto instance = std::unique_ptr<Engine>(new (std::nothrow) Engine());
    if (!instance) {
        return std::unexpected(EngineInitError::EngineAllocationFailed);
    }

    if (auto result = instance->InitInternal(cfg); !result) {
        return std::unexpected(result.error());
    }
    return std::move(instance);
}

// --- PROCESS-GLOBAL JOLT REGISTRATION ---
//
// JPH::Factory::sInstance and the registered type list are process state, not
// engine state. Acquisition was already guarded, but release was not: the first
// engine destroyed called JPH::UnregisterTypes() and deleted the factory out
// from under every other engine in the process. That is one of the things that
// made a second engine unusable, and it blocks running more than one physics
// world. Refcounted: first in registers, last out unregisters.
namespace {

std::mutex s_JoltRegistrationMutex;
uint32_t   s_JoltRegistrations = 0;

void AcquireJoltRegistration() {
    const std::lock_guard lock(s_JoltRegistrationMutex);
    if (s_JoltRegistrations++ > 0) {
        return;
    }

    JPH::RegisterDefaultAllocator();
    JPH::Trace = JoltTraceBridge;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = JoltAssertBridge;
#endif

    if (JPH::Factory::sInstance == nullptr) {
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
}

void ReleaseJoltRegistration() {
    const std::lock_guard lock(s_JoltRegistrationMutex);
    if (s_JoltRegistrations == 0 || --s_JoltRegistrations > 0) {
        return;
    }

    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

} // namespace

auto Engine::InitInternal(const EngineConfig& cfg) -> std::expected<void, Error> {
    ZHLN::Fiber::InitMainThread();

    _impl                       = std::make_unique<EngineImpl>();
    _impl->config               = cfg;
    _impl->fileSystemWatcher    = std::make_unique<FileSystemWatcher>();
    _impl->scriptRunner         = std::make_unique<ScriptRunner>();

    bool use_tty = false;

    if (cfg.render.headless) {
        // True headless mode: skip GLFW entirely. No display server is required.
        ZHLN::Log("[Engine] Headless mode enabled. Skipping GLFW initialization.");
    } else {
        glfwSetErrorCallback([](int error, const char* description) -> void {
            ZHLN::Log("[GLFW Error] Code {}: {}", error, description ? description : "(null)");
        });

        if constexpr (isLinux) {
            // Detects both RenderDoc and NVIDIA Nsight Graphics (Nomad) launch environments
            if (std::getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE") != nullptr || std::getenv("NOMAD_VULKAN_LAYER") != nullptr ||
                std::getenv("NGFX_INJECTION") != nullptr) {
                glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
            }
        }

        if (!glfwInit()) {
            const char* desc = nullptr;
            int err = glfwGetError(&desc);
            if (desc != nullptr) {
                ZHLN::Log("[Engine] glfwInit failed: ({}) {}", err, desc);
            }
            if (TTYBackend::IsSupported()) {
                ZHLN::Log("GLFW failed to initialize. Falling back to native TTY Display Mode.");
                use_tty = true;
            } else {
                return std::unexpected(EngineInitError::WindowCreationFailed);
            }
        }
    }

    // Keys land in InputStateComponent twice over: held state in the bitset for
    // gameplay, and a queue of presses plus typed characters for text fields.
    // The engine still does not interpret any of it as text -- it does not own a
    // GUI::Context (the caller does, see app/main.cpp:151), so it has no way to
    // know which field is focused. GUI::Context::BeginFrame drains the queue;
    // Context::PushKey/PushChar feed the same queue for hosts with no window.
    //
    // Every press is queued, not just the editing keys. Deciding which keys a
    // text field acts on is TextBuffer.hpp's business; this is only the pump.
    auto onKey = [](void* userdata, KeyCode key, bool pressed) -> void {
        auto* impl  = static_cast<EngineImpl*>(userdata);
        auto* reg   = &impl->registry;
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->SetKey(static_cast<uint8_t>(key), pressed);
        if (pressed) {
            state->QueueKeyPress(key);
        }
    };

    auto onMouseMove = [](void* userdata, float x, float y) -> void {
        auto* reg   = &static_cast<EngineImpl*>(userdata)->registry;
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->ApplyLocalMotion(x, y);
    };

    auto onMouseScroll = [](void* userdata, float delta) -> void {
        auto* reg   = &static_cast<EngineImpl*>(userdata)->registry;
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->ApplyWheel(delta);
    };

    auto onResize = [](void* userdata, Extent2D extent) -> void {
        auto* reg   = &static_cast<EngineImpl*>(userdata)->registry;
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->ApplyResize(extent);
    };

    auto onChar = [](void* userdata, unsigned int codepoint) -> void {
        auto* reg   = &static_cast<EngineImpl*>(userdata)->registry;
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->QueueChar(codepoint);
    };

    // userdata is the heap-allocated EngineImpl (stable for the engine's whole
    // life, unlike `this`), which owns the registry the callbacks write to.
    WindowInputReceiver receiver = {
        .userdata = _impl.get(), .onKey = onKey, .onMouseMove = onMouseMove, .onMouseScroll = onMouseScroll, .onResize = onResize,
        .onChar   = onChar
    };

    _impl->window =
        std::make_unique<Window>(cfg.render.appName.data(), cfg.render.width, cfg.render.height, cfg.render.fullscreen, receiver, use_tty, cfg.render.headless);

    // Singleton InputStateComponent must exist before the first event pump.
    _impl->registry.Create(Components::InputStateComponent {});

    if (use_tty && _impl->window->GetTTYContext() == nullptr) {
        return std::unexpected(EngineInitError::TTYInitializationFailed);
    }

    InitRenderDocAPI();

    AcquireJoltRegistration();
    _impl->joltAcquired = true;

    auto rc_res = RenderContext::Create(*_impl->window, cfg.render, _impl->fileSystemWatcher.get());
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());

    _impl->physicsContext = std::make_unique<PhysicsContext>(cfg.physics);
    _impl->audioContext   = std::make_unique<AudioContext>();
    _impl->assetManager   = std::make_unique<CreativeWorksManager>();
    _impl->nativeScriptModule = std::make_unique<NativeScriptModule>(*this, "scripts/gameplay");

    const auto reloadBootScript = [this](const FileWatchEvent& event) {
        if (_impl->activeGameplayDriver == GameplayDriver::Cpp || event.action == FileWatchAction::Deleted) {
            return;
        }
        _impl->scriptRunner->ReloadFile(event.path.string());
    };
    _impl->bootLuaWatch    = _impl->fileSystemWatcher->WatchFile("scripts/boot.lua", reloadBootScript);
    _impl->bootFennelWatch = _impl->fileSystemWatcher->WatchFile("scripts/boot.fnl", reloadBootScript);

    _impl->updateGraph   = std::make_unique<ECS::SystemGraph>();
    _impl->renderGraph   = std::make_unique<ECS::SystemGraph>();
    _impl->mainECB       = std::make_unique<ECS::EntityCommandBuffer>(_impl->registry);
    _impl->cullingSystem        = std::make_unique<CullingSystem>();
    _impl->articulationSystem   = std::make_unique<ArticulationSystem>();

    if (std::filesystem::exists("data/base.pak")) {
        _impl->assetManager->MountPak("data/base.pak");
    } else if (std::filesystem::exists("build/data/base.pak")) {
        _impl->assetManager->MountPak("build/data/base.pak");
    } else {
        ZHLN::Log("WARNING: Could not find 'data/base.pak' in working directory or build/ folder!");
    }

    return {};
}

Engine::~Engine() {
    // InitInternal can fail before _impl is built, and Engine::Create deletes a
    // half-built engine.
    if (_impl == nullptr) {
        return;
    }

    // The fallback preset parks entity handles in process-global storage. They
    // name entities in the registry that is about to be cleared, so they must
    // not survive into the next engine (see DefaultPreset::ReleaseFor).
    DefaultPreset::ReleaseFor(this);

    // Ragdolls retain Jolt resources outside the registry. Drain them while
    // both the components and PhysicsContext still exist. InitInternal may
    // fail before this system is created, so teardown must tolerate that path.
    if (_impl->articulationSystem != nullptr) {
        _impl->articulationSystem->Shutdown(*this);
    }
    _impl->registry.Clear();
    if (_impl->renderContext != nullptr) {
        _impl->renderContext->ReconcileEntityBuffers(_impl->registry.AliveQuery());
    }
    _impl->articulationSystem.reset();
    _impl->physicsContext.reset();
    _impl->renderContext.reset();
    _impl->nativeScriptModule.reset();
    _impl->fileSystemWatcher.reset();
    _impl->window.reset();
    _impl->assetManager.reset();
    _impl->audioContext.reset();
    _impl->scriptRunner.reset();
    _impl->updateGraph.reset();
    _impl->renderGraph.reset();
    _impl->mainECB.reset();
    _impl->cullingSystem.reset();

    // Process-global, and not refcounted the way the Jolt registration below
    // is: a second windowed engine would lose GLFW when the first one goes.
    // Headless engines never call glfwInit, so this does not constrain the
    // tests.
    if (!_impl->config.render.headless) {
        glfwTerminate();
    }

    if (_impl->joltAcquired) {
        ReleaseJoltRegistration();
    }
}

auto Engine::IsRunning() const -> bool {
    return _impl->window->IsRunning();
}

void Engine::ProcessEvents() {
    ZHLN::CheckForCrashes(this);

    auto&                            reg        = _impl->registry;
    Components::InputStateComponent* inputState = reg.GetSingleton<Components::InputStateComponent>();
    if (inputState != nullptr) {
        inputState->ResetDeltas();
    }

    if (_impl->window->IsHeadless()) {
        // True headless mode: no windowing event queue to poll.
        return;
    }

    if (_impl->window->IsTTY()) {
        // TTY path uses the same WindowInputReceiver callbacks as GLFW
        TTYBackend::ProcessEvents(_impl->window->GetTTYContext(), _impl->window->GetInputReceiver());
        if (inputState != nullptr) {
            inputState->wantCaptureKeyboard = false;
            inputState->wantCaptureMouse    = false;
        }
        return;
    }

    glfwPollEvents();

}

auto Engine::BeginFrame(bool& outDeviceLost) noexcept -> bool {
    outDeviceLost = false;
    auto res      = _impl->renderContext->BeginFrame();
    if (!res) {
        if (res.error() == RenderFrameResult::DeviceLost) {
            outDeviceLost = true;
            // Same contract as Steps::Present: a failed rebuild leaves no
            // RenderContext, so the window is closed to stop the host loop.
            if (auto lost_res = HandleDeviceLost(); !lost_res) {
                ZHLN::Log("[Engine] Fatal: GPU device recovery failed: {}", lost_res.error().Message());
                _impl->window->Close();
            }
        }
        return false;
    }
    return true;
}

auto Engine::EndFrame(bool& outDeviceLost) noexcept -> bool {
    outDeviceLost = false;
    auto res      = _impl->renderContext->EndFrame();
    if (!res) {
        if (res.error() == RenderFrameResult::DeviceLost) {
            outDeviceLost = true;
            if (auto lost_res = HandleDeviceLost(); !lost_res) {
                ZHLN::Log("[Engine] Fatal: GPU device recovery failed: {}", lost_res.error().Message());
                _impl->window->Close();
            }
        }
        return false;
    }
    return true;
}

auto Engine::GetCurrentFrame() const noexcept -> uint64_t {
    return _impl->frameCounter;
}

auto Engine::GetWindow() -> Window& {
    return *_impl->window;
}
auto Engine::GetPhysicsContext() -> PhysicsContext& {
    return *_impl->physicsContext;
}
auto Engine::GetRenderContext() -> RenderContext& {
    return *_impl->renderContext;
}
auto Engine::GetCamera() -> Camera& {
    return _impl->mainCamera;
}
auto Engine::GetCreativeWorksManager() -> CreativeWorksManager& {
    return *_impl->assetManager;
}
auto Engine::GetAudioContext() -> AudioContext& {
    return *_impl->audioContext;
}
auto Engine::GetScriptRunner() -> ScriptRunner& {
    return *_impl->scriptRunner;
}
auto Engine::GetFileSystemWatcher() -> FileSystemWatcher& {
    return *_impl->fileSystemWatcher;
}
auto Engine::GetRegistry() -> ECS::Registry& {
    return _impl->registry;
}

auto Engine::GetRegistry() const -> const ECS::Registry& {
    return _impl->registry;
}

auto Engine::GetUpdateGraph() -> ECS::SystemGraph& {
    return *_impl->updateGraph;
}
auto Engine::GetRenderGraph() -> ECS::SystemGraph& {
    return *_impl->renderGraph;
}
auto Engine::GetMainECB() -> ECS::EntityCommandBuffer& {
    return *_impl->mainECB;
}
auto Engine::GetFrameScheduler() -> FrameScheduler& {
    return _impl->scheduler;
}
auto Engine::GetCullingSystem() -> CullingSystem& {
    return *_impl->cullingSystem;
}
auto Engine::GetArticulationSystem() -> ArticulationSystem& {
    return *_impl->articulationSystem;
}
auto Engine::GetVisibleEntities() -> JPH::Array<Entity>& {
    return _impl->visibleEntities;
}
auto Engine::GetVisibleShadowEntities() -> JPH::Array<Entity>& {
    return _impl->visibleShadowEntities;
}
auto Engine::GetCurrentAlpha() -> float& {
    return _impl->currentAlpha;
}

auto Engine::GetGameState() const -> void* {
    return _impl->gameState;
}
void Engine::SetGameState(void* state) {
    _impl->gameState = state;
}

void Engine::SetUICallback(UICallback callback) {
    _impl->uiCallback = std::move(callback);
}

void Engine::AddDeviceLostCallback(DeviceLostCallback callback) {
    if (callback) {
        _impl->deviceLostCallbacks.push_back(std::move(callback));
    }
}

auto Engine::DeviceLostCallbackCount() const noexcept -> size_t {
    return _impl->deviceLostCallbacks.size();
}

auto Engine::GetUICallback() const noexcept -> const UICallback* {
    return _impl->uiCallback ? &_impl->uiCallback : nullptr;
}

void Engine::ProvokeDeviceLost() {
    _impl->renderContext->ProvokeDeviceLost();
}

namespace {

void CollectDespawnPostorder(ECS::Registry& registry, Entity entity, std::vector<Entity>& postorder, std::unordered_set<uint64_t>& seen) {
    if (!registry.IsAlive(entity) || !seen.insert(entity.Pack()).second) {
        return;
    }

    std::vector<Entity> children;
    for (const Entity candidate: registry.GetEntitiesWith<Components::HierarchyComponent>()) {
        if (const auto* hierarchy = registry.Get<Components::HierarchyComponent>(candidate); hierarchy != nullptr && hierarchy->parent == entity) {
            children.push_back(candidate);
        }
    }

    for (const Entity child: children) {
        CollectDespawnPostorder(registry, child, postorder, seen);
    }
    postorder.push_back(entity);
}

} // namespace

void DespawnEntity(Engine& engine, Entity entity) {
    auto& registry = engine.GetRegistry();
    std::vector<Entity> postorder;
    std::unordered_set<uint64_t> seen;
    CollectDespawnPostorder(registry, entity, postorder, seen);

    for (const Entity current: postorder) {
        if (!registry.IsAlive(current)) {
            continue;
        }

        // These systems keep external handles outside component storage, and
        // therefore receive the entity while its component data is still valid.
        engine.GetArticulationSystem().Release(engine, current);
        engine.GetAudioContext().ReleaseOwner(current);
        if (const auto* physics = registry.Get<Components::PhysicsComponent>(current); physics != nullptr) {
            engine.GetPhysicsContext().DestroyBody(physics->physicsHandle);
        }
        engine.GetRenderContext().ReleaseEntityBuffers(current);
        registry.Destroy(current);
    }
}

auto Engine::InitializeDefaultScene() -> bool {
    auto& rc  = GetRenderContext();
    auto& reg = GetRegistry();

    reg.RegisterAllComponentsIn<ZHLN::Components>();

    reg.Create(
        Components::MainCameraTagComponent {}, Components::CameraComponent {},
        Components::AASettingsComponent {.state = {.mode = AAMode::TAA, .taaFeedback = 0.95f}}, Components::FreeCamTagComponent {},
        Components::InputComponent {},
        Components::TargetCameraComponent {
            .distance          = 4.5f,
            .targetDistance    = 4.5f,
            .yaw               = -90.0f,
            .pitch             = -10.0f,
            .stiffness         = 15.0f,
            .vignetteIntensity = 1.10f,
            .vignettePower     = 1.50f,
            .fov               = 45.0f,
            .targetFov         = 45.0f
        }
    );

    reg.Create(
        Components::GlobalSettingsTagComponent {}, Components::PostProcessSettingsComponent {}, Components::ShadowSettingsComponent {},
        Components::DebugSettingsComponent {.physicsDrawMode = 0}
    );

    reg.Create(GUI::UISettingsComponent {});

    // The atlas is device state, so it survives the scene it was first built
    // for; only the component-side copy is re-seeded. Rebuilding it per scene
    // leaked a 1024x1024 texture every time.
    if (_impl->fontAtlas.has_value()) {
        if (auto* uiSettings = reg.GetSingleton<GUI::UISettingsComponent>(); uiSettings != nullptr) {
            uiSettings->fontAtlas        = *_impl->fontAtlas;
            uiSettings->defaultFontAtlas = _impl->fontAtlas->texture;
        }
    } else {
        CreativeWorksFactory::CreateFontAtlasTexture(rc, reg);
        if (const auto* uiSettings = reg.GetSingleton<GUI::UISettingsComponent>();
            uiSettings != nullptr && uiSettings->fontAtlas.texture != TextureHandle::Invalid) {
            _impl->fontAtlas = uiSettings->fontAtlas;
        }
    }

    BuildSystemGraphs(*this);
    BuildFrameScheduler(*this);
    return true;
}

auto Engine::Tick(float dt, GameplayDriver driver) -> GameplayStatus {
    _impl->activeGameplayDriver = driver;

    // Resource contexts retain owner/handle pairs outside ECS component
    // storage. Reconcile before any phase can observe this frame's world.
    _impl->renderContext->ReconcileEntityBuffers(_impl->registry.AliveQuery());

    FrameContext ctx {.driver = driver, .status = GameplayStatus::OK, .deviceLost = false};

    // The whole frame is the scheduler's ordered step list; the two SystemGraphs
    // are steps inside it (see BuildFrameScheduler), so their hazard analysis
    // only ever orders systems within a graph, never the phases around them.
    _impl->scheduler.Execute(*this, dt, ctx);

    _impl->frameCounter++;

    return ctx.status;
}

auto Engine::Run(const CommandLineOptions& options, UICallback uiCallback) -> std::expected<void, Error> {
    Platform::Init();
    ZHLN::SetupSignalHandler();
    TaskSystem::Init();

    uint32_t w = options.fullscreen ? 0 : 1280;
    uint32_t h = options.fullscreen ? 0 : 720;

    EngineConfig config {
        .physics = {.maxBodies = 5000, .maxBodyPairs = 10000, .maxContactConstraints = 10000, .tempAllocatorSize = 64 * 1024 * 1024},
        .render  = {
            .appName        = options.launchEditor ? "Zahlen World Editor" : "Zahlen Engine",
            .width          = w,
            .height         = h,
            .vsync          = options.vsync,
            .fullscreen     = options.fullscreen,
            .validationMode = options.validationMode,
            .headless       = options.headless,
        },
    };

    auto engine_res = Engine::Create(config);
    if (!engine_res) {
        TaskSystem::Shutdown();
        return std::unexpected(engine_res.error()); // Propagate the exact Error!
    }

    auto engine = std::move(engine_res.value());
    engine->GetWindow().Focus();
    engine->InitializeDefaultScene();

    if (uiCallback) {
        engine->SetUICallback(std::move(uiCallback));
    }

    const double targetFrameTime = options.fpsLimit > 0 ? 1.0 / static_cast<double>(options.fpsLimit) : 0.0;
    auto         frameStart      = std::chrono::high_resolution_clock::now();

    while (engine->IsRunning()) {
        engine->ProcessEvents();

        auto   frameEnd = std::chrono::high_resolution_clock::now();
        double elapsed  = std::chrono::duration<double>(frameEnd - frameStart).count();
        frameStart      = std::chrono::high_resolution_clock::now();

        float rawDt = std::min(static_cast<float>(elapsed), 0.1f);

        {
            auto& r = engine->GetRegistry();
            if (auto* st = r.GetSingleton<Components::InputStateComponent>(); st != nullptr && st->needsResize) {
                engine->GetRenderContext().SetResolution(st->newSize);
                st->needsResize = false;
                continue;
            }
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
    return {}; // Success!
}

} // namespace ZHLN
