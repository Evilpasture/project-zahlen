// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/EngineCOM.cpp
//
// One object implements the whole public API. Implements<> aggregates the
// interface branches and reflection synthesises QueryRaw across them, so this
// file contains no interface map, no switch and no GUID table -- only the
// translation between boundary vocabulary and the engine's own.
//
// Zahlen builds with -fno-exceptions, so the noexcept on every method here is
// a statement of fact rather than a promise that needs a try/catch to keep.

#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IActionContext.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IAudioDevice.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Engine/AssetManager.hpp>
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/AudioEngine.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/Internal/EngineAccess.hpp>
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Input/InputContext.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace ZHLN::COM {

namespace {

// The registry keys dynamic families by name and mints the family id on first
// use. Resolving per call keeps the boundary stateless; the registry's own
// lookup is the cache.
[[nodiscard]] auto FamilyFor(ECS::Registry& registry, StringView typeName, size_t size, size_t alignment) noexcept -> uint32_t {
    return registry.RegisterComponentDynamic(std::string_view(typeName), size, alignment);
}

constexpr uint32_t kInvalidFamily = 0xFFFFFFFFu;

} // namespace

class EngineCOM final
    : public Implements<EngineCOM, IEngine, IGraphicsDevice, IEntities, ISimulation, IAudioDevice, IAssetLibrary,
                        IActionContext> {
public:
    explicit EngineCOM(std::unique_ptr<Engine> engine) noexcept: m_engine(std::move(engine)) {
    }

    // ---------------------------------------------------------------- IEngine
    [[nodiscard]] bool IsRunning() const noexcept override {
        return m_engine->IsRunning();
    }

    void ProcessEvents() noexcept override {
        m_engine->ProcessEvents();
    }

    [[nodiscard]] Result<void> Tick(float dt) noexcept override {
        const auto outcome = m_engine->Tick(dt);
        if (!outcome) return outcome.error();
        return {};
    }

    void ClearScene() noexcept override {
        m_engine->ClearScene();
    }

    [[nodiscard]] IGraphicsDevice* Graphics() noexcept override {
        return static_cast<IGraphicsDevice*>(this);
    }

    [[nodiscard]] IEntities* Entities() noexcept override {
        return static_cast<IEntities*>(this);
    }

    [[nodiscard]] ISimulation* Simulation() noexcept override {
        return static_cast<ISimulation*>(this);
    }

    [[nodiscard]] IAudioDevice* Audio() noexcept override {
        return static_cast<IAudioDevice*>(this);
    }

    [[nodiscard]] IAssetLibrary* Assets() noexcept override {
        return static_cast<IAssetLibrary*>(this);
    }

    [[nodiscard]] IActionContext* Actions() noexcept override {
        return static_cast<IActionContext*>(this);
    }

    // -------------------------------------------------------- IGraphicsDevice
    void SetResolution(Extent2D resolution) noexcept override {
        m_engine->Graphics().SetResolution(resolution);
    }

    [[nodiscard]] ViewportRect Viewport() const noexcept override {
        return Device().Viewport();
    }

    [[nodiscard]] uint32_t FrameIndex() const noexcept override {
        return Device().FrameIndex();
    }

    [[nodiscard]] BufferID CreateBuffer(BufferUsage usage, Span<const std::byte> data) noexcept override {
        return m_engine->Graphics().CreateBuffer(BufferDesc {.usage = usage, .data = std::span<const std::byte>(data)});
    }

    void DestroyBuffer(BufferID handle) noexcept override {
        m_engine->Graphics().DestroyBuffer(handle);
    }

    [[nodiscard]] Result<void> UpdateBuffer(BufferID handle, Span<const std::byte> data) noexcept override {
        const auto outcome = m_engine->Graphics().UpdateBuffer(handle, std::span<const std::byte>(data));
        if (!outcome) return outcome.error();
        return {};
    }

    [[nodiscard]] Result<TextureID> CreateTexture(StringView name, Span<const std::byte> rgba, Extent2D extent,
                                                  bool isSRGB) noexcept override {
        auto outcome = m_engine->Graphics().CreateTexture(std::string_view(name), std::span<const std::byte>(rgba), extent, isSRGB);
        if (!outcome) return outcome.error();
        return *outcome;
    }

    void UnloadTexture(TextureID handle) noexcept override {
        m_engine->Graphics().UnloadTexture(handle);
    }

    // -------------------------------------------------------------- IEntities
    [[nodiscard]] Entity CreateEntity() noexcept override {
        return m_engine->Entities().Create();
    }

    void Despawn(Entity entity) noexcept override {
        m_engine->Entities().Despawn(entity);
    }

    [[nodiscard]] bool IsAlive(Entity entity) const noexcept override {
        return m_engine->Entities().IsAlive(entity);
    }

    [[nodiscard]] void* AttachRaw(Entity entity, StringView typeName, uint32_t size, uint32_t alignment) noexcept override {
        auto&          registry = EngineAccess::RegistryOf(*m_engine);
        const uint32_t family   = FamilyFor(registry, typeName, size, alignment);
        if (family == kInvalidFamily) return nullptr;
        return registry.AddDynamic(entity, family);
    }

    bool DetachRaw(Entity entity, StringView typeName) noexcept override {
        auto&          registry = EngineAccess::RegistryOf(*m_engine);
        const uint32_t family   = ECS::Registry::GetFamilyIDFromName(std::string_view(typeName));
        if (family == kInvalidFamily) return false;
        return registry.RemoveDynamic(entity, family);
    }

    [[nodiscard]] void* GetRaw(Entity entity, StringView typeName) const noexcept override {
        const uint32_t family = ECS::Registry::GetFamilyIDFromName(std::string_view(typeName));
        if (family == kInvalidFamily) return nullptr;
        return EngineAccess::RegistryOf(*m_engine).GetRawByFamily(entity, family);
    }

    // ------------------------------------------------------------ ISimulation
    [[nodiscard]] Physics::RaycastResult Raycast(Double3 origin, Float3 direction, float maxDistance,
                                                 Physics::BodyID ignore) const noexcept override {
        return Sim().Raycast(origin, direction, maxDistance, ignore);
    }

    // The facade's std::vector is built and destroyed entirely inside this
    // library; what crosses is scalars into the caller's own storage.
    [[nodiscard]] uint32_t BodiesInSphere(Double3 center, float radius, Physics::BodyID* bodies,
                                          uint32_t capacity) const noexcept override {
        const std::vector<Physics::BodyID> found = Sim().BodiesInSphere(center, radius);
        const auto                         total = static_cast<uint32_t>(found.size());
        if (bodies != nullptr && capacity > 0) {
            std::copy_n(found.begin(), std::min(total, capacity), bodies);
        }
        return total;
    }

    [[nodiscard]] Option<Double3> BodyPosition(Physics::BodyID body) const noexcept override {
        const std::optional<Double3> found = Sim().BodyPosition(body);
        if (!found.has_value()) return std::nullopt;
        return *found;
    }

    [[nodiscard]] Option<Physics::BodyStateSnapshot> BodyState(Physics::BodyID body) const noexcept override {
        const std::optional<Physics::BodyStateSnapshot> found = Sim().BodyState(body);
        if (!found.has_value()) return std::nullopt;
        return *found;
    }

    [[nodiscard]] Float3 CharacterVelocity(Physics::BodyID body) const noexcept override {
        return Sim().CharacterVelocity(body);
    }

    [[nodiscard]] bool IsCharacterOnGround(Physics::BodyID body) const noexcept override {
        return Sim().IsCharacterOnGround(body);
    }

    [[nodiscard]] bool IsBodyDynamic(Physics::BodyID body) const noexcept override {
        return Sim().IsBodyDynamic(body);
    }

    void AddImpulse(Physics::BodyID body, Float3 impulse) noexcept override {
        Sim().AddImpulse(body, impulse);
    }

    void SetCharacterVelocity(Physics::BodyID body, Float3 velocity) noexcept override {
        Sim().SetCharacterVelocity(body, velocity);
    }

    void DestroyBody(Physics::BodyID body) noexcept override {
        Sim().DestroyBody(body);
    }

    void DestroyBodies(Span<const Physics::BodyID> bodies) noexcept override {
        Sim().DestroyBodies(std::span<const Physics::BodyID>(bodies));
    }

    void DestroyRagdolls(Span<const Physics::RagdollID> ragdolls) noexcept override {
        Sim().DestroyRagdolls(std::span<const Physics::RagdollID>(ragdolls));
    }

    void Step(float deltaTime) noexcept override {
        Sim().Step(deltaTime);
    }

    [[nodiscard]] Physics::BodyID CreateCharacter(Double3 position) noexcept override {
        return Sim().CreateCharacter(position);
    }

    [[nodiscard]] Physics::DebugDrawData DebugGeometry(bool shapes, bool constraints, bool wireframe) noexcept override {
        return Sim().DebugDrawData(shapes, constraints, wireframe);
    }

    // ----------------------------------------------------------- IAudioDevice
    [[nodiscard]] bool Initialized() const noexcept override {
        return Sound().Initialized();
    }

    void PostEvent(const AudioEvent& event) noexcept override {
        Sound().PostEvent(event);
    }

    [[nodiscard]] VoiceID CreateVoice(StringView filepath, const VoiceDesc& desc) noexcept override {
        return Sound().CreateVoice(std::string_view(filepath), desc);
    }

    void PlayVoice(VoiceID voice) noexcept override {
        Sound().PlayVoice(voice);
    }

    void StopVoice(VoiceID voice, float fadeOutSeconds) noexcept override {
        Sound().StopVoice(voice, fadeOutSeconds);
    }

    [[nodiscard]] bool IsVoiceValid(VoiceID voice) const noexcept override {
        return Sound().IsVoiceValid(voice);
    }

    void SetVoiceVolume(VoiceID voice, float volume) noexcept override {
        Sound().SetVoiceVolume(voice, volume);
    }

    void SetVoiceLooping(VoiceID voice, bool looping) noexcept override {
        Sound().SetVoiceLooping(voice, looping);
    }

    void SetVoicePosition(VoiceID voice, Float3 position) noexcept override {
        Sound().SetVoicePosition(voice, position);
    }

    // ---------------------------------------------------------- IAssetLibrary
    [[nodiscard]] uint64_t PrefabHash(StringView virtualPath) const noexcept override {
        return HashAssetPath(std::string_view(virtualPath));
    }

    [[nodiscard]] bool HasPrefab(uint64_t hash) const noexcept override {
        return Library().BorrowCachedPrefab(hash) != nullptr;
    }

    [[nodiscard]] Option<PrefabInfo> DescribePrefab(uint64_t hash) const noexcept override {
        const ModelPrefab* prefab = Library().BorrowCachedPrefab(hash);
        if (prefab == nullptr) return std::nullopt;

        float radius = 0.0F;
        for (const ModelPart& part : prefab->parts) {
            radius = std::max(radius, part.boundingRadius);
        }
        return PrefabInfo {
            .hash           = hash,
            .partCount      = static_cast<uint32_t>(prefab->parts.size()),
            .nodeCount      = static_cast<uint32_t>(prefab->nodes.size()),
            .skeletonCount  = static_cast<uint32_t>(prefab->skeletons.size()),
            .animationCount = static_cast<uint32_t>(prefab->animations.size()),
            .lightCount     = static_cast<uint32_t>(prefab->lights.size()),
            .boundingRadius = radius,
        };
    }

    [[nodiscard]] uint32_t PrefabVirtualPath(uint64_t hash, char* buffer, uint32_t capacity) const noexcept override {
        const ModelPrefab* prefab = Library().BorrowCachedPrefab(hash);
        if (prefab == nullptr) return 0;

        const std::string_view path   = std::string_view(prefab->virtualPath);
        const auto             length = static_cast<uint32_t>(path.size());
        if (buffer != nullptr && capacity > 0) {
            const uint32_t copied = std::min(length, capacity - 1);
            std::memcpy(buffer, path.data(), copied);
            buffer[copied] = '\0';
        }
        return length;
    }

    [[nodiscard]] uint32_t Instantiate(uint64_t hash, const PrefabFactory::SpawnParams& params, Entity* entities,
                                       uint32_t capacity) noexcept override {
        const ModelPrefab* prefab = Library().BorrowCachedPrefab(hash);
        if (prefab == nullptr) return 0;
        return PrefabFactory::InstantiatePrefab(*m_engine, *prefab, params, entities, capacity);
    }

    [[nodiscard]] Option<EnvironmentImageInfo> FindEnvironmentImage(StringView key) const noexcept override {
        const std::optional<EnvironmentImageView> view = Library().FindEnvironmentImage(std::string_view(key));
        if (!view.has_value()) return std::nullopt;
        return EnvironmentImageInfo {
            .contentHash         = view->contentHash,
            .width               = view->width,
            .height              = view->height,
            .hasSun              = view->sun.has_value(),
            .hasLightingPanorama = !view->lightingRgba.empty(),
        };
    }

    [[nodiscard]] Span<const float> EnvironmentPixels(StringView key) const noexcept override {
        const std::optional<EnvironmentImageView> view = Library().FindEnvironmentImage(std::string_view(key));
        if (!view.has_value()) return {};
        return Span<const float>(view->rgba);
    }

    // --------------------------------------------------------- IActionContext
    void BeginFrame() noexcept override {
        Router().BeginFrame();
    }

    void RefreshActions() noexcept override {
        Router().RefreshActionsForCurrentFrame(&m_engine->GetEventBus());
    }

    void EvaluateFrame(float deltaSeconds) noexcept override {
        Router().EvaluateFrame(deltaSeconds, &m_engine->GetEventBus());
    }

    void InjectKey(KeyCode key, bool down, PlayerSeat seat) noexcept override {
        Router().InjectKey(key, down, seat);
    }

    void InjectMouseMove(float x, float y, PlayerSeat seat) noexcept override {
        Router().InjectMouseMove(x, y, seat);
    }

    void InjectMouseRelative(float x, float y, float dx, float dy, PlayerSeat seat) noexcept override {
        Router().InjectMouseRelative(x, y, dx, dy, seat);
    }

    void InjectMouseScroll(float delta, PlayerSeat seat) noexcept override {
        Router().InjectMouseScroll(delta, seat);
    }

    void InjectGamepadConnection(uint8_t slot, bool connected, StringView name) noexcept override {
        Router().InjectGamepadConnection(slot, connected, std::string_view(name));
    }

    void InjectGamepadButton(uint8_t slot, GamepadButton button, bool down) noexcept override {
        Router().InjectGamepadButton(slot, button, down);
    }

    void InjectGamepadAxis(uint8_t slot, GamepadAxis axis, float value) noexcept override {
        Router().InjectGamepadAxis(slot, axis, value);
    }

    [[nodiscard]] ActionState Action(ActionID id, PlayerSeat seat) const noexcept override {
        return Router().Action(id, seat);
    }

    [[nodiscard]] bool KeyDown(KeyCode key, PlayerSeat seat) const noexcept override {
        return Router().SeatKeyboardMouse(seat).IsKeyDown(key);
    }

    [[nodiscard]] bool KeyPressed(KeyCode key, PlayerSeat seat) const noexcept override {
        return Router().SeatKeyboardMouse(seat).IsKeyPressed(key);
    }

    [[nodiscard]] bool KeyReleased(KeyCode key, PlayerSeat seat) const noexcept override {
        return Router().SeatKeyboardMouse(seat).IsKeyReleased(key);
    }

    [[nodiscard]] Axis2DValue MousePosition(PlayerSeat seat) const noexcept override {
        const auto& kbm = Router().SeatKeyboardMouse(seat);
        return Axis2DValue {.x = kbm.mouseX, .y = kbm.mouseY};
    }

    [[nodiscard]] Axis2DValue MouseDelta(PlayerSeat seat) const noexcept override {
        const auto& kbm = Router().SeatKeyboardMouse(seat);
        return Axis2DValue {.x = kbm.mouseDeltaX, .y = kbm.mouseDeltaY};
    }

    [[nodiscard]] float MouseWheel(PlayerSeat seat) const noexcept override {
        return Router().SeatKeyboardMouse(seat).mouseWheel;
    }

    [[nodiscard]] InputDeviceKind LastActiveDevice(PlayerSeat seat) const noexcept override {
        return Router().LastActiveDevice(seat);
    }

    [[nodiscard]] uint32_t ConnectedGamepadCount() const noexcept override {
        return static_cast<uint32_t>(Router().ConnectedGamepadCount());
    }

    [[nodiscard]] bool GamepadConnected(uint8_t slot) const noexcept override {
        const auto pad = Router().Gamepad(slot);
        return static_cast<bool>(pad) && pad->connected;
    }

    [[nodiscard]] bool GamepadButtonDown(uint8_t slot, GamepadButton button) const noexcept override {
        const auto pad = Router().Gamepad(slot);
        return static_cast<bool>(pad) && pad->IsButtonDown(button);
    }

    [[nodiscard]] float GamepadAxisValue(uint8_t slot, GamepadAxis axis) const noexcept override {
        const auto pad = Router().Gamepad(slot);
        if (!pad) return 0.0F;
        return pad->Axis(axis);
    }

    [[nodiscard]] PlayerSeat GamepadSeat(uint8_t slot) const noexcept override {
        return Router().GamepadSeat(slot);
    }

    bool AssignGamepadToSeat(uint8_t slot, PlayerSeat seat) noexcept override {
        return Router().AssignGamepadToSeat(slot, seat);
    }

private:
    friend class Implements<EngineCOM, IEngine, IGraphicsDevice, IEntities, ISimulation, IAudioDevice, IAssetLibrary,
                            IActionContext>;

    ~EngineCOM() = default;

    // The capability accessors are non-const and hand back a fresh facade by
    // value, so a const boundary method still has to ask the engine mutably.
    // The facades are two pointers wide; this is a load, not an allocation.
    //
    // Each is named away from its interface accessor on purpose: Simulation(),
    // Audio(), Assets() and Actions() are already taken by IEngine above, and
    // a member name would hide the engine type of the same spelling.
    [[nodiscard]] auto Device() const noexcept -> GraphicsDevice {
        return const_cast<Engine&>(*m_engine).Graphics();
    }

    [[nodiscard]] auto Sim() const noexcept -> ZHLN::Simulation {
        return const_cast<Engine&>(*m_engine).Physics();
    }

    [[nodiscard]] auto Sound() const noexcept -> ZHLN::AudioEngine {
        return const_cast<Engine&>(*m_engine).Audio();
    }

    [[nodiscard]] auto Library() const noexcept -> ZHLN::AssetLibrary {
        return const_cast<Engine&>(*m_engine).Assets();
    }

    [[nodiscard]] auto Router() const noexcept -> ZHLN::InputContext& {
        return const_cast<Engine&>(*m_engine).GetInput();
    }

    std::unique_ptr<Engine> m_engine;
};

auto CreateEngine(const EngineConfigCOM& config) noexcept -> Result<IEngine*> {
    EngineConfig internal {};
    internal.appName           = String64(std::string_view(config.appName));
    internal.width             = config.width;
    internal.height            = config.height;
    internal.vsync             = config.vsync;
    internal.validationMode    = config.enableValidation ? ValidationMode::On : ValidationMode::Off;
    internal.fullscreen        = config.fullscreen;
    internal.headless          = config.headless;
    internal.enableMeshShading = config.enableMeshShading;
    internal.fpsLimit          = config.fpsLimit;
    if (config.crashDumpPath.size > 0) {
        internal.crashDumpPath = std::string(std::string_view(config.crashDumpPath));
    }

    auto created = Engine::Create(internal);
    if (!created) return created.error();

    auto* instance = new (std::nothrow) EngineCOM(std::move(*created));
    if (instance == nullptr) return ErrorCode(ComError::AllocationFailed);

    return static_cast<IEngine*>(instance);
}

} // namespace ZHLN::COM
