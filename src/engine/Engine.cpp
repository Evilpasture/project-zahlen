// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Engine.cpp
#include <GLFW/glfw3.h>
#include <thread>
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>
// clang-format on
#include "TTYBackend.hpp"
#include "engine/system/LODSystem.hpp"
#include "imgui.h"
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
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
#include <Zahlen/physics/Physics.hpp>
#include <engine/FileWatcher.hpp>
#include <engine/NativeScriptModule.hpp>
#include <engine/Platform.hpp>
#include <engine/system/AnimationSystem.hpp>
#include <engine/system/ArticulationSystem.hpp>
#include <engine/system/CameraSystem.hpp>
#include <engine/system/CullingSystem.hpp>
#include <engine/system/DecalSystem.hpp>
#include <engine/system/InputSystem.hpp>
#include <engine/system/InteractionSystem.hpp>
#include <engine/system/LightingSystem.hpp>
#include <engine/system/ParticleSystem.hpp>
#include <engine/system/PhysicsStateSystem.hpp>
#include <engine/system/PhysicsSystem.hpp>
#include <engine/system/RenderSystem.hpp>
#include <engine/system/TargetCameraSystem.hpp>
#include <engine/system/TerrainSystem.hpp>
#include <engine/system/TextureSystem.hpp>
#include <engine/system/TransformSystem.hpp>
#include <engine/system/UIInteractionSystem.hpp>
#include <engine/system/UIRenderSystem.hpp>
#include <filesystem>
#include <renderdoc_app.h>
#ifdef __linux__
#include <dlfcn.h>
#endif

#include <Zahlen/Threading/Thread.hpp>

namespace ZHLN {

thread_local Engine*        g_CurrentEngine = nullptr;
static Engine*              s_GlobalEngine  = nullptr;
static RENDERDOC_API_1_5_0* s_RDocAPI       = nullptr;

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
        auto R_GetAPI = (pRENDERDOC_GetAPI) dlsym(mod, "RENDERDOC_GetAPI");
        if (R_GetAPI != nullptr) {
            R_GetAPI(eRENDERDOC_API_Version_1_5_0, (void**) &s_RDocAPI);
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
    std::unique_ptr<Window>               window;
    std::unique_ptr<RenderContext>        renderContext;
    std::unique_ptr<PhysicsContext>       physicsContext;
    std::unique_ptr<AudioContext>         audioContext;
    std::unique_ptr<CreativeWorksManager> assetManager;
    std::unique_ptr<ScriptRunner>         scriptRunner;

    Engine::UICallback uiCallback = nullptr;

    Camera        mainCamera;
    ECS::Registry registry;

    std::unique_ptr<ECS::SystemGraph>         updateGraph;
    std::unique_ptr<ECS::SystemGraph>         renderGraph;
    std::unique_ptr<ECS::EntityCommandBuffer> mainECB;
    std::unique_ptr<CullingSystem>            cullingSystem;
    JPH::Array<Entity>                        visibleEntities;
    JPH::Array<Entity>                        visibleShadowEntities;
    float                                     currentAlpha = 0.0f;

    void*        gameState    = nullptr;
    uint64_t     frameCounter = 0;
    EngineConfig config;
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
            rc.SetGISettings({
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
            });
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

    updateGraph.AddSystem({
        .update_func    = Sys_PostProcess,
        .name           = "PostProcessSystem",
        .access_pattern = {Read<Components::PostProcessSettingsComponent>()},
        .enabled        = true,
    });

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

    auto rc_res = RenderContext::Create(*_impl->window, _impl->config.render);
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());
    CreativeWorksFactory::RebuildVulkanResources(*_impl->renderContext, *_impl->assetManager, _impl->registry);
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
    auto engine = std::unique_ptr<Engine>(new (std::nothrow) Engine());
    if (!engine) {
        return std::unexpected(EngineInitError::UnknownError);
    }

    auto res = engine->InitInternal(cfg);
    if (!res) {
        return std::unexpected(res.error());
    }

    return engine;
}

auto Engine::InitInternal(const EngineConfig& cfg) -> std::expected<void, Error> {
    g_CurrentEngine = this;
    s_GlobalEngine  = this;

    ZHLN::Fiber::InitMainThread();

    _impl               = std::make_unique<EngineImpl>();
    _impl->config       = cfg;
    _impl->scriptRunner = std::make_unique<ScriptRunner>();

    bool use_tty = false;

    if (cfg.render.headless) {
        // True headless mode: skip GLFW entirely. No display server is required.
        ZHLN::Log("[Engine] Headless mode enabled. Skipping GLFW initialization.");
    } else {
        if constexpr (isLinux) {
            // Detects both RenderDoc and NVIDIA Nsight Graphics (Nomad) launch environments
            if (std::getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE") != nullptr || std::getenv("NOMAD_VULKAN_LAYER") != nullptr ||
                std::getenv("NGFX_INJECTION") != nullptr) {
                glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
            }
        }

        if (!glfwInit()) {
            if (TTYBackend::IsSupported()) {
                ZHLN::Log("GLFW failed to initialize. Falling back to native TTY Display Mode.");
                use_tty = true;
            } else {
                return std::unexpected(EngineInitError::WindowCreationFailed);
            }
        }
    }

    auto onKey = [](void* userdata, KeyCode key, bool pressed) -> void {
        auto* reg   = static_cast<ECS::Registry*>(userdata);
        auto  ents  = reg->GetEntitiesWith<Components::InputStateComponent>();
        auto* state = ents.empty() ? reg->Get<Components::InputStateComponent>(reg->Create(Components::InputStateComponent {})) :
                                     reg->Get<Components::InputStateComponent>(ents[0]);
        state->SetKey(static_cast<uint8_t>(key), pressed);

        if (pressed) {
            // Handle text navigation directly on focused text input components
            for (Entity e: reg->GetEntitiesWith<Components::UITextInputComponent>()) {
                auto* inputComp = reg->Get<Components::UITextInputComponent>(e);
                if (inputComp && inputComp->isFocused) {
                    std::string_view curr = inputComp->text;
                    if (key == KeyCode::Backspace && inputComp->cursorIndex > 0) {
                        std::string next = std::string(curr.substr(0, inputComp->cursorIndex - 1)) + std::string(curr.substr(inputComp->cursorIndex));
                        inputComp->text.assign(next);
                        inputComp->cursorIndex--;
                    } else if (key == KeyCode::Left && inputComp->cursorIndex > 0) {
                        inputComp->cursorIndex--;
                    } else if (key == KeyCode::Right && inputComp->cursorIndex < curr.size()) {
                        inputComp->cursorIndex++;
                    }
                }
            }
        }
    };

    auto onMouseMove = [](void* userdata, float x, float y) -> void {
        auto* reg   = static_cast<ECS::Registry*>(userdata);
        auto  ents  = reg->GetEntitiesWith<Components::InputStateComponent>();
        auto* state = ents.empty() ? reg->Get<Components::InputStateComponent>(reg->Create(Components::InputStateComponent {})) :
                                     reg->Get<Components::InputStateComponent>(ents[0]);
        state->ApplyLocalMotion(x, y);
    };

    auto onMouseScroll = [](void* userdata, float delta) -> void {
        auto* reg   = static_cast<ECS::Registry*>(userdata);
        auto  ents  = reg->GetEntitiesWith<Components::InputStateComponent>();
        auto* state = ents.empty() ? reg->Get<Components::InputStateComponent>(reg->Create(Components::InputStateComponent {})) :
                                     reg->Get<Components::InputStateComponent>(ents[0]);
        state->ApplyWheel(delta);
    };

    auto onResize = [](void* userdata, Extent2D extent) -> void {
        auto* reg   = static_cast<ECS::Registry*>(userdata);
        auto  ents  = reg->GetEntitiesWith<Components::InputStateComponent>();
        auto* state = ents.empty() ? reg->Get<Components::InputStateComponent>(reg->Create(Components::InputStateComponent {})) :
                                     reg->Get<Components::InputStateComponent>(ents[0]);
        state->ApplyResize(extent);
    };

    auto onChar = [](void* userdata, unsigned int codepoint) -> void {
        auto* reg = static_cast<ECS::Registry*>(userdata);
        for (Entity e: reg->GetEntitiesWith<Components::UITextInputComponent>()) {
            auto* inputComp = reg->Get<Components::UITextInputComponent>(e);
            if (inputComp && inputComp->isFocused) {
                if (codepoint >= 32 && codepoint <= 126 && inputComp->text.size() < 255) {
                    std::string_view curr = inputComp->text;
                    std::string      next = std::string(curr.substr(0, inputComp->cursorIndex)) + static_cast<char>(codepoint) +
                                            std::string(curr.substr(inputComp->cursorIndex));
                    inputComp->text.assign(next);
                    inputComp->cursorIndex++;
                }
            }
        }
    };

    WindowInputReceiver receiver = {
        .userdata = &_impl->registry, .onKey = onKey, .onMouseMove = onMouseMove, .onMouseScroll = onMouseScroll, .onResize = onResize, .onChar = onChar
    };

    _impl->window =
        std::make_unique<Window>(cfg.render.appName.data(), cfg.render.width, cfg.render.height, cfg.render.fullscreen, receiver, use_tty, cfg.render.headless);

    // Singleton InputStateComponent must exist before the first event pump.
    _impl->registry.Create(Components::InputStateComponent {});

    if (use_tty && _impl->window->GetTTYContext() == nullptr) {
        return std::unexpected(EngineInitError::TTYInitializationFailed);
    }

    InitRenderDocAPI();

    JPH::RegisterDefaultAllocator();
    JPH::Trace = JoltTraceBridge;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = JoltAssertBridge;
#endif

    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    auto rc_res = RenderContext::Create(*_impl->window, cfg.render);
    if (!rc_res) {
        return std::unexpected(rc_res.error());
    }
    _impl->renderContext = std::move(rc_res.value());

    _impl->physicsContext = std::make_unique<PhysicsContext>(cfg.physics);
    _impl->audioContext   = std::make_unique<AudioContext>();
    _impl->assetManager   = std::make_unique<CreativeWorksManager>();

    _impl->updateGraph   = std::make_unique<ECS::SystemGraph>();
    _impl->renderGraph   = std::make_unique<ECS::SystemGraph>();
    _impl->mainECB       = std::make_unique<ECS::EntityCommandBuffer>(_impl->registry);
    _impl->cullingSystem = std::make_unique<CullingSystem>();

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
    _impl->registry.Clear();
    _impl->physicsContext.reset();
    _impl->renderContext.reset();
    _impl->window.reset();
    _impl->assetManager.reset();
    _impl->audioContext.reset();
    _impl->scriptRunner.reset();
    _impl->updateGraph.reset();
    _impl->renderGraph.reset();
    _impl->mainECB.reset();
    _impl->cullingSystem.reset();

    if (!_impl->config.render.headless) {
        glfwTerminate();
    }

    JPH::UnregisterTypes();
    if (JPH::Factory::sInstance != nullptr) {
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
}

auto Engine::IsRunning() const -> bool {
    return _impl->window->IsRunning();
}

void Engine::ProcessEvents() {
    ZHLN::CheckForCrashes(this);

    auto&                            reg        = _impl->registry;
    auto                             ents       = reg.GetEntitiesWith<Components::InputStateComponent>();
    Components::InputStateComponent* inputState = ents.empty() ? nullptr : reg.Get<Components::InputStateComponent>(ents[0]);
    if (inputState != nullptr) {
        inputState->ResetDeltas();
    }

    if (_impl->window->IsHeadless()) {
        // True headless mode: no windowing event queue to poll, no ImGui frames.
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
    _impl->renderContext->BeginImGuiFrame();

    // Mirror ImGui capture into ECS so gameplay systems stay ImGui-free.
    // High-level ImGui UI remains in main.cpp; only the capture flags cross here.
    if (inputState != nullptr) {
        const ImGuiIO& io               = ImGui::GetIO();
        inputState->wantCaptureKeyboard = io.WantCaptureKeyboard;
        inputState->wantCaptureMouse    = io.WantCaptureMouse;
    }
}

auto Engine::BeginFrame(bool& outDeviceLost) noexcept -> bool {
    outDeviceLost = false;
    auto res      = _impl->renderContext->BeginFrame();
    if (!res) {
        if (res.error() == RenderFrameResult::DeviceLost) {
            outDeviceLost = true;
            { [[maybe_unused]] auto _ = HandleDeviceLost(); }
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
            { [[maybe_unused]] auto _ = HandleDeviceLost(); }
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
auto Engine::GetCullingSystem() -> CullingSystem& {
    return *_impl->cullingSystem;
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

void Engine::ProvokeDeviceLost() {
    _impl->renderContext->ProvokeDeviceLost();
}

auto GetEngineContext() -> Engine* {
    if (g_CurrentEngine != nullptr) {
        return g_CurrentEngine;
    }
    return s_GlobalEngine;
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

    reg.Create(Components::UISettingsComponent {});

    CreativeWorksFactory::CreateFontAtlasTexture(rc);
    BuildSystemGraphs(*this);
    return true;
}

auto Engine::Tick(float dt, GameplayDriver driver) -> GameplayStatus {
    static FileWatcher        gameplayWatcher("scripts/boot.lua");
    static NativeScriptModule nativeModule("scripts/gameplay");
    static InputSystem        inputSystem;
    static TargetCameraSystem targetCamSys;
    static CameraSystem       camSys;
    static PhysicsSystem      physicsSystem;

    // 1. Process Input & UI Systems
    inputSystem.Update(*this);
    UIInteractionSystem::Update(*this, dt);

    if (_impl->uiCallback) {
        _impl->uiCallback(*this);
    }

    if (driver != GameplayDriver::Cpp && gameplayWatcher.CheckModified()) {
        GetScriptRunner().ReloadFile("scripts/boot.lua");
    }
    GetRenderContext().CheckShaderReload();

    // 2. Translate gameplay input using the previous resolved camera. Camera
    // transforms are finalized after physics and the update graph so rig-driven
    // first-person views cannot lag one simulation frame behind their body.
    inputSystem.PlayerInputTranslate(*this, GetCamera());

    // 3. Physics Fixed Step & WriteBack
    physicsSystem.Update(*this, dt);

    // 4. Gameplay Module Update
    GameplayStatus status = GameplayStatus::OK;
    switch (driver) {
        using enum GameplayDriver;
        case Cpp: {
            ZHLN::ScopedTimer profTimer("ECS System: Native C++ Gameplay Update");
            status = nativeModule.Update(this, dt);
            break;
        }
        case Fennel: {
            ZHLN::ScopedTimer profTimer("ECS System: Script/Lua Update");
            GetScriptRunner().CallUpdate(this, dt);
            break;
        }
        case Hybrid: {
            {
                ZHLN::ScopedTimer profTimer("ECS System: Native C++ Gameplay Update");
                status = nativeModule.Update(this, dt);
            }
            {
                ZHLN::ScopedTimer profTimer("ECS System: Script/Lua Update");
                GetScriptRunner().CallUpdate(this, dt);
            }
            break;
        }
    }

    // 5. Update Graph & Command Buffer Playback
    GetUpdateGraph().Execute(*this, dt);
    GetMainECB().Playback();

    // Resolve target cameras and camera matrices from current physics and
    // procedural rig poses immediately before visibility/render work.
    targetCamSys.Update(*this, dt, GetCurrentAlpha());
    camSys.Update(*this, dt, GetCurrentAlpha());
    LODSystem::Update(*this);

    // 6. Render Graph & Frame Submission
    GetRenderGraph().Execute(*this, dt);
    auto render_res = RenderSystem::Update(*this, dt);
    if (!render_res) {
        if (render_res.error().Is<RenderFrameResult>() && render_res.error().As<RenderFrameResult>() == RenderFrameResult::DeviceLost) {
{ [[maybe_unused]] auto _ = HandleDeviceLost(); }
        }
    }

    // Auto-detect missing gameplay scripts / modules and engage Fallback Preset
    if (!DefaultPreset::IsActive()) {
        if ((driver == GameplayDriver::Fennel || driver == GameplayDriver::Hybrid) && !std::filesystem::exists("scripts/boot.lua") &&
            !std::filesystem::exists("scripts/boot.fnl")) {
            DefaultPreset::BuildFallbackScene(*this, FallbackReason::MissingBootScript, "Script 'scripts/boot.lua' was not found in working directory.");
        } else if (driver == GameplayDriver::Cpp && !nativeModule.IsLoaded()) {
            DefaultPreset::BuildFallbackScene(
                *this, FallbackReason::MissingNativeModule, "Native gameplay module (libgameplay.so / gameplay.dll) was not found."
            );
        }
    }

    if (DefaultPreset::IsActive()) {
        DefaultPreset::Update(*this, dt);
    }

    // 7. Motion Vectors & Transform History
    {
        ZHLN::ScopedTimer      profTimer("ECS System: Update Transform History");
        static TransformSystem ts;
        ts.UpdateTransformHistory(GetRegistry());
    }

    _impl->frameCounter++;

    return status;
}

auto Engine::Run(const CommandLineOptions& options, UICallback uiCallback) -> int {
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
        ZHLN::Log("Error initializing Engine: {}", engine_res.error().Message());
        return EXIT_FAILURE;
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
            auto& r     = engine->GetRegistry();
            auto  iEnts = r.GetEntitiesWith<Components::InputStateComponent>();
            if (!iEnts.empty()) {
                if (auto* st = r.Get<Components::InputStateComponent>(iEnts[0]); st != nullptr && st->needsResize) {
                    engine->GetRenderContext().SetResolution(st->newSize);
                    st->needsResize = false;
                    continue;
                }
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
    return EXIT_SUCCESS;
}

} // namespace ZHLN
