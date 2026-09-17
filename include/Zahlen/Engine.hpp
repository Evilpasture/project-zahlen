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
#include <Zahlen/SystemContext.hpp>
#include <Zahlen/Viewport.hpp>    // ViewportMode
#include <Zahlen/WindowInput.hpp> // WindowInputReceiver
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
class CreativeWorksManager;
class ScriptRunner;
class FileSystemWatcher;
class Window;
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

/// Composition root: owns one Kernel (windows, GPU, audio, assets) and one
/// World (ECS registry, physics, camera, system graphs) plus the app-level
/// policy that ties them together -- the frame scheduler, hot-reload binding,
/// the UI/device-lost callback registries and the main loop.
///
/// The historical Get* accessors remain as delegates over Kernel/World so
/// existing call sites keep working; new code should prefer GetKernel(),
/// GetWorld() or MakeSystemContext() to make the layer it depends on explicit.
class ZHLN_API Engine {
  public:
    using UICallback = std::function<void(Engine&)>;

    /// One ordered unit of work contributed by an optional layer to the frame
    /// schedule. Applied by BuildFrameScheduler every time the schedule is
    /// (re)built -- including scene resets -- so a contributing layer never has
    /// to re-register after InitializeDefaultScene runs again.
    using FrameSchedulerExtension = void (*)(FrameScheduler&);

    /// Contributes systems to the hazard-analysed graphs. Runs inside
    /// BuildSystemGraphs after the core systems and before Compile(), so
    /// contributed nodes take part in hazard analysis and AddSystemBefore
    /// anchoring exactly like core ones. Same rebuild-on-reset guarantee as
    /// FrameSchedulerExtension.
    using SystemGraphsExtension = void (*)(ECS::SystemGraph& updateGraph, ECS::SystemGraph& renderGraph);

    /// Character-controller integration points inside the fixed physics
    /// substep: preStep runs after the previous substep's grounded write-back
    /// and before PhysicsContext::Step (locomotion integration + velocity
    /// commit), postStep runs right after Step (grounded read-back). Both are
    /// null in a bare engine: physics steps with no character steering, which
    /// is what hosts without the extras character controller want.
    struct CharacterStepHooks {
        void (*preStep)(Engine&, float dt) = nullptr;
        void (*postStep)(Engine&)          = nullptr;
    };

    /// Free-cam base speed for a target camera's tracked entity. Core's
    /// TargetCameraSystem asks this when it intercepts free-cam movement; the
    /// answer defaults to 12 when no query is installed, and a nullopt answer
    /// from an installed query keeps that default (the tracked entity carries
    /// no movement configuration). The extras character controller installs
    /// one that reports the tracked character's configured movement speed.
    using FreeCamSpeedQuery = std::optional<float> (*)(ECS::Registry&, Entity target);

    /// Runs from ~Engine before subsystem teardown, while the engine and its
    /// registry are still whole. Optional layers that park engine-scoped state
    /// in process-global storage release it here (the compiled-in fallback
    /// preset does exactly that).
    using TeardownHook = void (*)(Engine&);

    /// Notified from HandleDeviceLost() once the replacement VkDevice exists and
    /// core has rebuilt the GPU state it owns.
    ///
    /// Anything that uploaded GPU resources from outside the engine -- an
    /// importer, a host renderer, a plugin -- re-uploads them here. Core cannot
    /// do it for them: the handles those owners were holding died with the old
    /// device, and recreating them means re-reading the source file, which only
    /// the owner knows how to do. Callbacks run in registration order against
    /// the new context. The list lives on Engine rather than on RenderContext
    /// because the context is destroyed and rebuilt on the way through.
    using DeviceLostCallback = std::function<void(Engine&)>;

    Engine();
    ~Engine();

    auto HandleDeviceLost() noexcept -> std::expected<void, ErrorCode>;

    /// Builds an engine. Every external service receives this instance
    /// explicitly; no process-global engine context is published.
    static auto Create(const EngineConfig& cfg) -> std::expected<std::unique_ptr<Engine>, ErrorCode>;

    [[nodiscard]] auto IsRunning() const -> bool;
    void               ProcessEvents();

    /// Primary window (always index 0). Extra windows live in the same
    /// engine-owned vector; see AddWindow.
    auto               GetWindow() -> Window&;
    auto               GetWindow(size_t index) -> Window&;
    [[nodiscard]] auto WindowCount() const noexcept -> size_t;

    /// Opens another OS window owned by this engine. GLFW is already held from
    /// InitInternal; the new Window is pushed onto the engine vector and a
    /// viewport (swapchain) is created on the live renderer. Default UIOnly:
    /// PresentViewports blits the live frame plus that window's UI.
    /// Returns nullptr if the engine is headless/TTY or the OS window cannot
    /// be created.
    auto AddWindow(
        const String32&            title,
        uint32_t                   width,
        uint32_t                   height,
        bool                       fullscreen = false,
        const WindowInputReceiver& receiver   = {},
        ViewportMode               mode       = ViewportMode::UIOnly,
        Entity                     camera     = Entity::Null()
    ) -> Window*;
    /// Drops an extra window from the engine vector. The primary window cannot
    /// be removed this way. Its viewport is destroyed first.
    void RemoveWindow(Window& window);

    /// Platform/hardware substrate: windows, event pump, GPU, audio, assets.
    auto GetKernel() -> Kernel&;
    /// Simulation instance: registry, physics, camera, system graphs.
    auto GetWorld() -> World&;

    /// Assembles the per-frame SystemContext for SystemGraph::Execute: every
    /// service the graph systems may consume, plus the frame values (dt, alpha,
    /// frame counter). Built fresh each call so dt/alpha never go stale.
    auto MakeSystemContext(float dt) -> SystemContext;

    auto               GetPhysicsContext() -> PhysicsContext&;
    auto               GetRenderContext() -> RenderContext&;
    auto               GetCamera() -> Camera&;
    auto               GetCreativeWorksManager() -> CreativeWorksManager&;
    auto               GetAudioContext() -> AudioContext&;
    auto               GetScriptRunner() -> ScriptRunner&;
    auto               GetFileSystemWatcher() -> FileSystemWatcher&;
    [[nodiscard]] auto GetRegistry() -> ECS::Registry&;
    [[nodiscard]] auto GetRegistry() const -> const ECS::Registry&;

    auto GetUpdateGraph() -> ECS::SystemGraph&;
    auto GetRenderGraph() -> ECS::SystemGraph&;
    auto GetMainECB() -> ECS::EntityCommandBuffer&;
    /// The frame's ordered phase steps. `Tick` executes exactly this list.
    [[nodiscard]] auto GetFrameScheduler() -> FrameScheduler&;
    auto               GetCullingSystem() -> CullingSystem&;
    auto               GetArticulationSystem() -> ArticulationSystem&;
    auto               GetVisibleEntities() -> JPH::Array<Entity>&;
    auto               GetVisibleShadowEntities() -> JPH::Array<Entity>&;
    auto               GetCurrentAlpha() -> float&;

    [[nodiscard]] auto GetGameState() const -> void*;
    void               SetGameState(void* state);
    [[nodiscard]] auto GetCurrentFrame() const noexcept -> uint64_t;

    void SetUICallback(UICallback callback);

    /// Subscribes to the device-lost notification. See DeviceLostCallback.
    /// Idempotent only in the sense that a null callback is ignored; adding the
    /// same function twice registers it twice.
    void AddDeviceLostCallback(DeviceLostCallback callback);

    /// --- Optional-layer wiring ------------------------------------------------
    /// The composition root installs extras modules through these before
    /// InitializeDefaultScene. They are the sanctioned seam between core and
    /// extras: core never includes an extras header, and extras never reaches
    /// into core internals -- each side meets on these signatures.

    /// Contributes frame-phase steps; applied on every (re)build of the
    /// schedule. Null extensions are ignored. Registration order is preserved.
    void AddFrameSchedulerExtension(FrameSchedulerExtension ext);

    /// Contributes system-graph nodes; applied on every (re)build of the
    /// graphs, before Compile(). Null extensions are ignored.
    void AddSystemGraphsExtension(SystemGraphsExtension ext);

    /// Applied by BuildFrameScheduler / BuildSystemGraphs after the core
    /// schedule/graphs are in place. Exposed so those translation units (which
    /// only see an Engine&) reach the extension lists without friending Impl.
    void ApplyFrameSchedulerExtensions(FrameScheduler& scheduler);
    void ApplySystemGraphsExtensions(ECS::SystemGraph& updateGraph, ECS::SystemGraph& renderGraph);

    void               SetCharacterStepHooks(CharacterStepHooks hooks);
    [[nodiscard]] auto GetCharacterStepHooks() const noexcept -> const CharacterStepHooks&;

    /// Installs the animation modifier run inside the skinning pipeline; see
    /// BonePosePostProcessor. Replaces any previously installed processor.
    void               SetBonePosePostProcessor(BonePosePostProcessor processor);
    [[nodiscard]] auto GetBonePosePostProcessor() const noexcept -> BonePosePostProcessor;

    void               SetFreeCamSpeedQuery(FreeCamSpeedQuery query);
    [[nodiscard]] auto GetFreeCamSpeedQuery() const noexcept -> FreeCamSpeedQuery;

    /// Subscribes to engine teardown; see TeardownHook. Null hooks ignored.
    void AddTeardownHook(TeardownHook hook);

    /// How many device-lost subscribers are registered. Exposed so a host can
    /// assert that the owners it expects actually installed themselves.
    [[nodiscard]] auto DeviceLostCallbackCount() const noexcept -> size_t;
    /// The host editor callback, or nullptr when none is installed. Exposed so
    /// the frame scheduler can run it as an ordinary phase step.
    [[nodiscard]] auto GetUICallback() const noexcept -> const UICallback*;

    void ProvokeDeviceLost();

    /**
     * @brief Registers default engine components, camera, lighting settings,
     *        UI settings, and compiles internal System Graphs.
     */
    auto InitializeDefaultScene() -> bool;

    /**
     * @brief Executes a single synchronized frame tick in canonical order.
     * @param dt Frame delta time in seconds.
     * @param driver Gameplay driver (Cpp, Fennel, or Hybrid).
     */
    auto Tick(float dt, GameplayDriver driver = GameplayDriver::Cpp) -> GameplayStatus;

    /// Runs the native (C++) gameplay module update for one frame. The Cpp
    /// and Hybrid frame drivers call this; Fennel frames route through
    /// ScriptRunner instead.
    auto UpdateNativeGameplay(float dt) -> GameplayStatus;

    /// Whether the native gameplay module (libgameplay.so / gameplay.dll)
    /// currently exposes a loadable update entry point.
    [[nodiscard]] auto IsNativeGameplayLoaded() const noexcept -> bool;

    /// Whether the engine auto-builds the fallback scene when it detects that
    /// nothing playable would start (no boot script / no native module).
    [[nodiscard]] auto FallbackSceneEnabled() const noexcept -> bool;

    /// Gives the scene's UI settings the engine's persistent font atlas, or
    /// bakes and persists one on first use. The engine owns the atlas because
    /// Registry::Clear() discards the scene-owned copy.
    void SeedSceneFontAtlas(ECS::Registry& reg);

    /// Runs from the composition root after Engine::Create and before
    /// InitializeDefaultScene: registers the optional gameplay layers (extras)
    /// this host runs with. Core never calls one itself -- passing it through
    /// Run is what keeps the built-in loop path honest about its extensions.
    using ExtensionInstaller = void (*)(Engine&);

    /**
     * @brief Convenience entry point that manages the main loop, frame limiting,
     *        and clean shutdown.
     */
    static auto Run(const CommandLineOptions& options, CrashState& crashState, UICallback uiCallback = nullptr, ExtensionInstaller installExtensions = nullptr)
        -> std::expected<void, ErrorCode>;

  private:
    auto                        InitInternal(const EngineConfig& cfg) -> std::expected<void, ErrorCode>;

    /// Watches the installed runtime's boot entry points for hot reload, and
    /// drops the previous runtime's watches. The paths come from the runtime, so
    /// core never names a scripting language. Runs when a host installs a
    /// runtime, which is after InitInternal has returned.
    void RegisterBootScriptWatches();

    std::unique_ptr<EngineImpl> _impl;
};

/// Destroys an entity tree in child-before-parent order. External systems are
/// notified while each component is still present, before Registry::Destroy
/// invalidates its handle. Use this for immediate teardown; plain registry
/// destruction is still supported and is reconciled by the owning systems.
void DespawnEntity(Engine& engine, Entity entity);

} // namespace ZHLN
