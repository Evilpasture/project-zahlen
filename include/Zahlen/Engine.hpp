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
#include <Zahlen/EngineCode.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Types.hpp>
#include <expected>
#include <memory>

namespace ZHLN {

class InputContext;
class Window;
class RenderContext;
class PhysicsContext;
class AudioContext;
class CreativeWorksManager;
struct Camera;
struct EngineImpl;

namespace ALife {
class Simulator;
}

namespace ECS {
class Registry;
class SystemGraph;
class EntityCommandBuffer;
} // namespace ECS

class CullingSystem;

class ZHLN_API Engine {
  public:
    Engine();
    Engine(const EngineConfig& cfg);
    Engine(const EngineConfig& cfg, bool& outSuccess);
    ~Engine();

    void HandleDeviceLost() noexcept;

    static std::expected<std::unique_ptr<Engine>, Error> Create(const EngineConfig& cfg);

    [[nodiscard]] bool IsRunning() const;
    void               ProcessEvents();
    [[nodiscard]] bool BeginFrame(bool& outDeviceLost) noexcept;
    [[nodiscard]] bool EndFrame(bool& outDeviceLost) noexcept;

    Window&                            GetWindow();
    PhysicsContext&                    GetPhysicsContext();
    RenderContext&                     GetRenderContext();
    InputContext&                      GetInput();
    Camera&                            GetCamera();
    ALife::Simulator&                  GetALife();
    CreativeWorksManager&              GetCreativeWorksManager();
    AudioContext&                      GetAudioContext();
    [[nodiscard]] ECS::Registry&       GetRegistry();
    [[nodiscard]] const ECS::Registry& GetRegistry() const;

    ECS::SystemGraph&         GetUpdateGraph();
    ECS::SystemGraph&         GetRenderGraph();
    ECS::EntityCommandBuffer& GetMainECB();
    CullingSystem&            GetCullingSystem();
    JPH::Array<Entity>&       GetVisibleEntities();
    JPH::Array<Entity>&       GetVisibleShadowEntities();
    float&                    GetCurrentAlpha();

    [[nodiscard]] void*    GetGameState() const;
    void                   SetGameState(void* state);
    [[nodiscard]] uint64_t GetCurrentFrame() const noexcept;

    void ProvokeDeviceLost();

    /**
     * @brief Registers default engine components, camera, lighting settings,
     *        UI settings, and compiles internal System Graphs.
     */
    bool InitializeDefaultScene();

    /**
     * @brief Executes a single synchronized frame tick in canonical order.
     * @param dt Frame delta time in seconds.
     * @param driver Gameplay driver (Cpp, Fennel, or Hybrid).
     */
    GameplayStatus Tick(float dt, GameplayDriver driver = GameplayDriver::Cpp);

    /**
     * @brief Convenience entry point that manages the main loop, frame limiting,
     *        and clean shutdown.
     */
    static int Run(const CommandLineOptions& options);

  private:
    std::expected<void, Error>  InitInternal(const EngineConfig& cfg);
    std::unique_ptr<EngineImpl> _impl;
};

Engine* GetEngineContext();
} // namespace ZHLN
