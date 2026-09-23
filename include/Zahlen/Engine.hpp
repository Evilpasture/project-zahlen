// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Engine.hpp
#pragma once

#include <Jolt/Jolt.h>            // JPH::Array (Jolt's entry header; Core/Array.h is not self-contained)
#include <Zahlen/CommandLine.hpp> // GameplayDriver, GameplayStatus
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/CrashState.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/FrameResult.hpp> // FrameOutcome
#include <Zahlen/SystemContext.hpp>
#include <Zahlen/WindowInput.hpp> // WindowInputReceiver
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/gui/UIData.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>

namespace ZHLN {

class Kernel;
class World;
class RenderContext;
class PhysicsContext;
class AudioContext;
class AssetManager;
class ScriptRunner;
class Window;
namespace FS {
class FileSystemWatcher;
}

using FileSystemWatcher = FS::FileSystemWatcher;
class PlatformHost;
struct Camera;
struct EngineImpl;

namespace ECS {
class Registry;
class SystemGraph;
class EntityCommandBuffer;
} // namespace ECS

class FrameScheduler;

class CullingSystem;
class ArticulationSystem;

// Composition root: owns one Kernel (windows, GPU, audio, assets) and one World
// (registry, physics, camera, system graphs) plus the policy tying them together --
// frame scheduler, hot-reload binding, callback registries, main loop.
//
// The historical Get* accessors remain as delegates; new code should prefer
// GetKernel(), GetWorld() or MakeSystemContext() to name the layer it depends on.
class ZHLN_API Engine {
  public:
    using UICallback = std::function<void(Engine&)>;

    // One ordered unit of work an optional layer contributes to the frame schedule.
    // Applied on every (re)build, scene resets included, so a layer never has to
    // re-register after InitializeDefaultScene runs again.
    using FrameSchedulerExtension = void (*)(FrameScheduler&);

    // Contributes systems to the hazard-analysed graphs, inside BuildSystemGraphs
    // after the core systems and before Compile(), so contributed nodes take part in
    // hazard analysis and AddSystemBefore anchoring like core ones. Same
    // rebuild-on-reset guarantee as FrameSchedulerExtension.
    using SystemGraphsExtension = void (*)(ECS::SystemGraph& updateGraph, ECS::SystemGraph& renderGraph);

    // Character-controller hooks inside the fixed physics substep: preStep runs
    // after the previous substep's grounded write-back and before
    // PhysicsContext::Step, postStep right after it (grounded read-back). Both are
    // null in a bare engine, which steps with no character steering.
    struct CharacterStepHooks {
        void (*preStep)(Engine&, float dt) = nullptr;
        void (*postStep)(Engine&)          = nullptr;
    };

    // Free-cam base speed for a target camera's tracked entity. Defaults to 12 with
    // no query installed, and a nullopt answer keeps that default (the entity carries
    // no movement configuration); the extras character controller reports the tracked
    // character's configured speed.
    using FreeCamSpeedQuery = std::optional<float> (*)(ECS::Registry&, Entity target);

    // Runs from ~Engine before subsystem teardown, while engine and registry are
    // still whole: where an optional layer releases engine-scoped state it parked in
    // process-global storage.
    using TeardownHook = void (*)(Engine&);

    // Notified from HandleDeviceLost() once the replacement VkDevice exists and core
    // has rebuilt the GPU state it owns. Anything that uploaded GPU resources from
    // outside the engine re-uploads them here: the old handles are dead and only the
    // owner knows how to re-read the source. Callbacks run in registration order; the
    // list lives on Engine because RenderContext is destroyed and rebuilt on the way
    // through.
    using DeviceLostCallback = std::function<void(Engine&)>;

    Engine();
    ~Engine();

    auto HandleDeviceLost() noexcept -> std::expected<void, ErrorCode>;

    // Builds an engine. Every external service receives this instance
    // explicitly; no process-global engine context is published.
    static auto Create(const EngineConfig& cfg) -> std::expected<std::unique_ptr<Engine>, ErrorCode>;

    [[nodiscard]] auto IsRunning() const -> bool;
    void               ProcessEvents();

    // Re-pumps the platform event source without resetting the per-frame
    // deltas: mouse motion and wheel accumulate onto the frame-top sample,
    // key levels refresh in place. The player-intent step calls this
    // immediately before translating input for physics, so intent is latched
    // against the freshest device state instead of the frame-top pump -- the
    // input-to-photon age shrinks by the UI and hot-reload steps in between.
    // Edge-triggered state is unaffected (nothing here consumes edges), and a
    // headless host's pump is a no-op, so this is safe to call unconditionally.
    void PollLateInput();

    // Primary window (always index 0). Extra windows live in the same
    // engine-owned vector; see AddWindow.
    // The session's platform host: its event source, the target a frame is drawn
    // into, and the desktop conveniences where a desktop exists. This is what to
    // ask about "the display" -- it is a desktop window only when the session has
    // one.
    [[nodiscard]] auto GetPlatformHost() noexcept -> PlatformHost&;
    [[nodiscard]] auto GetPlatformHost() const noexcept -> const PlatformHost&;
    // The desktop window behind the host, or nullptr in a headless or KMS/DRM
    // session. Prefer GetPlatformHost() unless the OS window itself is the thing
    // you need; see Kernel::GetWindow.
    [[nodiscard]] auto GetWindow() noexcept -> Window*;
    // Opens another desktop window owned by the kernel; see Kernel::AddWindow.
    auto AddWindow(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver = {}) -> Window*;
    void RemoveWindow(Window& window);

    // What a frame draws into, resolved by the kernel that owns the session and
    // every window in it. See Kernel::AcquireTarget for what "the session's
    // target" resolves to and when a window becomes a destination; these are the
    // same four verbs, delegated.
    [[nodiscard]] auto AcquireTarget() noexcept -> FrameOutcome<RenderAttachment>;
    [[nodiscard]] auto AcquireTarget(Window& window) noexcept -> FrameOutcome<RenderAttachment>;
    [[nodiscard]] auto GetTargetAttachment() noexcept -> std::optional<RenderAttachment>;
    [[nodiscard]] auto GetTargetAttachment(Window& window) noexcept -> std::optional<RenderAttachment>;

    // Platform/hardware substrate: windows, event pump, GPU, audio, assets.
    auto GetKernel() -> Kernel&;
    // Simulation instance: registry, physics, camera, system graphs.
    auto GetWorld() -> World&;

    // Assembles the per-frame SystemContext for SystemGraph::Execute: every service
    // the graph systems may consume plus dt, alpha and the frame counter. Built fresh
    // each call so dt/alpha never go stale.
    auto MakeSystemContext(float dt) -> SystemContext;

    auto               GetPhysicsContext() -> PhysicsContext&;
    auto               GetRenderContext() -> RenderContext&;
    auto               GetCamera() -> Camera&;
    auto               GetAssetManager() -> AssetManager&;
    auto               GetAudioContext() -> AudioContext&;
    auto               GetScriptRunner() -> ScriptRunner&;
    auto               GetFileSystemWatcher() -> FileSystemWatcher&;
    [[nodiscard]] auto GetRegistry() -> ECS::Registry&;
    [[nodiscard]] auto GetRegistry() const -> const ECS::Registry&;

    auto GetUpdateGraph() -> ECS::SystemGraph&;
    auto GetRenderGraph() -> ECS::SystemGraph&;
    auto GetMainECB() -> ECS::EntityCommandBuffer&;
    // The frame's ordered phase steps. `Tick` executes exactly this list.
    [[nodiscard]] auto GetFrameScheduler() -> FrameScheduler&;
    auto               GetCullingSystem() -> CullingSystem&;
    auto               GetArticulationSystem() -> ArticulationSystem&;
    auto               GetVisibleEntities() -> JPH::Array<Entity>&;
    auto               GetVisibleShadowEntities() -> JPH::Array<Entity>&;
    auto               GetCurrentAlpha() -> float&;
    // Fixed-timestep leftover, injected into the stateless PhysicsSystem.
    auto GetPhysicsAccumulator() -> float&;

    [[nodiscard]] auto GetGameState() const -> void*;
    void               SetGameState(void* state);
    [[nodiscard]] auto GetCurrentFrame() const noexcept -> uint64_t;

    void SetUICallback(UICallback callback);

    // --- Pending 2D UI payload
    //
    // The UI phase runs before the renderer opens the frame, so a host that builds
    // Clay geometry there banks it here and RenderSystem composes it over the
    // finished scene in the same frame. The spans alias the producing GUI context's
    // storage, valid until that context's next BeginFrame.
    void               SetPendingUIData(const UIDrawData& uiData) noexcept;
    [[nodiscard]] auto GetPendingUIData() const noexcept -> UIDrawData;

    // Subscribes to the device-lost notification (see DeviceLostCallback). A null
    // callback is ignored; adding the same function twice registers it twice.
    void AddDeviceLostCallback(DeviceLostCallback callback);

    // --- Optional-layer wiring
    // The seam between core and extras, installed before InitializeDefaultScene: core
    // never includes an extras header and extras never reaches into core internals.

    // Contributes frame-phase steps, applied on every (re)build of the schedule. Null
    // extensions are ignored; registration order is preserved.
    void AddFrameSchedulerExtension(FrameSchedulerExtension ext);

    // Contributes system-graph nodes, applied on every (re)build before Compile().
    // Null extensions are ignored.
    void AddSystemGraphsExtension(SystemGraphsExtension ext);

    // Applied by BuildFrameScheduler / BuildSystemGraphs after the core schedule and
    // graphs are in place; exposed so those translation units reach the extension
    // lists without friending Impl.
    void ApplyFrameSchedulerExtensions(FrameScheduler& scheduler);
    void ApplySystemGraphsExtensions(ECS::SystemGraph& updateGraph, ECS::SystemGraph& renderGraph);

    void               SetCharacterStepHooks(CharacterStepHooks hooks);
    [[nodiscard]] auto GetCharacterStepHooks() const noexcept -> const CharacterStepHooks&;

    // Installs the animation modifier run inside the skinning pipeline (see
    // BonePosePostProcessor), replacing any previous one.
    void               SetBonePosePostProcessor(BonePosePostProcessor processor);
    [[nodiscard]] auto GetBonePosePostProcessor() const noexcept -> BonePosePostProcessor;

    void               SetFreeCamSpeedQuery(FreeCamSpeedQuery query);
    [[nodiscard]] auto GetFreeCamSpeedQuery() const noexcept -> FreeCamSpeedQuery;

    // Subscribes to engine teardown; see TeardownHook. Null hooks ignored.
    void AddTeardownHook(TeardownHook hook);

    // How many device-lost subscribers are registered, so a host can assert the owners
    // it expects actually installed themselves.
    [[nodiscard]] auto DeviceLostCallbackCount() const noexcept -> size_t;
    // The host editor callback, or nullptr; exposed so the frame scheduler can run it
    // as an ordinary phase step.
    [[nodiscard]] auto GetUICallback() const noexcept -> const UICallback*;

    void ProvokeDeviceLost();

    // Registers default engine components, camera, lighting and UI settings, and
    // compiles the internal system graphs.
    auto InitializeDefaultScene() -> bool;

    // Executes one synchronized frame tick in canonical order.
    auto Tick(float dt, GameplayDriver driver = GameplayDriver::Cpp) -> GameplayStatus;

    // Runs the native (C++) gameplay module update for one frame; the Cpp and
    // Hybrid drivers call this, scripted frames route through ScriptRunner.
    auto UpdateNativeGameplay(float dt) -> GameplayStatus;

    // Whether the native gameplay module currently exposes a loadable update entry
    // point.
    [[nodiscard]] auto IsNativeGameplayLoaded() const noexcept -> bool;

    // Whether the engine auto-builds the fallback scene when nothing playable would
    // start (no boot script, no native module).
    [[nodiscard]] auto FallbackSceneEnabled() const noexcept -> bool;

    // Gives the scene's UI settings the engine's persistent font atlas, baking one on
    // first use. The engine owns it because Registry::Clear() discards the
    // scene-owned copy.
    void SeedSceneFontAtlas(ECS::Registry& reg);

    // Runs after Engine::Create and before InitializeDefaultScene to register the
    // optional gameplay layers this host runs with. Core never calls one itself;
    // passing it through Run keeps the built-in loop honest about its extensions.
    using ExtensionInstaller = void (*)(Engine&);

    // Convenience entry point managing the main loop, frame limiting and clean
    // shutdown.
    static auto Run(const CommandLineOptions& options, CrashState& crashState, UICallback uiCallback = nullptr, ExtensionInstaller installExtensions = nullptr)
        -> std::expected<void, ErrorCode>;

  private:
    auto InitInternal(const EngineConfig& cfg) -> std::expected<void, ErrorCode>;

    // Watches the installed runtime's boot entry points for hot reload and drops the
    // previous runtime's watches. The paths come from the runtime, so core never names
    // a scripting language.
    void RegisterBootScriptWatches();

    std::unique_ptr<EngineImpl> _impl;
};

// Destroys an entity tree in child-before-parent order, notifying external systems
// while each component is still present. Use this for immediate teardown; plain
// registry destruction is still supported and reconciled by the owning systems.
void DespawnEntity(Engine& engine, Entity entity);

} // namespace ZHLN
