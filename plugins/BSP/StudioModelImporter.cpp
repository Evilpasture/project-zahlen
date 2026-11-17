// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BSPImporter.hpp"
#include <Zahlen/Math/Vec3.hpp>
#include "StudioModelImporter.hpp"
#include "StudioModelLoader.hpp"
#include "StudioMaterial.hpp"
#include "VMTParser.hpp"
#include "VTFDecoder.hpp"
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render/Meshlet.hpp>
#include <Zahlen/Render/Render.hpp>
#include <filesystem>
#include <fstream>

namespace ZHLN::BSP {
namespace {

auto LookupPrefab(AssetLibrary& assets, uint64_t hash) noexcept -> ZHLN::Optional<ModelPrefab&> {
    return assets.CachedPrefab(hash);
}

template <typename Assets>
auto CreateFallbackModelPrefab(
    Assets&              assetMgr,
    std::string_view     virtualPath,
    const ImportOptions& options
) -> ZHLN::Optional<ModelPrefab&> {
    const uint64_t hash = HashAssetPath(virtualPath);
    if (const auto cached = LookupPrefab(assetMgr, hash)) {
        return cached;
    }

    auto prefab                 = std::make_unique<ModelPrefab>();
    prefab->virtualPath         = String256(virtualPath);
    prefab->emissiveFactorScale = 1.0f;

    prefab->nodes.push_back(ModelNode {
        .name           = String64("root"),
        .parentIndex    = -1,
        .localTransform = Mat4x4::Identity(),
        .hasMesh        = false,
    });

    ModelPart fallbackPart;
    fallbackPart.name           = String64("placeholder");
    fallbackPart.nodeIndex      = 0;
    fallbackPart.localTransform = Mat4x4::Identity();
    fallbackPart.localMin       = {-0.25f, -0.25f, -0.25f};
    fallbackPart.localMax       = {0.25f, 0.25f, 0.25f};
    fallbackPart.boundingRadius = 0.433f;

    const std::string assetKey = std::string(virtualPath) + "#fallback";
    fallbackPart.meshAsset     = HashAssetID(assetKey);
    fallbackPart.materialAsset = HashAssetID(assetKey + "_mat");

    if (options.buildColliders) {
        const auto box = MakePrefabBoxCollider({0.25f, 0.25f, 0.25f});
        fallbackPart.boxCollider = box;
        fallbackPart.meshCollider = box;
    }

    prefab->parts.push_back(std::move(fallbackPart));
    assetMgr.CachePrefab(hash, std::move(prefab));
    return LookupPrefab(assetMgr, hash);
}

template <typename Gfx, typename Assets>
auto LoadStudioModelPrefabFromMemoryImpl(
    Gfx&                       ctx,
    Assets&                    assetMgr,
    const SourceVFS&           vfs,
    std::span<const std::byte> mdlBytes,
    std::span<const std::byte> vvdBytes,
    std::span<const std::byte> vtxBytes,
    std::span<const std::byte> phyBytes,
    std::string_view           virtualPath,
    const ImportOptions&       options
) -> ZHLN::Optional<ModelPrefab&> {
    const uint64_t hash = HashAssetPath(virtualPath);
    if (const auto cached = LookupPrefab(assetMgr, hash)) {
        return cached;
    }

    const auto parsedExp = ParseStudioModel(mdlBytes, vvdBytes, vtxBytes, phyBytes, options);
    if (!parsedExp.has_value()) {
        LogWarning("StudioModel: failed to parse model {}: {}", virtualPath, parsedExp.error());
        return CreateFallbackModelPrefab(assetMgr, virtualPath, options);
    }

    const auto& data            = *parsedExp;
    auto        prefab          = std::make_unique<ModelPrefab>();
    prefab->virtualPath         = String256(virtualPath);
    prefab->emissiveFactorScale = 1.0f;

    prefab->nodes.push_back(ModelNode {
        .name           = String64("root"),
        .parentIndex    = -1,
        .localTransform = Mat4x4::Identity(),
        .hasMesh        = false,
    });

    for (const auto& partData: data.parts) {
        if (partData.VertexCount() == 0 || partData.IndexCount() == 0) {
            continue;
        }

        const BufferID       posVbo     = ctx.template CreateBuffer<BufferUsage::Vertex>(std::span {partData.positions});
        const BufferID       frameVbo   = ctx.template CreateBuffer<BufferUsage::Vertex>(std::span {partData.tangentFrames});
        const BufferID       surfaceVbo = ctx.template CreateBuffer<BufferUsage::Vertex>(std::span {partData.surfaces});
        const BufferID       ibo        = ctx.template CreateBuffer<BufferUsage::Index>(std::span {partData.indices});
        const MeshletStreams ml         = ctx.UploadMeshlets(BuildMeshlets(std::span {partData.indices}, std::span {partData.positions}));

        Mesh subMesh = {
            .posBuffer           = posVbo,
            .tangentFrameBuffer  = frameVbo,
            .surfaceBuffer       = surfaceVbo,
            .skinBuffer          = BufferID::Invalid,
            .indexBuffer         = ibo,
            .vertexCount         = partData.VertexCount(),
            .indexCount          = partData.IndexCount(),
            .meshletBuffer       = ml.meshlets,
            .meshletVertexBuffer = ml.vertices,
            .meshletTriBuffer    = ml.triangles,
            .meshletCount        = ml.count,
        };

        if (auto res = ctx.BuildMeshBLAS(subMesh); !res) [[unlikely]] {
            if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
                LogWarning("StudioModel: failed to build BLAS: {}", res.error());
            }
        }

        const Material subMaterial = ctx.CreateMaterial(StudioMaterial::Resolve([&](std::string_view name, std::span<const std::byte> bytes, Extent2D extent, bool srgb) {
            return ctx.CreateTexture(name, bytes, extent, srgb).value_or(TextureID::Invalid);
        }, vfs, partData.materialName)).value_or(Material {});

        ModelPart modelPart;
        modelPart.name            = String64(partData.materialName.c_str());
        modelPart.mesh            = subMesh;
        modelPart.defaultMaterial = subMaterial;
        modelPart.nodeIndex       = 0;
        modelPart.localTransform  = Mat4x4::Identity();
        modelPart.localMin        = {partData.boundsMin.x, partData.boundsMin.y, partData.boundsMin.z};
        modelPart.localMax        = {partData.boundsMax.x, partData.boundsMax.y, partData.boundsMax.z};

        const std::string assetKey = std::string(virtualPath) + "#part_" + std::to_string(prefab->parts.size());
        modelPart.meshAsset        = HashAssetID(assetKey);
        modelPart.materialAsset    = HashAssetID(assetKey + "_mat");

        const FVec3 halfExtents{
            std::max(0.01f, (partData.boundsMax.x - partData.boundsMin.x) * 0.5f),
            std::max(0.01f, (partData.boundsMax.y - partData.boundsMin.y) * 0.5f),
            std::max(0.01f, (partData.boundsMax.z - partData.boundsMin.z) * 0.5f)
        };
        modelPart.boundingRadius = halfExtents.Length();

        if (options.buildColliders) {
            const auto shape = MakePrefabBoxCollider(halfExtents);
            modelPart.boxCollider = shape;
            modelPart.meshCollider = shape;
        }

        prefab->parts.push_back(std::move(modelPart));
    }

    assetMgr.CachePrefab(hash, std::move(prefab));
    return LookupPrefab(assetMgr, hash);
}

template <typename Gfx, typename Assets>
auto LoadStudioModelPrefabImpl(
    Gfx&                 ctx,
    Assets&              assetMgr,
    const SourceVFS&     vfs,
    std::string_view     path,
    const ImportOptions& options
) -> ZHLN::Optional<ModelPrefab&> {
    const uint64_t hash = HashAssetPath(path);
    if (const auto cached = LookupPrefab(assetMgr, hash)) {
        return cached;
    }

    const auto resolvedMdl = vfs.ResolveModel(path);
    if (!resolvedMdl.has_value()) {
        LogWarning("StudioModel: failed to resolve MDL path {}", path);
        return CreateFallbackModelPrefab(assetMgr, path, options);
    }

    const auto mdlBytes = vfs.ReadFile(*resolvedMdl);
    if (!mdlBytes.has_value()) {
        LogWarning("StudioModel: failed to read MDL {}", *resolvedMdl);
        return CreateFallbackModelPrefab(assetMgr, path, options);
    }

    std::string basePath = *resolvedMdl;
    if (basePath.ends_with(".mdl")) {
        basePath.resize(basePath.size() - 4);
    }

    const std::string vvdPath = basePath + ".vvd";
    std::string       vtxPath = basePath + ".dx90.vtx";
    if (!vfs.Exists(vtxPath)) {
        vtxPath = basePath + ".vtx";
    }
    const std::string phyPath = basePath + ".phy";

    const auto vvdBytes = vfs.ReadFile(vvdPath);
    if (!vvdBytes.has_value()) {
        LogWarning("StudioModel: missing VVD file for {}", *resolvedMdl);
        return CreateFallbackModelPrefab(assetMgr, path, options);
    }

    const auto vtxBytes = vfs.ReadFile(vtxPath);
    if (!vtxBytes.has_value()) {
        LogWarning("StudioModel: missing VTX file for {}", *resolvedMdl);
        return CreateFallbackModelPrefab(assetMgr, path, options);
    }

    const auto                       phyBytes = vfs.ReadFile(phyPath);
    const std::span<const std::byte> phySpan  = phyBytes.has_value() ? std::span<const std::byte>(*phyBytes) : std::span<const std::byte>();

    return LoadStudioModelPrefabFromMemoryImpl(ctx, assetMgr, vfs, *mdlBytes, *vvdBytes, *vtxBytes, phySpan, path, options);
}

} // namespace

auto LoadStudioModelPrefab(
    Engine&              engine,
    const SourceVFS&     vfs,
    std::string_view     path,
    const ImportOptions& options
) -> ZHLN::Optional<ModelPrefab&> {
    auto gfx    = engine.Graphics();
    auto assets = engine.Assets();
    return LoadStudioModelPrefabImpl(gfx, assets, vfs, path, options);
}

auto LoadStudioModelPrefabFromMemory(
    Engine&                    engine,
    const SourceVFS&           vfs,
    std::span<const std::byte> mdlBytes,
    std::span<const std::byte> vvdBytes,
    std::span<const std::byte> vtxBytes,
    std::span<const std::byte> phyBytes,
    std::string_view           virtualPath,
    const ImportOptions&       options
) -> ZHLN::Optional<ModelPrefab&> {
    auto gfx    = engine.Graphics();
    auto assets = engine.Assets();
    return LoadStudioModelPrefabFromMemoryImpl(gfx, assets, vfs, mdlBytes, vvdBytes, vtxBytes, phyBytes, virtualPath, options);
}

} // namespace ZHLN::BSP
