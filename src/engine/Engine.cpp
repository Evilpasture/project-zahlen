// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/Engine.cpp
#include "ArticulationSystem.hpp"
#include "CullingSystem.hpp"
#include "DefaultPreset.hpp"
#include "EngineGlobals.hpp"
#include "NativeScriptModule.hpp"
#include "Platform.hpp"
#include "diagnostics/CrashObservers.hpp"
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/FileSystemWatcher.hpp>
#include <Zahlen/FrameScheduler.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Kernel.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Scripting.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Window.hpp>
#include <Zahlen/World.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/ecs/EntityCommandBuffer.hpp>
#include <Zahlen/ecs/SystemGraph.hpp>
#include <Zahlen/gui/GUI.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace ZHLN {

// ============================================================================
// Core Lifecycle Errors (Tier 3)
// Application bootstrap code branches on these specific failure reasons.
// ============================================================================

enum class EngineInitError : uint8_t {
    // Window/TTY/render failures live on KernelInitError (Kernel.cpp), physics
    // on WorldInitError (World.cpp); the composition root itself can only fail
    // to allocate. Error carries the annotated description in every case.
    EngineAllocationFailed ZHLN_ANNOTATION(ZHLN::Description<"Engine instance allocation failed"> {}) = 1,
};

struct EngineImpl {
    // Declaration order encodes the teardown order (reverse of declaration):
    // the World (registry, physics, Jolt) dies before the Kernel (GPU, windows,
    // GLFW), and the script module dies before the Kernel's FileSystemWatcher
    // whose subscriptions it owns. Kernel is declared first so it outlives
    // every callback-owning client during normal and partial-init teardown.
    std::unique_ptr<Kernel> kernel;
    std::unique_ptr<World>  world;

    std::unique_ptr<ScriptRunner>       scriptRunner;
    std::unique_ptr<NativeScriptModule> nativeScriptModule;
    // Hot-reload watches for whichever boot scripts the installed runtime
    // declares. Empty until a host installs one; core names no file here.
    std::vector<FileWatchHandle> bootScriptWatches;
    GameplayDriver               activeGameplayDriver = GameplayDriver::Cpp;

    Engine::UICallback                      uiCallback = nullptr;
    std::vector<Engine::DeviceLostCallback> deviceLostCallbacks;

    // Optional-layer wiring; see the Engine.hpp seam docs. Vectors because any
    // number of extras modules may contribute, and each rebuild (scene reset)
    // replays the whole list.
    std::vector<Engine::FrameSchedulerExtension> frameSchedulerExtensions;
    std::vector<Engine::SystemGraphsExtension>   systemGraphsExtensions;
    Engine::CharacterStepHooks                   characterStepHooks;
    Engine::FreeCamSpeedQuery                    freeCamSpeedQuery     = nullptr;
    BonePosePostProcessor                        bonePosePostProcessor = nullptr;
    std::vector<Engine::TeardownHook>            teardownHooks;

    FrameScheduler scheduler;
    float          currentAlpha = 0.0f;

    // Built once per engine, not once per scene: the glyph packing costs
    // 96 SDF rasterisations, and the upload burns a 1024x1024 bindless texture
    // that nothing ever releases. The scene owns a *copy* in UISettingsComponent,
    // which Registry::Clear() throws away, so the engine keeps the authoritative
    // one and re-seeds each new scene from it. See InitializeDefaultScene.
    std::optional<FontAtlas> fontAtlas;

    void*        gameState    = nullptr;
    uint64_t     frameCounter = 0;
    EngineConfig config;
};

auto Engine::UpdateNativeGameplay(float dt) -> GameplayStatus {
    return _impl->nativeScriptModule->Update(this, dt);
}

auto Engine::IsNativeGameplayLoaded() const noexcept -> bool {
    return _impl->nativeScriptModule->IsLoaded();
}

auto Engine::FallbackSceneEnabled() const noexcept -> bool {
    return _impl->config.enableFallbackScene;
}

void Engine::SeedSceneFontAtlas(ECS::Registry& reg) {
    if (_impl->fontAtlas.has_value()) {
        if (auto* uiSettings = reg.GetSingleton<GUI::UISettingsComponent>(); uiSettings != nullptr) {
            uiSettings->fontAtlas        = *_impl->fontAtlas;
            uiSettings->defaultFontAtlas = _impl->fontAtlas->texture;
        }
    } else {
        CreativeWorksFactory::CreateFontAtlasTexture(GetRenderContext(), reg);
        if (const auto* uiSettings = reg.GetSingleton<GUI::UISettingsComponent>();
            uiSettings != nullptr && uiSettings->fontAtlas.texture != TextureHandle::Invalid) {
            _impl->fontAtlas = uiSettings->fontAtlas;
        }
    }
}

namespace {

// ============================================================================
// Crash Observers
// ============================================================================
// Each subsystem describes how to dump itself, and diagnostics/CrashHandler.cpp
// iterates whatever is registered without knowing any of these types exist.
// That is the whole point: the crash handler used to #include <Zahlen/Engine.hpp>,
// <Zahlen/Camera.hpp> and <Zahlen/physics/Physics.hpp> to reach into
// Camera::frustum and PhysicsContext directly, which made the crash path depend
// on the engine and on Jolt, and meant a new subsystem dump meant editing the
// crash handler.
//
// These run from a crash, on state that the fault may already have corrupted.
// They are only invoked from the deferred path (see DumpContext in
// CrashHandler.cpp), never from inside the signal handler itself.
//
// The `context` parameter is how a member function gets here: each of these is a
// captureless lambda or free function that casts the void* back to the
// subsystem it was registered with.

void DumpEngineState(void* context, const SignalEvent& /*event*/) noexcept {
    ZHLN::Trace(*static_cast<Engine*>(context));
}

void DumpCameraState(void* context, const SignalEvent& /*event*/) noexcept {
    auto& cam = *static_cast<Camera*>(context);

    auto cam_pos = ZHLN::Format("  Position:  ({}, {}, {})\n", cam.position.GetX(), cam.position.GetY(), cam.position.GetZ());
    auto cam_dir = ZHLN::Format("  Direction: Yaw: {}, Pitch: {}\n", cam.yaw, cam.pitch);
    Diagnostics::WriteCrashOutput(cam_pos);
    Diagnostics::WriteCrashOutput(cam_dir);

    auto& f         = cam.frustum;
    auto  frust_hdr = ZHLN::Format("\n{}--- FRUSTUM PLANE EQUATIONS (SIMD DECODED) ---{}\n", Color::Cyan, Color::Reset);
    Diagnostics::WriteCrashOutput(frust_hdr);
    const char* names[] = {"Left  ", "Right ", "Top   ", "Bottom", "Near  ", "Far   "};

    // Jolt packs the six planes into two SoA blocks of four lanes; the plane a
    // caller thinks of as "index i" is block i/4, lane i%4.
    for (int i = 0; i < 6; ++i) {
        const int block     = i / 4;
        const int lane      = i % 4;
        auto      plane_str = ZHLN::Format(
            "  Plane {}: [{}x {}y {}z] offset: {}\n", names[i], f.mX[block].mF32[lane], f.mY[block].mF32[lane], f.mZ[block].mF32[lane], f.mW[block].mF32[lane]
        );
        Diagnostics::WriteCrashOutput(plane_str);
    }

    ZHLN::Dump(cam.frustum);
}

void DumpPhysicsState(void* context, const SignalEvent& /*event*/) noexcept {
    static_cast<PhysicsContext*>(context)->TraceDiagnostics();
}

// Registers the subsystem dumps above. Returns nothing: a subsystem that fails
// to register costs its own section of the crash report and nothing else, and
// failing engine startup over a missing diagnostic would be the wrong trade.
void RegisterCrashObservers(CrashState& state, Engine& engine, World& world) {
    // Order matters -- it is the order the sections appear in the crash report.
    Diagnostics::RegisterCrashObserver(state, "ENGINE", DumpEngineState, &engine);
    Diagnostics::RegisterCrashObserver(state, "CAMERA DEEP", DumpCameraState, &world.GetCamera());
    Diagnostics::RegisterCrashObserver(state, "PHYSICS", DumpPhysicsState, &world.GetPhysics());
}

} // namespace

Engine::Engine(): _impl(nullptr) {
}

auto Engine::HandleDeviceLost() noexcept -> std::expected<void, Error> {
    // The Kernel rebuilds everything it owns: the GPU context and every
    // extra-window viewport. World-side state survives untouched, which is the
    // point of the split -- only GPU resources need re-uploading.
    if (auto rebuilt = _impl->kernel->HandleDeviceLost(); !rebuilt) {
        return std::unexpected(rebuilt.error());
    }
    CreativeWorksFactory::RebuildVulkanResources(_impl->kernel->GetRenderContext(), _impl->world->GetRegistry());

    // Core has rebuilt everything it owns. Owners outside the engine now
    // re-upload against the new context, in the order they registered.
    for (const auto& callback: _impl->deviceLostCallbacks) {
        if (callback) {
            callback(*this);
        }
    }
    return {};
}

auto Engine::Create(const EngineConfig& cfg) -> std::expected<std::unique_ptr<Engine>, Error> {
    auto instance = std::unique_ptr<Engine>(new (std::nothrow) Engine());
    if (!instance) {
        return std::unexpected(EngineInitError::EngineAllocationFailed);
    }

    if (auto result = instance->InitInternal(cfg); !result) {
        return std::unexpected(result.error());
    }
    return instance;
}

auto Engine::InitInternal(const EngineConfig& cfg) -> std::expected<void, Error> {
    ZHLN::Fiber::InitMainThread();

    _impl           = std::make_unique<EngineImpl>();
    _impl->config   = cfg;
    _impl->scriptRunner = std::make_unique<ScriptRunner>();
    // A host installs its runtime after Create() returns, so the boot-script
    // watches are registered when that happens rather than here -- and the paths
    // come from the runtime itself, never from core.
    _impl->scriptRunner->SetRuntimeChanged([this] { RegisterBootScriptWatches(); });

    // The World comes first: the window input callbacks write InputStateComponent
    // into the registry, so the registry must exist before the Kernel's first
    // event pump. It also means a Kernel-only host (UI editor, cooker) never
    // pays for physics or a simulation.
    auto world_res = World::Create(cfg.physics);
    if (!world_res) {
        return std::unexpected(world_res.error());
    }
    _impl->world = std::move(world_res.value());

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
        auto* reg   = &static_cast<World*>(userdata)->GetRegistry();
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->SetKey(static_cast<uint8_t>(key), pressed);
        if (pressed) {
            state->QueueKeyPress(key);
        }
    };

    auto onMouseMove = [](void* userdata, float x, float y) -> void {
        auto* reg   = &static_cast<World*>(userdata)->GetRegistry();
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->ApplyLocalMotion(x, y);
    };

    auto onMouseScroll = [](void* userdata, float delta) -> void {
        auto* reg   = &static_cast<World*>(userdata)->GetRegistry();
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->ApplyWheel(delta);
    };

    auto onResize = [](void* userdata, Extent2D extent) -> void {
        auto* reg   = &static_cast<World*>(userdata)->GetRegistry();
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->ApplyResize(extent);
    };

    auto onChar = [](void* userdata, unsigned int codepoint) -> void {
        auto* reg   = &static_cast<World*>(userdata)->GetRegistry();
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->QueueChar(codepoint);
    };

    // userdata is the heap-allocated World (stable for the engine's whole life,
    // unlike `this`), which owns the registry the callbacks write to. The Kernel
    // never touches ECS -- it only forwards events through this receiver.
    World* worldPtr = _impl->world.get();
    WindowInputReceiver receiver = {
        .userdata = worldPtr, .onKey = onKey, .onMouseMove = onMouseMove, .onMouseScroll = onMouseScroll, .onResize = onResize, .onChar = onChar
    };

    auto kernel_res = Kernel::Create(cfg.render, receiver);
    if (!kernel_res) {
        return std::unexpected(kernel_res.error());
    }
    _impl->kernel = std::move(kernel_res.value());

    _impl->nativeScriptModule = std::make_unique<NativeScriptModule>(*this, "scripts/gameplay");

    // From here on a crash report can include this engine's state. Done after
    // the contexts exist, since an observer holds a raw pointer to them. A host
    // that did not supply a CrashState gets no subsystem dumps.
    if (_impl->config.crashState != nullptr) {
        RegisterCrashObservers(*_impl->config.crashState, *this, *_impl->world);
    }

    return {};
}

void Engine::RegisterBootScriptWatches() {
    if (_impl == nullptr || _impl->kernel == nullptr) {
        return;
    }

    // Drop the previous runtime's watches first: a host may replace the runtime,
    // and the paths belong to whichever one is installed now.
    for (const FileWatchHandle handle: _impl->bootScriptWatches) {
        static_cast<void>(_impl->kernel->GetFileWatcher().Unwatch(handle));
    }
    _impl->bootScriptWatches.clear();

    if (_impl->scriptRunner == nullptr) {
        return;
    }

    const auto reloadBootScript = [this](const FileWatchEvent& event) {
        if (_impl->activeGameplayDriver == GameplayDriver::Cpp || event.action == FileWatchAction::Deleted) {
            return;
        }
        _impl->scriptRunner->ReloadFile(event.path.string());
    };
    for (const std::string_view path: _impl->scriptRunner->BootScriptPaths()) {
        _impl->bootScriptWatches.push_back(_impl->kernel->GetFileWatcher().WatchFile(std::filesystem::path(path), reloadBootScript));
    }
}

Engine::~Engine() {
    // InitInternal can fail before _impl is built, and Engine::Create deletes a
    // half-built engine.
    if (_impl == nullptr) {
        return;
    }

    // Before anything below is destroyed: a crash observer holds a raw pointer
    // to the camera and to the physics context, and a fault during teardown
    // would otherwise dump memory that has already been freed.
    if (_impl->config.crashState != nullptr) {
        Diagnostics::ClearCrashObservers(*_impl->config.crashState);
    }

    if (_impl->kernel != nullptr && _impl->world != nullptr) {
        // Optional layers that park engine-scoped state in process-global
        // storage release it here, while the engine and its registry are still
        // whole. Runs before any subsystem below is destroyed.
        for (const auto hook: _impl->teardownHooks) {
            hook(*this);
        }

        // The fallback preset parks entity handles in process-global storage. They
        // name entities in the registry that is about to be cleared, so they must
        // not survive into the next engine (see DefaultPreset::ReleaseFor).
        DefaultPreset::ReleaseFor(this);

        // Ragdolls retain Jolt resources outside the registry. Drain them while
        // both the components and PhysicsContext still exist. InitInternal may
        // fail before this system is created, so teardown must tolerate that path.
        _impl->world->GetArticulationSystem().Shutdown(*this);
        _impl->world->GetRegistry().Clear();
        _impl->kernel->GetRenderContext().ReconcileEntityBuffers(_impl->world->GetRegistry().AliveQuery());
    }

    // World first (registry, physics, Jolt), then the script module (its
    // watches live in the Kernel's FileSystemWatcher), then the Kernel
    // (GPU, windows, watcher, GLFW). See EngineImpl's declaration order.
    _impl->world.reset();
    // The subscriptions live in the watcher's own map, so they die with it; the
    // handles are only this side's bookkeeping and must not outlive it.
    _impl->bootScriptWatches.clear();
    _impl->nativeScriptModule.reset();
    _impl->kernel.reset();
}

auto Engine::IsRunning() const -> bool {
    return _impl->kernel->IsRunning();
}

void Engine::ProcessEvents() {
    if (_impl->config.crashState != nullptr) {
        ZHLN::CheckForCrashes(*_impl->config.crashState, this);
    }

    // Input-state bookkeeping is World-side: the pump writes into the registry.
    auto& reg        = _impl->world->GetRegistry();
    auto* inputState = reg.GetSingleton<Components::InputStateComponent>();
    if (inputState != nullptr) {
        inputState->ResetDeltas();
    }

    if (_impl->kernel->GetWindow().IsHeadless()) {
        // True headless mode: no windowing event queue to poll.
        return;
    }

    const bool isTTY = _impl->kernel->GetWindow().IsTTY();
    _impl->kernel->ProcessEvents();

    if (isTTY && inputState != nullptr) {
        // The TTY pump has no focus model, so UI capture never applies there.
        inputState->wantCaptureKeyboard = false;
        inputState->wantCaptureMouse    = false;
    }
}

auto Engine::GetCurrentFrame() const noexcept -> uint64_t {
    return _impl->frameCounter;
}

auto Engine::GetWindow() -> Window& {
    return _impl->kernel->GetWindow();
}

auto Engine::GetWindow(size_t index) -> Window& {
    return _impl->kernel->GetWindow(index);
}

auto Engine::WindowCount() const noexcept -> size_t {
    return _impl->kernel->WindowCount();
}

auto Engine::AddWindow(
    const String32&            title,
    uint32_t                   width,
    uint32_t                   height,
    bool                       fullscreen,
    const WindowInputReceiver& receiver,
    ViewportMode               mode,
    Entity                     camera
) -> Window* {
    return _impl->kernel->AddWindow(title, width, height, fullscreen, receiver, mode, camera);
}

void Engine::RemoveWindow(Window& window) {
    _impl->kernel->RemoveWindow(window);
}

auto Engine::GetKernel() -> Kernel& {
    return *_impl->kernel;
}
auto Engine::GetWorld() -> World& {
    return *_impl->world;
}

auto Engine::MakeSystemContext(float dt) -> SystemContext {
    return SystemContext {
        .registry              = _impl->world->GetRegistry(),
        .render                = &_impl->kernel->GetRenderContext(),
        .physics               = &_impl->world->GetPhysics(),
        .audio                 = &_impl->kernel->GetAudioContext(),
        .camera                = &_impl->world->GetCamera(),
        .culling               = &_impl->world->GetCullingSystem(),
        .articulation          = &_impl->world->GetArticulationSystem(),
        .bonePosePostProcessor = _impl->bonePosePostProcessor,
        .visibleEntities       = &_impl->world->GetVisibleEntities(),
        .visibleShadowEntities = &_impl->world->GetVisibleShadowEntities(),
        .frame                 = _impl->frameCounter,
        .alpha                 = _impl->currentAlpha,
        .dt                    = dt,
    };
}

auto Engine::GetPhysicsContext() -> PhysicsContext& {
    return _impl->world->GetPhysics();
}
auto Engine::GetRenderContext() -> RenderContext& {
    return _impl->kernel->GetRenderContext();
}
auto Engine::GetCamera() -> Camera& {
    return _impl->world->GetCamera();
}
auto Engine::GetCreativeWorksManager() -> CreativeWorksManager& {
    return _impl->kernel->GetAssetManager();
}
auto Engine::GetAudioContext() -> AudioContext& {
    return _impl->kernel->GetAudioContext();
}
auto Engine::GetScriptRunner() -> ScriptRunner& {
    return *_impl->scriptRunner;
}
auto Engine::GetFileSystemWatcher() -> FileSystemWatcher& {
    return _impl->kernel->GetFileWatcher();
}
auto Engine::GetRegistry() -> ECS::Registry& {
    return _impl->world->GetRegistry();
}

auto Engine::GetRegistry() const -> const ECS::Registry& {
    return _impl->world->GetRegistry();
}

auto Engine::GetUpdateGraph() -> ECS::SystemGraph& {
    return _impl->world->GetUpdateGraph();
}
auto Engine::GetRenderGraph() -> ECS::SystemGraph& {
    return _impl->world->GetRenderGraph();
}
auto Engine::GetMainECB() -> ECS::EntityCommandBuffer& {
    return _impl->world->GetMainECB();
}
auto Engine::GetFrameScheduler() -> FrameScheduler& {
    return _impl->scheduler;
}
auto Engine::GetCullingSystem() -> CullingSystem& {
    return _impl->world->GetCullingSystem();
}
auto Engine::GetArticulationSystem() -> ArticulationSystem& {
    return _impl->world->GetArticulationSystem();
}
auto Engine::GetVisibleEntities() -> JPH::Array<Entity>& {
    return _impl->world->GetVisibleEntities();
}
auto Engine::GetVisibleShadowEntities() -> JPH::Array<Entity>& {
    return _impl->world->GetVisibleShadowEntities();
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

void Engine::AddFrameSchedulerExtension(FrameSchedulerExtension ext) {
    if (ext != nullptr) {
        _impl->frameSchedulerExtensions.push_back(ext);
    }
}

void Engine::AddSystemGraphsExtension(SystemGraphsExtension ext) {
    if (ext != nullptr) {
        _impl->systemGraphsExtensions.push_back(ext);
    }
}

void Engine::ApplyFrameSchedulerExtensions(FrameScheduler& scheduler) {
    for (const auto ext: _impl->frameSchedulerExtensions) {
        ext(scheduler);
    }
}

void Engine::ApplySystemGraphsExtensions(ECS::SystemGraph& updateGraph, ECS::SystemGraph& renderGraph) {
    for (const auto ext: _impl->systemGraphsExtensions) {
        ext(updateGraph, renderGraph);
    }
}

void Engine::SetCharacterStepHooks(CharacterStepHooks hooks) {
    _impl->characterStepHooks = hooks;
}

auto Engine::GetCharacterStepHooks() const noexcept -> const CharacterStepHooks& {
    return _impl->characterStepHooks;
}

void Engine::SetBonePosePostProcessor(BonePosePostProcessor processor) {
    _impl->bonePosePostProcessor = processor;
}

auto Engine::GetBonePosePostProcessor() const noexcept -> BonePosePostProcessor {
    return _impl->bonePosePostProcessor;
}

void Engine::SetFreeCamSpeedQuery(FreeCamSpeedQuery query) {
    _impl->freeCamSpeedQuery = query;
}

auto Engine::GetFreeCamSpeedQuery() const noexcept -> FreeCamSpeedQuery {
    return _impl->freeCamSpeedQuery;
}

void Engine::AddTeardownHook(TeardownHook hook) {
    if (hook != nullptr) {
        _impl->teardownHooks.push_back(hook);
    }
}

auto Engine::GetUICallback() const noexcept -> const UICallback* {
    return _impl->uiCallback ? &_impl->uiCallback : nullptr;
}

void Engine::ProvokeDeviceLost() {
    _impl->kernel->ProvokeDeviceLost();
}

auto Engine::InitializeDefaultScene() -> bool {
    return DefaultPreset::InitializeDefaultScene(*this);
}

auto Engine::Tick(float dt, GameplayDriver driver) -> GameplayStatus {
    _impl->activeGameplayDriver = driver;

    // Resource contexts retain owner/handle pairs outside ECS component
    // storage. Reconcile before any phase can observe this frame's world.
    _impl->kernel->GetRenderContext().ReconcileEntityBuffers(_impl->world->GetRegistry().AliveQuery());

    FrameContext ctx {.driver = driver, .status = GameplayStatus::OK, .deviceLost = false};

    // The whole frame is the scheduler's ordered step list; the two SystemGraphs
    // are steps inside it (see BuildFrameScheduler), so their hazard analysis
    // only ever orders systems within a graph, never the phases around them.
    _impl->scheduler.Execute(*this, dt, ctx);

    _impl->frameCounter++;

    return ctx.status;
}

auto Engine::Run(const CommandLineOptions& options, CrashState& crashState, UICallback uiCallback, ExtensionInstaller installExtensions) -> std::expected<void, Error> {
    Platform::Init();
    ZHLN::SetupSignalHandler(crashState);
    TaskSystem::Init();

    uint32_t w = options.fullscreen ? 0 : 1280;
    uint32_t h = options.fullscreen ? 0 : 720;

    EngineConfig config {
        .physics = {.maxBodies = 5000, .maxBodyPairs = 10000, .maxContactConstraints = 10000, .tempAllocatorSize = 64 * 1024 * 1024},
        .render =
            {
                .appName        = options.launchEditor ? "Zahlen World Editor" : "Zahlen Engine",
                .width          = w,
                .height         = h,
                .vsync          = options.vsync,
                .fullscreen     = options.fullscreen,
                .validationMode = options.validationMode,
                .headless       = options.headless,
            },
        .crashState = &crashState,
    };

    auto engine_res = Engine::Create(config);
    if (!engine_res) {
        TaskSystem::Shutdown();
        return std::unexpected(engine_res.error()); // Propagate the exact Error!
    }

    auto engine = std::move(engine_res.value());
    engine->GetWindow().Focus();

    // Optional gameplay layers install before the default scene is built, so
    // their contributed systems and components are already wired when
    // InitializeDefaultScene registers components and compiles the graphs.
    if (installExtensions != nullptr) {
        installExtensions(*engine);
    }

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
