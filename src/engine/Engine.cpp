// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArticulationSystem.hpp"
#include "CullingSystem.hpp"
#include "EngineGlobals.hpp"
#include "Hierarchy.hpp"
#include "NativeScriptModule.hpp"
#include "Platform.hpp"
#include "SystemWiring.hpp"
#include "SceneCleanupSystem.hpp"
#include <Zahlen/Audio.hpp>
#include <Zahlen/Camera.hpp>
#include <Zahlen/Core/Arena.hpp>
#include <Zahlen/Core/Diagnostics.hpp>
#include <Zahlen/Core/Trace.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/FramePlan.hpp>
#include <Zahlen/Input.hpp>
#include <Zahlen/Kernel.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/PlatformHost.hpp>
#include <Zahlen/Render/Render.hpp>
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

    std::unique_ptr<NativeScriptModule> nativeScriptModule;
    GameplayDriver                      activeGameplayDriver = GameplayDriver::Cpp;

    Engine::UICallback uiCallback = nullptr;
    UIDrawData                              pendingUIData {};
    std::vector<Engine::DeviceLostCallback> deviceLostCallbacks;

    // The composition: what the application installed, in install order, and the
    // timeline those modules' declared steps resolved into. The engine keeps the
    // Module values rather than a list of callbacks, so a graph rebuild can ask
    // each module for its systems and a report can say what is running.
    std::vector<Module> modules;
    FramePlan           framePlan;
    bool                framePlanBuilt = false;

    Engine::PhysicsSubstepHooks                  physicsSubstepHooks;
    ECS::ExtensionServices                       extensions;
    std::vector<Engine::TeardownHook>            teardownHooks;
    std::vector<Engine::SceneCleanupPass>        sceneCleanupPasses;

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
    SceneData       sceneData;

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

void Engine::TraceDiagnostics() const noexcept {
    const auto writeSection = [](std::string_view title) noexcept {
        auto header = ZHLN::Format("\n{}--- {} STATE ---{}\n", Color::Cyan, title, Color::Reset);
        Diagnostics::WriteDiagnosticError(header.string_view());
    };

    writeSection("ENGINE");
    ZHLN::Trace(*this);

    if (_impl == nullptr || _impl->world == nullptr) {
        return;
    }

    writeSection("CAMERA DEEP");
    auto&      world   = *_impl->world;
    const auto camComp = world.GetRegistry().GetSingleton<Components::CameraComponent>();

    if (camComp) {
        const Camera& cam     = camComp->camera;
        auto          cam_pos = ZHLN::Format("  Position:  ({}, {}, {})\n", cam.position.GetX(), cam.position.GetY(), cam.position.GetZ());
        auto          cam_dir = ZHLN::Format("  Direction: Yaw: {}, Pitch: {}\n", cam.yaw, cam.pitch);
        Diagnostics::WriteDiagnosticError(cam_pos.string_view());
        Diagnostics::WriteDiagnosticError(cam_dir.string_view());
    } else {
        Diagnostics::WriteDiagnosticError("  (no main camera entity)\n");
    }

    Frustum    f {};
    const auto stats  = world.GetRegistry().GetSingleton<Components::CullingStatsComponent>();
    const bool frozen = stats && stats->stats.FreezeFrustum;
    if (camComp) {
        f.Update(frozen ? camComp->frozenViewProj : camComp->unjitteredViewProj);
    }
    auto        frust_hdr = ZHLN::Format("\n{}--- FRUSTUM PLANE EQUATIONS (SIMD DECODED) ---{}\n", Color::Cyan, Color::Reset);
    const char* names[]   = {"Left  ", "Right ", "Top   ", "Bottom", "Near  ", "Far   "};
    Diagnostics::WriteDiagnosticError(frust_hdr.string_view());

    for (int i = 0; i < 6; ++i) {
        const int block     = i / 4;
        const int lane      = i % 4;
        auto      plane_str = ZHLN::Format(
            "  Plane {}: [{}x {}y {}z] offset: {}\n", names[i], f.mX[block].mF32[lane], f.mY[block].mF32[lane], f.mZ[block].mF32[lane], f.mW[block].mF32[lane]
        );
        Diagnostics::WriteDiagnosticError(plane_str.string_view());
    }

    ZHLN::Dump(f);

    writeSection("PHYSICS");
    world.GetPhysics().TraceDiagnostics();
}

Engine::Engine(): _impl(nullptr) {
}

auto Engine::HandleDeviceLost() noexcept -> std::expected<void, ErrorCode> {
    // Dropping the old RenderContext retires emitter buffers in Destroy(); the
    // replacement renderer is created after that. Component authoring data survives.
    auto& reg = _impl->world->GetRegistry();
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

    _impl         = std::make_unique<EngineImpl>();
    _impl->config = cfg;

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

    return {};
}

Engine::~Engine() {
    if (_impl == nullptr) {
        return;
    }

    if (_impl->kernel != nullptr && _impl->world != nullptr) {
        for (const auto hook: _impl->teardownHooks) {
            hook(*this);
        }

        // Release external handles while both the World and Kernel still live.
        // The World destructor can safely clear its already-empty registry.
        ClearScene();
    }

    _impl->updateGraph.reset();
    _impl->renderGraph.reset();
    _impl->extensions.Clear();
    _impl->world.reset();
    _impl->nativeScriptModule.reset();
    _impl->kernel.reset();
}

auto Engine::IsRunning() const -> bool {
    return _impl->kernel->IsRunning();
}

void Engine::ProcessEvents() {
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
            .physics      = _impl->world->GetPhysics(),
            .audio        = _impl->kernel->GetAudioContext(),
            .assets       = _impl->kernel->GetAssetManager(),
            .articulation = _impl->world->GetArticulationSystem(),
            .extensions   = _impl->extensions,
            .poseUploads  = _impl->poseUploads,
        }
    );

    _impl->renderServices = std::make_unique<RenderServices>(
        RenderServices {
            .render     = render,
            .scene      = _impl->sceneData,
            .assets     = _impl->kernel->GetAssetManager(),
            .extensions = _impl->extensions,
            .visible    = VisibleEntities {_impl->world->GetVisibleEntities()},
            .shadow     = VisibleShadowEntities {_impl->world->GetVisibleShadowEntities()},
        }
    );

    _impl->updateGraph = std::make_unique<SimGraph>(_impl->world->GetRegistry(), *_impl->simServices, &MarkPendingDestroy);
    _impl->renderGraph = std::make_unique<RenderGraph>(_impl->world->GetRegistry(), *_impl->renderServices, &MarkPendingDestroy);
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
auto Engine::GetSceneData() -> SceneData& {
    return _impl->sceneData;
}
auto Engine::GetAssetManager() -> AssetManager& {
    return _impl->kernel->GetAssetManager();
}
auto Engine::GetAudioContext() -> AudioContext& {
    return _impl->kernel->GetAudioContext();
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

auto Engine::InstallModule(const Module& module) -> std::expected<void, FramePlanError> {
    if (!module.name.empty() &&
        std::ranges::any_of(_impl->modules, [&](const Module& existing) { return existing.name == module.name; })) {
        return {};
    }

    // The plan first, and on a plan that core has already seeded: a module's
    // steps are placed against names core owns, and placing them can fail. Doing
    // it before anything else is applied is what makes the failure clean -- a
    // module whose steps cannot be placed registers nothing at all.
    EnsureFramePlan();
    if (auto composed = _impl->framePlan.Compose(module.frameSteps); !composed) {
        return composed;
    }

    if (module.registerComponents != nullptr) {
        module.registerComponents(GetRegistry());
    }
    if (module.sceneCleanup != nullptr) {
        (void)AddSceneCleanupPass(module.sceneCleanup);
    }
    if (module.teardown != nullptr) {
        AddTeardownHook(module.teardown);
    }

    _impl->modules.push_back(module);
    if (module.addGraphSystems != nullptr && _impl->updateGraph != nullptr) {
        BuildSystemGraphs(*this);
    }
    return {};
}

auto Engine::GetInstalledModules() const noexcept -> std::span<const Module> {
    return _impl->modules;
}

auto Engine::GetFramePlan() -> FramePlan& {
    EnsureFramePlan();
    return _impl->framePlan;
}

auto Engine::GetFramePlan() const noexcept -> const FramePlan& {
    return _impl->framePlan;
}

void Engine::EnsureFramePlan() {
    if (_impl->framePlanBuilt) {
        return;
    }
    _impl->framePlanBuilt = true;
    BuildCoreFramePlan(*this);
}

void Engine::SetPhysicsSubstepHooks(PhysicsSubstepHooks hooks) {
    _impl->physicsSubstepHooks = hooks;
}

auto Engine::GetPhysicsSubstepHooks() const noexcept -> const PhysicsSubstepHooks& {
    return _impl->physicsSubstepHooks;
}

auto Engine::Services() noexcept -> ECS::ExtensionServices& {
    return _impl->extensions;
}

auto Engine::Services() const noexcept -> const ECS::ExtensionServices& {
    return _impl->extensions;
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

    // The plan is whatever the composition produced: core's own steps, plus one
    // set of steps per module the application installed. The engine executes it
    // and nothing else -- it holds no callbacks to replay and no ordering of its
    // own beyond the core steps BuildCoreFramePlan places.
    EnsureFramePlan();
    _impl->framePlan.Execute(*this, dt, ctx);

    _impl->frameCounter++;

    return ctx.status;
}

auto Engine::Run(const RunConfig& runConfig, UICallback uiCallback, ModuleInstaller installModules)
    -> std::expected<void, ErrorCode> {
    Platform::Init();
    // Construct before the engine so its destructor (and any cleanup tasks)
    // finishes before the worker threads and fibers are retired.
    TaskSystem::Scope tasks;

    uint32_t w = runConfig.fullscreen ? 0 : 1280;
    uint32_t h = runConfig.fullscreen ? 0 : 720;

    EngineConfig config {
        .physics = {.maxBodies = 5000, .maxBodyPairs = 10000, .maxContactConstraints = 10000, .tempAllocatorSize = 64 * 1024 * 1024},
        .render =
            {
                .appName        = String64(runConfig.appName),
                .width          = w,
                .height         = h,
                .vsync          = runConfig.vsync,
                .fullscreen     = runConfig.fullscreen,
                .validationMode = runConfig.validationMode,
                .headless       = runConfig.headless,
            },
    };

    auto engine_res = Engine::Create(config);
    if (!engine_res) {
        return std::unexpected(engine_res.error());
    }

    auto engine = std::move(engine_res.value());
    engine->GetPlatformHost().Focus();

    // The application's install pass, before the first scene: modules contribute
    // components and declare their frame steps, and InitializeDefaultScene then
    // builds the graphs with the systems they added.
    if (installModules != nullptr) {
        installModules(*engine);
    }

    engine->InitializeDefaultScene();

    if (uiCallback) {
        engine->SetUICallback(std::move(uiCallback));
    }

    const double targetFrameTime = runConfig.fpsLimit > 0 ? 1.0 / static_cast<double>(runConfig.fpsLimit) : 0.0;
    auto         frameStart      = std::chrono::high_resolution_clock::now();

    while (engine->IsRunning()) {
        if (runConfig.onFrameEvents) {
            runConfig.onFrameEvents(*engine);
        }
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

        constexpr double           kFpsCapSlack  = 1.05;
        const std::optional<float> pacedDt       = engine->GetRenderContext().GetPacedDeltaTime();
        const bool                 displayPaced  = pacedDt.has_value() &&
                                  (runConfig.fpsLimit <= 0 || targetFrameTime <= static_cast<double>(*pacedDt) * kFpsCapSlack);
        float tickDt = displayPaced ? *pacedDt : rawDt;

        GameplayStatus status = engine->Tick(tickDt, runConfig.driver);
        if (status == GameplayStatus::RequestQuit) {
            engine->GetPlatformHost().Close();
            break;
        }

        if (runConfig.fpsLimit > 0 && !displayPaced) {
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
