// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/EngineCOM.cpp
//
// One object implements the whole SDK surface. Implements<> aggregates the
// interface branches and reflection synthesises QueryRaw across them, so this
// file contains no interface map, no switch and no GUID table -- only the
// translation between boundary vocabulary and the engine's own.
//
// Zahlen builds with -fno-exceptions, so the noexcept on every method here is
// a statement of fact rather than a promise that needs a try/catch to keep.

#include <Zahlen/COM/EngineFactory.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/Internal/EngineAccess.hpp>

#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>

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

class EngineCOM final : public Implements<EngineCOM, IEngine, IGraphicsDevice, IEntities> {
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

private:
    friend class Implements<EngineCOM, IEngine, IGraphicsDevice, IEntities>;

    ~EngineCOM() = default;

    // Engine::Graphics() is non-const and hands back a fresh facade by value.
    [[nodiscard]] auto Device() const noexcept -> GraphicsDevice {
        return const_cast<Engine&>(*m_engine).Graphics();
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
