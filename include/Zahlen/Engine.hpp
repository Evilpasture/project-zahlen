// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Engine.hpp
#pragma once
// clang-format off
#include <Jolt/Jolt.h>
#include <Jolt/Core/Array.h>
// clang-format on

#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/Description.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Types.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>

namespace ZHLN {

// ============================================================================
// Core Lifecycle Errors (Tier 3)
// Application bootstrap code branches on these specific failure reasons.
// ============================================================================

enum class EngineInitError : uint8_t {
    WindowCreationFailed[[= ZHLN::Description<"Window creation failed"> {}]] = 1,
    TTYInitializationFailed[[= ZHLN::Description<"TTY initialization failed"> {}]],
    RenderInitializationFailed[[= ZHLN::Description<"Render initialization failed"> {}]],
    PhysicsInitializationFailed[[= ZHLN::Description<"Physics initialization failed"> {}]],
    AudioInitializationFailed[[= ZHLN::Description<"Audio initialization failed"> {}]],
    AssetInitializationFailed[[= ZHLN::Description<"Asset initialization failed"> {}]],
    EngineAllocationFailed[[= ZHLN::Description<"Engine instance allocation failed"> {}]],
};

class Window;
class RenderContext;
class PhysicsContext;
class AudioContext;
class CreativeWorksManager;
class ScriptRunner;
struct Camera;
struct EngineImpl;

namespace ECS {
class Registry;
class SystemGraph;
class EntityCommandBuffer;
} // namespace ECS

class FrameScheduler;

class Engine;

/// Publishes an engine as the ambient one for GetEngineContext(), for exactly
/// as long as the scope object lives.
///
/// Ambient access exists for the callers that structurally cannot be handed an
/// engine: component OnDestroy hooks (see include/ARCHITECTURE.md), the
/// terminal-signal diagnostic dump, and the scripting C ABI. Everything else
/// should take an `Engine&`.
///
/// The registration is owned by whoever opened it, not assigned by the engine
/// to itself. Engine::Create packages one into the ScopedEngine it returns, so
/// GetEngineContext() can never name a destroyed engine -- which it used to,
/// because the old `g_CurrentEngine = this` in initialisation had no
/// counterpart in teardown and a failed Create left the pointer aimed at freed
/// memory.
///
/// Scopes nest: the ambient engine is the most recently published one that is
/// still alive, and dropping a scope out of order falls back to the next one
/// still standing rather than to a stale pointer. A scope should be released on
/// the thread that created it; the process-wide fallback is corrected either
/// way, but the per-thread override is not.
class EngineContextScope {
  public:
    explicit EngineContextScope(Engine& engine);
    ~EngineContextScope();

    EngineContextScope(const EngineContextScope&)                    = delete;
    auto operator=(const EngineContextScope&) -> EngineContextScope& = delete;
    EngineContextScope(EngineContextScope&&)                         = delete;
    auto operator=(EngineContextScope&&) -> EngineContextScope&      = delete;

  private:
    Engine* _engine;
};

/// An engine and its ambient registration, owned as one thing.
///
/// Engine::Create hands this back instead of a bare unique_ptr so the two
/// lifetimes cannot drift apart. Nothing publishes itself: the caller holds the
/// registration, and when the caller drops it both the engine and its entry in
/// the ambient chain go away together, in that order -- the engine is torn down
/// while still published, because clearing the registry runs component
/// OnDestroy hooks that call GetEngineContext().
///
/// Shaped like the std::unique_ptr<Engine> it replaces: `*engine`, `engine->`,
/// `engine.get()`, `engine != nullptr` and `.reset()` all mean what they did.
class ZHLN_API ScopedEngine {
  public:
    ScopedEngine() noexcept = default;
    /// Publishes `engine` immediately, before it is initialised -- exactly when
    /// the old globals were assigned, so anything reached during initialisation
    /// still sees an ambient engine.
    explicit ScopedEngine(std::unique_ptr<Engine> engine);
    ~ScopedEngine();

    ScopedEngine(ScopedEngine&&) noexcept;
    auto operator=(ScopedEngine&&) noexcept -> ScopedEngine&;
    ScopedEngine(const ScopedEngine&)                    = delete;
    auto operator=(const ScopedEngine&) -> ScopedEngine& = delete;

    [[nodiscard]] auto get() const noexcept -> Engine* { return _engine.get(); }
    auto               operator->() const noexcept -> Engine* { return _engine.get(); }
    auto               operator*() const noexcept -> Engine& { return *_engine; }
    explicit           operator bool() const noexcept { return _engine != nullptr; }

    /// Destroys the engine and withdraws its registration.
    void reset();

    friend auto operator==(const ScopedEngine& lhs, std::nullptr_t) noexcept -> bool { return lhs._engine == nullptr; }

  private:
    // Declaration order is the teardown contract: _engine is destroyed first,
    // while _scope still publishes it.
    std::unique_ptr<EngineContextScope> _scope;
    std::unique_ptr<Engine>             _engine;
};


class CullingSystem;

class ZHLN_API Engine {
  public:
    using UICallback = std::function<void(Engine&)>;

    Engine();
    /// Legacy direct construction. Unlike Engine::Create these do not publish
    /// the engine for GetEngineContext(); open an EngineContextScope over the
    /// instance if the ambient callbacks need to find it.
    Engine(const EngineConfig& cfg);
    Engine(const EngineConfig& cfg, bool& outSuccess);
    ~Engine();

    auto HandleDeviceLost() noexcept -> std::expected<void, Error>;

    /// Builds an engine and publishes it as the ambient context for as long as
    /// the returned owner lives. See ScopedEngine.
    static auto Create(const EngineConfig& cfg) -> std::expected<ScopedEngine, Error>;

    [[nodiscard]] auto IsRunning() const -> bool;
    void               ProcessEvents();
    [[nodiscard]] auto BeginFrame(bool& outDeviceLost) noexcept -> bool;
    [[nodiscard]] auto EndFrame(bool& outDeviceLost) noexcept -> bool;

    auto               GetWindow() -> Window&;
    auto               GetPhysicsContext() -> PhysicsContext&;
    auto               GetRenderContext() -> RenderContext&;
    auto               GetCamera() -> Camera&;
    auto               GetCreativeWorksManager() -> CreativeWorksManager&;
    auto               GetAudioContext() -> AudioContext&;
    auto               GetScriptRunner() -> ScriptRunner&;
    [[nodiscard]] auto GetRegistry() -> ECS::Registry&;
    [[nodiscard]] auto GetRegistry() const -> const ECS::Registry&;

    auto GetUpdateGraph() -> ECS::SystemGraph&;
    auto GetRenderGraph() -> ECS::SystemGraph&;
    auto GetMainECB() -> ECS::EntityCommandBuffer&;
    /// The frame's ordered phase steps. `Tick` executes exactly this list.
    [[nodiscard]] auto GetFrameScheduler() -> FrameScheduler&;
    auto               GetCullingSystem() -> CullingSystem&;
    auto               GetVisibleEntities() -> JPH::Array<Entity>&;
    auto               GetVisibleShadowEntities() -> JPH::Array<Entity>&;
    auto               GetCurrentAlpha() -> float&;

    [[nodiscard]] auto GetGameState() const -> void*;
    void               SetGameState(void* state);
    [[nodiscard]] auto GetCurrentFrame() const noexcept -> uint64_t;

    void SetUICallback(UICallback callback);
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
    static auto Run(const CommandLineOptions& options, UICallback uiCallback = nullptr) -> std::expected<void, Error>;

  private:
    auto                        InitInternal(const EngineConfig& cfg) -> std::expected<void, Error>;
    std::unique_ptr<EngineImpl> _impl;
};

/// The innermost live EngineContextScope's engine, or nullptr when none is
/// published. Prefer passing an `Engine&`; see EngineContextScope for the cases
/// that cannot.
auto GetEngineContext() -> Engine*;
} // namespace ZHLN
