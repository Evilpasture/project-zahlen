#include "COMJointAllocator.hpp"
#include "ArticulationContext.hpp"
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ArticulationContext.hpp"
#include "CullingSystem.hpp"
#include "EngineGlobals.hpp"
#include "Hierarchy.hpp"
#include "InputContext.hpp"
#include "KernelSession.hpp"
#include "Platform.hpp"
#include "PrefabFactoryInternal.hpp"
#include "RenderSystem.hpp"
#include "SceneCleanupSystem.hpp"
#include "SystemWiring.hpp"
#include <Zahlen/Audio/Audio.hpp>
#include <Zahlen/COM/FrameTypes.hpp>
#include <Zahlen/Core/Arena.hpp>
#include <Zahlen/Core/Diagnostics.hpp>
#include <Zahlen/Core/HashMap.hpp>
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
#include <Zahlen/Engine/Capabilities/HostWindow.hpp>
#include <Zahlen/Engine/Capabilities/InputActions.hpp>
#include <Zahlen/Engine/Capabilities/InputHaptics.hpp>
#include <Zahlen/Engine/Capabilities/InputReader.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Clock.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/Frame.hpp>
#include <Zahlen/Engine/FramePlan.hpp>
#include <Zahlen/Engine/PoseUploads.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Engine/VisibilityList.hpp>
#include <Zahlen/Engine/World.hpp>
#include <Zahlen/FileSystem/FileWatcher.hpp>
#include <Zahlen/Input/Input.hpp>
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
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
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
    std::unique_ptr<KernelSession> kernel;
    std::unique_ptr<World>  world;

    // Host-owned custom draw hooks, keyed by stable window identity.
    ZHLN::HashMap<WindowID, Engine::WindowDrawCallback> windowDrawCallbacks;
    std::vector<Engine::DeviceLostCallback> deviceLostCallbacks;
    uint64_t                                deviceGeneration = 0;

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

    enum class FrameStage { Idle, Advancing, Ready, Rendering };
    FrameStage   frameStage = FrameStage::Idle;
    FramePlan    activeFramePlan;
    FrameContext activeFrameContext;
    float        activeFrameDt = 0;
    size_t       frameBoundary = 0;

    // Created on the first frame that asks for a system context, because that is
    // the first moment the task system's worker count is known and is also
    // before any graph dispatch. Sized for the whole app run; ResetWorkerScratch
    // is what the frame loop does with it.
    std::unique_ptr<WorkerScratchPool> workerScratch;

    // The engine's services, bound once (they are references), and the two
    // typed graphs built with them. Created during InitInternal once the
    // world and kernel exist; graph getters never initialize or rebuild them.
    // The simulation -> renderer channel for skinning poses. Engine-owned, so
    // both graphs reach the same queue through services.
    PoseUploadQueue poseUploads;
    SceneData       sceneData;

    std::unique_ptr<SimServices>    simServices;
    std::unique_ptr<RenderServices> renderServices;
    std::unique_ptr<SimGraph>       updateGraph;
    std::unique_ptr<RenderGraph>    renderGraph;

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
    const bool frozen = world.GetCullingStats().FreezeFrustum;
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
    ++_impl->deviceGeneration;
    // RenderServices stores references and the graph holds a reference to the
    // bundle. Reconstruct the bundle in place after a successful replacement:
    // this rebinds the renderer without rebuilding either graph (and losing
    // graph-local state). Recovery runs outside graph execution.
    if (_impl->renderServices != nullptr) {
        std::destroy_at(_impl->renderServices.get());
        std::construct_at(_impl->renderServices.get(), RenderServices {
            .render        = _impl->kernel->GetRenderContext(),
            .scene         = _impl->sceneData,
            .assets        = Assets(),
            .input         = InputReader {&_impl->kernel->InputFor()},
            .extensions    = _impl->extensions,
            .cullingStats  = _impl->world->GetCullingStats(),
            .debugSettings = _impl->world->GetDebugSettings(),
            .visible       = VisibleEntities {_impl->world->VisibleEntitiesForServices()},
            .shadow        = VisibleShadowEntities {_impl->world->VisibleShadowEntitiesForServices()},
        });
    }
    PrefabFactory::RebuildVulkanResources(Graphics(), _impl->world->GetRegistry());

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

    auto kernel_res = KernelSession::Create(cfg);
    if (!kernel_res) {
        return std::unexpected(kernel_res.error());
    }
    _impl->kernel = std::move(kernel_res.value());

    // World and KernelSession outlive the binding; it must exist before the
    // SimServices graph is initialized and before the first Engine::Physics call.
    _impl->world->GetPhysics().BindCollisionAssetResolver(
        [](void* context, CollisionAssetID id) noexcept -> std::shared_ptr<const Physics::MeshGeometry> {
            return static_cast<AssetManager*>(context)->FindCollisionMesh(id);
        },
        &_impl->kernel->GetAssetManager()
    );

    _impl->kernel->InputFor().SetRumbleSink(
        [](void* userdata, GamepadSlot slot, const GamepadRumble& rumble) noexcept { static_cast<PlatformHost*>(userdata)->SetGamepadRumble(slot, rumble); },
        &_impl->kernel->GetPlatformHost()
    );

    // Core's steps, components, baseline world entities and system graphs are
    // wired here so an Engine returned by Create() is immediately ready to run.
    SeedBaselineScene(GetInternalRegistry());
    EnsureFramePlan();
    InitializeSystemGraphs();

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

        // Release external handles while both the World and KernelSession still live.
        // The World destructor can safely clear its already-empty registry.
        ReleaseSceneContents();
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
    auto& input = _impl->kernel->InputFor();
    _impl->kernel->BeginInputFrame();
    _impl->kernel->ProcessEvents();
    SynchronizeResize();
    input.RefreshActionsForCurrentFrame(&_impl->world->GetEventBus());
}

void Engine::SynchronizeResize() {
    const auto resized = _impl->kernel->InputFor().ConsumePendingResize();
    if (!resized) {
        return;
    }
    const Extent2D actual = _impl->kernel->GetPlatformHost().GetSize();
    if (resized->width != actual.width || resized->height != actual.height) {
        // Headless/TTY receivers can submit a size without a primary-window
        // extent change; windowed targets were already handled by the KernelSession.
        _impl->kernel->GetRenderContext().SetResolution(*resized);
    }
}

void Engine::PollLateInput() {
    _impl->kernel->ProcessEvents();
    SynchronizeResize();
    _impl->kernel->InputFor().RefreshActionsForCurrentFrame(&_impl->world->GetEventBus());
}

auto Engine::GetCurrentFrame() const noexcept -> uint64_t {
    return _impl->frameCounter;
}

auto Engine::Host(WindowID window) noexcept -> HostWindow { return HostWindow {*_impl->kernel, window}; }
auto Engine::AddWindow(const String32& title, uint32_t width, uint32_t height, bool fullscreen) -> WindowID {
    return _impl->kernel->AddWindow(title, width, height, fullscreen);
}
void Engine::RemoveWindow(WindowID window) {
    if (window == WindowID::Primary) return;
    _impl->windowDrawCallbacks.Erase(window);
    _impl->kernel->RemoveWindow(window);
}
auto Engine::WindowIDs() const noexcept -> std::span<const WindowID> { return _impl->kernel->WindowIDs(); }
auto Engine::AcquireTarget(WindowID window) noexcept -> FrameOutcome<FrameTarget> { return _impl->kernel->AcquireTarget(window); }
auto Engine::GetAcquiredTarget(WindowID window) noexcept -> std::optional<FrameTarget> { return _impl->kernel->GetAcquiredTarget(window); }

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
auto Engine::State() const noexcept -> ZHLN::EngineState {
    return ZHLN::EngineState {const_cast<Engine&>(*this)};
}

auto EngineState::CullingStats() const noexcept -> ZHLN::CullingStats {
    return _engine->_impl->world->GetCullingStats();
}
void EngineState::ResetCullingStats() noexcept {
    _engine->_impl->world->GetCullingStats() = {};
}
void EngineState::SetCullingEnabled(bool enabled) noexcept {
    _engine->_impl->world->GetCullingStats().EnableCulling = enabled;
}
void EngineState::SetFreezeFrustum(bool freeze) noexcept {
    _engine->_impl->world->GetCullingStats().FreezeFrustum = freeze;
}
auto EngineState::PhysicsDrawMode() const noexcept -> int {
    return _engine->_impl->world->GetDebugSettings().physicsDrawMode;
}
void EngineState::SetPhysicsDrawMode(int mode) noexcept {
    _engine->_impl->world->GetDebugSettings().physicsDrawMode = mode;
}
auto EngineState::Configuration() const noexcept -> EngineConfig {
    return _engine->_impl->kernel->GetConfig();
}

struct EngineCore {
    static auto Registry(Engine& e) noexcept -> ECS::Registry& {
        return e.GetInternalRegistry();
    }
    static auto Assets(Engine& e) noexcept -> AssetManager& {
        return e._impl->kernel->GetAssetManager();
    }
    static auto Physics(Engine& e) noexcept -> PhysicsContext& {
        return e._impl->world->GetPhysics();
    }
    static auto Articulation(Engine& e) noexcept -> ArticulationContext& {
        return e._impl->world->GetArticulationContext();
    }
    static auto EvaluateInput(Engine& engine, float, FrameContext&) -> void {
        engine._impl->kernel->InputFor().RefreshActionsForCurrentFrame(&engine._impl->world->GetEventBus());
    }

    static auto Present(Engine& engine, float dt, FrameContext& ctx) -> void {
        auto render_res = RenderSystem::Update(
            engine, engine.GetInternalRegistry(), engine._impl->sceneData, engine._impl->poseUploads, engine._impl->world->GetCullingStats(), dt
        );
        if (!render_res) {
            if (render_res.error().Is(FrameResult::DeviceLost)) {
                if (auto lost_res = engine.HandleDeviceLost(); !lost_res) {
                    ZHLN::Log("[Engine] Fatal: GPU device recovery failed: {}", lost_res.error());
                    ctx.error = lost_res.error();
                    engine.Host().Close();
                }
            }
        }
    }
};

namespace PrefabFactory {
auto LoadFontAsset(Engine& engine, std::string_view path) -> std::expected<AssetID, ErrorCode> {
    return LoadFontAsset(EngineCore::Assets(engine), path);
}
auto CreateBox(Engine& engine, ZHLN::FVec3 halfExtents, const SpawnParams& params) -> Entity {
    return CreateBoxInternal(engine.Graphics(), EngineCore::Registry(engine), EngineCore::Physics(engine), halfExtents, params);
}
auto CreateSphere(Engine& engine, float radius, const SpawnParams& params) -> Entity {
    return CreateSphereInternal(engine.Graphics(), EngineCore::Registry(engine), EngineCore::Physics(engine), radius, params);
}
auto CreateCylinder(Engine& engine, float radius, float height, const SpawnParams& params) -> Entity {
    return CreateCylinderInternal(engine.Graphics(), EngineCore::Registry(engine), EngineCore::Physics(engine), radius, height, params);
}
auto CreateCone(Engine& engine, float radius, float height, const SpawnParams& params) -> Entity {
    return CreateConeInternal(engine.Graphics(), EngineCore::Registry(engine), EngineCore::Physics(engine), radius, height, params);
}
auto CreatePlane(Engine& engine, float extent, ZHLN::FVec4 color, const SpawnParams& params) -> Entity {
    return CreatePlaneInternal(engine.Graphics(), EngineCore::Registry(engine), EngineCore::Physics(engine), extent, color, params);
}
auto CreateBoxConfigured(Engine& engine, FVec3 halfExtents, const SpawnParams& params, bool buildBLAS) -> Entity {
    return CreateBoxInternal(engine.Graphics(), EngineCore::Registry(engine), EngineCore::Physics(engine), halfExtents, params, buildBLAS);
}
auto CreatePlaneConfigured(Engine& engine, float extent, FVec4 color, const SpawnParams& params, bool buildBLAS) -> Entity {
    return CreatePlaneInternal(engine.Graphics(), EngineCore::Registry(engine), EngineCore::Physics(engine), extent, color, params, buildBLAS);
}
auto CreateSphereConfigured(Engine& engine, float radius, const SpawnParams& params, bool buildBLAS) -> Entity {
    return CreateSphereInternal(engine.Graphics(), EngineCore::Registry(engine), EngineCore::Physics(engine), radius, params, buildBLAS);
}
auto CreateCylinderConfigured(Engine& engine, float radius, float height, const SpawnParams& params, bool buildBLAS) -> Entity {
    return CreateCylinderInternal(engine.Graphics(), EngineCore::Registry(engine), EngineCore::Physics(engine), radius, height, params, buildBLAS);
}
auto CreateConeConfigured(Engine& engine, float radius, float height, const SpawnParams& params, bool buildBLAS) -> Entity {
    return CreateConeInternal(engine.Graphics(), EngineCore::Registry(engine), EngineCore::Physics(engine), radius, height, params, buildBLAS);
}
auto LoadModelPrefab(Engine& engine, std::string_view path) -> ZHLN::Optional<ModelPrefab&> {
    // A lookup, not a load: an importer has already cached the prefab by the
    // time a scene names it.
    return EngineCore::Assets(engine).GetCachedPrefab(HashAssetPath(path));
}
auto InstantiatePrefab(Engine& engine, const ModelPrefab& prefab, const SpawnParams& params, Entity* outBuffer, uint32_t maxCount) -> uint32_t {
    return InstantiatePrefabInternal(
        engine.Graphics(), EngineCore::Registry(engine), EngineCore::Physics(engine), EngineCore::Articulation(engine), prefab, params, outBuffer, maxCount
    );
}
} // namespace PrefabFactory

namespace {
// Per-worker scratch. A system's SoAScratch allocation is capped by this, so it
// is the number to raise when a system reports that its scratch does not fit --
// one arena per worker, so the process total is this times the worker count.
constexpr size_t kWorkerScratchBytes = 256 * 1024;
} // namespace

void Engine::InitializeSystemGraphs() {
    auto& render = _impl->kernel->GetRenderContext();

    _impl->simServices = std::make_unique<SimServices>(SimServices {
        .physics      = _impl->world->GetPhysics(),
        .audio        = _impl->kernel->GetAudioContext(),
        .assets       = Assets(),
        .input        = InputReader {&_impl->kernel->InputFor()},
        .haptics      = InputHaptics {&_impl->kernel->InputFor()},
        .articulation = _impl->world->GetArticulationContext(),
        .events       = _impl->world->GetEventBus(),
        .extensions   = _impl->extensions,
        .poseUploads  = _impl->poseUploads,
    });

    _impl->renderServices = std::make_unique<RenderServices>(RenderServices {
        .render        = render,
        .scene         = _impl->sceneData,
        .assets        = Assets(),
        .input         = InputReader {&_impl->kernel->InputFor()},
        .extensions    = _impl->extensions,
        .cullingStats  = _impl->world->GetCullingStats(),
        .debugSettings = _impl->world->GetDebugSettings(),
        .visible       = VisibleEntities {_impl->world->VisibleEntitiesForServices()},
        .shadow        = VisibleShadowEntities {_impl->world->VisibleShadowEntitiesForServices()},
    });

    _impl->updateGraph       = std::make_unique<SimGraph>(_impl->world->GetRegistry(), *_impl->simServices, &MarkPendingDestroy);
    _impl->renderGraph       = std::make_unique<RenderGraph>(_impl->world->GetRegistry(), *_impl->renderServices, &MarkPendingDestroy);
    BuildSystemGraphs(*this);
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

// The capability accessors the engine hands out. They are two pointers wide and
// construct nothing: an InputReader is an address, not a subscription.
auto Engine::Input(WindowID window) noexcept -> InputActions {
    return InputActions {&_impl->kernel->InputFor(window)};
}
auto Engine::Input(WindowID window) const noexcept -> InputReader {
    return InputReader {&_impl->kernel->InputFor(window)};
}
auto Engine::Haptics() noexcept -> InputHaptics {
    return InputHaptics {&_impl->kernel->InputFor()};
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
    return *_impl->updateGraph;
}

auto Engine::GetRenderGraph() -> RenderGraph& {
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
    SceneCleanupSystem::ProcessPending(*this, GetInternalRegistry());
}

void Engine::ReleaseSceneContents() {
    SceneCleanupSystem::ClearAll(*this, GetInternalRegistry());
    // Queued destroys and any surviving bodies retain their own shapes.
    Physics().ClearMeshShapeCache();
    ClearVisibilityLists();
    _impl->world->GetEventBus().ClearEvents();
    _impl->world->GetCullingStats() = {};
    _impl->world->GetDebugSettings() = {};
    if (_impl->updateGraph != nullptr) {
        _impl->updateGraph->ResetLocals();
    }
    if (_impl->renderGraph != nullptr) {
        _impl->renderGraph->ResetLocals();
    }
}

uint32_t COM::JointAllocator::AllocateJoints(Engine& engine, uint32_t count) noexcept {
    return engine._impl->world->GetArticulationContext().AllocateJoints(count);
}

void Engine::ClearScene() {
    // A replacement between stages invalidates this frame's visibility/history
    // inputs. Consume its simulation once, without rendering the replacement.
    if (FramePending())
        (void) CancelFrame();
    ReleaseSceneContents();
    SeedBaselineScene(GetInternalRegistry());
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

auto Engine::GetWindowDrawCallback(WindowID window) const noexcept -> ZHLN::Optional<const WindowDrawCallback&> {
    return _impl->windowDrawCallbacks.Find(window);
}

void Engine::SetWindowDrawCallback(WindowID window, WindowDrawCallback callback) {
    if (callback) {
        _impl->windowDrawCallbacks.Insert(window, std::move(callback));
    } else {
        _impl->windowDrawCallbacks.Erase(window);
    }
}

void Engine::AddDeviceLostCallback(DeviceLostCallback callback) {
    if (callback) {
        _impl->deviceLostCallbacks.push_back(std::move(callback));
    }
}

auto Engine::DeviceLostCallbackCount() const noexcept -> size_t {
    return _impl->deviceLostCallbacks.size();
}

auto Engine::DeviceGeneration() const noexcept -> uint64_t {
    return _impl->deviceGeneration;
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
    if (module.addGraphSystems != nullptr) {
        // Installation is the mutation boundary. Consumers observe a complete,
        // compiled graph as soon as this call returns, never on a later read.
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
    BuildCoreFramePlan(_impl->framePlan, &EngineCore::Present, &EngineCore::EvaluateInput);
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

void Engine::ProvokeDeviceLost() {
    _impl->kernel->ProvokeDeviceLost();
}

namespace {
void ExecuteFrameRange(Engine& engine, const FramePlan& plan, size_t begin, size_t end, float dt, FrameContext& ctx) {
    const auto steps = plan.Steps();
    for (size_t i = begin; i < end && !ctx.error; ++i) {
        if (const auto run = steps[i].run)
            run(engine, dt, ctx);
    }
}
} // namespace

auto Engine::FramePending() const noexcept -> bool {
    return _impl->frameStage == EngineImpl::FrameStage::Ready;
}

auto Engine::AdvanceFrame(float dt) -> std::expected<void, ErrorCode> {
    using Stage = EngineImpl::FrameStage;
    if (_impl->frameStage != Stage::Idle)
        return std::unexpected(COM::FramePipelineError::FrameInProgress);
    if (!std::isfinite(dt) || dt < 0.0f)
        return std::unexpected(COM::FramePipelineError::InvalidDeltaTime);
    EnsureFramePlan();
    // Freeze the composition across both halves. Installation affects a later
    // frame; indices cannot drift while the host owns the staging interval.
    _impl->activeFramePlan = _impl->framePlan;
    const auto cleanup     = _impl->activeFramePlan.IndexOf("SceneCleanup");
    if (!cleanup)
        return std::unexpected(FrameError::StepFailed);
    _impl->frameBoundary = *cleanup + 1;
    _impl->frameStage    = Stage::Advancing;
    _impl->activeFrameDt = dt;
    _impl->kernel->InputFor().StepHaptics(dt);
    _impl->frameClock.Advance(static_cast<double>(dt));
    const FixedStepBatch batch = _impl->frameClock.ConsumeFixedSteps(kFixedStepDt, kMaxFixedSteps);
    _impl->activeFrameContext  = FrameContext {
        .error      = std::nullopt,
        .fixedSteps = batch.steps,
        .alpha      = FrameAlpha {batch.alpha},
    };
    auto& ctx = _impl->activeFrameContext;
    ExecuteFrameRange(*this, _impl->activeFramePlan, 0, _impl->frameBoundary, dt, ctx);
    if (ctx.error) {
        _impl->frameStage = Stage::Idle;
        ++_impl->frameCounter;
        return std::unexpected(*ctx.error);
    }
    _impl->frameStage = Stage::Ready;
    return {};
}

auto Engine::FinishFrame() -> std::expected<void, ErrorCode> {
    using Stage = EngineImpl::FrameStage;
    if (!FramePending())
        return std::unexpected(COM::FramePipelineError::NoPendingFrame);
    _impl->frameStage = Stage::Rendering;
    auto& ctx         = _impl->activeFrameContext;
    ExecuteFrameRange(*this, _impl->activeFramePlan, _impl->frameBoundary, _impl->activeFramePlan.Count(), _impl->activeFrameDt, ctx);
    _impl->frameStage = Stage::Idle;
    ++_impl->frameCounter;
    if (ctx.error)
        return std::unexpected(*ctx.error);
    return {};
}

auto Engine::CancelFrame() -> std::expected<void, ErrorCode> {
    using Stage = EngineImpl::FrameStage;
    if (!FramePending())
        return std::unexpected(COM::FramePipelineError::NoPendingFrame);
    // Simulation is consumed, but no image will be presented. Preserve the
    // last presented transform/camera history for the next rendered frame.
    _impl->frameStage = Stage::Idle;
    ++_impl->frameCounter;
    return {};
}

auto Engine::Tick(float dt) -> std::expected<void, ErrorCode> {
    if (auto advanced = AdvanceFrame(dt); !advanced)
        return advanced;
    return FinishFrame();
}

auto Engine::Run(const EngineConfig& config, FrameCallback onFrame, ModuleInstaller installModules, EventPoller pollEvents) -> std::expected<void, ErrorCode> {
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
    engine->Host().Focus();

    // The application's install pass: modules contribute components, declare
    // their frame steps, and mark the system graphs dirty for the first Tick.
    if (installModules != nullptr) {
        installModules(*engine);
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

        // KernelSession/Engine already synchronized a resize during ProcessEvents.
        constexpr double           kFpsCapSlack = 1.05;
        const std::optional<float> pacedDt      = engine->Graphics().PacedDeltaTime();
        const bool  displayPaced = pacedDt.has_value() && (maxFramesPerSecond == 0 || targetFrameTime <= static_cast<double>(*pacedDt) * kFpsCapSlack);
        const float tickDt       = displayPaced ? *pacedDt : rawDt;
        // Propagate the frame's real ErrorCode; Engine::Run has no richer
        // diagnosis of its own and must not flatten it to a generic one.
        if (onFrame) onFrame(*engine, tickDt);
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
