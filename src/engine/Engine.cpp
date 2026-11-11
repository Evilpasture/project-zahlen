// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArticulationSystem.hpp"
#include "CullingSystem.hpp"
#include "EngineGlobals.hpp"
#include "Hierarchy.hpp"
#include "Platform.hpp"
#include "SceneCleanupSystem.hpp"
#include "SystemWiring.hpp"
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/Core/Arena.hpp>
#include <Zahlen/Core/Diagnostics.hpp>
#include <Zahlen/Core/Trace.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/ECS/EntityCommandBuffer.hpp>
#include <Zahlen/ECS/EventBus.hpp>
#include <Zahlen/ECS/SystemGraph.hpp>
#include <Zahlen/Engine/AssetManager.hpp>
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/AudioEngine.hpp>
#include <Zahlen/Engine/Capabilities/DebugDraw.hpp>
#include <Zahlen/Engine/Capabilities/EngineState.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Clock.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/Frame.hpp>
#include <Zahlen/Engine/FramePlan.hpp>
#include <Zahlen/Engine/Internal/EngineAccess.hpp>
#include <Zahlen/Engine/Kernel.hpp>
#include <Zahlen/Engine/PoseUploads.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Engine/VisibilityList.hpp>
#include <Zahlen/Engine/World.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/GUI/FontLoader.hpp>
#include <Zahlen/GUI/GUI.hpp>
#include <Zahlen/Input/Input.hpp>
#include <Zahlen/Input/InputContext.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Window/PlatformHost.hpp>
#include <Zahlen/Window/Window.hpp>
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

enum class EngineRunError : uint8_t {
    SceneInitializationFailed ZHLN_ANNOTATION(ZHLN::Description<"Engine default scene initialization failed"> {}) = 1,
};

struct EngineImpl {
    std::unique_ptr<Kernel> kernel;
    std::unique_ptr<World>  world;

    Engine::UICallback                      uiCallback = nullptr;
    UIDrawData                              pendingUIData {};
    std::vector<Engine::DeviceLostCallback> deviceLostCallbacks;

    // The composition: what the application installed, in install order, and the
    // timeline those modules' declared steps resolved into. The engine keeps the
    // Module values rather than a list of callbacks, so a graph rebuild can ask
    // each module for its systems and a report can say what is running.
    std::vector<Module> modules;
    FramePlan           framePlan;
    bool                framePlanBuilt = false;

    Engine::PhysicsSubstepHooks           physicsSubstepHooks;
    ECS::ExtensionServices                extensions;
    std::vector<Engine::TeardownHook>     teardownHooks;
    std::vector<Engine::SceneCleanupPass> sceneCleanupPasses;

    // The simulation clock owns the fixed-step accumulator. Engine::Tick feeds
    // it once per frame and drains it with ConsumeFixedSteps; no mutable alpha
    // or accumulator is stored on the engine any more.
    Clock frameClock;

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

    FontAtlas             fontAtlas         = GUI::BuildFontAtlas(GUI::GetDefaultBakedFont(), TextureID::Invalid);
    bool                  fontAtlasUploaded = false;
    std::vector<KeyCode>  uiPressedKeys;
    std::vector<uint32_t> uiCharacterStream;

    void*        gameState    = nullptr;
    uint64_t     frameCounter = 0;
    // Read by the pacing loop in Run() on every iteration; settable at any
    // time, which is why it is state here rather than a field of config.
    uint32_t     maxFramesPerSecond = 0;
    EngineConfig config;
};

void Engine::SetMaxFramesPerSecond(uint32_t fps) noexcept {
    _impl->maxFramesPerSecond = fps;
}

auto Engine::MaxFramesPerSecond() const noexcept -> uint32_t {
    return _impl->maxFramesPerSecond;
}

void Engine::EnsureFontAtlas() {
    if (_impl->fontAtlasUploaded) {
        return;
    }
    PrefabFactory::PrimeDefaultBakedFont(_impl->kernel->GetAssetManager());
    const FontAtlas uploaded = PrefabFactory::CreateFontAtlas(_impl->kernel->GetRenderContext(), _impl->kernel->GetAssetManager(), GUI::kDefaultFontAssetID);
    if (uploaded.glyphCount > 0) {
        _impl->fontAtlas = uploaded;
    }
    _impl->fontAtlasUploaded = true;
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
        auto          cam_pos = ZHLN::Format("  Position:  ({}, {}, {})\n", cam.position.x, cam.position.y, cam.position.z);
        auto          cam_dir = ZHLN::Format("  Direction: Yaw: {}, Pitch: {}\n", cam.yaw.value, cam.pitch.value);
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
            "  Plane {}: [{}x {}y {}z] offset: {}\n", names[i], f.mX[static_cast<size_t>(block)][static_cast<size_t>(lane)], f.mY[static_cast<size_t>(block)][static_cast<size_t>(lane)], f.mZ[static_cast<size_t>(block)][static_cast<size_t>(lane)], f.mW[static_cast<size_t>(block)][static_cast<size_t>(lane)]
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
        skeleton.skinnedScratch     = BufferID::Invalid;
        skeleton.scratchVertexCount = 0;
    }
    for (auto& owned: reg.GetRawArray<Components::OwnedMeshComponent>()) {
        owned.mesh = {}; // the old context will reclaim its pool; never destroy these on the new device
    }

    if (auto rebuilt = _impl->kernel->HandleDeviceLost(); !rebuilt) {
        return std::unexpected(rebuilt.error());
    }
    PrefabFactory::RebuildVulkanResources(Graphics(), _impl->world->GetRegistry());
    _impl->fontAtlasUploaded = false;
    EnsureFontAtlas();

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
    _impl->config   = cfg;
    // Seeded once here; from now on the engine's own value is authoritative
    // and EngineConfig is never consulted for pacing again.
    _impl->maxFramesPerSecond = cfg.fpsLimit;

    auto world_res = World::Create(cfg.physics, true);
    if (!world_res) {
        return std::unexpected(world_res.error());
    }
    _impl->world = std::move(world_res.value());

    auto onKey = [](void* userdata, KeyCode key, bool pressed) -> void {
        auto* world = static_cast<World*>(userdata);
        auto* reg   = &world->GetRegistry();
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->SetKey(static_cast<uint8_t>(key), pressed);
        if (pressed) {
            state->QueueKeyPress(key);
        }
        world->GetInput().InjectKey(key, pressed);
    };

    auto onMouseMove = [](void* userdata, float x, float y) -> void {
        auto* world = static_cast<World*>(userdata);
        auto* reg   = &world->GetRegistry();
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->ApplyLocalMotion(x, y);
        world->GetInput().InjectMouseMove(x, y);
    };

    auto onMouseRelative = [](void* userdata, float x, float y, float dx, float dy) -> void {
        auto* world = static_cast<World*>(userdata);
        auto* reg   = &world->GetRegistry();
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->ApplyRelativeMotion(x, y, dx, dy);
        world->GetInput().InjectMouseRelative(x, y, dx, dy);
    };

    auto onMouseScroll = [](void* userdata, float delta) -> void {
        auto* world = static_cast<World*>(userdata);
        auto* reg   = &world->GetRegistry();
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->ApplyWheel(delta);
        world->GetInput().InjectMouseScroll(delta);
    };

    auto onResize = [](void* userdata, Extent2D extent) -> void {
        auto* reg   = &static_cast<World*>(userdata)->GetRegistry();
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->ApplyResize(extent);
    };

    auto onChar = [](void* userdata, unsigned int codepoint) -> void {
        auto* world = static_cast<World*>(userdata);
        auto* reg   = &world->GetRegistry();
        auto* state = &reg->GetOrEmplaceSingleton<Components::InputStateComponent>();
        state->QueueChar(codepoint);
        world->GetInput().InjectChar(codepoint);
    };

    auto onGamepadConnection = [](void* userdata, uint8_t slot, bool connected, std::string_view name) -> void {
        static_cast<World*>(userdata)->GetInput().InjectGamepadConnection(slot, connected, name);
    };

    auto onGamepadState = [](void* userdata, uint8_t slot, const GamepadRawSample& sample) -> void {
        static_cast<World*>(userdata)->GetInput().InjectGamepadSample(slot, sample);
    };

    World*              worldPtr = _impl->world.get();
    WindowInputReceiver receiver = {
        .userdata            = worldPtr,
        .onKey               = onKey,
        .onMouseMove         = onMouseMove,
        .onMouseRelative     = onMouseRelative,
        .onMouseScroll       = onMouseScroll,
        .onResize            = onResize,
        .onChar              = onChar,
        .onGamepadConnection = onGamepadConnection,
        .onGamepadState      = onGamepadState,
    };

    auto kernel_res = Kernel::Create(cfg, receiver);
    if (!kernel_res) {
        return std::unexpected(kernel_res.error());
    }
    _impl->kernel = std::move(kernel_res.value());

    // World and Kernel outlive the binding; it must exist before the lazy
    // SimServices graph is created and before the first Engine::Physics call.
    _impl->world->GetPhysics().BindCollisionAssetResolver(
        [](void* context, CollisionAssetID id) noexcept -> std::shared_ptr<const Physics::MeshGeometry> {
            return static_cast<AssetManager*>(context)->FindCollisionMesh(id);
        },
        &_impl->kernel->GetAssetManager()
    );

    _impl->world->GetInput().SetRumbleSink(
        [](void* userdata, uint8_t slot, const GamepadRumble& rumble) noexcept { static_cast<PlatformHost*>(userdata)->SetGamepadRumble(slot, rumble); },
        &_impl->kernel->GetPlatformHost()
    );

    // Core's steps are composition state, not scene state: build them once here
    // so the const, noexcept Steps() view is always a complete schedule.
    EnsureFramePlan();

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
    _impl->kernel.reset();
}

auto Engine::IsRunning() const -> bool {
    return _impl->kernel->IsRunning();
}

void Engine::ProcessEvents() {
    auto& reg        = _impl->world->GetRegistry();
    auto& input      = _impl->world->GetInput();
    auto  inputState = reg.GetSingleton<Components::InputStateComponent>();

    input.BeginFrame();
    if (inputState) {
        inputState->BeginFrame();
    }

    _impl->kernel->ProcessEvents();
    SynchronizeResize();

    if (inputState) {
        input.SyncCaptureFlags(inputState->wantCaptureKeyboard, inputState->wantCaptureMouse);
    }
    input.RefreshActionsForCurrentFrame(&_impl->world->GetEventBus());
}

void Engine::SynchronizeResize() {
    auto state = _impl->world->GetRegistry().GetSingleton<Components::InputStateComponent>();
    if (!state || !state->needsResize) {
        return;
    }
    const Extent2D actual = _impl->kernel->GetPlatformHost().GetSize();
    if (state->newSize.width != actual.width || state->newSize.height != actual.height) {
        // Headless/TTY receivers can submit a size without a primary-window
        // extent change; windowed targets were already handled by the Kernel.
        _impl->kernel->GetRenderContext().SetResolution(state->newSize);
    }
    state->needsResize = false;
}

void Engine::PollLateInput() {
    _impl->kernel->ProcessEvents();
    SynchronizeResize();
    auto& input = _impl->world->GetInput();
    if (auto inputState = _impl->world->GetRegistry().GetSingleton<Components::InputStateComponent>()) {
        input.SyncCaptureFlags(inputState->wantCaptureKeyboard, inputState->wantCaptureMouse);
    }
    input.RefreshActionsForCurrentFrame(&_impl->world->GetEventBus());
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

// --- Capability facades -------------------------------------------------------
// Each facade is a value carrying a pointer to an engine-owned subsystem. The
// engine keeps ownership; nothing here returns a mutable reference to a
// subsystem, and the concrete types are not named in the public header.

auto Engine::Graphics() noexcept -> GraphicsDevice {
    return GraphicsDevice {_impl->kernel->GetRenderContext()};
}
auto Engine::Physics() noexcept -> Simulation {
    return Simulation {_impl->world->GetPhysics()};
}
auto Engine::Debug() noexcept -> DebugDraw {
    return DebugDraw {_impl->sceneData};
}
auto Engine::Assets() noexcept -> AssetLibrary {
    return AssetLibrary {_impl->kernel->GetAssetManager()};
}
auto Engine::Audio() noexcept -> AudioEngine {
    return AudioEngine {_impl->kernel->GetAudioContext()};
}
auto Engine::State() noexcept -> ZHLN::EngineState {
    return ZHLN::EngineState {*this};
}

// Build-tree-only seam for engine implementation units and in-repo tooling.
auto ZHLN::EngineAccess::KernelOf(Engine& e) noexcept -> Kernel& {
    return *e._impl->kernel;
}
auto ZHLN::EngineAccess::WorldOf(Engine& e) noexcept -> World& {
    return *e._impl->world;
}
auto ZHLN::EngineAccess::RenderOf(Engine& e) noexcept -> RenderContext& {
    return e._impl->kernel->GetRenderContext();
}
auto ZHLN::EngineAccess::PhysicsOf(Engine& e) noexcept -> PhysicsContext& {
    return e._impl->world->GetPhysics();
}
auto ZHLN::EngineAccess::AssetsOf(Engine& e) noexcept -> AssetManager& {
    return e._impl->kernel->GetAssetManager();
}
auto ZHLN::EngineAccess::AudioOf(Engine& e) noexcept -> AudioContext& {
    return e._impl->kernel->GetAudioContext();
}
auto ZHLN::EngineAccess::SceneOf(Engine& e) noexcept -> SceneData& {
    return e._impl->sceneData;
}
auto ZHLN::EngineAccess::PhysicsOf(Simulation& sim) noexcept -> PhysicsContext& {
    return *sim._ctx;
}
auto ZHLN::EngineAccess::RegistryOf(Engine& e) noexcept -> ECS::Registry& {
    return e.GetInternalRegistry();
}
auto ZHLN::EngineAccess::RegistryOf(const Engine& e) noexcept -> const ECS::Registry& {
    return e.GetInternalRegistry();
}
auto ZHLN::EngineAccess::PoseUploadsOf(Engine& e) noexcept -> PoseUploadQueue& {
    return e._impl->poseUploads;
}
auto ZHLN::EngineAccess::ArticulationOf(Engine& e) noexcept -> ArticulationSystem& {
    return e._impl->world->GetArticulationSystem();
}
auto ZHLN::EngineAccess::FramePlanOf(Engine& e) -> FramePlan& {
    e.EnsureFramePlan();
    return e._impl->framePlan;
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

    _impl->simServices = std::make_unique<SimServices>(SimServices {
        .physics      = _impl->world->GetPhysics(),
        .audio        = _impl->kernel->GetAudioContext(),
        .assets       = _impl->kernel->GetAssetManager(),
        .input        = _impl->world->GetInput(),
        .articulation = _impl->world->GetArticulationSystem(),
        .events       = _impl->world->GetEventBus(),
        .extensions   = _impl->extensions,
        .poseUploads  = _impl->poseUploads,
    });

    _impl->renderServices = std::make_unique<RenderServices>(RenderServices {
        .render     = render,
        .scene      = _impl->sceneData,
        .assets     = _impl->kernel->GetAssetManager(),
        .extensions = _impl->extensions,
        .visible    = VisibleEntities {_impl->world->VisibleEntitiesForServices()},
        .shadow     = VisibleShadowEntities {_impl->world->VisibleShadowEntitiesForServices()},
    });

    _impl->updateGraph = std::make_unique<SimGraph>(_impl->world->GetRegistry(), *_impl->simServices, &MarkPendingDestroy);
    _impl->renderGraph = std::make_unique<RenderGraph>(_impl->world->GetRegistry(), *_impl->renderServices, &MarkPendingDestroy);
}

auto Engine::MakeFrame(float dt, FrameAlpha alpha) -> Frame {
    if (_impl->workerScratch == nullptr) {
        _impl->workerScratch = std::make_unique<WorkerScratchPool>(kWorkerScratchBytes, TaskSystem::GetWorkerCount());
    }

    return Frame {
        .frame   = FrameIndex {_impl->frameCounter},
        .alpha   = alpha,
        .dt      = FrameDt {dt},
        .scratch = _impl->workerScratch.get(),
    };
}

void Engine::ResetWorkerScratch() noexcept {
    if (_impl->workerScratch != nullptr) {
        _impl->workerScratch->ResetAll();
    }
}

auto Engine::GetInput() noexcept -> InputContext& {
    return _impl->world->GetInput();
}
auto Engine::GetInput() const noexcept -> const InputContext& {
    return _impl->world->GetInput();
}
auto Engine::GetFileSystemWatcher() -> FS::FileSystemWatcher& {
    return _impl->kernel->GetFileSystemWatcher();
}
auto Engine::GetInternalRegistry() noexcept -> ECS::Registry& {
    return _impl->world->GetRegistry();
}

auto Engine::GetInternalRegistry() const noexcept -> const ECS::Registry& {
    return _impl->world->GetRegistry();
}

auto Engine::Entities() noexcept -> ZHLN::Entities {
    return ZHLN::Entities {*this};
}

auto Engine::Entities() const noexcept -> ZHLN::Entities {
    // const overload because Entities is a lightweight value-type facade whose Get and GetSingleton methods are already const
    return ZHLN::Entities {const_cast<Engine&>(*this)};
}

auto Engine::GetUpdateGraph() -> SimGraph& {
    EnsureSystemGraphs();
    return *_impl->updateGraph;
}

auto Engine::GetRenderGraph() -> RenderGraph& {
    EnsureSystemGraphs();
    return *_impl->renderGraph;
}
auto Engine::Commands() noexcept -> ECS::EntityCommandBuffer& {
    return _impl->world->GetMainECB();
}
auto Engine::GetEventBus() noexcept -> ECS::EventBus& {
    return _impl->world->GetEventBus();
}
auto Engine::GetEventBus() const noexcept -> const ECS::EventBus& {
    return _impl->world->GetEventBus();
}

void Engine::ProcessPendingDestroy() {
    SceneCleanupSystem::ProcessPending(*this);
}

void Engine::ClearScene() {
    SceneCleanupSystem::ClearAll(*this);
    // Queued destroys and any surviving bodies retain their own shapes.
    Physics().ClearMeshShapeCache();
    ClearVisibilityLists();
    _impl->world->GetEventBus().ClearEvents();
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

void Engine::ClearVisibilityLists() noexcept {
    _impl->world->VisibleEntitiesForServices().Clear();
    _impl->world->VisibleShadowEntitiesForServices().Clear();
}

auto Engine::GetVisibleEntities() const noexcept -> std::span<const Entity> {
    return _impl->world->GetVisibleEntities();
}
auto Engine::GetVisibleShadowEntities() const noexcept -> std::span<const Entity> {
    return _impl->world->GetVisibleShadowEntities();
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

void Engine::SetPendingUIData(UIDrawData uiData) noexcept {
    _impl->pendingUIData = uiData;
}

auto Engine::GetPendingUIData() const noexcept -> UIDrawData {
    return _impl->pendingUIData;
}

void Engine::SetFontAtlas(const FontAtlas& fontAtlas) noexcept {
    _impl->fontAtlas         = fontAtlas;
    _impl->fontAtlasUploaded = true;
}

auto Engine::GetFontAtlas() -> const FontAtlas& {
    EnsureFontAtlas();
    return _impl->fontAtlas;
}

auto Engine::GetFontAtlas() const noexcept -> const FontAtlas& {
    return _impl->fontAtlas;
}

auto Engine::ConsumeUIInput(float dt) noexcept -> GUI::UIInput {
    _impl->uiPressedKeys.clear();
    _impl->uiCharacterStream.clear();

    GUI::UIInput out {
        .viewport  = GetPlatformHost().GetSize(),
        .dt        = dt,
        .clipboard = {
            .userdata = &GetPlatformHost(),
            .set      = [](void* ud, std::string_view text) -> void { static_cast<PlatformHost*>(ud)->SetClipboardText(text); },
            .get      = [](void* ud) -> std::string { return static_cast<PlatformHost*>(ud)->GetClipboardText(); },
        },
    };

    if (auto input = _impl->world->GetRegistry().GetSingleton<Components::InputStateComponent>()) {
        out.mouseX       = input->mouseX;
        out.mouseY       = input->mouseY;
        out.mouseScroll  = input->mouseWheel;
        out.mouseDown[0] = input->IsMouseButtonDownRaw(static_cast<uint8_t>(KeyCode::LButton));
        out.mouseDown[1] = input->IsMouseButtonDownRaw(static_cast<uint8_t>(KeyCode::RButton));
        out.mouseDown[2] = input->IsMouseButtonDownRaw(static_cast<uint8_t>(KeyCode::MButton));

        KeyModifiers mods = KeyModifiers::None;
        if (input->IsKeyDownRaw(static_cast<uint8_t>(KeyCode::LShift)) || input->IsKeyDownRaw(static_cast<uint8_t>(KeyCode::RShift))) {
            mods |= KeyModifiers::Shift;
        }
        if (input->IsKeyDownRaw(static_cast<uint8_t>(KeyCode::LControl)) || input->IsKeyDownRaw(static_cast<uint8_t>(KeyCode::RControl))) {
            mods |= KeyModifiers::Ctrl;
        }
        if (input->IsKeyDownRaw(static_cast<uint8_t>(KeyCode::LAlt)) || input->IsKeyDownRaw(static_cast<uint8_t>(KeyCode::RAlt))) {
            mods |= KeyModifiers::Alt;
        }
        if (input->IsKeyDownRaw(static_cast<uint8_t>(KeyCode::LSuper)) || input->IsKeyDownRaw(static_cast<uint8_t>(KeyCode::RSuper))) {
            mods |= KeyModifiers::Super;
        }
        out.modifiers = mods;

        for (size_t i = 0; i < input->queuedInputCount; ++i) {
            const auto& queued = input->queuedInput[i];
            if (queued.isChar) {
                _impl->uiCharacterStream.push_back(queued.value);
            } else {
                _impl->uiPressedKeys.push_back(static_cast<KeyCode>(queued.value));
            }
        }
        input->ClearQueuedInput();
    }

    out.pressedKeys     = _impl->uiPressedKeys;
    out.characterStream = _impl->uiCharacterStream;
    return out;
}

void Engine::SubmitUI(UIDrawData uiData) noexcept {
    SetPendingUIData(uiData);
    if (auto inputState = _impl->world->GetRegistry().GetSingleton<Components::InputStateComponent>()) {
        inputState->wantCaptureKeyboard = uiData.wantsCaptureKeyboard;
        inputState->wantCaptureMouse    = uiData.wantsCaptureMouse;
    }
    _impl->world->GetInput().SyncCaptureFlags(uiData.wantsCaptureKeyboard, uiData.wantsCaptureMouse);
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
    if (!module.name.empty() && std::ranges::any_of(_impl->modules, [&](const Module& existing) { return existing.name == module.name; })) {
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
        module.registerComponents(GetInternalRegistry());
    }
    if (module.sceneCleanup != nullptr) {
        (void) AddSceneCleanupPass(module.sceneCleanup);
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

auto Engine::Steps() const noexcept -> std::span<const FrameStep> {
    if (_impl == nullptr) {
        return {};
    }
    return _impl->framePlan.Steps();
}

void Engine::EnsureFramePlan() {
    if (_impl->framePlanBuilt) {
        return;
    }
    _impl->framePlanBuilt = true;
    BuildCoreFramePlan(_impl->framePlan);
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

auto Engine::Tick(float dt) -> std::expected<void, ErrorCode> {
    _impl->world->GetInput().StepHaptics(dt);

    // The clock owns the fixed-step accumulator and is drained exactly once per
    // frame, here, before any step of the plan can run. The physics step runs
    // `fixedSteps` steps; the leftover fraction is the frame's interpolation
    // alpha. Nothing writes an alpha into the engine between phases.
    _impl->frameClock.Advance(static_cast<double>(dt));
    const FixedStepBatch batch = _impl->frameClock.ConsumeFixedSteps(kFixedStepDt, kMaxFixedSteps);
    FrameContext         ctx {
        .error      = std::nullopt,
        .fixedSteps = batch.steps,
        .alpha      = FrameAlpha {batch.alpha},
    };

    // The plan is whatever the composition produced: core's own steps, plus one
    // set of steps per module the application installed. The engine executes it
    // and nothing else -- it holds no callbacks to replay and no ordering of its
    // own beyond the core steps BuildCoreFramePlan places.
    EnsureFramePlan();
    _impl->framePlan.Execute(*this, dt, ctx);

    _impl->frameCounter++;

    if (ctx.error) {
        return std::unexpected(*ctx.error);
    }
    return {};
}

auto Engine::Run(const EngineConfig& config, UICallback uiCallback, ModuleInstaller installModules, EventPoller pollEvents) -> std::expected<void, ErrorCode> {
    Platform::Init();
    SetLogLevel(config.logLevel);
    // Construct before the engine so its destructor (and any cleanup tasks)
    // finishes before the worker threads and fibers are retired.
    TaskSystem::Scope tasks;

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
    if (!engine->InitializeDefaultScene()) {
        return std::unexpected(EngineRunError::SceneInitializationFailed);
    }

    if (uiCallback) {
        engine->SetUICallback(std::move(uiCallback));
    }
    Clock        runClock({.maxDeltaTime = 0.1f});

    while (engine->IsRunning()) {
        const auto frameStart = Clock::SteadyClock::now();
        if (pollEvents != nullptr) {
            pollEvents(*engine);
        }
        engine->ProcessEvents();
        if (!engine->IsRunning()) {
            break;
        }

        // Re-read per frame: this is a load from EngineImpl, not a config
        // lookup, and it lets SetMaxFramesPerSecond take effect mid-run.
        const uint32_t maxFramesPerSecond = engine->MaxFramesPerSecond();
        const double   targetFrameTime =
            maxFramesPerSecond > 0 ? 1.0 / static_cast<double>(maxFramesPerSecond) : 0.0;

        const float rawDt = runClock.Tick();
        // The pacing clock only samples wall time; the simulation accumulator
        // belongs to Engine::Tick, so discard this unused one.
        runClock.ResetFixedAccumulator();

        // Kernel/Engine already synchronized a resize during ProcessEvents.
        constexpr double           kFpsCapSlack = 1.05;
        const std::optional<float> pacedDt      = engine->Graphics().PacedDeltaTime();
        const bool  displayPaced = pacedDt.has_value() && (maxFramesPerSecond == 0 || targetFrameTime <= static_cast<double>(*pacedDt) * kFpsCapSlack);
        const float tickDt       = displayPaced ? *pacedDt : rawDt;
        // Propagate the frame's real ErrorCode; Engine::Run has no richer
        // diagnosis of its own and must not flatten it to a generic one.
        if (auto tick = engine->Tick(tickDt); !tick) {
            return std::unexpected(tick.error());
        }

        if (maxFramesPerSecond > 0 && !displayPaced) {
            auto   now          = Clock::SteadyClock::now();
            double frameElapsed = std::chrono::duration<double>(now - frameStart).count();
            if (frameElapsed < targetFrameTime) {
                double sleepTime = targetFrameTime - frameElapsed;
                if (sleepTime > 0.002) {
                    std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>((sleepTime - 0.001) * 1e6)));
                }
                while (std::chrono::duration<double>(Clock::SteadyClock::now() - frameStart).count() < targetFrameTime) {
                    CPURelax();
                }
            }
        }
    }

    return {};
}

} // namespace ZHLN
