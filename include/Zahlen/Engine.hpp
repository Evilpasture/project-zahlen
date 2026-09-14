// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Engine.hpp
#pragma once

#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/CrashState.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Types.hpp>       // GameplayStatus, JPH::Array
#include <Zahlen/Viewport.hpp>    // ViewportMode
#include <Zahlen/WindowInput.hpp> // WindowInputReceiver
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>

namespace ZHLN {

class RenderContext;
class PhysicsContext;
class AudioContext;
class CreativeWorksManager;
class ScriptRunner;
class FileSystemWatcher;
class EngineFrameStepAccess;
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

class ZHLN_API Engine {
  public:
    using UICallback = std::function<void(Engine&)>;

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

    auto HandleDeviceLost() noexcept -> std::expected<void, Error>;

    /// Builds an engine. Every external service receives this instance
    /// explicitly; no process-global engine context is published.
    static auto Create(const EngineConfig& cfg) -> std::expected<std::unique_ptr<Engine>, Error>;

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

    /**
     * @brief Convenience entry point that manages the main loop, frame limiting,
     *        and clean shutdown.
     */
    static auto Run(const CommandLineOptions& options, CrashState& crashState, UICallback uiCallback = nullptr) -> std::expected<void, Error>;

  private:
    friend class EngineFrameStepAccess;

    auto                        InitInternal(const EngineConfig& cfg) -> std::expected<void, Error>;

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
