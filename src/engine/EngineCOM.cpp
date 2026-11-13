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
#include <Zahlen/Config.hpp>
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/COM/Chain.hpp>
#include <Zahlen/Core/COM/Reflect.hpp>
#include <Zahlen/Core/Reflection/CanonicalName.hpp>
#include <Zahlen/Core/Reflection/LayoutHash.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Engine/AssetManager.hpp>
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/AudioEngine.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Engine/SceneResources.hpp>
#include <Zahlen/Input/ActionValues.hpp>
#include <Zahlen/Input/InputContext.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Physics/PhysicsShapes.hpp>
#include <Zahlen/Physics/PhysicsValues.hpp>
#include <Zahlen/Render/EnvironmentImage.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Render/Meshlet.hpp>
#include <Zahlen/Window/PlatformHost.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ZHLN::COM {

static_assert(AuditInterfaceABI<IEngine>());
static_assert(AuditInterfaceABI<IGraphicsDevice>());
static_assert(AuditInterfaceABI<IEntities>());
static_assert(AuditInterfaceABI<ISimulation>());
static_assert(AuditInterfaceABI<IAudioDevice>());
static_assert(AuditInterfaceABI<IAssetLibrary>());
static_assert(AuditInterfaceABI<IActionContext>());

namespace {

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
struct PublishedComponent {
    std::string_view name;
    uint32_t         size;
    uint32_t         alignment;
    uint64_t         layoutHash;
    uint32_t (*familyID)() noexcept;
    void* (*ensure)(Engine&, ECS::Registry&, Entity) noexcept;
    bool (*remove)(Engine&, ECS::Registry&, Entity) noexcept;
    void* (*fetch)(const ECS::Registry&, Entity) noexcept;
};

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
        .ensure =
            +[](Engine& engine, ECS::Registry& registry, Entity entity) noexcept -> void* {
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
            if (!registry.IsAlive(entity)) return nullptr;
            return &registry.Add<T>(entity);
        },
        .remove =
            +[](Engine& engine, ECS::Registry& registry, Entity entity) noexcept -> bool {
            if constexpr (SceneResources::OwnsExternalResource<T>) {
                return SceneResources::Detach<T>(engine, registry, entity);
            } else {
                const uint32_t id = ECS::ComponentFamily::GetTypeID<T>();
                if (registry.GetRawByFamily(entity, id) == nullptr) return false;
                registry.Remove<T>(entity);
                return true;
            }
        },
        .fetch =
            +[](const ECS::Registry& registry, Entity entity) noexcept -> void* {
            return registry.GetRawByFamily(entity, ECS::ComponentFamily::GetTypeID<T>());
        },
    };
}

constexpr std::array kPublishedComponents {
    Publish<Components::TransformComponent>(),
    Publish<Components::WorldTransformComponent>(),
    Publish<Components::HierarchyComponent>(),
    Publish<Components::NameComponent>(),
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
    Publish<Components::SunTagComponent>(),
    Publish<Components::EnvironmentMapComponent>(),
    Publish<Components::DecalComponent>(),
    Publish<Components::ParticleEmitterComponent>(),
    Publish<Components::MeshParticleEmitterComponent>(),
    Publish<Components::VolumetricFogComponent>(),
    Publish<Components::VolumetricVolumeComponent>(),
    Publish<Components::AnimatorComponent>(),
    Publish<Components::PhysicsComponent>(),
    Publish<Components::ImpulseCommand>(),
    Publish<Components::RagdollComponent>(),
    Publish<Components::RagdollHitReactionCommand>(),
    Publish<Components::RagdollImpulseCommand>(),
    Publish<Components::AudioListenerComponent>(),
    Publish<Components::AudioSourceComponent>(),
    Publish<Components::PlayerTagComponent>(),
    Publish<Components::PlayerSeatComponent>(),
    Publish<Components::CullingStatsComponent>(),
    Publish<Components::DebugSettingsComponent>(),
};

[[nodiscard]] auto FindPublished(StringView typeName) noexcept -> const PublishedComponent* {
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

class EngineCOM final
    : public Implements<EngineCOM, IEngine, IGraphicsDevice, IEntities, ISimulation, IAudioDevice, IAssetLibrary,
                        IActionContext> {
public:
    explicit EngineCOM(std::unique_ptr<Engine> engine) noexcept : m_engine(std::move(engine)) {}

    // ---------------------------------------------------------------- IEngine
    [[nodiscard]] bool IsRunning() const noexcept override {
        return m_engine->IsRunning();
    }

    void ProcessEvents() noexcept override {
        m_engine->ProcessEvents();
    }

    void RequestClose() noexcept override {
        m_engine->GetPlatformHost().Close();
    }

    [[nodiscard]] Result<void> Tick(float dt) noexcept override {
        const auto outcome = m_engine->Tick(dt);
        if (!outcome) return outcome.error();
        return {};
    }

    void ClearScene() noexcept override {
        m_engine->ClearScene();
    }

    [[nodiscard]] Result<void> InitializeDefaultScene() noexcept override {
        m_engine->InitializeDefaultScene();
        return {};
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

    [[nodiscard]] Result<void> ApplySettings(const ChainInNode* settingsChain) noexcept override {
        if (settingsChain == nullptr) return ComError::NullArgument;

        GraphicsSettings merged   = Device().Settings();
        bool             modified = false;

        const Result<void> chainRes =
            DispatchInChain<GraphicsSettings, AAState, GISettings, ShadowSettings, RayTracingConfig,
                            EnvironmentSettings>(settingsChain, [&]<typename NodeT>(const NodeT& node) -> Result<void> {
                if constexpr (std::is_same_v<NodeT, GraphicsSettings>) {
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
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!modified) return ComError::InvalidArgument;

        m_engine->Graphics().ApplySettings(merged);
        return {};
    }

    [[nodiscard]] Result<void> GetSettings(ChainOutNode* settingsChain) const noexcept override {
        const GraphicsSettings& current = Device().Settings();
        return DispatchOutChain<GraphicsSettings, AAState, GISettings, ShadowSettings, RayTracingConfig,
                                EnvironmentSettings>(settingsChain, [&]<typename NodeT>(NodeT& out) -> Result<void> {
            if constexpr (std::is_same_v<NodeT, GraphicsSettings>) {
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
        });
    }

    [[nodiscard]] Result<BufferID> CreateBuffer(const ChainInNode* bufferChain) noexcept override {
        if (bufferChain == nullptr) return ComError::NullArgument;

        std::optional<BufferCreateInfo> createInfo;
        const Result<void>              chainRes =
            DispatchInChain<BufferCreateInfo>(bufferChain, [&](const BufferCreateInfo& info) -> Result<void> {
                createInfo = info;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes.error;
        if (!createInfo.has_value()) return ComError::InvalidArgument;

        BufferDesc desc {};
        desc.usage  = createInfo->usage;
        desc.data   = std::span<const std::byte>(createInfo->data);
        desc.size   = createInfo->size != 0 ? createInfo->size : createInfo->data.size;
        desc.stride = createInfo->stride;
        return m_engine->Graphics().CreateBuffer(desc);
    }

    void DestroyBuffer(BufferID handle) noexcept override {
        m_engine->Graphics().DestroyBuffer(handle);
    }

    [[nodiscard]] Result<void> UpdateBuffer(BufferID handle, Span<const std::byte> data) noexcept override {
        const auto outcome = m_engine->Graphics().UpdateBuffer(handle, std::span<const std::byte>(data));
        if (!outcome) return outcome.error();
        return {};
    }

    [[nodiscard]] Result<TextureID> CreateTexture(const ChainInNode* textureChain) noexcept override {
        if (textureChain == nullptr) return ComError::NullArgument;

        std::optional<TextureCreateInfo> info;
        const Result<void>               chainRes =
            DispatchInChain<TextureCreateInfo>(textureChain, [&](const TextureCreateInfo& tex) -> Result<void> {
                info = tex;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes.error;
        if (!info.has_value()) return ComError::InvalidArgument;
        if (info->pixels.data == nullptr || info->pixels.size == 0 || info->extent.width == 0 || info->extent.height == 0) {
            return ComError::InvalidArgument;
        }

        const std::string_view           texName(info->name);
        const std::span<const std::byte> bytes(info->pixels);
        if (info->isHDR) {
            auto hdrRes = m_engine->Graphics().CreateTextureHDR(texName, bytes, info->extent, info->generateMips);
            if (!hdrRes) return hdrRes.error();
            return *hdrRes;
        }
        auto texRes = m_engine->Graphics().CreateTexture(texName, bytes, info->extent, info->isSRGB, info->generateMips);
        if (!texRes) return texRes.error();
        return *texRes;
    }

    void UnloadTexture(TextureID handle) noexcept override {
        m_engine->Graphics().UnloadTexture(handle);
    }

    [[nodiscard]] Result<void> CreateMaterial(const ChainInNode* descChain, ChainOutNode* outMaterialChain) noexcept override {
        if (descChain == nullptr || outMaterialChain == nullptr) return ComError::NullArgument;

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
        if (sourceCount != 1U || !desc.has_value()) return ComError::InvalidArgument;

        auto matRes = m_engine->Graphics().CreateMaterial(*desc);
        if (!matRes) return matRes.error();

        return DispatchOutChain<ZHLN::Material>(outMaterialChain, [&](ZHLN::Material& outMat) -> Result<void> {
            outMat = *matRes;
            return {};
        });
    }

    [[nodiscard]] Result<void> RegisterMaterial(MaterialID id, const ChainInNode* materialChain) noexcept override {
        if (materialChain == nullptr) return ComError::NullArgument;

        std::optional<ZHLN::Material> mat;
        const Result<void>            chainRes =
            DispatchInChain<ZHLN::Material>(materialChain, [&](const ZHLN::Material& inMat) -> Result<void> {
                mat = inMat;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!mat.has_value()) return ComError::InvalidArgument;

        m_engine->Graphics().RegisterMaterial(id, *mat);
        return {};
    }

    [[nodiscard]] Result<void> GetMaterial(MaterialID id, ChainOutNode* outMaterialChain) const noexcept override {
        if (outMaterialChain == nullptr) return ComError::NullArgument;

        const std::optional<ZHLN::Material> found = Device().Material(id);
        if (!found.has_value()) return ComError::AssetNotCached;
        return DispatchOutChain<ZHLN::Material>(outMaterialChain, [&](ZHLN::Material& outMat) -> Result<void> {
            outMat = *found;
            return {};
        });
    }

    void UnregisterMaterial(MaterialID id) noexcept override {
        m_engine->Graphics().UnregisterMaterial(id);
    }

    [[nodiscard]] Result<void> CreateMesh(const ChainInNode* buildChain, ChainOutNode* outMeshChain) noexcept override {
        if (buildChain == nullptr || outMeshChain == nullptr) return ComError::NullArgument;

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
        if (geometryNodeCount != 1U) return ComError::InvalidArgument;

        GraphicsDevice gfx = m_engine->Graphics();
        ZHLN::Mesh     createdMesh {};

        if (rawStreams.has_value()) {
            const MeshStreamsDesc& s = *rawStreams;
            if (s.positions.data == nullptr || s.positions.size == 0) return ComError::InvalidArgument;
            if (s.indices16.size > 0 && s.indices32.size > 0) return ComError::InvalidArgument;

            const size_t vertexCount = s.positions.size;
            if ((s.tangentFrames.size > 0 && s.tangentFrames.size != vertexCount) ||
                (s.surfaces.size > 0 && s.surfaces.size != vertexCount) ||
                (s.skin.size > 0 && s.skin.size != vertexCount)) {
                return ComError::InvalidArgument;
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
            createdMesh = PrefabFactory::CreateBoxMesh(gfx, boxDesc->halfExtents, boxDesc->color);
        } else if (planeDesc.has_value()) {
            createdMesh = PrefabFactory::CreatePlaneMesh(gfx, planeDesc->extent, planeDesc->color);
        } else if (sphereDesc.has_value()) {
            createdMesh = PrefabFactory::CreateSphereMesh(gfx, sphereDesc->radius, sphereDesc->color);
        } else if (cylinderDesc.has_value()) {
            createdMesh = PrefabFactory::CreateCylinderMesh(gfx, cylinderDesc->radius, cylinderDesc->height,
                                                            cylinderDesc->color);
        } else if (coneDesc.has_value()) {
            createdMesh = PrefabFactory::CreateConeMesh(gfx, coneDesc->radius, coneDesc->height, coneDesc->color);
        } else if (tetraDesc.has_value()) {
            createdMesh = PrefabFactory::CreateTetrahedronMesh(gfx);
        }

        return DispatchOutChain<ZHLN::Mesh>(outMeshChain, [&](ZHLN::Mesh& outMesh) -> Result<void> {
            outMesh = createdMesh;
            return {};
        });
    }

    [[nodiscard]] Result<void> RegisterMesh(AssetID id, const ChainInNode* meshChain) noexcept override {
        if (meshChain == nullptr) return ComError::NullArgument;

        std::optional<ZHLN::Mesh> mesh;
        const Result<void>        chainRes =
            DispatchInChain<ZHLN::Mesh>(meshChain, [&](const ZHLN::Mesh& inMesh) -> Result<void> {
                mesh = inMesh;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!mesh.has_value()) return ComError::InvalidArgument;

        m_engine->Graphics().RegisterMesh(id, *mesh);
        return {};
    }

    [[nodiscard]] Result<void> GetMesh(AssetID id, ChainOutNode* outMeshChain) const noexcept override {
        if (outMeshChain == nullptr) return ComError::NullArgument;

        const std::optional<ZHLN::Mesh> found = Device().Mesh(id);
        if (!found.has_value()) return ComError::AssetNotCached;
        return DispatchOutChain<ZHLN::Mesh>(outMeshChain, [&](ZHLN::Mesh& outMesh) -> Result<void> {
            outMesh = *found;
            return {};
        });
    }

    void UnregisterMesh(AssetID id) noexcept override {
        m_engine->Graphics().UnregisterMesh(id);
    }

    [[nodiscard]] Result<void> DestroyMesh(const ChainInNode* meshChain) noexcept override {
        if (meshChain == nullptr) return ComError::NullArgument;

        std::optional<ZHLN::Mesh> mesh;
        const Result<void>        chainRes =
            DispatchInChain<ZHLN::Mesh>(meshChain, [&](const ZHLN::Mesh& inMesh) -> Result<void> {
                mesh = inMesh;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!mesh.has_value()) return ComError::InvalidArgument;

        m_engine->Graphics().DestroyMesh(*mesh);
        return {};
    }

    [[nodiscard]] Result<void> BuildMeshBLAS(const ChainInNode* inMeshChain, ChainOutNode* outMeshChain) noexcept override {
        if (inMeshChain == nullptr || outMeshChain == nullptr) return ComError::NullArgument;

        std::optional<ZHLN::Mesh> mesh;
        const Result<void>        chainRes =
            DispatchInChain<ZHLN::Mesh>(inMeshChain, [&](const ZHLN::Mesh& inMesh) -> Result<void> {
                mesh = inMesh;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!mesh.has_value()) return ComError::InvalidArgument;

        auto blasRes = m_engine->Graphics().BuildMeshBLAS(*mesh);
        if (!blasRes) return blasRes.error();

        return DispatchOutChain<ZHLN::Mesh>(outMeshChain, [&](ZHLN::Mesh& outMesh) -> Result<void> {
            outMesh = *mesh;
            return {};
        });
    }

    // -------------------------------------------------------------- IEntities
    [[nodiscard]] Entity CreateEntity() noexcept override {
        return m_engine->Entities().Create();
    }

    void Despawn(Entity entity) noexcept override {
        m_engine->Entities().Despawn(entity);
    }

    [[nodiscard]] bool IsAlive(Entity entity) const noexcept override {
        return m_engine->GetInternalRegistry().IsAlive(entity);
    }

    [[nodiscard]] void* AttachRaw(Entity entity, StringView typeName, uint32_t size, uint32_t alignment) noexcept override {
        auto&          registry = m_engine->GetInternalRegistry();
        const uint32_t family   = FamilyFor(registry, typeName, size, alignment);
        if (family == kInvalidFamily) return nullptr;
        return registry.AddDynamic(entity, family);
    }

    bool DetachRaw(Entity entity, StringView typeName) noexcept override {
        auto&          registry = m_engine->GetInternalRegistry();
        const uint32_t family   = ECS::Registry::GetFamilyIDFromName(std::string_view(typeName));
        if (family == kInvalidFamily) return false;
        return registry.RemoveDynamic(entity, family);
    }

    [[nodiscard]] void* GetRaw(Entity entity, StringView typeName) const noexcept override {
        const uint32_t family = ECS::Registry::GetFamilyIDFromName(std::string_view(typeName));
        if (family == kInvalidFamily) return nullptr;
        return m_engine->GetInternalRegistry().GetRawByFamily(entity, family);
    }

    [[nodiscard]] bool IsEngineComponent(StringView typeName) const noexcept override {
        return FindPublished(typeName) != nullptr;
    }

    [[nodiscard]] void* AttachEngineRaw(Entity entity, StringView typeName, uint32_t size, uint32_t alignment,
                                        uint64_t layoutHash) noexcept override {
        const PublishedComponent* published = FindPublished(typeName);
        if (published == nullptr) return nullptr;
        // All three must agree. The hash subsumes the other two, but checking
        // them separately makes a stale consumer fail for a legible reason.
        if (published->size != size || published->alignment != alignment || published->layoutHash != layoutHash) {
            return nullptr;
        }
        auto& registry = m_engine->GetInternalRegistry();
        if (!registry.IsAlive(entity)) return nullptr;
        return published->ensure(*m_engine, registry, entity);
    }

    bool DetachEngineRaw(Entity entity, StringView typeName) noexcept override {
        const PublishedComponent* published = FindPublished(typeName);
        if (published == nullptr) return false;
        auto& registry = m_engine->GetInternalRegistry();
        if (!registry.IsAlive(entity)) return false;
        return published->remove(*m_engine, registry, entity);
    }

    [[nodiscard]] void* GetEngineRaw(Entity entity, StringView typeName) const noexcept override {
        const PublishedComponent* published = FindPublished(typeName);
        if (published == nullptr) return nullptr;
        return published->fetch(m_engine->GetInternalRegistry(), entity);
    }

    [[nodiscard]] uint32_t EntitiesWithRaw(StringView typeName, Entity* outEntities,
                                           uint32_t capacity) const noexcept override {
        const auto& registry = m_engine->GetInternalRegistry();
        uint32_t    family   = kInvalidFamily;
        if (const PublishedComponent* published = FindPublished(typeName)) {
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
        const std::vector<Physics::BodyID> found = Sim().BodiesInSphere(center, radius);
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

    void SetCharacterVelocity(Physics::BodyID body, FVec3 velocity) noexcept override {
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

    void ClearMeshShapeCache() noexcept override {
        Sim().ClearMeshShapeCache();
    }

    void Step(float deltaTime) noexcept override {
        Sim().Step(deltaTime);
    }

    [[nodiscard]] Result<Physics::BodyID> CreateRigidBody(const ChainInNode* descChain) noexcept override {
        if (descChain == nullptr) return ComError::NullArgument;

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
                                return ComError::InvalidArgument;
                            }
                            builtShape = Physics::AnyShape {Physics::Shape<Physics::Box>(node.halfExtents)};
                        } else if constexpr (std::is_same_v<NodeT, Physics::SphereShapeDesc>) {
                            if (node.radius <= 0.0F) return ComError::InvalidArgument;
                            builtShape = Physics::AnyShape {Physics::Shape<Physics::Sphere>(node.radius)};
                        } else if constexpr (std::is_same_v<NodeT, Physics::CapsuleShapeDesc>) {
                            if (node.halfHeight <= 0.0F || node.radius <= 0.0F) return ComError::InvalidArgument;
                            builtShape =
                                Physics::AnyShape {Physics::Shape<Physics::Capsule>(node.halfHeight, node.radius)};
                        } else if constexpr (std::is_same_v<NodeT, Physics::CylinderShapeDesc>) {
                            if (node.halfHeight <= 0.0F || node.radius <= 0.0F) return ComError::InvalidArgument;
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
                                    if (!geom.has_value()) return ComError::InvalidArgument;
                                    (void)m_engine->Assets().CacheCollisionMesh(node.asset, std::move(*geom));
                                    pinnedMesh = Library().FindCollisionMesh(node.asset);
                                }
                                if (!pinnedMesh) return ComError::AssetNotCached;
                                Physics::Shape<Physics::Mesh> meshShape(pinnedMesh);
                                meshShape.asset = node.asset;
                                builtShape      = Physics::AnyShape {std::move(meshShape)};
                            } else {
                                auto geom = ValidateAndBuildMeshGeometry(node.vertices, node.vertexCount, node.indices,
                                                                         node.indexCount);
                                if (!geom.has_value()) return ComError::InvalidArgument;
                                pinnedMesh = std::make_shared<Physics::MeshGeometry>(std::move(*geom));
                                builtShape = Physics::AnyShape {Physics::Shape<Physics::Mesh>(pinnedMesh)};
                            }
                        } else if constexpr (std::is_same_v<NodeT, Physics::HeightFieldShapeDesc>) {
                            if (node.heights == nullptr || node.sampleCount < 2U || node.worldSize <= 0.0F) {
                                return ComError::InvalidArgument;
                            }
                            const size_t totalSamples = static_cast<size_t>(node.sampleCount) * node.sampleCount;
                            builtShape                = Physics::AnyShape {Physics::Shape<Physics::HeightField>(
                                std::span<const float>(node.heights, totalSamples), node.sampleCount, node.worldSize)};
                        } else if constexpr (std::is_same_v<NodeT, Physics::CompoundShapeDesc>) {
                            if (node.children == nullptr || node.childCount == 0U) return ComError::InvalidArgument;
                            Physics::Shape<Physics::Compound> comp {};
                            comp.children.reserve(node.childCount);
                            for (uint32_t i = 0; i < node.childCount; ++i) {
                                const Physics::CompoundChildDesc& child = node.children[i];
                                auto                              sub   = MakePrimitiveSubShape(child);
                                if (!sub.has_value()) return ComError::InvalidArgument;
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
        if (shapeNodeCount != 1U || !builtShape.has_value()) return ComError::InvalidArgument;

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

    [[nodiscard]] Result<Physics::BodyID> CreateCharacter(const ChainInNode* descChain) noexcept override {
        Physics::CharacterCreateDesc desc {};
        if (descChain != nullptr) {
            const Result<void> chainRes = DispatchInChain<Physics::CharacterCreateDesc>(
                descChain, [&](const Physics::CharacterCreateDesc& inDesc) -> Result<void> {
                    desc = inDesc;
                    return {};
                });
            if (!chainRes.HasValue()) return chainRes.error;
        }
        if (desc.useCustomCapsule && (desc.capsuleRadius <= 0.0F || desc.capsuleHalfHeight <= 0.0F)) {
            return ComError::InvalidArgument;
        }
        return Sim().CreateCustomCharacter(desc);
    }

    [[nodiscard]] Physics::DebugDrawData DebugGeometry(bool shapes, bool constraints, bool wireframe) noexcept override {
        return Sim().DebugDrawData(shapes, constraints, wireframe);
    }

    // ------------------------------------------------------------ IAudioDevice
    [[nodiscard]] bool Initialized() const noexcept override {
        return Sound().Initialized();
    }

    [[nodiscard]] Result<void> PostEvent(const ChainInNode* eventChain) noexcept override {
        if (eventChain == nullptr) return ComError::NullArgument;

        std::optional<AudioEvent> evt;
        const Result<void>        chainRes =
            DispatchInChain<AudioEvent>(eventChain, [&](const AudioEvent& inEvent) -> Result<void> {
                evt = inEvent;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!evt.has_value()) return ComError::InvalidArgument;

        Sound().PostEvent(*evt);
        return {};
    }

    [[nodiscard]] Result<VoiceID> CreateVoice(StringView filepath, const ChainInNode* voiceChain) noexcept override {
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

    [[nodiscard]] Result<uint32_t> Instantiate(uint64_t hash, const ChainInNode* spawnChain, Entity* entities,
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

    [[nodiscard]] Result<Entity> SpawnPrimitive(const ChainInNode* primitiveChain) noexcept override {
        if (primitiveChain == nullptr) return ComError::NullArgument;

        PrefabFactory::SpawnParams      params {};
        std::optional<BoxMeshDesc>      boxDesc;
        std::optional<PlaneMeshDesc>    planeDesc;
        std::optional<SphereMeshDesc>   sphereDesc;
        std::optional<CylinderMeshDesc> cylinderDesc;
        std::optional<ConeMeshDesc>     coneDesc;
        uint32_t                        shapeCount = 0;

        const Result<void> chainRes =
            DispatchInChain<PrefabFactory::SpawnParams, BoxMeshDesc, PlaneMeshDesc, SphereMeshDesc, CylinderMeshDesc,
                            ConeMeshDesc>(primitiveChain, [&]<typename NodeT>(const NodeT& node) -> Result<void> {
                if constexpr (std::is_same_v<NodeT, PrefabFactory::SpawnParams>) {
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
            });
        if (!chainRes.HasValue()) return chainRes.error;
        if (shapeCount != 1U) return ComError::InvalidArgument;

        if (boxDesc.has_value()) {
            PrefabFactory::SpawnParams resolved = params;
            if (boxDesc->color.w >= 0.0F) {
                resolved.color = boxDesc->color;
                if (resolved.materialOverride.pipeline == PipelineID::Invalid) {
                    resolved.materialOverride.baseColorFactor = boxDesc->color;
                }
            }
            return PrefabFactory::CreateBox(*m_engine, boxDesc->halfExtents, resolved);
        }
        if (planeDesc.has_value()) {
            const FVec4 planeColor = planeDesc->color.w >= 0.0F ? planeDesc->color : FVec4 {0.6F, 0.6F, 0.6F, 1.0F};
            return PrefabFactory::CreatePlane(*m_engine, planeDesc->extent, planeColor, params);
        }
        if (sphereDesc.has_value()) {
            PrefabFactory::SpawnParams resolved = params;
            if (sphereDesc->color.w >= 0.0F) resolved.color = sphereDesc->color;
            return PrefabFactory::CreateSphere(*m_engine, sphereDesc->radius, resolved);
        }
        if (cylinderDesc.has_value()) {
            PrefabFactory::SpawnParams resolved = params;
            if (cylinderDesc->color.w >= 0.0F) resolved.color = cylinderDesc->color;
            return PrefabFactory::CreateCylinder(*m_engine, cylinderDesc->radius, cylinderDesc->height, resolved);
        }
        if (coneDesc.has_value()) {
            PrefabFactory::SpawnParams resolved = params;
            if (coneDesc->color.w >= 0.0F) resolved.color = coneDesc->color;
            return PrefabFactory::CreateCone(*m_engine, coneDesc->radius, coneDesc->height, resolved);
        }
        return ComError::InvalidArgument;
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

    [[nodiscard]] Result<void> ConfigureActionMap(const ChainInNode* mapChain) noexcept override {
        if (mapChain == nullptr) return ComError::NullArgument;

        std::optional<ActionMapConfig> cfg;
        const Result<void>             chainRes =
            DispatchInChain<ActionMapConfig>(mapChain, [&](const ActionMapConfig& inCfg) -> Result<void> {
                cfg = inCfg;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!cfg.has_value()) return ComError::InvalidArgument;

        const std::string_view mapName(cfg->name);
        if (mapName.empty()) return ComError::InvalidArgument;

        InputActionMap& map = Router().CreateActionMap(mapName, cfg->priority);
        map.SetEnabled(cfg->enabled);
        map.SetSeatMask(cfg->seatMask);
        map.SetBlocksLowerPriority(cfg->blocksLowerPriority);
        map.SetConsumePhysicalInputs(cfg->consumePhysicalInputs);
        return {};
    }

    bool RemoveActionMap(StringView mapName) noexcept override {
        return Router().RemoveActionMap(std::string_view(mapName));
    }

    [[nodiscard]] bool HasActionMap(StringView mapName) const noexcept override {
        return static_cast<bool>(Router().FindActionMap(std::string_view(mapName)));
    }

    [[nodiscard]] bool IsActionMapEnabled(StringView mapName) const noexcept override {
        const auto found = Router().FindActionMap(std::string_view(mapName));
        return static_cast<bool>(found) && found->Enabled();
    }

    bool SetActionMapEnabled(StringView mapName, bool enabled) noexcept override {
        return Router().SetActionMapEnabled(std::string_view(mapName), enabled);
    }

    bool SetActionMapEnabledForSeat(StringView mapName, PlayerSeat seat, bool enabled) noexcept override {
        return Router().SetActionMapEnabledForSeat(std::string_view(mapName), seat, enabled);
    }

    [[nodiscard]] Result<ActionID> ConfigureAction(const ChainInNode* actionChain) noexcept override {
        if (actionChain == nullptr) return ComError::NullArgument;

        std::optional<ActionConfig> cfg;
        const Result<void>          chainRes =
            DispatchInChain<ActionConfig>(actionChain, [&](const ActionConfig& inCfg) -> Result<void> {
                cfg = inCfg;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes.error;
        if (!cfg.has_value()) return ComError::InvalidArgument;

        const std::string_view mapName(cfg->mapName);
        const std::string_view actionName(cfg->actionName);
        if (mapName.empty() || actionName.empty()) return ComError::InvalidArgument;
        if (cfg->bindingCount > 0U && cfg->bindings == nullptr) return ComError::InvalidArgument;

        auto mapOpt = Router().FindActionMap(mapName);
        if (!mapOpt) return ComError::InvalidArgument;

        InputAction& action = mapOpt->AddAction(actionName, cfg->kind);
        action.SetClampUnitCircle(cfg->clampUnitCircle);
        action.SetActuationThreshold(cfg->actuationThreshold);

        if (cfg->replaceBindings) {
            const std::span<const InputBinding> spanBindings {cfg->bindings, cfg->bindingCount};
            action.ReplaceBindings(spanBindings);
        } else {
            for (uint32_t i = 0; i < cfg->bindingCount; ++i) {
                action.AppendBinding(cfg->bindings[i]);
            }
        }
        return action.Id();
    }

    [[nodiscard]] Result<void> BindAction(StringView mapName, ActionID action,
                                          const ChainInNode* bindingChain) noexcept override {
        if (bindingChain == nullptr) return ComError::NullArgument;

        auto mapOpt = Router().FindActionMap(std::string_view(mapName));
        if (!mapOpt) return ComError::InvalidArgument;
        auto actOpt = mapOpt->FindAction(action);
        if (!actOpt) return ComError::InvalidArgument;

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
        if (opCount != 1U) return ComError::InvalidArgument;

        if (appendBinding.has_value()) {
            actOpt->AppendBinding(*appendBinding);
            return {};
        }
        if (rebindCfg.has_value()) {
            if (!actOpt->Rebind(rebindCfg->slotIndex, rebindCfg->binding)) return ComError::InvalidArgument;
            return {};
        }
        return ComError::InvalidArgument;
    }

    bool ClearBindings(StringView mapName, ActionID action) noexcept override {
        auto mapOpt = Router().FindActionMap(std::string_view(mapName));
        if (!mapOpt) return false;
        auto actOpt = mapOpt->FindAction(action);
        if (!actOpt) return false;
        actOpt->ClearBindings();
        return true;
    }

    void SnapshotDefaults() noexcept override {
        Router().SnapshotAllDefaults();
    }

    bool ResetActionMapToDefaults(StringView mapName) noexcept override {
        return Router().ResetActionMapToDefaults(std::string_view(mapName));
    }

    void ResetAllToDefaults() noexcept override {
        Router().ResetAllToDefaults();
    }

    [[nodiscard]] Result<void> ConfigureRouting(const ChainInNode* routingChain) noexcept override {
        if (routingChain == nullptr) return ComError::NullArgument;

        std::optional<InputRoutingConfig> cfg;
        const Result<void>                chainRes =
            DispatchInChain<InputRoutingConfig>(routingChain, [&](const InputRoutingConfig& inCfg) -> Result<void> {
                cfg = inCfg;
                return {};
            });
        if (!chainRes.HasValue()) return chainRes;
        if (!cfg.has_value()) return ComError::InvalidArgument;

        Router().SetRoutingPolicy(cfg->policy);
        for (PlayerSeat seat = 0; seat < kMaxPlayerSeats; ++seat) {
            const bool receives = (cfg->keyboardMouseSeatMask & (1U << seat)) != 0U;
            Router().SetKeyboardMouseSeat(seat, receives);
        }
        return {};
    }

    [[nodiscard]] Result<void> GetRouting(ChainOutNode* routingChain) const noexcept override {
        return DispatchOutChain<InputRoutingConfig>(routingChain, [&](InputRoutingConfig& outCfg) -> Result<void> {
            outCfg.policy = Router().RoutingPolicy();
            uint8_t mask  = 0U;
            for (PlayerSeat seat = 0; seat < kMaxPlayerSeats; ++seat) {
                if (Router().SeatReceivesKeyboardMouse(seat)) {
                    mask = static_cast<uint8_t>(mask | (1U << seat));
                }
            }
            outCfg.keyboardMouseSeatMask = mask;
            return {};
        });
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

// --- Factory ----------------------------------------------------------------

auto CreateEngine(const ChainInNode* configChain) noexcept -> Result<IEngine*> {
    EngineConfig config {};
    uint32_t     maxFramesPerSecond     = 0;
    bool         initializeDefaultScene = false;

    const Result<void> chainRes =
        DispatchInChain<EngineAppConfig, EngineWindowConfig, EngineRenderConfig, PhysicsConfig>(
            configChain, [&]<typename NodeT>(const NodeT& node) -> Result<void> {
                if constexpr (std::is_same_v<NodeT, EngineAppConfig>) {
                    const std::string_view nameView(node.appName);
                    if (!nameView.empty()) config.appName = node.appName;
                    config.logLevel        = node.logLevel;
                    maxFramesPerSecond     = node.maxFramesPerSecond;
                    initializeDefaultScene = node.initializeDefaultScene;
                } else if constexpr (std::is_same_v<NodeT, EngineWindowConfig>) {
                    config.width      = node.width;
                    config.height     = node.height;
                    config.fullscreen = node.fullscreen;
                    config.vsync      = node.vsync;
                    config.headless   = node.headless;
                } else if constexpr (std::is_same_v<NodeT, EngineRenderConfig>) {
                    config.validationMode            = node.validationMode;
                    config.enableMeshShading         = node.enableMeshShading;
                    config.initialPipelineBinaryData = std::span<const uint8_t>(node.initialPipelineBinary);
                } else if constexpr (std::is_same_v<NodeT, PhysicsConfig>) {
                    config.physics = node;
                }
                return {};
            });
    if (!chainRes.HasValue()) return chainRes.error;

    auto created = Engine::Create(config);
    if (!created) return created.error();

    (*created)->SetMaxFramesPerSecond(maxFramesPerSecond);
    if (initializeDefaultScene) {
        (*created)->InitializeDefaultScene();
    }

    auto* instance = new (std::nothrow) EngineCOM(std::move(*created));
    if (instance == nullptr) return ErrorCode(ComError::AllocationFailed);
    return static_cast<IEngine*>(instance);
}

} // namespace ZHLN::COM
