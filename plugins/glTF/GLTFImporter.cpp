// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
// Legacy native entry points retained for the unported inspector/samples.
// Parsing, materials, skins, animation and morph data are decoded once by the
// same host Document implementation used by the capability-only archive.
#include "GLTFImporter.hpp"
#include "GLTFDocument.hpp"
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render/Meshlet.hpp>
#include <Zahlen/Render/Render.hpp>
#include <algorithm>
#include <unordered_map>
namespace ZHLN::GLTF {
namespace {
struct NativePrimitive {
    Mesh     mesh;
    Material material;
    uint32_t morphOffset = 0;
};
auto UploadPrimitive(GraphicsDevice& ctx, const PrimitiveDocument& primitive, const std::vector<TextureID>& textures) -> NativePrimitive {
    const BufferID posVbo     = ctx.template CreateBuffer<BufferUsage::Vertex>(std::span {primitive.positions});
    const BufferID frameVbo   = ctx.template CreateBuffer<BufferUsage::Vertex>(std::span {primitive.tangentFrames});
    const BufferID surfaceVbo = ctx.template CreateBuffer<BufferUsage::Vertex>(std::span {primitive.surfaces});

    const BufferID skinVbo = !primitive.skins.empty() ? ctx.template CreateBuffer<BufferUsage::Vertex>(std::span {primitive.skins}) : BufferID::Invalid;

    const BufferID ibo =
        (static_cast<uint32_t>(primitive.indices.size()) > 0) ?
            ctx.template CreateBuffer<BufferUsage::Index>(std::span {primitive.indices}.first(static_cast<uint32_t>(primitive.indices.size()))) :
            BufferID::Invalid;

    // VK_EXT_mesh_shader streams. They are plain storage buffers read through
    // BDA by the task/mesh shaders; the vertex/index buffers above stay live
    // for BLAS builds and for the legacy vertex pipeline.
    const MeshletStreams ml = ctx.UploadMeshlets(primitive.meshlets);

    Mesh subMesh = {
        .posBuffer           = posVbo,
        .tangentFrameBuffer  = frameVbo,
        .surfaceBuffer       = surfaceVbo,
        .skinBuffer          = skinVbo,
        .indexBuffer         = ibo,
        .vertexCount         = static_cast<uint32_t>(primitive.positions.size()),
        .indexCount          = static_cast<uint32_t>(primitive.indices.size()),
        .meshletBuffer       = ml.meshlets,
        .meshletVertexBuffer = ml.vertices,
        .meshletTriBuffer    = ml.triangles,
        .meshletCount        = ml.count,
    };

    if (auto res = ctx.BuildMeshBLAS(subMesh); !res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("GLTF Importer: Failed to build mesh BLAS: {}", res.error());
        }
    }

    const uint32_t finalMorphOffset = (primitive.activeMorphCount > 0) ? ctx.AllocateMorphDeltas(std::span {primitive.morphDeltas}) : 0;

    auto desc = primitive.material;
    for (const auto& binding: primitive.textures)
        if (binding.field && binding.imageIndex < textures.size())
            desc.*binding.field = textures[binding.imageIndex];
    return {subMesh, ctx.CreateMaterial(desc).value_or(Material {}), finalMorphOffset};
}
auto UploadNative(GraphicsDevice& ctx, const Document& document) -> std::vector<NativePrimitive> {
    std::vector<TextureID> textures(document.textures.size(), TextureID::Invalid);
    for (size_t i = 0; i < textures.size(); ++i) {
        const auto& image = document.textures[i];
        if (image.rgba.empty())
            continue;
        const auto name = document.virtualPath + "#tex_" + std::to_string(i);
        textures[i]     = ctx.CreateTexture(name, std::span {image.rgba}, image.extent, image.isSRGB).value_or(TextureID::Invalid);
    }
    std::vector<NativePrimitive> primitives;
    for (const auto& primitive: document.primitives)
        primitives.push_back(UploadPrimitive(ctx, primitive, textures));
    return primitives;
}
auto NeedsRefresh(const ModelPrefab& prefab) -> bool {
    return std::any_of(prefab.parts.begin(), prefab.parts.end(), [](const auto& part) { return part.mesh.posBuffer == BufferID::Invalid; });
}
auto Matches(const ModelPrefab& prefab, ImportOptions options) -> bool {
    return prefab.emissiveFactorScale == options.emissiveFactorScale && prefab.maxTextureDimension == options.maxTextureDimension;
}
void RegisterNative(GraphicsDevice& gfx, const ModelPrefab& prefab) {
    for (const auto& part: prefab.parts)
        if (part.mesh.posBuffer != BufferID::Invalid) {
            gfx.RegisterMesh(part.meshAsset, part.mesh);
            if (part.defaultMaterial.pipeline != PipelineID::Invalid)
                gfx.RegisterMaterial(part.materialAsset, part.defaultMaterial);
        }
}
void RefreshNative(GraphicsDevice& gfx, ModelPrefab& prefab, const Document& document) {
    if (prefab.parts.size() != document.parts.size())
        return;
    for (const auto& part: prefab.parts)
        gfx.UnregisterMesh(part.meshAsset);
    for (auto& part: prefab.parts) {
        gfx.DestroyMesh(part.mesh);
        part.mesh = {};
    }
    const auto uploaded = UploadNative(gfx, document);
    for (size_t i = 0; i < prefab.parts.size(); ++i) {
        const auto primitive             = document.parts[i].primitive;
        prefab.parts[i].mesh             = uploaded[primitive].mesh;
        prefab.parts[i].defaultMaterial  = uploaded[primitive].material;
        prefab.parts[i].morphOffset      = uploaded[primitive].morphOffset;
        prefab.parts[i].activeMorphCount = document.primitives[primitive].activeMorphCount;
    }
    RegisterNative(gfx, prefab);
}
auto CacheNative(GraphicsDevice& gfx, AssetLibrary& assets, const Document& document) -> Optional<ModelPrefab&> {
    const auto uploaded         = UploadNative(gfx, document);
    auto       prefab           = std::make_unique<ModelPrefab>();
    prefab->virtualPath         = String256(document.virtualPath);
    prefab->emissiveFactorScale = document.options.emissiveFactorScale;
    prefab->maxTextureDimension = document.options.maxTextureDimension;
    for (const auto& node: document.nodes)
        prefab->nodes.push_back({node.name, node.parentIndex, node.localTransform, node.hasMesh});
    prefab->skeletons  = document.skeletons;
    prefab->animations = document.animations;
    for (const auto& light: document.lights)
        prefab->lights.push_back(
            {light.name, light.nodeIndex, light.type, light.color, light.intensity, light.range, light.innerConeAngle, light.outerConeAngle}
        );
    for (size_t i = 0; i < document.parts.size(); ++i) {
        const auto& source    = document.parts[i];
        const auto& primitive = document.primitives[source.primitive];
        const auto& gpu       = uploaded[source.primitive];
        const auto  key       = document.virtualPath + "#part" + std::to_string(i);
        ModelPart   part {
            .name                = source.name,
            .meshAsset           = HashAssetID(key),
            .materialAsset       = HashAssetID(key + "_mat"),
            .mesh                = gpu.mesh,
            .defaultMaterial     = gpu.material,
            .isSkinned           = source.isSkinned,
            .nodeIndex           = source.nodeIndex,
            .skeletonIndex       = source.skeletonIndex,
            .morphOffset         = gpu.morphOffset,
            .activeMorphCount    = primitive.activeMorphCount,
            .defaultMorphWeights = source.defaultMorphWeights,
            .boundingRadius      = primitive.boundingRadius,
            .localMin            = primitive.localMin,
            .localMax            = primitive.localMax
        };
        if (!primitive.positions.empty()) {
            part.boxCollider = MakePrefabBoxCollider((primitive.localMax - primitive.localMin) * 0.5f, (primitive.localMax + primitive.localMin) * 0.5f);
            if (primitive.wantsMeshCollider)
                part.meshCollider = MakePrefabMeshCollider(primitive.positions, primitive.indices);
        }
        part.csgModifiers = source.csgModifiers;
        prefab->parts.push_back(std::move(part));
    }
    assets.UseGraphicsDevice(gfx);
    ModelPrefab& value = *prefab;
    assets.CachePrefab(HashAssetPath(document.virtualPath), std::move(prefab));
    return value;
}
} // namespace

auto LoadGLBPrefab(Engine& engine, std::string_view path, ImportOptions options) -> Optional<ModelPrefab&> {
    auto assets = engine.Assets();
    auto gfx    = engine.Graphics();
    auto cached = assets.CachedPrefab(HashAssetPath(path));
    if (cached) {
        if (!Matches(*cached, options))
            return std::nullopt;
        if (!NeedsRefresh(*cached))
            return cached;
    }
    auto document = Read(path, options);
    if (!document)
        return std::nullopt;
    if (cached) {
        assets.UseGraphicsDevice(gfx);
        RefreshNative(gfx, *cached, *document);
        return cached;
    }
    return CacheNative(gfx, assets, *document);
}
auto LoadGLBPrefabFromMemory(Engine& engine, std::span<const uint8_t> bytes, std::string_view path, std::string_view bytesPath, ImportOptions options)
    -> Optional<ModelPrefab&> {
    auto assets = engine.Assets();
    auto gfx    = engine.Graphics();
    auto cached = assets.CachedPrefab(HashAssetPath(path));
    if (cached) {
        if (!Matches(*cached, options))
            return std::nullopt;
        if (!NeedsRefresh(*cached))
            return cached;
    }
    auto document = Decode(bytes, path, bytesPath, options);
    if (!document)
        return std::nullopt;
    if (cached) {
        assets.UseGraphicsDevice(gfx);
        RefreshNative(gfx, *cached, *document);
        return cached;
    }
    return CacheNative(gfx, assets, *document);
}
void RebuildPrefabGPUResources(GraphicsDevice gfx, ModelPrefab* prefab) {
    if (!prefab)
        return;
    auto document = Read(std::string_view(prefab->virtualPath), {prefab->emissiveFactorScale, prefab->maxTextureDimension});
    if (document)
        RefreshNative(gfx, *prefab, *document);
}
auto InstantiatePrefabFromMemory(
    Engine&                           engine,
    std::span<const uint8_t>          bytes,
    std::string_view                  path,
    const PrefabFactory::SpawnParams& params,
    Entity*                           out,
    uint32_t                          count
) -> uint32_t {
    auto loaded = LoadGLBPrefabFromMemory(engine, bytes, path);
    return loaded ? PrefabFactory::InstantiatePrefab(engine, *loaded, params, out, count) : 0;
}
void RebuildCachedPrefabs(Engine& engine) {
    auto assets = engine.Assets();
    auto gfx    = engine.Graphics();
    assets.UseGraphicsDevice(gfx);
    std::vector<ModelPrefab*> prefabs(assets.CachedPrefabs(nullptr, 0));
    (void) assets.CachedPrefabs(prefabs.data(), static_cast<uint32_t>(prefabs.size()));
    for (auto* prefab: prefabs)
        RebuildPrefabGPUResources(gfx, prefab);
}
void InstallDeviceLostHandler(Engine& engine) {
    engine.AddDeviceLostCallback([](Engine& value) { RebuildCachedPrefabs(value); });
}
} // namespace ZHLN::GLTF
