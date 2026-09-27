// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Jolt/Jolt.h>
#include <Zahlen/CommandLine.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/CrashState.hpp>
#include <Zahlen/Core/String.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/FrameResult.hpp>
#include <Zahlen/SystemContext.hpp>
#include <Zahlen/WindowInput.hpp>
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
}

class FrameScheduler;

class CullingSystem;
class ArticulationSystem;

class ZHLN_API Engine {
  public:
    using UICallback = std::function<void(Engine&)>;

    using FrameSchedulerExtension = void (*)(FrameScheduler&);

    using SystemGraphsExtension = void (*)(ECS::SystemGraph& updateGraph, ECS::SystemGraph& renderGraph);

    struct CharacterStepHooks {
        void (*preStep)(Engine&, float dt) = nullptr;
        void (*postStep)(Engine&)          = nullptr;
    };

    using FreeCamSpeedQuery = std::optional<float> (*)(ECS::Registry&, Entity target);

    using TeardownHook = void (*)(Engine&);

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
    [[nodiscard]] auto GetWindow() noexcept -> Window*;
    auto AddWindow(const String32& title, uint32_t width, uint32_t height, bool fullscreen, const WindowInputReceiver& receiver = {}) -> Window*;
    void RemoveWindow(Window& window);

    [[nodiscard]] auto AcquireTarget() noexcept -> FrameOutcome<RenderAttachment>;
    [[nodiscard]] auto AcquireTarget(Window& window) noexcept -> FrameOutcome<RenderAttachment>;
    [[nodiscard]] auto GetTargetAttachment() noexcept -> std::optional<RenderAttachment>;
    [[nodiscard]] auto GetTargetAttachment(Window& window) noexcept -> std::optional<RenderAttachment>;

    auto GetKernel() -> Kernel&;
    auto GetWorld() -> World&;

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
    [[nodiscard]] auto GetFrameScheduler() -> FrameScheduler&;
    auto               GetCullingSystem() -> CullingSystem&;
    auto               GetArticulationSystem() -> ArticulationSystem&;
    auto               GetVisibleEntities() -> JPH::Array<Entity>&;
    auto               GetVisibleShadowEntities() -> JPH::Array<Entity>&;
    auto               GetCurrentAlpha() -> float&;
    auto GetPhysicsAccumulator() -> float&;

    [[nodiscard]] auto GetGameState() const -> void*;
    void               SetGameState(void* state);
    [[nodiscard]] auto GetCurrentFrame() const noexcept -> uint64_t;

    void SetUICallback(UICallback callback);

    void               SetPendingUIData(const UIDrawData& uiData) noexcept;
    [[nodiscard]] auto GetPendingUIData() const noexcept -> UIDrawData;

    void AddDeviceLostCallback(DeviceLostCallback callback);


    void AddFrameSchedulerExtension(FrameSchedulerExtension ext);

    void AddSystemGraphsExtension(SystemGraphsExtension ext);

    void ApplyFrameSchedulerExtensions(FrameScheduler& scheduler);
    void ApplySystemGraphsExtensions(ECS::SystemGraph& updateGraph, ECS::SystemGraph& renderGraph);

    void               SetCharacterStepHooks(CharacterStepHooks hooks);
    [[nodiscard]] auto GetCharacterStepHooks() const noexcept -> const CharacterStepHooks&;

    void               SetBonePosePostProcessor(BonePosePostProcessor processor);
    [[nodiscard]] auto GetBonePosePostProcessor() const noexcept -> BonePosePostProcessor;

    void               SetFreeCamSpeedQuery(FreeCamSpeedQuery query);
    [[nodiscard]] auto GetFreeCamSpeedQuery() const noexcept -> FreeCamSpeedQuery;

    void AddTeardownHook(TeardownHook hook);

    [[nodiscard]] auto DeviceLostCallbackCount() const noexcept -> size_t;
    [[nodiscard]] auto GetUICallback() const noexcept -> const UICallback*;

    void ProvokeDeviceLost();

    auto InitializeDefaultScene() -> bool;

    auto Tick(float dt, GameplayDriver driver = GameplayDriver::Cpp) -> GameplayStatus;

    auto UpdateNativeGameplay(float dt) -> GameplayStatus;

    [[nodiscard]] auto IsNativeGameplayLoaded() const noexcept -> bool;

    [[nodiscard]] auto FallbackSceneEnabled() const noexcept -> bool;

    void SeedSceneFontAtlas(ECS::Registry& reg);

    using ExtensionInstaller = void (*)(Engine&);

    static auto Run(const CommandLineOptions& options, CrashState& crashState, UICallback uiCallback = nullptr, ExtensionInstaller installExtensions = nullptr)
        -> std::expected<void, ErrorCode>;

  private:
    auto InitInternal(const EngineConfig& cfg) -> std::expected<void, ErrorCode>;

    void RegisterBootScriptWatches();

    std::unique_ptr<EngineImpl> _impl;
};

void DespawnEntity(Engine& engine, Entity entity);

}
