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
class FrameScheduler;
} // namespace ECS

class CullingSystem;

class ZHLN_API Engine {
  public:
    using UICallback = std::function<void(Engine&)>;

    Engine();
    Engine(const EngineConfig& cfg);
    Engine(const EngineConfig& cfg, bool& outSuccess);
    ~Engine();

    auto HandleDeviceLost() noexcept -> std::expected<void, Error>;

    static auto Create(const EngineConfig& cfg) -> std::expected<std::unique_ptr<Engine>, Error>;

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
    [[nodiscard]] auto GetFrameScheduler() -> ECS::FrameScheduler&;
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

auto GetEngineContext() -> Engine*;
} // namespace ZHLN
