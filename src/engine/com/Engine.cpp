#include <engine/InputContext.hpp>
#include <engine/KernelSession.hpp>
#include <engine/PrefabFactoryInternal.hpp>
#include <audio/Audio.hpp>
#include <engine/AssetManager.hpp>
#include <engine/SceneResources.hpp>
#include <engine/World.hpp>
#include <physics/Physics.hpp>
#include <render/RenderContext.hpp>
#include <render/SceneData.hpp>
// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

// src/engine/com/Engine.cpp
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
#include <Zahlen/COM/IDebugDraw.hpp>
#include <Zahlen/COM/IEngine.hpp>
#include <Zahlen/COM/IEntities.hpp>
#include <Zahlen/COM/IFramePipeline.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <Zahlen/COM/ISimulation.hpp>
#include <Zahlen/COM/IState.hpp>
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/Reflection/CanonicalName.hpp>
#include <Zahlen/Core/Reflection/LayoutHash.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <ecs/ECS.hpp>
#include <engine/AssetManager.hpp>
#include <engine/Engine.hpp>
#include <engine/ModelPrefab.hpp>
#include <engine/PublishedComponents.hpp>
#include <engine/PoseUploads.hpp>
#include <render/RenderGraphDump.hpp>
#include <engine/PrefabFactory.hpp>
#include <engine/SceneResources.hpp>
#include <Zahlen/FileSystem/VFS.hpp>
#include <Zahlen/Input/ActionValues.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Physics/PhysicsShapes.hpp>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <Zahlen/Render/EnvironmentImage.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Render/Meshlet.hpp>
#include <render/RenderContext.hpp>
#include <window/PlatformHost.hpp>
#include <window/Window.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::COM {

static_assert(AuditInterfaceABI<IEngine>());
static_assert(AuditInterfaceABI<IFramePipeline>());
static_assert(AuditInterfaceABI<IGraphicsDevice>());
static_assert(AuditInterfaceABI<IEntities>());
static_assert(AuditInterfaceABI<ISimulation>());
static_assert(AuditInterfaceABI<IAudioDevice>());
static_assert(AuditInterfaceABI<IAssetLibrary>());
static_assert(AuditInterfaceABI<IActionContext>());
static_assert(AuditInterfaceABI<IDebugDraw>());

namespace {

// Validate boundary views before forming any std view or doing pointer arithmetic.
template <typename T> bool Valid(Span<T> value) noexcept { return value.size == 0 || value.data != nullptr; }
bool Valid(StringView value) noexcept { return value.size == 0 || value.data != nullptr; }
auto Text(StringView value) noexcept -> std::string_view { return value.size ? std::string_view(value) : std::string_view {}; }
template <typename T> auto ReadNode(const ChainInNode* chain) noexcept -> Result<T> {
    if (!chain) return Error::NullArgument;
    std::optional<T> value;
    const auto result = DispatchInChain<T>(chain, [&](const T& node) -> Result<void> { value = node; return {}; });
    if (!result.HasValue()) return result.error;
    if (!value) return Error::InvalidArgument;
    return *value;
}

// The registry keys dynamic families by name and mints the family id on first
// use. Resolving per call keeps the boundary stateless; the registry's own
// lookup is the cache.
[[nodiscard]] auto FamilyFor(ECS::Registry& registry, StringView typeName, size_t size, size_t alignment) noexcept
    -> uint32_t {
    return registry.RegisterComponentDynamic(std::string_view(typeName), size, alignment);
}

// The engine components a COM consumer may attach to typed storage.
//
// Membership is a public ABI commitment: a published component's layout is
// frozen for every consumer built against it, which is why this list is
// written out by hand rather than deduced from "everything that happens to be
// trivially copyable".
//
// Components satisfying SceneResources::OwnsExternalResource<T> release their
// owned Jolt bodies, ragdolls, audio voices, or GPU mesh/skin buffers before
// overwrite or detach so COM consumers cannot leak external handles.
template <typename T>
[[nodiscard]] consteval auto Publish() -> PublishedComponent {
    static_assert(std::is_trivially_copyable_v<T>,
                  "a published component must be trivially copyable: the boundary copies it as bytes");
    static_assert(std::is_standard_layout_v<T>,
                  "a published component must be standard layout: the consumer models it from the same offsets");
    return PublishedComponent {
        .name       = Reflect::CanonicalTypeName<T>(),
        .size       = static_cast<uint32_t>(sizeof(T)),
        .alignment  = static_cast<uint32_t>(alignof(T)),
        .layoutHash = Reflect::LayoutHash<T>(),
        .familyID   = +[]() noexcept -> uint32_t { return ECS::ComponentFamily::GetTypeID<T>(); },
        .ensure     = +[](ZHLN::Engine& engine, ECS::Registry& registry, Entity entity) noexcept -> void* {
            const uint32_t id = ECS::ComponentFamily::GetTypeID<T>();
            if (void* existing = registry.GetRawByFamily(entity, id)) {
                if constexpr (SceneResources::OwnsExternalResource<T>) {
                    SceneResources::Release(engine, *static_cast<T*>(existing));
                    *static_cast<T*>(existing) = T {};
                }
                return existing;
            }
            // Add() panics on a dead entity rather than returning; the caller
            // has already refused those, and this keeps that contract local.
            if (!registry.IsAlive(entity))
                return nullptr;
            return &registry.Add<T>(entity);
        },
        .remove = +[](ZHLN::Engine& engine, ECS::Registry& registry, Entity entity) noexcept -> bool {
            if constexpr (SceneResources::OwnsExternalResource<T>) {
                return SceneResources::Detach<T>(engine, registry, entity);
            } else {
                const uint32_t id = ECS::ComponentFamily::GetTypeID<T>();
                if (registry.GetRawByFamily(entity, id) == nullptr)
                    return false;
                registry.Remove<T>(entity);
                return true;
            }
        },
        .fetch = +[](const ECS::Registry& registry, Entity entity) noexcept -> void* {
            return registry.GetRawByFamily(entity, ECS::ComponentFamily::GetTypeID<T>());
        },
    };
}

constexpr std::array kPublishedComponents {
    Publish<Components::PendingDestroy>(),
    Publish<Components::TransformComponent>(),
    Publish<Components::WorldTransformComponent>(),
    Publish<Components::HierarchyComponent>(),
    Publish<Components::NameComponent>(),
    Publish<Components::PrefabInstanceComponent>(),
    Publish<Components::SceneSourceComponent>(),
    Publish<Components::PBRComponent>(),
    Publish<Components::AmbientCubeComponent>(),
    Publish<Components::MeshComponent>(),
    Publish<Components::OwnedMeshComponent>(),
    Publish<Components::SkeletalMeshComponent>(),
    Publish<Components::MorphTargetComponent>(),
    Publish<Components::LODComponent>(),
    Publish<Components::CameraComponent>(),
    Publish<Components::MainCameraTagComponent>(),
    Publish<Components::LightComponent>(),
    Publish<Components::SceneLightTagComponent>(),
    Publish<Components::EnvironmentSunTagComponent>(),
    Publish<Components::SunTagComponent>(),
    Publish<Components::EnvironmentMapComponent>(),
    Publish<Components::DecalComponent>(),
    Publish<Components::ParticleEmitterComponent>(),
    Publish<Components::MeshParticleEmitterComponent>(),
    Publish<Components::VolumetricFogComponent>(),
    Publish<Components::VolumetricVolumeComponent>(),
    Publish<Components::CharacterMotorComponent>(),
    Publish<Components::CharacterMotorStateComponent>(),
    Publish<Components::PhysicsComponent>(),
    Publish<Components::ImpulseCommand>(),
    Publish<Components::RagdollComponent>(),
    Publish<Components::RagdollHitReactionCommand>(),
    Publish<Components::RagdollImpulseCommand>(),
    Publish<Components::AudioListenerComponent>(),
    Publish<Components::AudioSourceComponent>(),
    Publish<Components::PlayerTagComponent>(),
    Publish<Components::PlayerSeatComponent>(),
};

[[nodiscard]] auto FindPublished(ZHLN::Engine& engine, StringView typeName) noexcept -> const PublishedComponent* {
    const std::string_view wanted(typeName);
    for (const auto& entry : kPublishedComponents) {
        if (entry.name == wanted) return &entry;
    }
    return nullptr;
}

constexpr uint32_t kInvalidFamily = 0xFFFFFFFFu;

[[nodiscard]] auto MakePrimitiveSubShape(const Physics::CompoundChildDesc& child) noexcept
    -> std::optional<Physics::AnyPrimitiveShape> {
    switch (child.kind) {
        case Physics::PrimitiveColliderKind::Box:
            if (child.dimensions.x <= 0.0F || child.dimensions.y <= 0.0F || child.dimensions.z <= 0.0F) {
                return std::nullopt;
            }
            return Physics::AnyPrimitiveShape {Physics::Shape<Physics::Box>(child.dimensions)};
        case Physics::PrimitiveColliderKind::Sphere:
            if (child.dimensions.x <= 0.0F) return std::nullopt;
            return Physics::AnyPrimitiveShape {Physics::Shape<Physics::Sphere>(child.dimensions.x)};
        case Physics::PrimitiveColliderKind::Capsule:
            if (child.dimensions.x <= 0.0F || child.dimensions.y <= 0.0F) return std::nullopt;
            return Physics::AnyPrimitiveShape {Physics::Shape<Physics::Capsule>(child.dimensions.x, child.dimensions.y)};
        case Physics::PrimitiveColliderKind::Cylinder:
            if (child.dimensions.x <= 0.0F || child.dimensions.y <= 0.0F) return std::nullopt;
            return Physics::AnyPrimitiveShape {
                Physics::Shape<Physics::Cylinder>(child.dimensions.x, child.dimensions.y)};
        case Physics::PrimitiveColliderKind::Plane:
            return Physics::AnyPrimitiveShape {Physics::Shape<Physics::Plane>(child.dimensions, child.constant)};
    }
    return std::nullopt;
}

[[nodiscard]] auto ValidateAndBuildMeshGeometry(const FVec3* vertices, size_t vertexCount, const uint32_t* indices,
                                                size_t indexCount) -> std::optional<Physics::MeshGeometry> {
    if (vertices == nullptr || vertexCount == 0 || indices == nullptr || indexCount < 3 || (indexCount % 3U) != 0U) {
        return std::nullopt;
    }
    for (size_t i = 0; i < indexCount; ++i) {
        if (indices[i] >= vertexCount) return std::nullopt;
    }
    Physics::MeshGeometry geom {};
    geom.vertices.assign(vertices, vertices + vertexCount);
    geom.indices.assign(indices, indices + indexCount);
    return geom;
}

} // namespace

class Engine final:
    public Implements<
        Engine,
        IEngine,
        IGraphicsDevice,
        IEntities,
        ISimulation,
        IAudioDevice,
        IAssetLibrary,
        IActionContext,
        IState,
        IDebugDraw,
        IFramePipeline> {
  public:
    explicit Engine(std::unique_ptr<ZHLN::Engine> engine) noexcept : m_engine(std::move(engine)) {}

    void TraceDiagnostics() const noexcept override;

    // ---------------------------------------------------------------- IEngine
    [[nodiscard]] bool IsRunning() const noexcept override {
        return m_engine->IsRunning();
    }

    void ProcessEvents() noexcept override {
        m_engine->ProcessEvents();
    }

    void RequestClose() noexcept override {
        Session().CloseWindow();
    }

    Result<void> Tick(float dt) noexcept override {
        const auto outcome = m_engine->Tick(dt);
        if (!outcome) return outcome.error();
        return {};
    }

    Result<void> AdvanceFrame(float dt) noexcept override {
        const auto result = m_engine->AdvanceFrame(dt);
        if (!result)
            return result.error();
        return {};
    }
    Result<void> FinishFrame() noexcept override {
        const auto result = m_engine->FinishFrame();
        if (!result)
            return result.error();
        return {};
    }
    Result<void> CancelFrame() noexcept override {
        const auto result = m_engine->CancelFrame();
        if (!result)
            return result.error();
        return {};
    }
    [[nodiscard]] bool FramePending() const noexcept override {
        return m_engine->FramePending();
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

    [[nodiscard]] IState* State() noexcept override {
        return static_cast<IState*>(this);
    }

    [[nodiscard]] IDebugDraw* Debug() noexcept override { return static_cast<IDebugDraw*>(this); }
    void SetMaxFramesPerSecond(uint32_t fps) noexcept override { m_engine->SetMaxFramesPerSecond(fps); }
    [[nodiscard]] uint32_t MaxFramesPerSecond() const noexcept override { return m_engine->MaxFramesPerSecond(); }
    [[nodiscard]] uint64_t CurrentFrame() const noexcept override { return m_engine->GetCurrentFrame(); }
    [[nodiscard]] uint64_t DeviceGeneration() const noexcept override { return m_engine->DeviceGeneration(); }

    Result<void> RecreateGraphicsDevice() noexcept override {
        if (m_engine->FramePending())
            return FramePipelineError::FrameInProgress;
        const auto result = m_engine->HandleDeviceLost();
        if (!result) return result.error();
        return {};
    }

    Result<uint64_t> AddDeviceLostCallback(void (*callback)(void*), void* userdata) noexcept override {
        if (callback == nullptr) return Error::NullArgument;
        if (!m_deviceLostListeners) {
            m_deviceLostListeners = std::make_shared<DeviceLostListeners>();
            // Only one engine hook, irrespective of registration churn. A weak
            // capture cannot prolong listener or COM object lifetime.
            std::weak_ptr<DeviceLostListeners> weak = m_deviceLostListeners;
            m_engine->AddDeviceLostCallback([weak](ZHLN::Engine&) {
                if (auto state = weak.lock()) {
                    const auto snapshot = state->entries;
                    for (const auto& entry: snapshot) {
                        const auto live = std::ranges::find(state->entries, entry.token, &DeviceLostEntry::token);
                        if (live != state->entries.end()) {
                            // Copy before invoking: callbacks may add/remove
                            // entries and reallocate the live vector.
                            const auto call = *live;
                            call.callback(call.userdata);
                        }
                    }
                }
            });
        }
        auto& state = *m_deviceLostListeners;
        if (state.nextToken == 0) return Error::InvalidArgument;
        const uint64_t token = state.nextToken++;
        state.entries.push_back({token, callback, userdata});
        return token;
    }

    void RemoveDeviceLostCallback(uint64_t token) noexcept override {
        if (m_deviceLostListeners) {
            std::erase_if(m_deviceLostListeners->entries, [token](const auto& entry) { return entry.token == token; });
        }
    }

    Result<WindowID> AddWindow(const ChainInNode* chain) noexcept override {
        const auto info = ReadNode<WindowCreateInfo>(chain);
        if (!info.HasValue()) return info.error;
        if (!Valid(info.value.title)) return Error::NullArgument;
        if (info.value.width == 0 || info.value.height == 0 || info.value.title.size >= 32) return Error::InvalidArgument;
        const auto id = m_engine->AddWindow(String32(Text(info.value.title)), info.value.width, info.value.height, info.value.fullscreen);
        if (id == WindowID::Invalid) return Error::InvalidArgument;
        return id;
    }
    void RemoveWindow(WindowID window) noexcept override {
        if (window == WindowID::Primary) return;
        Session().SetWindowFileDropHandler(window, nullptr, nullptr);
        m_dropSubscriptions.erase(window);
        m_engine->RemoveWindow(window);
    }
    [[nodiscard]] bool IsWindowRunning(WindowID window) const noexcept override { return Session().WindowRunning(window); }
    void CloseWindow(WindowID window) noexcept override { Session().CloseWindow(window); }
    void FocusWindow(WindowID window) noexcept override { Session().FocusWindow(window); }
    void SetCursorMode(WindowID window, CursorMode mode) noexcept override { Session().SetWindowCursorMode(window, mode); }
    [[nodiscard]] CursorMode GetCursorMode(WindowID window) const noexcept override { return Session().WindowCursorMode(window); }
    void CaptureMouse(WindowID window, bool captured) noexcept override { Session().CaptureWindowMouse(window, captured); }
    [[nodiscard]] bool IsMouseCaptured(WindowID window) const noexcept override { return Session().WindowMouseCaptured(window); }
    Result<void> SetClipboardText(StringView text) noexcept override {
        if (!Valid(text)) return Error::NullArgument;
        Session().SetClipboard(Text(text));
        return {};
    }
    Result<uint32_t> ClipboardText(char* buffer, uint32_t capacity) const noexcept override {
        if (buffer == nullptr && capacity != 0) return Error::NullArgument;
        const auto text = Session().Clipboard();
        if (text.size() > UINT32_MAX) return Error::InvalidArgument;
        if (buffer && capacity >= text.size() && !text.empty()) std::memcpy(buffer, text.data(), text.size());
        return static_cast<uint32_t>(text.size());
    }
    Result<void> SetFileDropCallback(WindowID window, FileDropCallback callback, void* userdata) noexcept override {
        if (!WindowExists(window)) return Error::InvalidArgument;
        // Detach before releasing the stable userdata object. A callback may
        // replace/remove itself; the trampoline copies call state first.
        Session().SetWindowFileDropHandler(window, nullptr, nullptr);
        m_dropSubscriptions.erase(window);
        if (!callback) return {};
        auto subscription = std::make_unique<DropSubscription>(DropSubscription {window, callback, userdata});
        auto* stable = subscription.get();
        m_dropSubscriptions.emplace(window, std::move(subscription));
        Session().SetWindowFileDropHandler(window, +[](void* opaque, const FileDrop* files, uint32_t count) {
            const auto call = *static_cast<const DropSubscription*>(opaque);
            std::vector<FileDropView> views;
            views.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                const auto& file = files[i];
                views.push_back({StringView(file.format), StringView(file.fileName), StringView(file.sourcePath), file.data, file.byteSize});
            }
            call.callback(call.userdata, call.window, views);
        }, stable);
        return {};
    }
    [[nodiscard]] Option<float> PacedDeltaTime() const noexcept override {
        const auto value = Device().GetPacedDeltaTime();
        return value ? Option<float>(*value) : Option<float> {};
    }
    Result<void> ReadHostInput(WindowID window, ChainOutNode* output, bool consume) noexcept override {
        if (!output) return Error::NullArgument;
        if (!WindowExists(window)) return Error::InvalidArgument;
        const auto checked = DispatchOutChain<HostInputInfo>(output, [](HostInputInfo&) -> Result<void> { return {}; });
        if (!checked.HasValue()) return checked;
        const auto state = consume ? Session().InputFor(window).ConsumePhysicalInput() : Session().InputFor(window).SeatKeyboardMouse();
        HostInputInfo info;
        info.mouseX = state.mouseX; info.mouseY = state.mouseY; info.mouseWheel = state.mouseWheel;
        info.modifiers = state.Modifiers();
        for (uint32_t i = 0; i < kKeyCodeCount; ++i) info.keyDown[i] = state.IsKeyDown(static_cast<KeyCode>(i));
        for (size_t i = 0; i < state.queuedInputCount; ++i) {
            const auto& event = state.queuedInput[i];
            if (event.isChar) { if (info.characterCount < 64) info.characters[info.characterCount++] = event.codepoint; }
            else if (info.pressedKeyCount < 64) info.pressedKeys[info.pressedKeyCount++] = event.key;
        }
        return DispatchOutChain<HostInputInfo>(output, [&](HostInputInfo& out) -> Result<void> { out = info; return {}; });
    }
    Result<void> SubmitUI(WindowID window, const ChainInNode* chain) noexcept override {
        const auto info = ReadNode<UISubmitInfo>(chain);
        if (!info.HasValue()) return info.error;
        const auto& ui = info.value;
        if (!WindowExists(window)) return Error::InvalidArgument;
        if (!Valid(ui.batches) || !Valid(ui.positions) || !Valid(ui.surfaces)) return Error::NullArgument;
        if (ui.positions.size != ui.surfaces.size) return Error::InvalidArgument;
        for (const auto& batch: std::span<const UIBatch>(ui.batches)) {
            if (batch.vertexStart > ui.positions.size || batch.vertexCount > ui.positions.size - batch.vertexStart || batch.vertexCount % 3 != 0)
                return Error::InvalidArgument;
        }
        Session().InputFor(window).SyncCaptureFlags(ui.wantsCaptureKeyboard, ui.wantsCaptureMouse);
        Device().SubmitUI(window, UIDrawData {.batches = ui.batches, .positions = ui.positions, .surfaces = ui.surfaces,
            .wantsCaptureMouse = ui.wantsCaptureMouse, .wantsCaptureKeyboard = ui.wantsCaptureKeyboard}, ui.append);
        return {};
    }

    Result<uint32_t> AllocateJoints(uint32_t count) noexcept override {
        const auto offset = m_engine->AllocateJoints(count);
        if (offset == UINT32_MAX) return Error::InvalidArgument;
        return offset;
    }
    Result<void> UpdateJointMatrices(uint32_t offset, Span<const Mat4x4> matrices) noexcept override {
        if (!Valid(matrices)) return Error::NullArgument;
        if (offset > 8192 || matrices.size > 8192 - offset) return Error::InvalidArgument;
        if (matrices.size) Device().UpdateJointMatrices(offset, matrices);
        return {};
    }
    Result<uint32_t> AllocateMorphDeltas(Span<const float> deltas) noexcept override {
        if (!Valid(deltas)) return Error::NullArgument;
        if (deltas.size == 0 || deltas.size > UINT32_MAX || deltas.size % 4 != 0) return Error::InvalidArgument;
        const auto offset = Device().AllocateMorphDeltas(deltas);
        if (offset == UINT32_MAX) return Error::InvalidArgument;
        return offset;
    }
    Result<TextureID> CreateTextureCube(const ChainInNode* chain) noexcept override {
        const auto info = ReadNode<TextureCubeCreateInfo>(chain);
        if (!info.HasValue()) return info.error;
        const auto& cube = info.value;
        const uint64_t texels = uint64_t(cube.faceSize) * cube.faceSize;
        if (!cube.faceSize || texels > SIZE_MAX / 4) return Error::InvalidArgument;
        std::array<std::span<const std::byte>, 6> faces;
        for (size_t i = 0; i < faces.size(); ++i) {
            if (!Valid(cube.faces[i])) return Error::NullArgument;
            if (cube.faces[i].size != texels * 4) return Error::InvalidArgument;
            faces[i] = cube.faces[i];
        }
        const auto result = Device().CreateTextureCube(faces, cube.faceSize);
        if (!result) return result.error();
        return *result;
    }
    Result<TextureID> CreateRenderTexture(const ChainInNode* chain) noexcept override {
        const auto info = ReadNode<RenderTextureCreateInfo>(chain);
        if (!info.HasValue()) return info.error;
        if (!info.value.extent.width || !info.value.extent.height) return Error::InvalidArgument;
        const auto result = Device().CreateRenderTexture(info.value.extent.width, info.value.extent.height, info.value.hdr);
        if (!result) return result.error();
        return *result;
    }
    void DestroyRenderTexture(TextureID texture) noexcept override { Device().DestroyRenderTexture(texture); }
    Result<void> SetEnvironmentRadiance(const ChainInNode* chain) noexcept override {
        const auto info = ReadNode<EnvironmentRadianceInfo>(chain);
        if (!info.HasValue()) return info.error;
        const auto& env = info.value;
        if (!Valid(env.rgba) || !Valid(env.lightingRgba) || !Valid(env.diffuseSH)) return Error::NullArgument;
        const uint64_t texels = uint64_t(env.extent.width) * env.extent.height;
        if (!texels || texels > SIZE_MAX / 4 || env.rgba.size != texels * 4 ||
            (env.lightingRgba.size && env.lightingRgba.size != env.rgba.size) ||
            (env.diffuseSH.size && env.diffuseSH.size != 9)) return Error::InvalidArgument;
        const auto result = Device().SetEnvironmentRadiance(EnvironmentRadianceDesc {
            .rgba = env.rgba, .lightingRgba = env.lightingRgba, .diffuseSH = env.diffuseSH,
            .extent = env.extent, .contentHash = env.contentHash, .renderSkybox = env.renderSkybox});
        if (!result) return result.error();
        return {};
    }
    Result<uint64_t> CaptureFrame(ChainOutNode* info, Span<uint8_t> pixels) noexcept override {
        if (!Valid(pixels)) return Error::NullArgument;
        if (!info) return Error::NullArgument;
        const auto checked = DispatchOutChain<CaptureInfo>(info, [](CaptureInfo&) -> Result<void> { return {}; });
        if (!checked.HasValue()) return checked.error;
        const auto frame = Device().CaptureFrame();
        if (!frame) return frame.error();
        const auto written = DispatchOutChain<CaptureInfo>(info, [&](CaptureInfo& out) -> Result<void> {
            out = {frame->extent, frame->format, frame->metrics, frame->pixels.size()};
            return {};
        });
        if (!written.HasValue()) return written.error;
        if (pixels.data && pixels.size >= frame->pixels.size() && !frame->pixels.empty())
            std::memcpy(pixels.data, frame->pixels.data(), frame->pixels.size());
        return static_cast<uint64_t>(frame->pixels.size());
    }

    // ------------------------------------------------------------- IDebugDraw
    void AddLine(const FVec3& start, const FVec3& end, const FVec4& colorStart, const FVec4& colorEnd) noexcept override {
        m_engine->Scene().AddLine(start, end, colorStart, colorEnd);
    }
    Result<void> AddBillboards(TextureID texture, Span<const BillboardQuad> quads, bool additive) noexcept override {
        if (quads.size != 0 && quads.data == nullptr) return Error::NullArgument;
        if (quads.size != 0) m_engine->Scene().AddBillboards(texture, std::span<const BillboardQuad>(quads), additive);
        return {};
    }
    Result<void> AddDebugTriangles(Span<const VertexPosition> positions,
                                                 Span<const VertexSurface> surfaces) noexcept override {
        if ((positions.size != 0 && positions.data == nullptr) ||
            (surfaces.size != 0 && surfaces.data == nullptr)) return Error::NullArgument;
        if (positions.size != surfaces.size || positions.size % 3 != 0) return Error::InvalidArgument;
        if (positions.size != 0) m_engine->Scene().AddDebugTriangles(std::span<const VertexPosition>(positions),
                                                                   std::span<const VertexSurface>(surfaces));
        return {};
    }
    Result<void> SubmitParticles(const ChainInNode* chain) noexcept override {
        if (chain == nullptr) return Error::NullArgument;
        std::optional<ParticleEmitterSubmission> submission;
        const auto valid = DispatchInChain<ParticleEmitterSubmission>(chain, [&](const auto& value) -> Result<void> {
            submission = value;
            return {};
        });
        if (!valid.HasValue()) return valid;
        if (!submission || submission->emitterId == 0 || submission->maxParticles == 0) return Error::InvalidArgument;
        m_engine->Scene().particleEmitters.push_back(*submission);
        return {};
    }
    Result<void> SubmitMeshParticles(const ChainInNode* chain) noexcept override {
        if (chain == nullptr) return Error::NullArgument;
        std::optional<MeshParticleEmitterSubmission> submission;
        const auto valid = DispatchInChain<MeshParticleEmitterSubmission>(chain, [&](const auto& value) -> Result<void> {
            submission = value;
            return {};
        });
        if (!valid.HasValue()) return valid;
        if (!submission || submission->emitterId == 0 || submission->maxParticles == 0) return Error::InvalidArgument;
        m_engine->Scene().meshParticleEmitters.push_back(*submission);
        return {};
    }
    void Clear() noexcept override { m_engine->Scene().Clear(); }

    // ----------------------------------------------------------------- windows
    [[nodiscard]] WindowID PrimaryWindow() const noexcept override {
        return WindowID::Primary;
    }

    [[nodiscard]] uint32_t Windows(WindowID* outWindows, uint32_t capacity) const noexcept override {
        const auto windows = m_engine->WindowIDs();
        if (outWindows) std::copy_n(windows.begin(), std::min<size_t>(capacity, windows.size()), outWindows);
        return static_cast<uint32_t>(windows.size());
    }
    [[nodiscard]] Extent2D WindowExtent(WindowID window) const noexcept override { return Session().WindowExtent(window); }
    [[nodiscard]] bool IsWindowFocused(WindowID window) const noexcept override { return Session().WindowFocused(window); }

    // ----------------------------------------------------------------- IState
    [[nodiscard]] ZHLN::CullingStats CullingStats() const noexcept override {
        return m_engine->CullingStatsSnapshot();
    }

    void ResetCullingStats() noexcept override {
        m_engine->ResetCullingStats();
    }

    void SetCullingEnabled(bool enabled) noexcept override {
        m_engine->SetCullingEnabled(enabled);
    }

    void SetFreezeFrustum(bool freeze) noexcept override {
        m_engine->SetFreezeFrustum(freeze);
    }

    [[nodiscard]] int PhysicsDrawMode() const noexcept override {
        return m_engine->PhysicsDrawMode();
    }

    void SetPhysicsDrawMode(int mode) noexcept override {
        m_engine->SetPhysicsDrawMode(mode);
    }

    // -------------------------------------------------------- IGraphicsDevice
    void SetResolution(Extent2D resolution) noexcept override {
        Device().SetResolution(resolution);
    }

    [[nodiscard]] ViewportRect Viewport() const noexcept override {
        return Device().GetViewport();
    }

    [[nodiscard]] uint32_t FrameIndex() const noexcept override {
        return Device().GetFrameIndex();
    }

    Result<void> ApplySettings(const ChainInNode* settingsChain) noexcept override {
        if (settingsChain == nullptr) return Error::NullArgument;

        GraphicsSettings merged   = Device().GetSettings();
        bool             modified = false;
        std::optional<ViewportRect> viewport;

        const Result<void> chainRes =
            DispatchInChain<GraphicsSettings, AAState, GISettings, ShadowSettings, RayTracingConfig, EnvironmentSettings, ViewportInfo>(
                settingsChain, [&]<typename NodeT>(const NodeT& node) -> Result<void> {
                    if constexpr (std::is_same_v<NodeT, ViewportInfo>) {
                        // Stage until the whole chain validates: no partial writes.
                        viewport = node.viewport;
                        return {};
                    } else if constexpr (std::is_same_v<NodeT, GraphicsSettings>) {
                        merged = node;
                    } else if constexpr (std::is_same_v<NodeT, AAState>) {
                        merged.antiAliasing = node;
                    } else if constexpr (std::is_same_v<NodeT, GISettings>) {
                        merged.post = node;
                    } else if constexpr (std::is_same_v<NodeT, ShadowSettings>) {
                        merged.shadows = node;
                    } else if constexpr (std::is_same_v<NodeT, RayTracingConfig>) {
                        merged.rayTracing = node;
                    } else if constexpr (std::is_same_v<NodeT, EnvironmentSettings>) {
                        merged.environment = node;
                    }
                    modified = true;
                    return {};
                }
            );
        if (!chainRes.HasValue()) return chainRes;
        if (!modified && !viewport)
            return Error::InvalidArgument;
        if (modified)
            Device().ApplySettings(merged);
        if (viewport)
            Device().SetViewport(*viewport);
        return {};
    }

    Result<void> GetSettings(ChainOutNode* settingsChain) const noexcept override {
        const GraphicsSettings& current = Device().GetSettings();
        return DispatchOutChain<GraphicsSettings, AAState, GISettings, ShadowSettings, RayTracingConfig, EnvironmentSettings, GraphicsDeviceInfo>(
            settingsChain, [&]<typename NodeT>(NodeT& out) -> Result<void> {
                if constexpr (std::is_same_v<NodeT, GraphicsDeviceInfo>) {
                    const auto info = Device().GetInfo();
                    out             = {
                        .rendererName         = String64(info.rendererName),
                        .gpuName              = String256(info.gpuName),
                        .deviceType           = info.deviceType,
                        .presentationMode     = info.presentationMode,
                        .pacingPolicy         = info.pacingPolicy,
                        .meshShadingSupported = info.meshShadingSupported,
                        .meshShadingActive    = info.meshShadingActive,
                        .rayTracingSupported  = info.rayTracingSupported
                    };
                } else if constexpr (std::is_same_v<NodeT, GraphicsSettings>) {
                    out = current;
                } else if constexpr (std::is_same_v<NodeT, AAState>) {
                    out = current.antiAliasing;
                } else if constexpr (std::is_same_v<NodeT, GISettings>) {
                    out = current.post;
                } else if constexpr (std::is_same_v<NodeT, ShadowSettings>) {
                    out = current.shadows;
                } else if constexpr (std::is_same_v<NodeT, RayTracingConfig>) {
                    out = current.rayTracing;
                } else if constexpr (std::is_same_v<NodeT, EnvironmentSettings>) {
                    out = current.environment;
                }
                return {};
            }
        );
    }

    Result<BufferID> CreateBuffer(const ChainInNode* bufferChain) noexcept override {
        if (bufferChain == nullptr) return Error::NullArgument;

        std::optional<BufferCreateInfo> createInfo;
        const Result<void>              chainRes =
            DispatchInChain<BufferCreateInfo>(bufferChain, [&](const BufferCreateInfo& info) -> Result<void> {
                createInfo = info;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes.error;
        if (!createInfo.has_value()) return Error::InvalidArgument;

        BufferDesc desc {};
        desc.usage  = createInfo->usage;
        desc.data   = std::span<const std::byte>(createInfo->data);
        desc.size   = createInfo->size != 0 ? createInfo->size : createInfo->data.size;
        desc.stride = createInfo->stride;
        return Device().CreateBuffer(desc);
    }

    void DestroyBuffer(BufferID handle) noexcept override {
        Device().DestroyBuffer(handle);
    }

    Result<void> UpdateBuffer(BufferID handle, Span<const std::byte> data) noexcept override {
        const auto outcome = Device().UpdateBuffer(handle, std::span<const std::byte>(data));
        if (!outcome) return outcome.error();
        return {};
    }

    Result<TextureID> CreateTexture(const ChainInNode* textureChain) noexcept override {
        if (textureChain == nullptr) return Error::NullArgument;

        std::optional<TextureCreateInfo> info;
        const Result<void>               chainRes =
            DispatchInChain<TextureCreateInfo>(textureChain, [&](const TextureCreateInfo& tex) -> Result<void> {
                info = tex;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes.error;
        if (!info.has_value()) return Error::InvalidArgument;
        if (info->pixels.data == nullptr || info->pixels.size == 0 || info->extent.width == 0 || info->extent.height == 0) {
            return Error::InvalidArgument;
        }

        const std::string_view           texName(info->name);
        const std::span<const std::byte> bytes(info->pixels);
        if (info->isHDR) {
            auto hdrRes = Device().CreateTextureHDR(texName, bytes, info->extent, info->generateMips);
            if (!hdrRes) return hdrRes.error();
            return *hdrRes;
        }
        auto texRes = Device().CreateTexture(texName, bytes, info->extent, info->isSRGB, info->generateMips);
        if (!texRes) return texRes.error();
        return *texRes;
    }

    void UnloadTexture(TextureID handle) noexcept override {
        Device().UnloadTexture(handle);
    }

    Result<void> CreateMaterial(const ChainInNode* descChain, ChainOutNode* outMaterialChain) noexcept override {
        if (descChain == nullptr || outMaterialChain == nullptr) return Error::NullArgument;

        std::optional<MaterialDesc> desc;
        uint32_t                    sourceCount = 0;

        const Result<void> chainRes = DispatchInChain<MaterialDesc, PbrMaterialConfig>(
            descChain, [&]<typename NodeT>(const NodeT& node) -> Result<void> {
                ++sourceCount;
                if constexpr (std::is_same_v<NodeT, MaterialDesc>) {
                    desc = node;
                } else if constexpr (std::is_same_v<NodeT, PbrMaterialConfig>) {
                    MaterialDesc built = node.alphaBlend
                                             ? MaterialDesc::Transparent(node.baseColor, node.roughness, node.doubleSided)
                                             : MaterialDesc::Basic(node.baseColor, node.roughness, node.metallic, node.doubleSided);
                    built.unlit             = node.unlit;
                    built.alphaMode         = node.alphaBlend ? 2U : node.alphaMode;
                    built.alphaCutoff       = node.alphaCutoff;
                    built.metallic          = node.metallic;
                    built.roughness         = node.roughness;
                    built.baseColor         = node.baseColor;
                    built.emissive          = node.emissive;
                    built.normalScale       = node.normalScale;
                    built.occlusionStrength = node.occlusionStrength;
                    built.albedoMap         = node.albedoMap;
                    built.normalMap         = node.normalMap;
                    built.pbrMap            = node.pbrMap;
                    built.emissiveMap       = node.emissiveMap;
                    built.occlusionMap      = node.occlusionMap;
                    desc                    = built;
                }
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (sourceCount != 1U || !desc.has_value()) return Error::InvalidArgument;

        auto matRes = Device().CreateMaterial(*desc);
        if (!matRes) return matRes.error();

        return DispatchOutChain<ZHLN::Material>(outMaterialChain, [&](ZHLN::Material& outMat) -> Result<void> {
            outMat = *matRes;
            return {};
        });
    }

    Result<void> RegisterMaterial(MaterialID id, const ChainInNode* materialChain) noexcept override {
        if (materialChain == nullptr) return Error::NullArgument;

        std::optional<ZHLN::Material> mat;
        const Result<void>            chainRes =
            DispatchInChain<ZHLN::Material>(materialChain, [&](const ZHLN::Material& inMat) -> Result<void> {
                mat = inMat;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!mat.has_value()) return Error::InvalidArgument;

        Device().RegisterGPUMaterial(id, *mat);
        return {};
    }

    Result<void> GetMaterial(MaterialID id, ChainOutNode* outMaterialChain) const noexcept override {
        if (outMaterialChain == nullptr) return Error::NullArgument;

        const std::optional<ZHLN::Material> found = Device().GetGPUMaterial(id);
        if (!found.has_value()) return Error::AssetNotCached;
        return DispatchOutChain<ZHLN::Material>(outMaterialChain, [&](ZHLN::Material& outMat) -> Result<void> {
            outMat = *found;
            return {};
        });
    }

    void UnregisterMaterial(MaterialID id) noexcept override {
        Device().UnregisterGPUMaterial(id);
    }

    Result<void> CreateMesh(const ChainInNode* buildChain, ChainOutNode* outMeshChain) noexcept override {
        if (buildChain == nullptr || outMeshChain == nullptr) return Error::NullArgument;

        MeshBuildOptions                   options {};
        std::optional<MeshStreamsDesc>     rawStreams;
        std::optional<BoxMeshDesc>         boxDesc;
        std::optional<PlaneMeshDesc>       planeDesc;
        std::optional<SphereMeshDesc>      sphereDesc;
        std::optional<CylinderMeshDesc>    cylinderDesc;
        std::optional<ConeMeshDesc>        coneDesc;
        std::optional<TetrahedronMeshDesc> tetraDesc;
        uint32_t                           geometryNodeCount = 0;

        const Result<void> chainRes =
            DispatchInChain<MeshBuildOptions, MeshStreamsDesc, BoxMeshDesc, PlaneMeshDesc, SphereMeshDesc,
                            CylinderMeshDesc, ConeMeshDesc, TetrahedronMeshDesc>(
                buildChain, [&]<typename NodeT>(const NodeT& node) -> Result<void> {
                    if constexpr (std::is_same_v<NodeT, MeshBuildOptions>) {
                        options = node;
                    } else {
                        ++geometryNodeCount;
                        if constexpr (std::is_same_v<NodeT, MeshStreamsDesc>) {
                            rawStreams = node;
                        } else if constexpr (std::is_same_v<NodeT, BoxMeshDesc>) {
                            boxDesc = node;
                        } else if constexpr (std::is_same_v<NodeT, PlaneMeshDesc>) {
                            planeDesc = node;
                        } else if constexpr (std::is_same_v<NodeT, SphereMeshDesc>) {
                            sphereDesc = node;
                        } else if constexpr (std::is_same_v<NodeT, CylinderMeshDesc>) {
                            cylinderDesc = node;
                        } else if constexpr (std::is_same_v<NodeT, ConeMeshDesc>) {
                            coneDesc = node;
                        } else if constexpr (std::is_same_v<NodeT, TetrahedronMeshDesc>) {
                            tetraDesc = node;
                        }
                    }
                    return {};
                });
        if (!chainRes.HasValue()) return chainRes;
        if (geometryNodeCount != 1U) return Error::InvalidArgument;

        RenderContext& gfx = Device();
        ZHLN::Mesh     createdMesh {};

        if (rawStreams.has_value()) {
            const MeshStreamsDesc& s = *rawStreams;
            if (s.positions.data == nullptr || s.positions.size == 0) return Error::InvalidArgument;
            if (s.indices16.size > 0 && s.indices32.size > 0) return Error::InvalidArgument;

            const size_t vertexCount = s.positions.size;
            if ((s.tangentFrames.size > 0 && s.tangentFrames.size != vertexCount) ||
                (s.surfaces.size > 0 && s.surfaces.size != vertexCount) ||
                (s.skin.size > 0 && s.skin.size != vertexCount)) {
                return Error::InvalidArgument;
            }

            const std::span<const VertexPosition> posSpan(s.positions);
            createdMesh.posBuffer   = gfx.CreateBuffer<BufferUsage::Vertex>(posSpan);
            createdMesh.vertexCount = static_cast<uint32_t>(vertexCount);

            if (s.tangentFrames.data != nullptr && s.tangentFrames.size > 0) {
                createdMesh.tangentFrameBuffer =
                    gfx.CreateBuffer<BufferUsage::Vertex>(std::span<const VertexTangentFrame>(s.tangentFrames));
            } else {
                const Packed1010102             defNormal  = Math::PackNormal(0.0F, 1.0F, 0.0F);
                const Packed1010102             defTangent = Math::PackNormal(1.0F, 0.0F, 0.0F, 1.0F);
                std::vector<VertexTangentFrame> fallbackFrames(
                    vertexCount, VertexTangentFrame {.normal = defNormal, .tangent = defTangent});
                createdMesh.tangentFrameBuffer = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {fallbackFrames});
            }

            if (s.surfaces.data != nullptr && s.surfaces.size > 0) {
                createdMesh.surfaceBuffer =
                    gfx.CreateBuffer<BufferUsage::Vertex>(std::span<const VertexSurface>(s.surfaces));
            } else {
                const PackedHalf2          defUV    = Math::PackUV(0.0F, 0.0F);
                const PackedRGBA8          defColor = Math::PackColor(1.0F, 1.0F, 1.0F, 1.0F);
                std::vector<VertexSurface> fallbackSurfaces(
                    vertexCount, VertexSurface {.uv = defUV, .color = defColor, .uv1 = defUV});
                createdMesh.surfaceBuffer = gfx.CreateBuffer<BufferUsage::Vertex>(std::span {fallbackSurfaces});
            }

            if (s.skin.data != nullptr && s.skin.size > 0) {
                createdMesh.skinBuffer = gfx.CreateBuffer<BufferUsage::Vertex>(std::span<const VertexSkin>(s.skin));
            }

            if ((s.indices32.data != nullptr && s.indices32.size > 0) ||
                (s.indices16.data != nullptr && s.indices16.size > 0)) {
                std::vector<uint32_t>     widenedIndices;
                std::span<const uint32_t> idxSpan {};
                if (s.indices32.data != nullptr && s.indices32.size > 0) {
                    idxSpan = std::span<const uint32_t>(s.indices32);
                } else {
                    widenedIndices.resize(s.indices16.size);
                    for (size_t i = 0; i < s.indices16.size; ++i) {
                        widenedIndices[i] = s.indices16.data[i];
                    }
                    idxSpan = std::span<const uint32_t> {widenedIndices};
                }
                createdMesh.indexBuffer         = gfx.CreateBuffer<BufferUsage::Index>(idxSpan);
                createdMesh.indexCount          = static_cast<uint32_t>(idxSpan.size());
                const MeshletStreams ml         = gfx.UploadMeshlets(BuildMeshlets(idxSpan, posSpan));
                createdMesh.meshletBuffer       = ml.meshlets;
                createdMesh.meshletVertexBuffer = ml.vertices;
                createdMesh.meshletTriBuffer    = ml.triangles;
                createdMesh.meshletCount        = ml.count;
            } else {
                const MeshletStreams ml         = gfx.UploadMeshlets(BuildMeshlets(posSpan));
                createdMesh.meshletBuffer       = ml.meshlets;
                createdMesh.meshletVertexBuffer = ml.vertices;
                createdMesh.meshletTriBuffer    = ml.triangles;
                createdMesh.meshletCount        = ml.count;
            }

            if (options.buildBLAS) {
                auto blasRes = gfx.BuildMeshBLAS(createdMesh);
                if (!blasRes && !blasRes.error().Is(RenderFeatureError::FeatureNotSupported)) {
                    gfx.DestroyMesh(createdMesh);
                    return blasRes.error();
                }
            }
        } else if (boxDesc.has_value()) {
            createdMesh = PrefabFactory::BuildBoxMesh(gfx, boxDesc->halfExtents, boxDesc->color, options.buildBLAS);
        } else if (planeDesc.has_value()) {
            createdMesh = PrefabFactory::BuildPlaneMesh(gfx, planeDesc->extent, planeDesc->color, options.buildBLAS);
        } else if (sphereDesc.has_value()) {
            createdMesh = PrefabFactory::BuildSphereMesh(gfx, sphereDesc->radius, sphereDesc->color, options.buildBLAS);
        } else if (cylinderDesc.has_value()) {
            createdMesh = PrefabFactory::BuildCylinderMesh(gfx, cylinderDesc->radius, cylinderDesc->height, cylinderDesc->color, options.buildBLAS);
        } else if (coneDesc.has_value()) {
            createdMesh = PrefabFactory::BuildConeMesh(gfx, coneDesc->radius, coneDesc->height, coneDesc->color, options.buildBLAS);
        } else if (tetraDesc.has_value()) {
            createdMesh = PrefabFactory::BuildTetrahedronMesh(gfx, options.buildBLAS);
        }

        return DispatchOutChain<ZHLN::Mesh>(outMeshChain, [&](ZHLN::Mesh& outMesh) -> Result<void> {
            outMesh = createdMesh;
            return {};
        });
    }

    Result<void> RegisterMesh(AssetID id, const ChainInNode* meshChain) noexcept override {
        if (meshChain == nullptr) return Error::NullArgument;

        std::optional<ZHLN::Mesh> mesh;
        const Result<void>        chainRes =
            DispatchInChain<ZHLN::Mesh>(meshChain, [&](const ZHLN::Mesh& inMesh) -> Result<void> {
                mesh = inMesh;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!mesh.has_value()) return Error::InvalidArgument;

        Device().RegisterGPUMesh(id, *mesh);
        return {};
    }

    Result<void> GetMesh(AssetID id, ChainOutNode* outMeshChain) const noexcept override {
        if (outMeshChain == nullptr) return Error::NullArgument;

        const std::optional<ZHLN::Mesh> found = Device().GetGPUMesh(id);
        if (!found.has_value()) return Error::AssetNotCached;
        return DispatchOutChain<ZHLN::Mesh>(outMeshChain, [&](ZHLN::Mesh& outMesh) -> Result<void> {
            outMesh = *found;
            return {};
        });
    }

    void UnregisterMesh(AssetID id) noexcept override {
        Device().UnregisterGPUMesh(id);
    }

    Result<void> DestroyMesh(const ChainInNode* meshChain) noexcept override {
        if (meshChain == nullptr) return Error::NullArgument;

        std::optional<ZHLN::Mesh> mesh;
        const Result<void>        chainRes =
            DispatchInChain<ZHLN::Mesh>(meshChain, [&](const ZHLN::Mesh& inMesh) -> Result<void> {
                mesh = inMesh;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!mesh.has_value()) return Error::InvalidArgument;

        Device().DestroyMesh(*mesh);
        return {};
    }

    Result<void> BuildMeshBLAS(const ChainInNode* inMeshChain, ChainOutNode* outMeshChain) noexcept override {
        if (inMeshChain == nullptr || outMeshChain == nullptr) return Error::NullArgument;

        std::optional<ZHLN::Mesh> mesh;
        const Result<void>        chainRes =
            DispatchInChain<ZHLN::Mesh>(inMeshChain, [&](const ZHLN::Mesh& inMesh) -> Result<void> {
                mesh = inMesh;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!mesh.has_value()) return Error::InvalidArgument;

        auto blasRes = Device().BuildMeshBLAS(*mesh);
        if (!blasRes) return blasRes.error();

        return DispatchOutChain<ZHLN::Mesh>(outMeshChain, [&](ZHLN::Mesh& outMesh) -> Result<void> {
            outMesh = *mesh;
            return {};
        });
    }

    // -------------------------------------------------------------- IEntities
    [[nodiscard]] Entity CreateEntity() noexcept override {
        return m_engine->Registry().Create();
    }

    void Despawn(Entity entity) noexcept override {
        DespawnEntity(*m_engine, entity);
    }

    [[nodiscard]] bool IsAlive(Entity entity) const noexcept override {
        return m_engine->Registry().IsAlive(entity);
    }

    [[nodiscard]] void* AttachRaw(Entity entity, StringView typeName, uint32_t size, uint32_t alignment) noexcept override {
        auto&          registry = m_engine->Registry();
        const uint32_t family   = FamilyFor(registry, typeName, size, alignment);
        if (family == kInvalidFamily) return nullptr;
        return registry.AddDynamic(entity, family);
    }

    bool DetachRaw(Entity entity, StringView typeName) noexcept override {
        auto&          registry = m_engine->Registry();
        const uint32_t family   = ECS::Registry::GetFamilyIDFromName(std::string_view(typeName));
        if (family == kInvalidFamily) return false;
        return registry.RemoveDynamic(entity, family);
    }

    [[nodiscard]] void* GetRaw(Entity entity, StringView typeName) const noexcept override {
        const uint32_t family = ECS::Registry::GetFamilyIDFromName(std::string_view(typeName));
        if (family == kInvalidFamily) return nullptr;
        return m_engine->Registry().GetDynamicRaw(entity, family);
    }

    [[nodiscard]] bool IsEngineComponent(StringView typeName) const noexcept override {
        return FindPublished(*m_engine, typeName) != nullptr;
    }

    [[nodiscard]] void* AttachEngineRaw(Entity entity, StringView typeName, uint32_t size, uint32_t alignment,
                                        uint64_t layoutHash) noexcept override {
        const PublishedComponent* published = FindPublished(*m_engine, typeName);
        if (published == nullptr) return nullptr;
        // All three must agree. The hash subsumes the other two, but checking
        // them separately makes a stale consumer fail for a legible reason.
        if (published->size != size || published->alignment != alignment || published->layoutHash != layoutHash) {
            return nullptr;
        }
        auto& registry = m_engine->Registry();
        if (!registry.IsAlive(entity)) return nullptr;
        return published->ensure(*m_engine, registry, entity);
    }

    bool DetachEngineRaw(Entity entity, StringView typeName) noexcept override {
        const PublishedComponent* published = FindPublished(*m_engine, typeName);
        if (published == nullptr) return false;
        auto& registry = m_engine->Registry();
        if (!registry.IsAlive(entity)) return false;
        return published->remove(*m_engine, registry, entity);
    }

    [[nodiscard]] void* GetEngineRaw(Entity entity, StringView typeName) const noexcept override {
        const PublishedComponent* published = FindPublished(*m_engine, typeName);
        if (published == nullptr) return nullptr;
        return published->fetch(m_engine->Registry(), entity);
    }

    [[nodiscard]] uint32_t EntitiesWithRaw(StringView typeName, Entity* outEntities,
                                           uint32_t capacity) const noexcept override {
        const auto& registry = m_engine->Registry();
        uint32_t    family   = kInvalidFamily;
        if (const PublishedComponent* published = FindPublished(*m_engine, typeName)) {
            family = published->familyID();
        } else {
            family = ECS::Registry::GetFamilyIDFromName(std::string_view(typeName));
        }
        if (family == kInvalidFamily) return 0;

        const std::span<const Entity> matching = registry.GetEntitiesByFamilyID(family);
        const auto                    total    = static_cast<uint32_t>(matching.size());
        if (outEntities != nullptr && capacity > 0 && total > 0) {
            std::copy_n(matching.begin(), std::min(total, capacity), outEntities);
        }
        return total;
    }

    // ------------------------------------------------------------ ISimulation
    [[nodiscard]] Physics::RaycastResult Raycast(DVec3 origin, FVec3 direction, float maxDistance,
                                                 Physics::BodyID ignore) const noexcept override {
        return Sim().Raycast(origin, direction, maxDistance, ignore);
    }

    // The facade's std::vector is built and destroyed entirely inside this
    // library; what crosses is scalars into the caller's own storage.
    [[nodiscard]] uint32_t BodiesInSphere(DVec3 center, float radius, Physics::BodyID* bodies,
                                          uint32_t capacity) const noexcept override {
        const std::vector<Physics::BodyID> found = Sim().OverlapSphere(center, radius);
        const auto                         total = static_cast<uint32_t>(found.size());
        if (bodies != nullptr && capacity > 0) {
            std::copy_n(found.begin(), std::min(total, capacity), bodies);
        }
        return total;
    }

    [[nodiscard]] Option<DVec3> BodyPosition(Physics::BodyID body) const noexcept override {
        const std::optional<DVec3> found = Sim().BodyPosition(body);
        if (!found.has_value()) return std::nullopt;
        return *found;
    }

    [[nodiscard]] Option<Physics::BodyStateSnapshot> BodyState(Physics::BodyID body) const noexcept override {
        const std::optional<Physics::BodyStateSnapshot> found = Sim().BodyState(body);
        if (!found.has_value()) return std::nullopt;
        return *found;
    }

    [[nodiscard]] Option<FVec3> BodyLinearVelocity(Physics::BodyID body) const noexcept override {
        const std::optional<FVec3> found = Sim().BodyLinearVelocity(body);
        if (!found.has_value()) return std::nullopt;
        return *found;
    }

    [[nodiscard]] float CharacterGroundOffset(Physics::BodyID body) const noexcept override {
        return Sim().GetCharacterGroundOffset(body);
    }

    [[nodiscard]] FVec3 CharacterVelocity(Physics::BodyID body) const noexcept override {
        return Sim().CharacterVelocity(body);
    }

    [[nodiscard]] bool IsCharacterOnGround(Physics::BodyID body) const noexcept override {
        return Sim().IsCharacterOnGround(body);
    }

    [[nodiscard]] bool IsBodyDynamic(Physics::BodyID body) const noexcept override {
        return Sim().IsBodyDynamic(body);
    }

    void AddImpulse(Physics::BodyID body, FVec3 impulse) noexcept override {
        Sim().AddImpulse(body, impulse);
    }

    void SetBodyLinearVelocity(Physics::BodyID body, FVec3 velocity) noexcept override {
        Sim().SetLinearVelocity(body, velocity);
    }

    void SetKinematicPose(Physics::BodyID body, DVec3 position, Quat rotation) noexcept override {
        Sim().SetKinematicPose(body, position, rotation.AsVec4());
    }

    void SetCollisionFilter(Physics::BodyID body, uint32_t category, uint32_t mask) noexcept override {
        Sim().SetCollisionFilter(body, category, mask);
    }

    void RegisterPhysicsMaterial(uint32_t materialID, float friction, float restitution) noexcept override {
        Sim().RegisterMaterial(materialID, friction, restitution);
    }

    void SetCharacterPosition(Physics::BodyID body, DVec3 position) noexcept override {
        Sim().SetCharacterPosition(body, position);
    }

    void SetCharacterVelocity(Physics::BodyID body, FVec3 velocity) noexcept override {
        Sim().SetCharacterVelocity(body, velocity);
    }

    void DestroyBody(Physics::BodyID body) noexcept override {
        Sim().DestroyBody(body);
    }

    void DestroyBodies(Span<const Physics::BodyID> bodies) noexcept override {
        Sim().DestroyBodies(std::span<const Physics::BodyID>(bodies));
    }

    void QueueKinematicPose(uint32_t jointOffset, Span<const Mat4x4> modelJoints) noexcept override {
        if (modelJoints.data == nullptr || modelJoints.size == 0) return;
        m_engine->PushKinematicPose(jointOffset, std::span<const Mat4x4> {modelJoints.data, modelJoints.size});
    }

    void DestroyRagdolls(Span<const Physics::RagdollID> ragdolls) noexcept override {
        Sim().DestroyRagdolls(std::span<const Physics::RagdollID>(ragdolls));
    }

    void ClearMeshShapeCache() noexcept override {
        Sim().ClearMeshShapeCache();
    }

    void Step(float deltaTime) noexcept override {
        Sim().Step(deltaTime);
    }

    Result<Physics::BodyID> CreateRigidBody(const ChainInNode* descChain) noexcept override {
        if (descChain == nullptr) return Error::NullArgument;

        Physics::RigidBodyPose                       pose {};
        Physics::RigidBodySurface                    surface {};
        Physics::RigidBodyFilter                     filter {};
        bool                                         hasSurface     = false;
        uint32_t                                     shapeNodeCount = 0;
        std::optional<Physics::AnyShape>             builtShape;
        std::shared_ptr<const Physics::MeshGeometry> pinnedMesh;

        const Result<void> chainRes =
            DispatchInChain<Physics::BoxShapeDesc, Physics::SphereShapeDesc, Physics::CapsuleShapeDesc,
                            Physics::CylinderShapeDesc, Physics::PlaneShapeDesc, Physics::MeshShapeDesc,
                            Physics::HeightFieldShapeDesc, Physics::CompoundShapeDesc, Physics::RigidBodyPose,
                            Physics::RigidBodySurface, Physics::RigidBodyFilter>(
                descChain, [&]<typename NodeT>(const NodeT& node) -> Result<void> {
                    if constexpr (std::is_same_v<NodeT, Physics::RigidBodyPose>) {
                        pose = node;
                    } else if constexpr (std::is_same_v<NodeT, Physics::RigidBodySurface>) {
                        surface    = node;
                        hasSurface = true;
                    } else if constexpr (std::is_same_v<NodeT, Physics::RigidBodyFilter>) {
                        filter = node;
                    } else {
                        ++shapeNodeCount;
                        if constexpr (std::is_same_v<NodeT, Physics::BoxShapeDesc>) {
                            if (node.halfExtents.x <= 0.0F || node.halfExtents.y <= 0.0F || node.halfExtents.z <= 0.0F) {
                                return Error::InvalidArgument;
                            }
                            builtShape = Physics::AnyShape {Physics::Shape<Physics::Box>(node.halfExtents)};
                        } else if constexpr (std::is_same_v<NodeT, Physics::SphereShapeDesc>) {
                            if (node.radius <= 0.0F) return Error::InvalidArgument;
                            builtShape = Physics::AnyShape {Physics::Shape<Physics::Sphere>(node.radius)};
                        } else if constexpr (std::is_same_v<NodeT, Physics::CapsuleShapeDesc>) {
                            if (node.halfHeight <= 0.0F || node.radius <= 0.0F) return Error::InvalidArgument;
                            builtShape =
                                Physics::AnyShape {Physics::Shape<Physics::Capsule>(node.halfHeight, node.radius)};
                        } else if constexpr (std::is_same_v<NodeT, Physics::CylinderShapeDesc>) {
                            if (node.halfHeight <= 0.0F || node.radius <= 0.0F) return Error::InvalidArgument;
                            builtShape =
                                Physics::AnyShape {Physics::Shape<Physics::Cylinder>(node.halfHeight, node.radius)};
                        } else if constexpr (std::is_same_v<NodeT, Physics::PlaneShapeDesc>) {
                            builtShape = Physics::AnyShape {Physics::Shape<Physics::Plane>(node.normal, node.constant)};
                        } else if constexpr (std::is_same_v<NodeT, Physics::MeshShapeDesc>) {
                            if (node.asset != CollisionAssetID::Invalid) {
                                pinnedMesh = Library().FindCollisionMesh(node.asset);
                                if (!pinnedMesh && node.vertices != nullptr && node.indices != nullptr) {
                                    auto geom = ValidateAndBuildMeshGeometry(node.vertices, node.vertexCount,
                                                                             node.indices, node.indexCount);
                                    if (!geom.has_value()) return Error::InvalidArgument;
                                    (void)Library().CacheCollisionMesh(node.asset, std::move(*geom));
                                    pinnedMesh = Library().FindCollisionMesh(node.asset);
                                }
                                if (!pinnedMesh) return Error::AssetNotCached;
                                Physics::Shape<Physics::Mesh> meshShape(pinnedMesh);
                                meshShape.asset = node.asset;
                                builtShape      = Physics::AnyShape {std::move(meshShape)};
                            } else {
                                auto geom = ValidateAndBuildMeshGeometry(node.vertices, node.vertexCount, node.indices,
                                                                         node.indexCount);
                                if (!geom.has_value()) return Error::InvalidArgument;
                                pinnedMesh = std::make_shared<Physics::MeshGeometry>(std::move(*geom));
                                builtShape = Physics::AnyShape {Physics::Shape<Physics::Mesh>(pinnedMesh)};
                            }
                        } else if constexpr (std::is_same_v<NodeT, Physics::HeightFieldShapeDesc>) {
                            if (node.heights == nullptr || node.sampleCount < 2U || node.worldSize <= 0.0F) {
                                return Error::InvalidArgument;
                            }
                            const size_t totalSamples = static_cast<size_t>(node.sampleCount) * node.sampleCount;
                            builtShape                = Physics::AnyShape {Physics::Shape<Physics::HeightField>(
                                std::span<const float>(node.heights, totalSamples), node.sampleCount, node.worldSize)};
                        } else if constexpr (std::is_same_v<NodeT, Physics::CompoundShapeDesc>) {
                            if (node.children == nullptr || node.childCount == 0U) return Error::InvalidArgument;
                            Physics::Shape<Physics::Compound> comp {};
                            comp.children.reserve(node.childCount);
                            for (uint32_t i = 0; i < node.childCount; ++i) {
                                const Physics::CompoundChildDesc& child = node.children[i];
                                auto                              sub   = MakePrimitiveSubShape(child);
                                if (!sub.has_value()) return Error::InvalidArgument;
                                comp.children.push_back(Physics::Shape<Physics::Compound>::Child {
                                    .position = child.position,
                                    .rotation = child.rotation,
                                    .shape    = std::move(*sub),
                                });
                            }
                            builtShape = Physics::AnyShape {std::move(comp)};
                        }
                    }
                    return {};
                });
        if (!chainRes.HasValue()) return chainRes.error;
        if (shapeNodeCount != 1U || !builtShape.has_value()) return Error::InvalidArgument;

        if (hasSurface && surface.registerMaterial) {
            Sim().RegisterMaterial(surface.materialID, surface.friction, surface.restitution);
        }

        const Physics::ObjectLayer resolvedLayer =
            filter.hasExplicitLayer ? filter.layer
                                    : (pose.motionType == Physics::MotionType::Static ? Physics::ObjectLayer::NonMoving
                                                                                      : Physics::ObjectLayer::Moving);

        return Sim().CreateRigidBody(*builtShape, pose.position, pose.rotation.AsVec4(), pose.motionType, resolvedLayer,
                                     surface.materialID, filter.category, filter.mask, pose.activation, pose.scale);
    }

    Result<Physics::BodyID> CreateCharacter(const ChainInNode* descChain) noexcept override {
        Physics::CharacterCreateDesc           desc {};
        std::optional<Physics::DualShapeConfig> dual {};
        if (descChain != nullptr) {
            const Result<void> chainRes = DispatchInChain<Physics::CharacterCreateDesc, Physics::DualShapeConfig>(
                descChain, [&]<typename Node>(const Node& node) -> Result<void> {
                    if constexpr (std::is_same_v<Node, Physics::CharacterCreateDesc>) desc = node;
                    else dual = node;
                    return {};
                });
            if (!chainRes.HasValue()) return chainRes.error;
        }
        if (dual.has_value()) {
            if (dual->lifterRadius <= 0.0F || dual->bumperRadiusXZ <= 0.0F || dual->bumperRadiusY <= 0.0F) {
                return Error::InvalidArgument;
            }
            const Physics::CharacterParams params {
                .shape                    = Physics::CreateDualShape(*dual),
                .maxSlopeAngle            = desc.maxSlopeRadians,
                .maxStrength              = desc.maxStrength,
                .characterPadding         = desc.characterPadding,
                .penetrationRecoverySpeed = desc.penetrationRecoverySpeed,
                .supportingVolume         = JPH::Plane(JPH::Vec3::sAxisY(), -dual->GetLifterOffsetY()),
                .category                 = desc.category,
                .mask                     = desc.mask,
            };
            return Sim().CreateCharacter(desc.position, params);
        }
        if (desc.useCustomCapsule && (desc.capsuleRadius <= 0.0F || desc.capsuleHalfHeight <= 0.0F)) {
            return Error::InvalidArgument;
        }
        return Sim().CreateCustomCharacter(desc);
    }

    [[nodiscard]] Physics::DebugDrawData DebugGeometry(bool shapes, bool constraints, bool wireframe) noexcept override {
        return Sim().GetDebugDrawData(shapes, constraints, wireframe);
    }

    // ------------------------------------------------------------ IAudioDevice
    [[nodiscard]] bool Initialized() const noexcept override {
        return Sound().IsInitialized();
    }

    Result<void> PostEvent(const ChainInNode* eventChain) noexcept override {
        if (eventChain == nullptr) return Error::NullArgument;

        std::optional<AudioEvent> evt;
        const Result<void>        chainRes =
            DispatchInChain<AudioEvent>(eventChain, [&](const AudioEvent& inEvent) -> Result<void> {
                evt = inEvent;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!evt.has_value()) return Error::InvalidArgument;

        Sound().PostEvent(*evt);
        return {};
    }

    Result<VoiceID> CreateVoice(StringView filepath, const ChainInNode* voiceChain) noexcept override {
        VoiceDesc desc {};
        if (voiceChain != nullptr) {
            const Result<void> chainRes =
                DispatchInChain<VoiceDesc>(voiceChain, [&](const VoiceDesc& inDesc) -> Result<void> {
                    desc = inDesc;
                    return {};
                });
            if (!chainRes.HasValue()) return chainRes.error;
        }
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

    void SetVoicePosition(VoiceID voice, FVec3 position) noexcept override {
        Sound().SetVoicePosition(voice, position);
    }

    // ----------------------------------------------------------- IAssetLibrary
    [[nodiscard]] AssetID AssetHash(StringView name) const noexcept override {
        return HashAssetID(std::string_view(name));
    }

    [[nodiscard]] uint64_t PrefabHash(StringView virtualPath) const noexcept override {
        return HashAssetPath(std::string_view(virtualPath));
    }

    [[nodiscard]] bool HasPrefab(uint64_t hash) const noexcept override {
        return Library().BorrowCachedPrefab(hash) != nullptr;
    }

    [[nodiscard]] Option<PrefabInfo> DescribePrefab(uint64_t hash) const noexcept override {
        const ModelPrefab* prefab = Library().BorrowCachedPrefab(hash);
        if (prefab == nullptr) return std::nullopt;

        float maxRadius = 0.0F;
        for (const ModelPart& part : prefab->parts) {
            maxRadius = std::max(maxRadius, part.boundingRadius);
        }
        return PrefabInfo {
            .hash           = hash,
            .partCount      = static_cast<uint32_t>(prefab->parts.size()),
            .nodeCount      = static_cast<uint32_t>(prefab->nodes.size()),
            .skeletonCount  = static_cast<uint32_t>(prefab->skeletons.size()),
            .animationCount = static_cast<uint32_t>(prefab->animations.size()),
            .lightCount     = static_cast<uint32_t>(prefab->lights.size()),
            .boundingRadius = maxRadius,
            .emissiveFactorScale = prefab->emissiveFactorScale,
            .maxTextureDimension = prefab->maxTextureDimension,
            .gpuResourcesReady = prefab->gpuResourcesReady,
            .gpuDeviceGeneration = prefab->gpuDeviceGeneration,
            .gpuRevision = prefab->gpuRevision,
        };
    }

    Result<void> GetPrefabBounds(uint64_t hash, ChainOutNode* out) const noexcept override {
        if (!out)
            return Error::NullArgument;
        const auto* prefab = Library().BorrowCachedPrefab(hash);
        if (!prefab)
            return Error::AssetNotCached;
        PrefabBoundsInfo bounds {};
        for (const auto& part: prefab->parts) {
            if (!bounds.hasParts) {
                bounds.min      = part.localMin;
                bounds.max      = part.localMax;
                bounds.hasParts = true;
            } else {
                bounds.min = {std::min(bounds.min.x, part.localMin.x), std::min(bounds.min.y, part.localMin.y), std::min(bounds.min.z, part.localMin.z)};
                bounds.max = {std::max(bounds.max.x, part.localMax.x), std::max(bounds.max.y, part.localMax.y), std::max(bounds.max.z, part.localMax.z)};
            }
        }
        return DispatchOutChain<PrefabBoundsInfo>(out, [&](PrefabBoundsInfo& value) -> Result<void> {
            value = bounds;
            return {};
        });
    }

    Result<void> ReadPrefabRig(uint64_t hash, ChainOutNode* out) const noexcept override {
        if (out == nullptr) return Error::NullArgument;
        const ModelPrefab* prefabPtr = Library().BorrowCachedPrefab(hash);
        if (prefabPtr == nullptr) return Error::AssetNotCached;
        const ModelPrefab& prefab = *prefabPtr;

        size_t totalJoints    = 0;
        size_t totalChannels  = 0;
        size_t totalKeyTimes  = 0;
        size_t totalKeyValues = 0;
        for (const auto& skel : prefab.skeletons) totalJoints += skel.joints.size();
        for (const auto& anim : prefab.animations) {
            totalChannels += anim.channels.size();
            for (const auto& ch : anim.channels) {
                totalKeyTimes  += ch.keyTimes.size();
                totalKeyValues += ch.keyValues.size();
            }
        }

        return DispatchOutChain<PrefabRigCounts, PrefabRigData>(
            out, [&]<typename Node>(Node& node) -> Result<void> {
                if constexpr (std::is_same_v<Node, PrefabRigCounts>) {
                    node = PrefabRigCounts {
                        .revision       = prefab.gpuRevision,
                        .nodeCount      = static_cast<uint32_t>(prefab.nodes.size()),
                        .partCount      = static_cast<uint32_t>(prefab.parts.size()),
                        .skeletonCount  = static_cast<uint32_t>(prefab.skeletons.size()),
                        .jointCount     = static_cast<uint32_t>(totalJoints),
                        .animationCount = static_cast<uint32_t>(prefab.animations.size()),
                        .channelCount   = static_cast<uint32_t>(totalChannels),
                        .keyTimeCount   = static_cast<uint32_t>(totalKeyTimes),
                        .keyValueCount  = static_cast<uint32_t>(totalKeyValues),
                    };
                    return {};
                } else {
                    PrefabRigData& data = node;
                    if (data.nodes.size < prefab.nodes.size() || data.parts.size < prefab.parts.size() ||
                        data.skeletons.size < prefab.skeletons.size() || data.joints.size < totalJoints ||
                        data.animations.size < prefab.animations.size() || data.channels.size < totalChannels ||
                        data.keyTimes.size < totalKeyTimes || data.keyValues.size < totalKeyValues) {
                        return Error::InvalidArgument;
                    }
                    for (size_t i = 0; i < prefab.nodes.size(); ++i) {
                        const auto& n      = prefab.nodes[i];
                        data.nodes.data[i] = PrefabNodeInfo {
                            .name           = String64(std::string_view(n.name)),
                            .parentIndex    = n.parentIndex,
                            .localTransform = PrefabMatrixToNative(n.localTransform),
                            .hasMesh        = n.hasMesh,
                        };
                    }
                    for (size_t i = 0; i < prefab.parts.size(); ++i) {
                        const auto& p      = prefab.parts[i];
                        data.parts.data[i] = PrefabPartBoundsInfo {
                            .nodeIndex      = p.nodeIndex,
                            .isSkinned      = p.isSkinned,
                            .localTransform = PrefabMatrixToNative(p.localTransform),
                            .localMin       = p.localMin,
                            .localMax       = p.localMax,
                        };
                    }
                    uint32_t jointCursor = 0;
                    for (size_t i = 0; i < prefab.skeletons.size(); ++i) {
                        const auto&    s     = prefab.skeletons[i];
                        const uint32_t first = jointCursor;
                        for (const auto& j : s.joints) {
                            data.joints.data[jointCursor++] = PrefabJointInfo {
                                .name              = String64(std::string_view(j.name)),
                                .parentIndex       = j.parentIndex,
                                .nodeIndex         = j.nodeIndex,
                                .inverseBindMatrix = j.inverseBindMatrix,
                            };
                        }
                        data.skeletons.data[i] = PrefabSkeletonInfo {
                            .name   = String64(std::string_view(s.name)),
                            .joints = {.first = first, .count = static_cast<uint32_t>(s.joints.size())},
                        };
                    }
                    uint32_t chCursor   = 0;
                    uint32_t timeCursor = 0;
                    uint32_t valCursor  = 0;
                    for (size_t i = 0; i < prefab.animations.size(); ++i) {
                        const auto&    a       = prefab.animations[i];
                        const uint32_t chFirst = chCursor;
                        for (const auto& ch : a.channels) {
                            const uint32_t tFirst = timeCursor;
                            for (float t : ch.keyTimes) data.keyTimes.data[timeCursor++] = t;
                            const uint32_t vFirst = valCursor;
                            for (float v : ch.keyValues) data.keyValues.data[valCursor++] = v;
                            data.channels.data[chCursor++] = PrefabChannelInfo {
                                .targetNodeIndex = ch.targetNodeIndex,
                                .path            = ch.path,
                                .interpolation   = ch.interpolation,
                                .keyTimes        = {.first = tFirst, .count = static_cast<uint32_t>(ch.keyTimes.size())},
                                .keyValues       = {.first = vFirst, .count = static_cast<uint32_t>(ch.keyValues.size())},
                            };
                        }
                        data.animations.data[i] = PrefabAnimationInfo {
                            .name     = String64(std::string_view(a.name)),
                            .duration = a.duration,
                            .channels = {.first = chFirst, .count = static_cast<uint32_t>(a.channels.size())},
                        };
                    }
                    return {};
                }
            });
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

    Result<uint32_t> Instantiate(uint64_t hash, const ChainInNode* spawnChain, Entity* entities,
                                               uint32_t capacity) noexcept override {
        PrefabFactory::SpawnParams params {};
        if (spawnChain != nullptr) {
            const Result<void> chainRes = DispatchInChain<PrefabFactory::SpawnParams>(
                spawnChain, [&](const PrefabFactory::SpawnParams& inParams) -> Result<void> {
                    params = inParams;
                    return {};
                });
            if (!chainRes.HasValue()) return chainRes.error;
        }

        const ModelPrefab* prefab = Library().BorrowCachedPrefab(hash);
        if (prefab == nullptr) return uint32_t {0};
        return PrefabFactory::InstantiatePrefab(*m_engine, *prefab, params, entities, capacity);
    }

    Result<Entity> SpawnPrimitive(const ChainInNode* primitiveChain) noexcept override {
        if (primitiveChain == nullptr) return Error::NullArgument;

        PrefabFactory::SpawnParams      params {};
        std::optional<BoxMeshDesc>      boxDesc;
        std::optional<PlaneMeshDesc>    planeDesc;
        std::optional<SphereMeshDesc>   sphereDesc;
        std::optional<CylinderMeshDesc> cylinderDesc;
        std::optional<ConeMeshDesc>     coneDesc;
        uint32_t                        shapeCount = 0;
        MeshBuildOptions                options {.buildBLAS = true};

        const Result<void> chainRes =
            DispatchInChain<MeshBuildOptions, PrefabFactory::SpawnParams, BoxMeshDesc, PlaneMeshDesc, SphereMeshDesc, CylinderMeshDesc, ConeMeshDesc>(
                primitiveChain, [&]<typename NodeT>(const NodeT& node) -> Result<void> {
                    if constexpr (std::is_same_v<NodeT, MeshBuildOptions>) {
                        options = node;
                    } else if constexpr (std::is_same_v<NodeT, PrefabFactory::SpawnParams>) {
                        params = node;
                    } else {
                        ++shapeCount;
                        if constexpr (std::is_same_v<NodeT, BoxMeshDesc>) {
                            boxDesc = node;
                        } else if constexpr (std::is_same_v<NodeT, PlaneMeshDesc>) {
                            planeDesc = node;
                        } else if constexpr (std::is_same_v<NodeT, SphereMeshDesc>) {
                            sphereDesc = node;
                        } else if constexpr (std::is_same_v<NodeT, CylinderMeshDesc>) {
                            cylinderDesc = node;
                        } else if constexpr (std::is_same_v<NodeT, ConeMeshDesc>) {
                            coneDesc = node;
                        }
                    }
                    return {};
                }
            );
        if (!chainRes.HasValue()) return chainRes.error;
        if (shapeCount != 1U) return Error::InvalidArgument;

        if (boxDesc.has_value()) {
            PrefabFactory::SpawnParams resolved = params;
            if (boxDesc->color.w >= 0.0F) {
                resolved.color = boxDesc->color;
                if (resolved.materialOverride.pipeline == PipelineID::Invalid) {
                    resolved.materialOverride.baseColorFactor = boxDesc->color;
                }
            }
            return PrefabFactory::CreateBoxConfigured(*m_engine, boxDesc->halfExtents, resolved, options.buildBLAS);
        }
        if (planeDesc.has_value()) {
            const FVec4 planeColor = planeDesc->color.w >= 0.0F ? planeDesc->color : FVec4 {0.6F, 0.6F, 0.6F, 1.0F};
            return PrefabFactory::CreatePlaneConfigured(*m_engine, planeDesc->extent, planeColor, params, options.buildBLAS);
        }
        if (sphereDesc.has_value()) {
            PrefabFactory::SpawnParams resolved = params;
            if (sphereDesc->color.w >= 0.0F) resolved.color = sphereDesc->color;
            return PrefabFactory::CreateSphereConfigured(*m_engine, sphereDesc->radius, resolved, options.buildBLAS);
        }
        if (cylinderDesc.has_value()) {
            PrefabFactory::SpawnParams resolved = params;
            if (cylinderDesc->color.w >= 0.0F) resolved.color = cylinderDesc->color;
            return PrefabFactory::CreateCylinderConfigured(*m_engine, cylinderDesc->radius, cylinderDesc->height, resolved, options.buildBLAS);
        }
        if (coneDesc.has_value()) {
            PrefabFactory::SpawnParams resolved = params;
            if (coneDesc->color.w >= 0.0F) resolved.color = coneDesc->color;
            return PrefabFactory::CreateConeConfigured(*m_engine, coneDesc->radius, coneDesc->height, resolved, options.buildBLAS);
        }
        return Error::InvalidArgument;
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

    Result<void> MountDirectory(StringView path) noexcept override {
        if (!Valid(path)) return Error::NullArgument;
        if (!path.size || !Library().VFS().MountDirectory(Text(path))) return Error::InvalidArgument;
        return {};
    }
    Result<void> UnmountDirectory(StringView path) noexcept override {
        if (!Valid(path)) return Error::NullArgument;
        if (!Library().VFS().UnmountDirectory(Text(path))) return Error::InvalidArgument;
        return {};
    }
    Result<void> MountPak(StringView path) noexcept override {
        if (!Valid(path)) return Error::NullArgument;
        if (!path.size || !Library().VFS().MountPak(Text(path))) return Error::InvalidArgument;
        return {};
    }
    [[nodiscard]] bool VirtualFileExists(StringView path) const noexcept override {
        return Valid(path) && path.size && Library().VFS().Exists(Text(path));
    }
    Result<uint64_t> ReadVirtualFile(StringView path, Span<uint8_t> bytes) const noexcept override {
        if (!Valid(path) || !Valid(bytes)) return Error::NullArgument;
        if (!path.size || !Library().VFS().Exists(Text(path))) return Error::AssetNotCached;
        const auto file = Library().ReadVirtualFile(Text(path));
        if (!file) return uint64_t {0};
        if (bytes.data && bytes.size >= file->size() && !file->empty()) std::memcpy(bytes.data, file->data(), file->size());
        return static_cast<uint64_t>(file->size());
    }
    void ClearCache() noexcept override { Library().ClearCache(); }
    Result<void> CacheCollisionMesh(const ChainInNode* chain) noexcept override {
        const auto info = ReadNode<CollisionMeshInfo>(chain);
        if (!info.HasValue()) return info.error;
        const auto& mesh = info.value;
        if (!Valid(mesh.vertices) || !Valid(mesh.indices)) return Error::NullArgument;
        if (mesh.id == CollisionAssetID::Invalid || mesh.vertices.size < 3 || mesh.vertices.size > UINT32_MAX ||
            mesh.indices.size < 3 || mesh.indices.size % 3 != 0) return Error::InvalidArgument;
        for (const auto i: std::span<const uint32_t>(mesh.indices)) if (i >= mesh.vertices.size) return Error::InvalidArgument;
        Physics::MeshGeometry geometry;
        geometry.vertices.assign(mesh.vertices.data, mesh.vertices.data + mesh.vertices.size);
        geometry.indices.assign(mesh.indices.data, mesh.indices.data + mesh.indices.size);
        if (!Library().CacheCollisionMesh(mesh.id, std::move(geometry))) return Error::InvalidArgument;
        return {};
    }
    Result<void> GetCollisionMesh(CollisionAssetID id, ChainOutNode* out) const noexcept override {
        if (!out) return Error::NullArgument;
        const auto mesh = Library().FindCollisionMesh(id);
        if (!mesh) return Error::AssetNotCached;
        return DispatchOutChain<CollisionMeshInfo>(out, [&](CollisionMeshInfo& info) -> Result<void> {
            info = {id, mesh->vertices, mesh->indices};
            return {};
        });
    }
    Result<void> CacheEnvironmentImage(const ChainInNode* chain) noexcept override {
        const auto info = ReadNode<EnvironmentImageCacheInfo>(chain);
        if (!info.HasValue()) return info.error;
        const auto& desc = info.value;
        if (!Valid(desc.key) || !Valid(desc.rgba) || !Valid(desc.lightingRgba)) return Error::NullArgument;
        const uint64_t texels = uint64_t(desc.extent.width) * desc.extent.height;
        if (!desc.key.size || !texels || texels > SIZE_MAX / 4 || desc.rgba.size != texels * 4 ||
            (desc.lightingRgba.size && desc.lightingRgba.size != desc.rgba.size)) return Error::InvalidArgument;
        EnvironmentImage image;
        image.width = desc.extent.width; image.height = desc.extent.height; image.contentHash = desc.contentHash;
        image.rgba.assign(desc.rgba.data, desc.rgba.data + desc.rgba.size);
        if (desc.lightingRgba.size) image.lightingRgba.assign(desc.lightingRgba.data, desc.lightingRgba.data + desc.lightingRgba.size);
        if (desc.sun.HasValue()) image.sun = desc.sun.value;
        if (!Library().CacheEnvironmentImage(Text(desc.key), std::move(image))) return Error::InvalidArgument;
        return {};
    }
    Result<void> GetEnvironmentImage(StringView key, ChainOutNode* out) const noexcept override {
        if (!Valid(key) || !out) return Error::NullArgument;
        const auto image = Library().FindEnvironmentImage(Text(key));
        if (!image) return Error::AssetNotCached;
        return DispatchOutChain<EnvironmentImageCacheInfo>(out, [&](EnvironmentImageCacheInfo& desc) -> Result<void> {
            desc = {.key = {}, .extent = {image->width, image->height}, .rgba = image->rgba,
                    .lightingRgba = image->lightingRgba, .sun = image->sun ? Option<EnvironmentSun>(*image->sun) : Option<EnvironmentSun> {}, .contentHash = image->contentHash};
            // The queried key is caller-owned; do not return a borrow of it.
            return {};
        });
    }
    Result<uint64_t> RegisterPrefab(const ChainInNode* chain) noexcept override {
        const auto info = ReadNode<PrefabRegistrationInfo>(chain);
        if (!info.HasValue()) return info.error;
        const auto& desc = info.value;
        if (!Valid(desc.virtualPath) || !Valid(desc.nodes) || !Valid(desc.parts) || !Valid(desc.skeletons) || !Valid(desc.joints) ||
            !Valid(desc.animations) || !Valid(desc.channels) || !Valid(desc.keyTimes) || !Valid(desc.keyValues) ||
            !Valid(desc.lights) || !Valid(desc.csgModifiers)) return Error::NullArgument;
        if (!desc.virtualPath.size || desc.virtualPath.size >= 256 || desc.nodes.size > INT32_MAX || desc.skeletons.size > INT32_MAX ||
            desc.parts.size > UINT32_MAX || desc.animations.size > UINT32_MAX || desc.lights.size > UINT32_MAX)
            return Error::InvalidArgument;
        const auto hash = PrefabHash(desc.virtualPath);
        if (Library().GetCachedPrefab(hash)) return Error::InvalidArgument;
        const auto index = [](int32_t value, size_t count) { return value == -1 || (value >= 0 && size_t(value) < count); };
        const auto slice = [](AssetSlice value, size_t size) { return value.first <= size && value.count <= size - value.first; };
        for (size_t i = 0; i < desc.nodes.size; ++i) {
            auto parent = desc.nodes.data[i].parentIndex;
            if (!index(parent, desc.nodes.size)) return Error::InvalidArgument;
            // Detect cycles without requiring importer nodes to be topologically sorted.
            for (size_t hops = 0; parent >= 0; ++hops) {
                if (hops >= desc.nodes.size || !index(parent, desc.nodes.size)) return Error::InvalidArgument;
                parent = desc.nodes.data[parent].parentIndex;
            }
        }
        for (const auto& skeleton: std::span<const PrefabSkeletonInfo>(desc.skeletons)) {
            if (!slice(skeleton.joints, desc.joints.size) || skeleton.joints.count > 8192) return Error::InvalidArgument;
            for (uint32_t j = 0; j < skeleton.joints.count; ++j) {
                const auto& joint = desc.joints.data[skeleton.joints.first + j];
                if (!index(joint.nodeIndex, desc.nodes.size) || !index(joint.parentIndex, skeleton.joints.count))
                    return Error::InvalidArgument;
                // glTF skin.joints is not required to be parent-first.
                auto parent = joint.parentIndex;
                for (uint32_t hops = 0; parent >= 0; ++hops) {
                    if (hops >= skeleton.joints.count || !index(parent, skeleton.joints.count))
                        return Error::InvalidArgument;
                    parent = desc.joints.data[skeleton.joints.first + parent].parentIndex;
                }
            }
        }
        for (const auto& part: std::span<const PrefabPartInfo>(desc.parts)) {
            if (!index(part.nodeIndex, desc.nodes.size) || !index(part.skeletonIndex, desc.skeletons.size) ||
                (part.isSkinned && part.skeletonIndex < 0) || part.activeMorphCount > 4 ||
                !slice(part.csgModifiers, desc.csgModifiers.size)) return Error::InvalidArgument;
            if (part.collision != CollisionAssetID::Invalid && !Library().FindCollisionMesh(part.collision)) return Error::AssetNotCached;
            if (part.additionalMeshCollision != CollisionAssetID::Invalid) {
                if (part.collision == CollisionAssetID::Invalid && !part.useBoxForMeshCollider) return Error::InvalidArgument;
                if (!Library().FindCollisionMesh(part.additionalMeshCollision)) return Error::AssetNotCached;
            }
            if (part.useBoxForMeshCollider && (!part.hasBoxCollider || part.collision != CollisionAssetID::Invalid))
                return Error::InvalidArgument;
            if (part.hasBoxCollider && (part.boxHalfExtents.x <= 0 || part.boxHalfExtents.y <= 0 || part.boxHalfExtents.z <= 0))
                return Error::InvalidArgument;
        }
        for (const auto& clip: std::span<const PrefabAnimationInfo>(desc.animations)) {
            if (!slice(clip.channels, desc.channels.size) || !std::isfinite(clip.duration) || clip.duration < 0) return Error::InvalidArgument;
        }
        for (const auto& channel: std::span<const PrefabChannelInfo>(desc.channels)) {
            if (!index(channel.targetNodeIndex, desc.nodes.size) || channel.targetNodeIndex < 0 ||
                !slice(channel.keyTimes, desc.keyTimes.size) || !slice(channel.keyValues, desc.keyValues.size) ||
                channel.path > AnimationPathType::Weights || channel.interpolation > InterpolationType::CubicSpline) return Error::InvalidArgument;
            const uint64_t factor = channel.interpolation == InterpolationType::CubicSpline && channel.path != AnimationPathType::Weights ? 3 : 1;
            const uint64_t count = uint64_t(channel.keyTimes.count) * factor;
            if (!count || channel.keyValues.count % count != 0) return Error::InvalidArgument;
            const auto width = channel.keyValues.count / count;
            if ((channel.path == AnimationPathType::Rotation && width != 4) ||
                ((channel.path == AnimationPathType::Translation || channel.path == AnimationPathType::Scale) && width != 3) || !width)
                return Error::InvalidArgument;
            for (uint32_t k = 0; k < channel.keyTimes.count; ++k) {
                const auto t = desc.keyTimes.data[channel.keyTimes.first + k];
                if (!std::isfinite(t) || t < 0 || (k && t <= desc.keyTimes.data[channel.keyTimes.first + k - 1])) return Error::InvalidArgument;
            }
        }
        for (const auto& light: std::span<const PrefabLightInfo>(desc.lights))
            if (!index(light.nodeIndex, desc.nodes.size)) return Error::InvalidArgument;

        auto prefab = std::make_unique<ModelPrefab>();
        prefab->virtualPath = String256(Text(desc.virtualPath));
        prefab->emissiveFactorScale = desc.emissiveFactorScale;
        prefab->maxTextureDimension = desc.maxTextureDimension;
        for (const auto& node: std::span<const PrefabNodeInfo>(desc.nodes))
            prefab->nodes.push_back({node.name, node.parentIndex, node.localTransform, node.hasMesh});
        for (const auto& authored: std::span<const PrefabPartInfo>(desc.parts)) {
            ModelPart part;
            part.name = authored.name; part.meshAsset = authored.meshAsset; part.materialAsset = authored.materialAsset;
            part.mesh = authored.mesh; part.defaultMaterial = authored.defaultMaterial; part.localTransform = authored.localTransform;
            part.jointOffset = authored.jointOffset; part.isSkinned = authored.isSkinned; part.nodeIndex = authored.nodeIndex;
            part.skeletonIndex = authored.skeletonIndex; part.morphOffset = authored.morphOffset; part.activeMorphCount = authored.activeMorphCount;
            part.defaultMorphWeights = authored.defaultMorphWeights; part.boundingRadius = authored.boundingRadius;
            part.localMin = authored.localMin; part.localMax = authored.localMax; part.drawFlags = authored.drawFlags;
            if (authored.csgModifiers.count) {
                const auto* first = desc.csgModifiers.data + authored.csgModifiers.first;
                for (uint32_t k = 0; k < authored.csgModifiers.count; ++k)
                    part.csgModifiers.push_back({first[k].operation, std::string(std::string_view(first[k].operandName))});
            }
            if (authored.collision != CollisionAssetID::Invalid)
                part.meshCollider = Physics::AnyShape(Physics::Shape<Physics::Mesh>(Library().FindCollisionMesh(authored.collision)));
            if (authored.hasBoxCollider) part.boxCollider = MakePrefabBoxCollider(authored.boxHalfExtents, authored.boxCenter);
            if (authored.useBoxForMeshCollider) part.meshCollider = part.boxCollider;
            if (authored.additionalMeshCollision != CollisionAssetID::Invalid) {
                Physics::Shape<Physics::Compound> combined;
                AppendPrefabCollider(combined, *part.meshCollider);
                combined.Add(Physics::Shape<Physics::Mesh>(Library().FindCollisionMesh(authored.additionalMeshCollision)));
                part.meshCollider = Physics::AnyShape(std::move(combined));
            }
            prefab->parts.push_back(std::move(part));
        }
        for (const auto& authored: std::span<const PrefabSkeletonInfo>(desc.skeletons)) {
            Skeleton skeleton; skeleton.name = authored.name;
            for (uint32_t j = 0; j < authored.joints.count; ++j) {
                const auto& joint = desc.joints.data[authored.joints.first + j];
                skeleton.joints.push_back({joint.name, joint.parentIndex, joint.nodeIndex, joint.inverseBindMatrix});
            }
            prefab->skeletons.push_back(std::move(skeleton));
        }
        for (const auto& authored: std::span<const PrefabAnimationInfo>(desc.animations)) {
            AnimationClip clip; clip.name = authored.name; clip.duration = authored.duration;
            for (uint32_t c = 0; c < authored.channels.count; ++c) {
                const auto& source = desc.channels.data[authored.channels.first + c];
                AnimationChannel channel; channel.targetNodeIndex = source.targetNodeIndex; channel.path = source.path;
                channel.interpolation = source.interpolation;
                if (source.keyTimes.count) channel.keyTimes.assign(desc.keyTimes.data + source.keyTimes.first, desc.keyTimes.data + source.keyTimes.first + source.keyTimes.count);
                if (source.keyValues.count) channel.keyValues.assign(desc.keyValues.data + source.keyValues.first, desc.keyValues.data + source.keyValues.first + source.keyValues.count);
                clip.channels.push_back(std::move(channel));
            }
            prefab->animations.push_back(std::move(clip));
        }
        for (const auto& light: std::span<const PrefabLightInfo>(desc.lights))
            prefab->lights.push_back({light.name, light.nodeIndex, light.type, light.color, light.intensity, light.range, light.innerConeAngle, light.outerConeAngle});
        if (m_nextPrefabGPURevision == 0) return Error::InvalidArgument;
        prefab->gpuRevision = m_nextPrefabGPURevision++;
        prefab->gpuDeviceGeneration = DeviceGeneration();
        Library().UseRenderContext(Device());
        Library().CachePrefab(hash, std::move(prefab));
        return hash;
    }

    Result<uint64_t> ReplacePrefabGPUResources(const ChainInNode* chain) noexcept override {
        const auto info = ReadNode<PrefabGPUReplacementInfo>(chain);
        if (!info.HasValue()) return info.error;
        const auto& desc = info.value;
        if (!Valid(desc.parts)) return Error::NullArgument;
        AssetManager& library = Library();
        auto prefab = library.GetCachedPrefab(desc.hash);
        if (!prefab) return Error::AssetNotCached;
        if (desc.deviceGeneration != DeviceGeneration() || desc.expectedRevision != prefab->gpuRevision ||
            desc.parts.size != prefab->parts.size() || m_nextPrefabGPURevision == 0)
            return Error::InvalidArgument;
        std::set<BufferID> oldBuffers;
        std::set<PipelineID> oldPipelines;
        std::set<MaterialID> replacementMaterialKeys;
        std::map<AssetID, const PrefabPartGPUInfo*> replacements;
        for (const auto& existing: prefab->parts) {
            for (const auto buffer: {existing.mesh.posBuffer, existing.mesh.tangentFrameBuffer, existing.mesh.surfaceBuffer,
                 existing.mesh.skinBuffer, existing.mesh.indexBuffer, existing.mesh.meshletBuffer,
                 existing.mesh.meshletVertexBuffer, existing.mesh.meshletTriBuffer})
                if (buffer != BufferID::Invalid) oldBuffers.insert(buffer);
            if (existing.defaultMaterial.pipeline != PipelineID::Invalid) oldPipelines.insert(existing.defaultMaterial.pipeline);
            if (existing.defaultMaterial.prePassPipeline != PipelineID::Invalid) oldPipelines.insert(existing.defaultMaterial.prePassPipeline);
        }
        // Complete validation precedes any publication or resource retirement.
        for (size_t i = 0; i < desc.parts.size; ++i) {
            const auto& part = desc.parts.data[i];
            const auto& old = prefab->parts[i];
            if (part.meshAsset == InvalidAssetID || part.materialAsset == InvalidMaterialID ||
                part.meshAsset != old.meshAsset || part.materialAsset != old.materialAsset ||
                part.activeMorphCount != old.activeMorphCount || part.mesh.posBuffer == BufferID::Invalid ||
                part.material.pipeline == PipelineID::Invalid || !part.mesh.vertexCount)
                return Error::InvalidArgument;
            // New resources may be shared by new parts, never borrowed from
            // any old part: retirement must not destroy a replacement alias.
            for (const auto fresh: {part.mesh.posBuffer, part.mesh.tangentFrameBuffer, part.mesh.surfaceBuffer,
                 part.mesh.skinBuffer, part.mesh.indexBuffer, part.mesh.meshletBuffer,
                 part.mesh.meshletVertexBuffer, part.mesh.meshletTriBuffer})
                if (oldBuffers.contains(fresh)) return Error::InvalidArgument;
            if (oldPipelines.contains(part.material.pipeline) || oldPipelines.contains(part.material.prePassPipeline) ||
                !replacements.emplace(part.meshAsset, &part).second || !replacementMaterialKeys.insert(part.materialAsset).second)
                return Error::InvalidArgument;
        }
        RenderContext& gfx = Device();
        Library().UseRenderContext(gfx);
        // Every subsequent operation is non-failing. Retire all aliases before
        // buffers (parts may share a primitive), then publish the complete set.
        for (const auto& old: prefab->parts) {
            gfx.UnregisterGPUMesh(old.meshAsset);
            // The cache owns its default material even before first spawn;
            // an alias may not have been registered yet.
            gfx.RegisterGPUMaterial(old.materialAsset, old.defaultMaterial);
            gfx.UnregisterGPUMaterial(old.materialAsset);
        }
        for (const auto& old: prefab->parts) gfx.DestroyMesh(old.mesh);
        auto& registry = m_engine->Registry();
        for (const auto entity: registry.GetEntitiesWith<Components::MeshComponent>()) {
            const auto mesh = registry.Get<Components::MeshComponent>(entity);
            const auto replacement = replacements.find(mesh->meshAsset);
            if (replacement == replacements.end()) continue;
            const auto& part = *replacement->second;
            if (auto morph = registry.Get<Components::MorphTargetComponent>(entity)) {
                morph->offset = part.morphOffset;
                morph->activeCount = part.activeMorphCount;
            }
            if (auto skin = registry.Get<Components::SkeletalMeshComponent>(entity)) {
                // Engine::HandleDeviceLost already invalidates scratch on
                // generation changes; this also handles same-device refresh.
                gfx.DestroyBuffer(skin->skinnedScratch);
                skin->skinnedScratch = BufferID::Invalid;
                skin->scratchVertexCount = 0;
            }
        }
        for (size_t i = 0; i < desc.parts.size; ++i) {
            const auto& part = desc.parts.data[i]; auto& target = prefab->parts[i];
            target.mesh = part.mesh; target.defaultMaterial = part.material;
            target.morphOffset = part.morphOffset;
            gfx.RegisterGPUMesh(part.meshAsset, part.mesh);
            gfx.RegisterGPUMaterial(part.materialAsset, part.material);
        }
        prefab->gpuResourcesReady = true;
        prefab->gpuDeviceGeneration = desc.deviceGeneration;
        prefab->gpuRevision = m_nextPrefabGPURevision++;
        return prefab->gpuRevision;
    }

    // --------------------------------------------------------- IActionContext
    //
    // Every override below is written against the same three capability values
    // an application gets from Engine::Input()/Haptics(). There is no router
    // accessor here and no InputContext include: the COM boundary has exactly
    // the surface a host is allowed to use, which is the point of the split.
    //
    // The router is reached through a mutable Engine because the capabilities
    // are constructed by it; the calls themselves are the same value operations
    // a caller makes.

    Result<void> ConfigureActionMap(const ChainInNode* mapChain) noexcept override {
        if (mapChain == nullptr) return Error::NullArgument;

        std::optional<ActionMapConfig> cfg;
        const Result<void>             chainRes =
            DispatchInChain<ActionMapConfig>(mapChain, [&](const ActionMapConfig& inCfg) -> Result<void> {
                cfg = inCfg;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!cfg.has_value()) return Error::InvalidArgument;

        const std::string_view mapName(cfg->name);
        if (mapName.empty()) return Error::InvalidArgument;

        // One call: the capability owns the map, the COM override does not.
        Inputs().ConfigureActionMap(*cfg);
        return {};
    }

    bool RemoveActionMap(StringView mapName) noexcept override {
        return Inputs().RemoveActionMap(std::string_view(mapName));
    }

    // HasActionMap / IsActionMapEnabled / GetRouting below are const and answer
    // from reads alone, so they take the reader capability instead of asking a
    // non-const InputActions for the same answer -- no const_cast, and the
    // method says by its callee that it mutates nothing.
    [[nodiscard]] bool HasActionMap(StringView mapName) const noexcept override {
        return Reader().HasActionMap(std::string_view(mapName));
    }

    [[nodiscard]] bool IsActionMapEnabled(StringView mapName) const noexcept override {
        return Reader().IsActionMapEnabled(std::string_view(mapName));
    }

    bool SetActionMapEnabled(StringView mapName, bool enabled) noexcept override {
        return Inputs().SetActionMapEnabled(std::string_view(mapName), enabled);
    }

    bool SetActionMapEnabledForSeat(StringView mapName, PlayerSeat seat, bool enabled) noexcept override {
        return Inputs().SetActionMapEnabledForSeat(std::string_view(mapName), seat, enabled);
    }

    Result<ActionID> ConfigureAction(const ChainInNode* actionChain) noexcept override {
        if (actionChain == nullptr) return Error::NullArgument;

        std::optional<ActionConfig> cfg;
        const Result<void>          chainRes =
            DispatchInChain<ActionConfig>(actionChain, [&](const ActionConfig& inCfg) -> Result<void> {
                cfg = inCfg;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes.error;
        if (!cfg.has_value()) return Error::InvalidArgument;

        const std::string_view mapName(cfg->mapName);
        const std::string_view actionName(cfg->actionName);
        if (mapName.empty() || actionName.empty()) return Error::InvalidArgument;
        if (cfg->bindingCount > 0U && cfg->bindings == nullptr) return Error::InvalidArgument;

        // No HasActionMap pre-check: ConfigureAction already resolves the map and
        // returns a null ActionID when it does not exist, so this is one lookup
        // instead of two and cannot disagree with itself.
        const ActionID action = Inputs().ConfigureAction(*cfg);
        if (!action.Valid()) return Error::InvalidArgument;
        return action;
    }

    Result<void> BindAction(StringView mapName, ActionID action,
                                          const ChainInNode* bindingChain) noexcept override {
        if (bindingChain == nullptr) return Error::NullArgument;

        std::optional<InputBinding>       appendBinding;
        std::optional<ActionRebindConfig> rebindCfg;
        uint32_t                          opCount = 0;

        const Result<void> chainRes = DispatchInChain<InputBinding, ActionRebindConfig>(
            bindingChain, [&]<typename NodeT>(const NodeT& node) -> Result<void> {
                ++opCount;
                if constexpr (std::is_same_v<NodeT, InputBinding>) {
                    appendBinding = node;
                } else if constexpr (std::is_same_v<NodeT, ActionRebindConfig>) {
                    rebindCfg = node;
                }
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (opCount != 1U) return Error::InvalidArgument;

        if (appendBinding.has_value()) {
            if (!Inputs().BindAction(mapName, action, *appendBinding)) return Error::InvalidArgument;
            return {};
        }
        if (rebindCfg.has_value()) {
            if (!Inputs().RebindAction(mapName, action, rebindCfg->slotIndex, rebindCfg->binding)) return Error::InvalidArgument;
            return {};
        }
        return Error::InvalidArgument;
    }

    bool ClearBindings(StringView mapName, ActionID action) noexcept override {
        return Inputs().ClearActionBindings(std::string_view(mapName), action);
    }

    void SnapshotDefaults() noexcept override {
        Inputs().SnapshotAllDefaults();
    }

    bool ResetActionMapToDefaults(StringView mapName) noexcept override {
        return Inputs().ResetActionMapToDefaults(std::string_view(mapName));
    }

    void ResetAllToDefaults() noexcept override {
        Inputs().ResetAllToDefaults();
    }

    Result<void> ConfigureRouting(const ChainInNode* routingChain) noexcept override {
        if (routingChain == nullptr) return Error::NullArgument;

        std::optional<InputRoutingConfig> cfg;
        const Result<void>                chainRes =
            DispatchInChain<InputRoutingConfig>(routingChain, [&](const InputRoutingConfig& inCfg) -> Result<void> {
                cfg = inCfg;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!cfg.has_value()) return Error::InvalidArgument;

        Inputs().SetRoutingPolicy(cfg->policy);
        for (size_t seatIdx = 0; seatIdx < kMaxPlayerSeats; ++seatIdx) {
            const PlayerSeat seat {static_cast<uint8_t>(seatIdx)};
            Inputs().SetKeyboardMouseSeat(seat, cfg->keyboardMouseSeatMask.Contains(seat));
        }
        return {};
    }

    Result<void> GetRouting(ChainOutNode* routingChain) const noexcept override {
        return DispatchOutChain<InputRoutingConfig>(routingChain, [&](InputRoutingConfig& outCfg) -> Result<void> {
            outCfg.policy = Reader().RoutingPolicy();
            SeatMask mask = SeatMask::None();
            for (size_t seatIdx = 0; seatIdx < kMaxPlayerSeats; ++seatIdx) {
                const PlayerSeat seat {static_cast<uint8_t>(seatIdx)};
                if (Reader().SeatReceivesKeyboardMouse(seat)) {
                    mask.Set(seat);
                }
            }
            outCfg.keyboardMouseSeatMask = mask;
            return {};
        });
    }

    [[nodiscard]] ActionState Action(ActionID id, PlayerSeat seat) const noexcept override {
        return Reader().Action(id, seat);
    }

    // The COM contract is deliberately *physical*: a host asking whether a key is
    // down is asking about the device, not about who currently owns the keyboard.
    // InputReader answers exactly that way, so the two surfaces agree.
    [[nodiscard]] bool KeyDown(KeyCode key, PlayerSeat seat) const noexcept override {
        return Reader().SeatKeyboardMouse(seat).IsKeyDown(key);
    }

    [[nodiscard]] bool KeyPressed(KeyCode key, PlayerSeat seat) const noexcept override {
        return Reader().SeatKeyboardMouse(seat).IsKeyPressed(key);
    }

    [[nodiscard]] bool KeyReleased(KeyCode key, PlayerSeat seat) const noexcept override {
        return Reader().SeatKeyboardMouse(seat).IsKeyReleased(key);
    }

    [[nodiscard]] Axis2DValue MousePosition(PlayerSeat seat) const noexcept override {
        return Axis2DValue {.x = Reader().SeatKeyboardMouse(seat).mouseX, .y = Reader().SeatKeyboardMouse(seat).mouseY};
    }

    [[nodiscard]] Axis2DValue MouseDelta(PlayerSeat seat) const noexcept override {
        return Axis2DValue {.x = Reader().SeatKeyboardMouse(seat).mouseDeltaX, .y = Reader().SeatKeyboardMouse(seat).mouseDeltaY};
    }

    [[nodiscard]] float MouseWheel(PlayerSeat seat) const noexcept override {
        return Reader().SeatKeyboardMouse(seat).mouseWheel;
    }

    [[nodiscard]] InputDeviceKind LastActiveDevice(PlayerSeat seat) const noexcept override {
        return Reader().LastActiveDevice(seat);
    }

    [[nodiscard]] uint32_t ConnectedGamepadCount() const noexcept override {
        return static_cast<uint32_t>(Reader().ConnectedGamepadCount());
    }

    [[nodiscard]] bool GamepadConnected(GamepadSlot slot) const noexcept override {
        return (static_cast<bool>(Reader().Gamepad(slot)) && Reader().Gamepad(slot)->connected);
    }

    [[nodiscard]] bool GamepadButtonDown(GamepadSlot slot, GamepadButton button) const noexcept override {
        return (static_cast<bool>(Reader().Gamepad(slot)) && Reader().Gamepad(slot)->IsButtonDown(button));
    }

    [[nodiscard]] float GamepadAxisValue(GamepadSlot slot, GamepadAxis axis) const noexcept override {
        return (static_cast<bool>(Reader().Gamepad(slot)) ? Reader().Gamepad(slot)->Axis(axis) : 0.0f);
    }

    [[nodiscard]] PlayerSeat GamepadSeat(GamepadSlot slot) const noexcept override {
        return Reader().GamepadSeat(slot);
    }

    bool AssignGamepadToSeat(GamepadSlot slot, PlayerSeat seat) noexcept override {
        return Inputs().AssignGamepadToSeat(slot, seat);
    }

    [[nodiscard]] bool MouseButtonDown(MouseButton button, PlayerSeat seat) const noexcept override { return Reader().SeatKeyboardMouse(seat).IsMouseButtonDown(button); }
    [[nodiscard]] bool MouseButtonPressed(MouseButton button, PlayerSeat seat) const noexcept override { return Reader().SeatKeyboardMouse(seat).IsMouseButtonPressed(button); }
    [[nodiscard]] bool MouseButtonReleased(MouseButton button, PlayerSeat seat) const noexcept override { return Reader().SeatKeyboardMouse(seat).IsMouseButtonReleased(button); }
    [[nodiscard]] KeyModifiers Modifiers(PlayerSeat seat) const noexcept override { return Reader().SeatKeyboardMouse(seat).Modifiers(); }
    [[nodiscard]] bool HasModifiers(KeyModifiers required, bool exact, PlayerSeat seat) const noexcept override { return Reader().SeatKeyboardMouse(seat).HasModifiers(required, exact); }
    [[nodiscard]] bool GamepadButtonPressed(GamepadSlot slot, GamepadButton button) const noexcept override { return (static_cast<bool>(Reader().Gamepad(slot)) && Reader().Gamepad(slot)->IsButtonPressed(button)); }
    [[nodiscard]] bool GamepadButtonReleased(GamepadSlot slot, GamepadButton button) const noexcept override { return (static_cast<bool>(Reader().Gamepad(slot)) && Reader().Gamepad(slot)->IsButtonReleased(button)); }
    [[nodiscard]] StickSample GamepadLeftStick(GamepadSlot slot) const noexcept override { return (static_cast<bool>(Reader().Gamepad(slot)) ? Reader().Gamepad(slot)->LeftStick() : StickSample {}); }
    [[nodiscard]] StickSample GamepadRightStick(GamepadSlot slot) const noexcept override { return (static_cast<bool>(Reader().Gamepad(slot)) ? Reader().Gamepad(slot)->RightStick() : StickSample {}); }
    [[nodiscard]] float GamepadLeftTrigger(GamepadSlot slot) const noexcept override { return (static_cast<bool>(Reader().Gamepad(slot)) ? Reader().Gamepad(slot)->LeftTrigger() : 0.0f); }
    [[nodiscard]] float GamepadRightTrigger(GamepadSlot slot) const noexcept override { return (static_cast<bool>(Reader().Gamepad(slot)) ? Reader().Gamepad(slot)->RightTrigger() : 0.0f); }
    bool SetGamepadRumble(GamepadSlot slot, const GamepadRumble& rumble) noexcept override {
        return Inputs().SetGamepadRumble(slot, rumble);
    }
    uint32_t RumbleSeat(PlayerSeat seat, const GamepadRumble& rumble) noexcept override {
        return Inputs().RumbleSeat(seat, rumble);
    }
    void StopRumble(GamepadSlot slot) noexcept override { Inputs().StopRumble(slot); }
    void StopAllRumble() noexcept override { Inputs().StopAllRumble(); }
    void BeginBindingCapture(PlayerSeat seat, bool captureAxes, KeyCode cancelKey) noexcept override {
        Inputs().BeginBindingCapture(seat, captureAxes, cancelKey);
    }
    void CancelBindingCapture() noexcept override { Inputs().CancelBindingCapture(); }
    [[nodiscard]] Option<InputBinding> TakeCapturedBinding() noexcept override {
        const auto captured = Inputs().TakeCapturedBinding();
        if (!captured) return std::nullopt;
        return *captured;
    }
    [[nodiscard]] BindingCaptureStatus CaptureStatus() const noexcept override { return Inputs().CaptureStatus(); }
    Result<uint32_t> SerializeBindings(char* buffer, uint32_t capacity) const noexcept override {
        if (buffer == nullptr && capacity != 0) return Error::NullArgument;
        const auto text = Inputs().SerializeBindings();
        if (text.size() > std::numeric_limits<uint32_t>::max()) return Error::InvalidArgument;
        const auto needed = static_cast<uint32_t>(text.size());
        if (buffer != nullptr && capacity >= needed && needed != 0) std::memcpy(buffer, text.data(), needed);
        return needed;
    }
    Result<void> DeserializeBindings(StringView text) noexcept override {
        if (text.data == nullptr && text.size != 0) return Error::NullArgument;
        const auto outcome = Inputs().DeserializeBindings(text.size == 0 ? std::string_view {} : std::string_view(text));
        if (!outcome) return outcome.error();
        return {};
    }

private:
  friend class Implements<
      Engine,
      IEngine,
      IGraphicsDevice,
      IEntities,
      ISimulation,
      IAudioDevice,
      IAssetLibrary,
      IActionContext,
      IState,
      IDebugDraw,
      IFramePipeline>;

  ~Engine() {
      for (const auto& [window, subscription]: m_dropSubscriptions)
          Session().SetWindowFileDropHandler(window, nullptr, nullptr);
  }
    [[nodiscard]] bool WindowExists(WindowID window) const noexcept {
        const auto ids = m_engine->WindowIDs();
        return std::ranges::find(ids, window) != ids.end();
    }
    struct DropSubscription {
        WindowID window;
        FileDropCallback callback;
        void* userdata;
    };
    std::map<WindowID, std::unique_ptr<DropSubscription>> m_dropSubscriptions;

    // The capability accessors are non-const and hand back a fresh facade by
    // value, so a const boundary method still has to ask the engine mutably.
    // The facades are two pointers wide; this is a load, not an allocation.
    //
    // Each is named away from its interface accessor on purpose: Simulation(),
    // Audio(), Assets() and Actions() are already taken by IEngine above, and
    // a member name would hide the engine type of the same spelling.
    [[nodiscard]] auto Session() const noexcept -> KernelSession& {
        return m_engine->Session();
    }

    [[nodiscard]] auto Device() const noexcept -> RenderContext& {
        return m_engine->Render();
    }

    [[nodiscard]] auto Sim() const noexcept -> PhysicsContext& {
        return m_engine->Physics();
    }

    [[nodiscard]] auto Sound() const noexcept -> AudioContext& {
        return m_engine->Audio();
    }

    [[nodiscard]] auto Library() const noexcept -> AssetManager& {
        return m_engine->Assets();
    }

    [[nodiscard]] auto Reader() const noexcept -> const InputContext& {
        return std::as_const(*m_engine).Input();
    }

    [[nodiscard]] auto Inputs() const noexcept -> InputContext& {
        return m_engine->Input();
    }

    struct DeviceLostEntry {
        uint64_t token;
        void (*callback)(void*);
        void* userdata;
    };
    struct DeviceLostListeners {
        std::vector<DeviceLostEntry> entries;
        uint64_t nextToken = 1;
    };
    uint64_t m_nextPrefabGPURevision = 1;
    std::unique_ptr<ZHLN::Engine> m_engine;
    std::shared_ptr<DeviceLostListeners> m_deviceLostListeners;
};

void Engine::TraceDiagnostics() const noexcept {
    m_engine->TraceDiagnostics();
}

// --- Factory ----------------------------------------------------------------

auto CreateEngine(const ChainInNode* configChain) noexcept -> Result<IEngine*> {
    EngineConfig config {};
    uint32_t     maxFramesPerSecond = 0;

    const Result<void> chainRes =
        DispatchInChain<AppConfig, WindowConfig, RenderConfig, PhysicsConfig, DiagnosticsInfo>(
            configChain, [&]<typename NodeT>(const NodeT& node) -> Result<void> {
                if constexpr (std::is_same_v<NodeT, DiagnosticsInfo>) {
                    if (!Valid(node.crashDumpPath)) return Error::NullArgument;
                    config.crashDumpPath = Text(node.crashDumpPath);
                } else if constexpr (std::is_same_v<NodeT, AppConfig>) {
                    const std::string_view nameView(node.appName);
                    if (!nameView.empty()) config.appName = node.appName;
                    config.logLevel    = node.logLevel;
                    maxFramesPerSecond = node.maxFramesPerSecond;
                } else if constexpr (std::is_same_v<NodeT, WindowConfig>) {
                    config.width      = node.width;
                    config.height     = node.height;
                    config.fullscreen = node.fullscreen;
                    config.vsync      = node.vsync;
                    config.headless   = node.headless;
                } else if constexpr (std::is_same_v<NodeT, RenderConfig>) {
                    config.validationMode            = node.validationMode;
                    config.enableMeshShading         = node.enableMeshShading;
                    config.initialPipelineBinaryData = std::span<const uint8_t>(node.initialPipelineBinary);
                } else if constexpr (std::is_same_v<NodeT, PhysicsConfig>) {
                    config.physics = node;
                }
                return {};
            });
    if (!chainRes.HasValue()) return chainRes.error;

    config.fpsLimit = maxFramesPerSecond;
    auto created = ZHLN::Engine::Create(config);
    if (!created) return created.error();

    (*created)->SetMaxFramesPerSecond(maxFramesPerSecond);
    (*created)->EmplaceService<EngineConfig>((*created)->Configuration());

    auto* instance = new (std::nothrow) Engine(std::move(*created));
    if (instance == nullptr) return ErrorCode(Error::AllocationFailed);
    return static_cast<IEngine*>(instance);
}


auto RenderGraphDump(AAMode mode) noexcept -> StringView {
    const std::string_view dump = ZHLN::GetRenderGraphDump(mode);
    return StringView(dump.data(), dump.size());
}

} // namespace ZHLN::COM
