// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/FramePlan.hpp>
#include <Zahlen/GameModule.hpp>
#include <Zahlen/PoseUploads.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <Zahlen/SystemContext.hpp>
#include <Zahlen/WindowInput.hpp>
#include <Zahlen/Render/Handles.hpp>
#include <Zahlen/gui/Font.hpp>
#include <Zahlen/gui/UIData.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace ZHLN {

class Kernel;
class World;
class RenderContext;
struct SceneData;
class PhysicsContext;
class AudioContext;
class AssetManager;
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
template <typename Services>
class SystemGraph;
class EntityCommandBuffer;
class EventBus;
}

namespace GUI {
struct UIInput;
}

class ArticulationSystem;
class Engine;

class ZHLN_API Engine {
  public:
    using UICallback = std::function<void(Engine&)>;

    // What runs around every fixed-timestep substep PhysicsSystem takes, inside
    // its accumulator loop and not once per frame.
    //
    // This is the physics lifecycle's extension point, which is why it is not
    // named after the one extension that uses it: the engine knows "between
    // substeps", not "characters". It is also why it cannot be a declared
    // FramePlan step. A step runs once per frame, at whatever frame dt arrived;
    // this runs per 1/60 s substep with the *fixed* dt, which is the whole point
    // for the character integration that uses it -- steering integrated at the
    // variable frame rate would make the controller's determinism depend on
    // present timing. Anything that only needs per-frame granularity belongs in
    // the plan instead.
    struct PhysicsSubstepHooks {
        void (*preStep)(Engine&, float fixedDt) = nullptr;
        void (*postStep)(Engine&)               = nullptr;
    };

    // The hooks GameModule.hpp declares, spelled here too because they are part
    // of this class's own API. Same types either way.
    using TeardownHook = ZHLN::TeardownHook;
    // Bulk extension systems run while marked components still exist. `all`
    // selects scene teardown rather than just PendingDestroy entities.
    using SceneCleanupPass = ZHLN::SceneCleanupPass;

    using DeviceLostCallback = std::function<void(Engine&)>;

    Engine();
    ~Engine();

    auto HandleDeviceLost() noexcept -> std::expected<void, ErrorCode>;

    static auto Create(const EngineConfig& cfg) -> std::expected<std::unique_ptr<Engine>, ErrorCode>;

    [[nodiscard]] auto IsRunning() const -> bool;
    void               ProcessEvents();

    void PollLateInput();

    [[nodiscard]] auto GetPlatformHost() noexcept -> PlatformHost&;
    [[nodiscard]] auto GetPlatformHost() const noexcept -> const PlatformHost&;
    [[nodiscard]] auto GetWindow() noexcept -> ZHLN::Optional<Window&>;
    auto AddWindow(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver = {})
        -> ZHLN::Optional<Window&>;
    void RemoveWindow(Window& window);

    [[nodiscard]] auto AcquireTarget() noexcept -> FrameOutcome<FrameTarget>;
    [[nodiscard]] auto AcquireTarget(Window& window) noexcept -> FrameOutcome<FrameTarget>;
    [[nodiscard]] auto GetAcquiredTarget() noexcept -> std::optional<FrameTarget>;
    [[nodiscard]] auto GetAcquiredTarget(Window& window) noexcept -> std::optional<FrameTarget>;

    auto GetKernel() -> Kernel&;
    auto GetWorld() -> World&;

    // This execution's clock and scratch pool. Engine services are not part of
    // it: they belong to the graphs, bound once (see EnsureSystemGraphs).
    auto MakeFrame(float dt) -> Frame;

    // Hand every worker's scratch arena back. Called at the start of each graph,
    // where no system is running: the memory the previous graph's systems were
    // handed stops being valid here.
    void ResetWorkerScratch() noexcept;

    auto               GetPhysicsContext() -> PhysicsContext&;
    auto               GetRenderContext() -> RenderContext&;
    auto               GetSceneData() -> SceneData&;
    auto               GetAssetManager() -> AssetManager&;
    auto               GetAudioContext() -> AudioContext&;
    auto               GetInput() noexcept -> InputContext&;
    [[nodiscard]] auto GetInput() const noexcept -> const InputContext&;
    auto               GetFileSystemWatcher() -> FileSystemWatcher&;
    [[nodiscard]] auto GetRegistry() -> ECS::Registry&;
    [[nodiscard]] auto GetRegistry() const -> const ECS::Registry&;

    auto GetUpdateGraph() -> SimGraph&;
    auto GetRenderGraph() -> RenderGraph&;
    auto GetMainECB() -> ECS::EntityCommandBuffer&;
    auto GetEventBus() noexcept -> ECS::EventBus&;
    [[nodiscard]] auto GetEventBus() const noexcept -> const ECS::EventBus&;
    // Run the batched cleanup pass now (normally after ECB playback each frame).
    void ProcessPendingDestroy();
    // Release every scene-owned resource before clearing the registry/ECB.
    void ClearScene();
    // Register an extra bulk cleanup system once per Engine, not per entity.
    [[nodiscard]] auto AddSceneCleanupPass(SceneCleanupPass pass) -> bool;
    void RunSceneCleanupPasses(bool all);

    // Simulation-produced skinning poses, drained once per frame by RenderSystem.
    auto               GetPoseUploads() -> PoseUploadQueue&;
    auto               GetArticulationSystem() -> ArticulationSystem&;
    auto               GetVisibleEntities() -> JPH::Array<Entity>&;
    auto               GetVisibleShadowEntities() -> JPH::Array<Entity>&;
    auto               GetCurrentAlpha() -> float&;
    auto GetPhysicsAccumulator() -> float&;

    // Host-owned, per-engine state (also used by the glTF inspector).
    [[nodiscard]] auto GetGameState() const -> void*;
    void               SetGameState(void* state);
    [[nodiscard]] auto GetCurrentFrame() const noexcept -> uint64_t;

    void SetUICallback(UICallback callback);

    void               SetPendingUIData(UIDrawData uiData) noexcept;
    [[nodiscard]] auto GetPendingUIData() const noexcept -> UIDrawData;

    void               SetFontAtlas(const FontAtlas& fontAtlas) noexcept;
    [[nodiscard]] auto GetFontAtlas() -> const FontAtlas&;
    [[nodiscard]] auto GetFontAtlas() const noexcept -> const FontAtlas&;

    [[nodiscard]] auto ConsumeUIInput(float dt) noexcept -> GUI::UIInput;
    void               SubmitUI(UIDrawData uiData) noexcept;

    void AddDeviceLostCallback(DeviceLostCallback callback);

    // --- Composition ---------------------------------------------------------
    //
    // What a module contributes arrives here, because installing one is an act
    // the application performs -- not something a module does to the engine on
    // its way past. Installing:
    //
    //   * composes the module's declared frame steps into the frame plan, which
    //     is all-or-nothing and reports FramePlanError::AnchorMissing rather
    //     than placing a step somewhere it was not asked to be;
    //   * registers the module's components (idempotent; families survive a
    //     scene reset);
    //   * records the module, so its graph systems, scene cleanup, teardown and
    //     script verbs are applied by the engine-owned lifecycle that needs them
    //     and so the composition can be reported.
    //
    // A failure leaves nothing half-applied: the plan is untouched and no
    // components are registered.
    [[nodiscard]] auto InstallModule(const Module& module) -> std::expected<void, FramePlanError>;

    // What the application installed, in install order. Read-only: the engine
    // keeps these as the record of the composition, not as a registry of
    // callbacks to replay.
    [[nodiscard]] auto GetInstalledModules() const noexcept -> std::span<const Module>;

    // The non-const form may build core's steps on first use, so it is the one
    // accessor here that is not noexcept; the const form reads what exists.
    [[nodiscard]] auto GetFramePlan() -> FramePlan&;
    [[nodiscard]] auto GetFramePlan() const noexcept -> const FramePlan&;

    void               SetPhysicsSubstepHooks(PhysicsSubstepHooks hooks);
    [[nodiscard]] auto GetPhysicsSubstepHooks() const noexcept -> const PhysicsSubstepHooks&;

    [[nodiscard]] auto Services() noexcept -> ECS::ExtensionServices&;
    [[nodiscard]] auto Services() const noexcept -> const ECS::ExtensionServices&;

    template <typename T, typename... Args>
    auto EmplaceService(Args&&... args) -> T& {
        return Services().Emplace<T>(std::forward<Args>(args)...);
    }

    template <typename T>
    [[nodiscard]] auto Service() noexcept -> ZHLN::Optional<T&> {
        return Services().Get<T>();
    }

    template <typename T>
    [[nodiscard]] auto Service() const noexcept -> ZHLN::Optional<const T&> {
        return Services().Get<T>();
    }

    void AddTeardownHook(TeardownHook hook);

    [[nodiscard]] auto DeviceLostCallbackCount() const noexcept -> size_t;
    [[nodiscard]] auto GetUICallback() const noexcept -> ZHLN::Optional<const UICallback&>;

    void ProvokeDeviceLost();

    auto InitializeDefaultScene() -> bool;

    auto Tick(float dt) -> FrameStatus;

    [[nodiscard]] auto FallbackSceneEnabled() const noexcept -> bool;

    void EnsureFontAtlas();

    // Best-effort diagnostic snapshot (engine summary, active camera + SIMD
    // frustum planes, and physics body/contact trace) exposed for caller-owned
    // crash/signal handlers without exposing Engine internals.
    void TraceDiagnostics() const noexcept;

    // The composition root's install pass, run once the engine exists and before
    // the first scene. It names the modules the engine runs with; the engine
    // cannot name them itself (ARCHITECTURE.md 1.2).
    using ModuleInstaller = void (*)(Engine&);
    using EventPoller     = void (*)(Engine&);
    // Low-level runner for custom hosts. The optional event poller is an
    // explicit frame seam (not a lifecycle hook smuggled through EngineConfig).
    static auto Run(const EngineConfig& config = {}, UICallback uiCallback = nullptr, ModuleInstaller installModules = nullptr,
                    EventPoller pollEvents = nullptr) -> std::expected<void, ErrorCode>;

  private:
    // Consume window-input resize notifications during the event pump. The
    // application and bespoke hosts never need to inspect ECS resize flags.
    void SynchronizeResize();

    // Build the service bundles and the two typed graphs on first use.
    void EnsureSystemGraphs();

    // Core's own frame steps, placed once. Not a scene-reset concern: the plan a
    // module composed into has to survive a scene rebuild, and rebuilding it per
    // scene is what used to accumulate duplicate steps.
    void EnsureFramePlan();

    auto InitInternal(const EngineConfig& cfg) -> std::expected<void, ErrorCode>;

    std::unique_ptr<EngineImpl> _impl;
};

// Mark an entity and its hierarchy for the Engine's batched scene cleanup.
// Components remain readable until ProcessPendingDestroy/SceneCleanup runs.
void DespawnEntity(Engine& engine, Entity entity);

}
