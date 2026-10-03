// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArticulationSystem.hpp"
#include "CullingSystem.hpp"
#include "EngineGlobals.hpp"
#include "NativeScriptModule.hpp"
#include "Platform.hpp"
#include "SystemWiring.hpp"
#include "SceneCleanupSystem.hpp"
#include "diagnostics/CrashObservers.hpp"
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Core/Arena.hpp>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/FrameScheduler.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Kernel.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Render/Render.hpp>
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
#include <utility>
#include <vector>

namespace ZHLN {


enum class EngineInitError : uint8_t {
    EngineAllocationFailed ZHLN_ANNOTATION(ZHLN::Description<"Engine instance allocation failed"> {}) = 1,
};

struct EngineImpl {
    std::unique_ptr<Kernel> kernel;
    std::unique_ptr<World>  world;

    std::unique_ptr<ScriptRunner>       scriptRunner;
    std::unique_ptr<NativeScriptModule> nativeScriptModule;
    std::vector<FS::FileWatchHandle> bootScriptWatches;
    GameplayDriver               activeGameplayDriver = GameplayDriver::Cpp;

    Engine::UICallback uiCallback = nullptr;
    UIDrawData                              pendingUIData {};
    std::vector<Engine::DeviceLostCallback> deviceLostCallbacks;

    std::vector<Engine::FrameSchedulerExtension> frameSchedulerExtensions;
    std::vector<Engine::SystemGraphsExtension>   systemGraphsExtensions;
    Engine::CharacterStepHooks                   characterStepHooks;
    Engine::FreeCamSpeedQuery                    freeCamSpeedQuery     = nullptr;
    BonePosePostProcessor                        bonePosePostProcessor = nullptr;
    std::vector<Engine::TeardownHook>            teardownHooks;
    std::vector<Engine::SceneCleanupPass>        sceneCleanupPasses;

    FrameScheduler scheduler;
    float          currentAlpha = 0.0f;
    float physicsAccumulator = 0.0f;

    // Created on the first frame that asks for a system context, because that is
    // the first moment the task system's worker count is known and is also
    // before any graph dispatch. Sized for the whole app run; ResetWorkerScratch
    // is what the frame loop does with it.
    std::unique_ptr<WorkerScratchPool> workerScratch;

    // The engine's services, bound once (they are references), and the two
    // typed graphs built with them. Created on first use: the graphs need the
    // world and the kernel to exist, and nothing runs before that.
    // The simulation -> renderer channel for skinning poses. Engine-owned, so
    // both graphs reach the same queue through services.
    PoseUploadQueue poseUploads;

    std::unique_ptr<SimServices>    simServices;
    std::unique_ptr<RenderServices> renderServices;
    std::unique_ptr<SimGraph>       updateGraph;
    std::unique_ptr<RenderGraph>    renderGraph;

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
        if (auto uiSettings = reg.GetSingleton<GUI::UISettingsComponent>()) {
            uiSettings->fontAtlas        = *_impl->fontAtlas;
            uiSettings->defaultFontAtlas = _impl->fontAtlas->texture;
        }
    } else {
        PrefabFactory::PrimeDefaultBakedFont(GetAssetManager());
        PrefabFactory::CreateFontAtlasTexture(
            GetRenderContext(), reg, GetAssetManager(), GUI::kDefaultFontAssetID
        );
        if (const auto uiSettings = reg.GetSingleton<GUI::UISettingsComponent>();
            uiSettings && uiSettings->fontAtlas.texture != TextureHandle::Invalid) {
            _impl->fontAtlas = uiSettings->fontAtlas;
        }
    }
}

namespace {


void DumpEngineState(void* context, const SignalEvent& ) noexcept {
    ZHLN::Trace(*static_cast<Engine*>(context));
}

void DumpCameraState(void* context, const SignalEvent& ) noexcept {
    auto& cam = *static_cast<Camera*>(context);

    auto cam_pos = ZHLN::Format("  Position:  ({}, {}, {})\n", cam.position.GetX(), cam.position.GetY(), cam.position.GetZ());
    auto cam_dir = ZHLN::Format("  Direction: Yaw: {}, Pitch: {}\n", cam.yaw, cam.pitch);
    Diagnostics::WriteCrashOutput(cam_pos);
    Diagnostics::WriteCrashOutput(cam_dir);

    auto& f         = cam.frustum;
    auto  frust_hdr = ZHLN::Format("\n{}--- FRUSTUM PLANE EQUATIONS (SIMD DECODED) ---{}\n", Color::Cyan, Color::Reset);
    Diagnostics::WriteCrashOutput(frust_hdr);
    const char* names[] = {"Left  ", "Right ", "Top   ", "Bottom", "Near  ", "Far   "};

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

void DumpPhysicsState(void* context, const SignalEvent& ) noexcept {
    static_cast<PhysicsContext*>(context)->TraceDiagnostics();
}

void RegisterCrashObservers(CrashState& state, Engine& engine, World& world) {
    Diagnostics::RegisterCrashObserver(state, "ENGINE", DumpEngineState, &engine);
    Diagnostics::RegisterCrashObserver(state, "CAMERA DEEP", DumpCameraState, &world.GetCamera());
    Diagnostics::RegisterCrashObserver(state, "PHYSICS", DumpPhysicsState, &world.GetPhysics());
}

}

Engine::Engine(): _impl(nullptr) {
}

auto Engine::HandleDeviceLost() noexcept -> std::expected<void, ErrorCode> {
    // The old context owns the old buffers. Never pass its handles to the
    // replacement context (generational slots may reuse the same numbers).
    auto& reg = _impl->world->GetRegistry();
    for (auto& emitter: reg.GetRawArray<Components::ParticleEmitterComponent>()) {
        emitter.gpuBuffer = BufferHandle::Invalid;
        emitter.bufferCapacity = 0;
    }
    for (auto& emitter: reg.GetRawArray<Components::MeshParticleEmitterComponent>()) {
        emitter.gpuBuffer = BufferHandle::Invalid;
        emitter.bufferCapacity = 0;
    }
    for (auto& skeleton: reg.GetRawArray<Components::SkeletalMeshComponent>()) {
        skeleton.skinnedScratch = BufferHandle::Invalid;
        skeleton.scratchVertexCount = 0;
    }
    for (auto& owned: reg.GetRawArray<Components::OwnedMeshComponent>()) {
        owned.mesh = {}; // the old context will reclaim its pool; never destroy these on the new device
    }

    if (auto rebuilt = _impl->kernel->HandleDeviceLost(); !rebuilt) {
        return std::unexpected(rebuilt.error());
    }
    PrefabFactory::RebuildVulkanResources(_impl->kernel->GetRenderContext(), _impl->world->GetRegistry());

    for (const auto& callback: _impl->deviceLostCallbacks) {
        if (callback) {
            callback(*this);
        }
    }
    return {};
}

auto Engine::Create(const EngineConfig& cfg) -> std::expected<std::unique_ptr<Engine>, ErrorCode> {
    auto instance = std::unique_ptr<Engine>(new (std::nothrow) Engine());
    if (!instance) {
        return std::unexpected(EngineInitError::EngineAllocationFailed);
    }

    if (auto result = instance->InitInternal(cfg); !result) {
        return std::unexpected(result.error());
    }
    return instance;
}

auto Engine::InitInternal(const EngineConfig& cfg) -> std::expected<void, ErrorCode> {
    ZHLN::Fiber::InitMainThread();

    _impl               = std::make_unique<EngineImpl>();
    _impl->config       = cfg;
    _impl->scriptRunner = std::make_unique<ScriptRunner>();
    _impl->scriptRunner->SetRuntimeChanged([this] { RegisterBootScriptWatches(); });

    auto world_res = World::Create(cfg.physics, true);
    if (!world_res) {
        return std::unexpected(world_res.error());
    }
    _impl->world = std::move(world_res.value());

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

    World*              worldPtr = _impl->world.get();
    WindowInputReceiver receiver = {
        .userdata = worldPtr, .onKey = onKey, .onMouseMove = onMouseMove, .onMouseScroll = onMouseScroll, .onResize = onResize, .onChar = onChar
    };

    auto kernel_res = Kernel::Create(cfg.render, receiver);
    if (!kernel_res) {
        return std::unexpected(kernel_res.error());
    }
    _impl->kernel = std::move(kernel_res.value());

    _impl->nativeScriptModule = std::make_unique<NativeScriptModule>(*this, "scripts/gameplay");

    if (_impl->config.crashState != nullptr) {
        RegisterCrashObservers(*_impl->config.crashState, *this, *_impl->world);
    }

    return {};
}

void Engine::RegisterBootScriptWatches() {
    if (_impl == nullptr || _impl->kernel == nullptr) {
        return;
    }

    for (const FS::FileWatchHandle handle: _impl->bootScriptWatches) {
        static_cast<void>(_impl->kernel->GetFileSystemWatcher().Unwatch(handle));
    }
    _impl->bootScriptWatches.clear();

    if (_impl->scriptRunner == nullptr) {
        return;
    }

    const auto reloadBootScript = [this](const FS::FileWatchEvent& event) {
        if (_impl->activeGameplayDriver == GameplayDriver::Cpp || event.action == FS::FileWatchAction::Deleted) {
            return;
        }
        _impl->scriptRunner->ReloadFile(event.path.string());
    };
    for (const std::string_view path: _impl->scriptRunner->BootScriptPaths()) {
        _impl->bootScriptWatches.push_back(_impl->kernel->GetFileSystemWatcher().WatchFile(std::filesystem::path(path), reloadBootScript));
    }
}

Engine::~Engine() {
    if (_impl == nullptr) {
        return;
    }

    if (_impl->config.crashState != nullptr) {
        Diagnostics::ClearCrashObservers(*_impl->config.crashState);
    }

    if (_impl->kernel != nullptr && _impl->world != nullptr) {
        for (const auto hook: _impl->teardownHooks) {
            hook(*this);
        }

        // Release external handles while both the World and Kernel still live.
        // The World destructor can safely clear its already-empty registry.
        ClearScene();
    }

    _impl->world.reset();
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

    auto& reg        = _impl->world->GetRegistry();
    auto  inputState = reg.GetSingleton<Components::InputStateComponent>();
    if (inputState) {
        inputState->ResetDeltas();
    }

    _impl->kernel->ProcessEvents();

    const auto& host = _impl->kernel->GetPlatformHost();
    if (inputState && !host.AsWindow() && host.HasNativeSurface()) {
        inputState->wantCaptureKeyboard = false;
        inputState->wantCaptureMouse    = false;
    }
}

void Engine::PollLateInput() {
    _impl->kernel->ProcessEvents();
}

auto Engine::GetCurrentFrame() const noexcept -> uint64_t {
    return _impl->frameCounter;
}

auto Engine::GetPlatformHost() noexcept -> PlatformHost& {
    return _impl->kernel->GetPlatformHost();
}

auto Engine::GetPlatformHost() const noexcept -> const PlatformHost& {
    return _impl->kernel->GetPlatformHost();
}

auto Engine::GetWindow() noexcept -> ZHLN::Optional<Window&> {
    return _impl->kernel->GetWindow();
}

auto Engine::AddWindow(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver)
    -> ZHLN::Optional<Window&> {
    return _impl->kernel->AddWindow(title, width, height, fullscreen, receiver);
}

void Engine::RemoveWindow(Window& window) {
    _impl->kernel->RemoveWindow(window);
}

auto Engine::AcquireTarget() noexcept -> FrameOutcome<FrameTarget> {
    return _impl->kernel->AcquireTarget();
}

auto Engine::AcquireTarget(Window& window) noexcept -> FrameOutcome<FrameTarget> {
    return _impl->kernel->AcquireTarget(window);
}

auto Engine::GetAcquiredTarget() noexcept -> std::optional<FrameTarget> {
    return _impl->kernel->GetAcquiredTarget();
}

auto Engine::GetAcquiredTarget(Window& window) noexcept -> std::optional<FrameTarget> {
    return _impl->kernel->GetAcquiredTarget(window);
}

auto Engine::GetKernel() -> Kernel& {
    return *_impl->kernel;
}
auto Engine::GetWorld() -> World& {
    return *_impl->world;
}

namespace {
// Per-worker scratch. A system's SoAScratch allocation is capped by this, so it
// is the number to raise when a system reports that its scratch does not fit --
// one arena per worker, so the process total is this times the worker count.
constexpr size_t kWorkerScratchBytes = 256 * 1024;
} // namespace

void Engine::EnsureSystemGraphs() {
    if (_impl->updateGraph != nullptr) {
        return;
    }

    auto& render = _impl->kernel->GetRenderContext();

    _impl->simServices = std::make_unique<SimServices>(
        SimServices {
            .physics               = _impl->world->GetPhysics(),
            .audio                 = _impl->kernel->GetAudioContext(),
            .assets                = _impl->kernel->GetAssetManager(),
            .camera                = _impl->world->GetCamera(),
            .articulation          = _impl->world->GetArticulationSystem(),
            .bonePosePostProcessor = _impl->bonePosePostProcessor,
            .poseUploads           = _impl->poseUploads,
        }
    );

    _impl->renderServices = std::make_unique<RenderServices>(
        RenderServices {
            .render  = render,
            .assets  = _impl->kernel->GetAssetManager(),
            .camera  = _impl->world->GetCamera(),
            .culling = _impl->world->GetCullingSystem(),
            .visible = VisibleEntities {_impl->world->GetVisibleEntities()},
            .shadow  = VisibleShadowEntities {_impl->world->GetVisibleShadowEntities()},
        }
    );

    _impl->updateGraph = std::make_unique<SimGraph>(_impl->world->GetRegistry(), *_impl->simServices);
    _impl->renderGraph = std::make_unique<RenderGraph>(_impl->world->GetRegistry(), *_impl->renderServices);
}

auto Engine::GetPoseUploads() -> PoseUploadQueue& {
    return _impl->poseUploads;
}

auto Engine::MakeFrame(float dt) -> Frame {
    if (_impl->workerScratch == nullptr) {
        _impl->workerScratch = std::make_unique<WorkerScratchPool>(kWorkerScratchBytes, TaskSystem::GetWorkerCount());
    }

    return Frame {
        .frame   = _impl->frameCounter,
        .alpha   = _impl->currentAlpha,
        .dt      = dt,
        .scratch = _impl->workerScratch.get(),
    };
}

void Engine::ResetWorkerScratch() noexcept {
    if (_impl->workerScratch != nullptr) {
        _impl->workerScratch->ResetAll();
    }
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
auto Engine::GetAssetManager() -> AssetManager& {
    return _impl->kernel->GetAssetManager();
}
auto Engine::GetAudioContext() -> AudioContext& {
    return _impl->kernel->GetAudioContext();
}
auto Engine::GetScriptRunner() -> ScriptRunner& {
    return *_impl->scriptRunner;
}
auto Engine::GetFileSystemWatcher() -> FS::FileSystemWatcher& {
    return _impl->kernel->GetFileSystemWatcher();
}
auto Engine::GetRegistry() -> ECS::Registry& {
    return _impl->world->GetRegistry();
}

auto Engine::GetRegistry() const -> const ECS::Registry& {
    return _impl->world->GetRegistry();
}

auto Engine::GetUpdateGraph() -> SimGraph& {
    EnsureSystemGraphs();
    return *_impl->updateGraph;
}

auto Engine::GetRenderGraph() -> RenderGraph& {
    EnsureSystemGraphs();
    return *_impl->renderGraph;
}
auto Engine::GetMainECB() -> ECS::EntityCommandBuffer& {
    return _impl->world->GetMainECB();
}

void Engine::ProcessPendingDestroy() {
    SceneCleanupSystem::ProcessPending(*this);
}

void Engine::ClearScene() {
    SceneCleanupSystem::ClearAll(*this);
}

auto Engine::AddSceneCleanupPass(SceneCleanupPass pass) -> bool {
    if (pass == nullptr || std::find(_impl->sceneCleanupPasses.begin(), _impl->sceneCleanupPasses.end(), pass) != _impl->sceneCleanupPasses.end()) {
        return false;
    }
    _impl->sceneCleanupPasses.push_back(pass);
    return true;
}

void Engine::RunSceneCleanupPasses(bool all) {
    for (const auto pass: _impl->sceneCleanupPasses) {
        pass(*this, all);
    }
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
auto Engine::GetPhysicsAccumulator() -> float& {
    return _impl->physicsAccumulator;
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

void Engine::SetPendingUIData(const UIDrawData& uiData) noexcept {
    _impl->pendingUIData = uiData;
}

auto Engine::GetPendingUIData() const noexcept -> UIDrawData {
    return _impl->pendingUIData;
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

void Engine::ApplySystemGraphsExtensions(SimGraph& updateGraph, RenderGraph& renderGraph) {
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

auto Engine::GetUICallback() const noexcept -> ZHLN::Optional<const UICallback&> {
    return _impl->uiCallback ? ZHLN::Optional<const UICallback&> {_impl->uiCallback} : ZHLN::Optional<const UICallback&> {std::nullopt};
}

void Engine::ProvokeDeviceLost() {
    _impl->kernel->ProvokeDeviceLost();
}

auto Engine::InitializeDefaultScene() -> bool {
    return ZHLN::InitializeDefaultScene(*this);
}

auto Engine::Tick(float dt, GameplayDriver driver) -> GameplayStatus {
    _impl->activeGameplayDriver = driver;

    FrameContext ctx {.driver = driver, .status = GameplayStatus::OK, .deviceLost = false};

    _impl->scheduler.Execute(*this, dt, ctx);

    _impl->frameCounter++;

    return ctx.status;
}

auto Engine::Run(const CommandLineOptions& options, CrashState& crashState, UICallback uiCallback, ExtensionInstaller installExtensions)
    -> std::expected<void, ErrorCode> {
    Platform::Init();
    ZHLN::SetupSignalHandler(crashState);
    // Construct before the engine so its destructor (and any cleanup tasks)
    // finishes before the worker threads and fibers are retired.
    TaskSystem::Scope tasks;

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
        return std::unexpected(engine_res.error());
    }

    auto engine = std::move(engine_res.value());
    engine->GetPlatformHost().Focus();

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
            if (auto st = r.GetSingleton<Components::InputStateComponent>(); st && st->needsResize) {
                engine->GetRenderContext().SetResolution(st->newSize);
                st->needsResize = false;
                continue;
            }
        }

        constexpr double           kFpsCapSlack = 1.05;
        const std::optional<float> pacedDt      = engine->GetRenderContext().GetPacedDeltaTime();
        const bool                 displayPaced = pacedDt.has_value() &&
                                 (options.fpsLimit <= 0 || targetFrameTime <= static_cast<double>(*pacedDt) * kFpsCapSlack);
        float tickDt = displayPaced ? *pacedDt : rawDt;

        GameplayStatus status = engine->Tick(tickDt, options.driver);
        if (status == GameplayStatus::RequestQuit) {
            engine->GetPlatformHost().Close();
            break;
        }

        if (options.fpsLimit > 0 && !displayPaced) {
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

    return {};
}

}
