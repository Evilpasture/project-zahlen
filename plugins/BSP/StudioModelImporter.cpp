// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "StudioModelImporter.hpp"
#include "StudioModelLoader.hpp"
#include "VMTParser.hpp"
#include "VTFDecoder.hpp"
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Core/Hash.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/Render/Render.hpp>
// clang-format off
#include <Jolt/Jolt.h>
// clang-format on
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <filesystem>
#include <fstream>
#include <stb_image.h>

namespace ZHLN::BSP {
namespace {

auto LoadTextureFromBytes(RenderContext& ctx, std::string_view name, std::span<const std::byte> bytes, bool isSRGB = true) -> TextureHandle {
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "VTF\0", 4) == 0) {
        auto vtfExp = DecodeVTF(bytes);
        if (vtfExp.has_value()) {
            auto texRes = ctx.CreateTexture(name, vtfExp->rgba8, Extent2D {vtfExp->width, vtfExp->height}, isSRGB);
            if (texRes.has_value()) {
                return *texRes;
            }
        }
    }

    int            width    = 0;
    int            height   = 0;
    int            channels = 0;
    unsigned char* pixels   = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()), &width, &height, &channels, 4
    );
    if (pixels) {
        const std::span<const std::byte> rgba(reinterpret_cast<const std::byte*>(pixels), static_cast<size_t>(width) * height * 4);
        auto texRes = ctx.CreateTexture(name, rgba, Extent2D {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}, isSRGB);
        stbi_image_free(pixels);
        if (texRes.has_value()) {
            return *texRes;
        }
    }

    return TextureHandle::Invalid;
}

auto ResolveStudioMaterial(RenderContext& ctx, const SourceVFS& vfs, std::string_view materialName) -> MaterialDesc {
    MaterialDesc desc = MaterialDesc::Basic({1.0f, 1.0f, 1.0f, 1.0f}, 0.8f, 0.0f, false);

    const auto vmtPath = vfs.ResolveMaterial(materialName);
    if (!vmtPath.has_value()) {
        return desc;
    }

    const auto vmtBytes = vfs.ReadFile(*vmtPath);
    if (!vmtBytes.has_value() || vmtBytes->empty()) {
        return desc;
    }

    const std::string_view vmtText(reinterpret_cast<const char*>(vmtBytes->data()), vmtBytes->size());
    const auto             vmtMatExp = ParseVMT(vmtText);
    if (!vmtMatExp.has_value()) {
        return desc;
    }

    const auto& vmt   = *vmtMatExp;
    desc.baseColor   = vmt.baseColor;
    desc.doubleSided = vmt.noCull;
    if (vmt.isTranslucent) {
        desc.alphaBlend = true;
        desc.alphaMode  = 2;
    } else if (vmt.isAlphaTest) {
        desc.alphaMode   = 1;
        desc.alphaCutoff = vmt.alphaCutoff;
    }

    if (!vmt.baseTexture.empty()) {
        const auto texPath = vfs.ResolveTexture(vmt.baseTexture);
        if (texPath.has_value()) {
            const auto texBytes = vfs.ReadFile(*texPath);
            if (texBytes.has_value()) {
                desc.albedoMap = LoadTextureFromBytes(ctx, vmt.baseTexture, *texBytes, true);
            }
        }
    }

    if (!vmt.bumpMap.empty()) {
        const auto bumpPath = vfs.ResolveTexture(vmt.bumpMap);
        if (bumpPath.has_value()) {
            const auto bumpBytes = vfs.ReadFile(*bumpPath);
            if (bumpBytes.has_value()) {
                desc.normalMap = LoadTextureFromBytes(ctx, vmt.bumpMap, *bumpBytes, false);
            }
        }
    }

    return desc;
}

} // namespace

auto LoadStudioModelPrefab(
    RenderContext&       ctx,
    AssetManager&        assetMgr,
    const SourceVFS&     vfs,
    std::string_view     path,
    const ImportOptions& options
) -> ZHLN::Optional<ModelPrefab&> {
    const uint64_t hash = HashAssetPath(path);
    if (const auto cached = assetMgr.GetCachedPrefab(hash)) {
        return cached;
    }

    const auto resolvedMdl = vfs.ResolveModel(path);
    if (!resolvedMdl.has_value()) {
        LogWarning("StudioModel: failed to resolve MDL path {}", path);
        return std::nullopt;
    }

    const auto mdlBytes = vfs.ReadFile(*resolvedMdl);
    if (!mdlBytes.has_value()) {
        LogWarning("StudioModel: failed to read MDL {}", *resolvedMdl);
        return std::nullopt;
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
        return std::nullopt;
    }

    const auto vtxBytes = vfs.ReadFile(vtxPath);
    if (!vtxBytes.has_value()) {
        LogWarning("StudioModel: missing VTX file for {}", *resolvedMdl);
        return std::nullopt;
    }

    const auto                       phyBytes = vfs.ReadFile(phyPath);
    const std::span<const std::byte> phySpan  = phyBytes.has_value() ? std::span<const std::byte>(*phyBytes) : std::span<const std::byte>();

    return LoadStudioModelPrefabFromMemory(ctx, assetMgr, vfs, *mdlBytes, *vvdBytes, *vtxBytes, phySpan, path, options);
}

auto LoadStudioModelPrefabFromMemory(
    RenderContext&             ctx,
    AssetManager&              assetMgr,
    const SourceVFS&           vfs,
    std::span<const std::byte> mdlBytes,
    std::span<const std::byte> vvdBytes,
    std::span<const std::byte> vtxBytes,
    std::span<const std::byte> phyBytes,
    std::string_view           virtualPath,
    const ImportOptions&       options
) -> ZHLN::Optional<ModelPrefab&> {
    const uint64_t hash = HashAssetPath(virtualPath);
    if (const auto cached = assetMgr.GetCachedPrefab(hash)) {
        return cached;
    }

    const auto parsedExp = ParseStudioModel(mdlBytes, vvdBytes, vtxBytes, phyBytes, options);
    if (!parsedExp.has_value()) {
        LogWarning("StudioModel: failed to parse model {}: {}", virtualPath, parsedExp.error());
        return std::nullopt;
    }

    const auto& data            = *parsedExp;
    auto        prefab          = std::make_unique<ModelPrefab>();
    prefab->virtualPath         = String256(virtualPath);
    prefab->emissiveFactorScale = 1.0f;

    prefab->nodes.push_back(ModelNode {
        .name           = String64("root"),
        .parentIndex    = -1,
        .localTransform = JPH::Mat44::sIdentity(),
        .hasMesh        = false,
    });

    for (const auto& partData: data.parts) {
        if (partData.VertexCount() == 0 || partData.IndexCount() == 0) {
            continue;
        }

        const auto meshletResult = BuildMeshlets(std::span {partData.indices}, std::span {partData.positions});

        const BufferHandle posVbo     = ctx.CreateBuffer<BufferUsage::Vertex>(std::span {partData.positions});
        const BufferHandle frameVbo   = ctx.CreateBuffer<BufferUsage::Vertex>(std::span {partData.tangentFrames});
        const BufferHandle surfaceVbo = ctx.CreateBuffer<BufferUsage::Vertex>(std::span {partData.surfaces});
        const BufferHandle ibo        = ctx.CreateBuffer<BufferUsage::Index>(std::span {partData.indices});

        const bool hasMeshlets    = !meshletResult.Empty();
        const auto packedMeshlets = PackMeshlets(meshletResult.meshlets);

        BufferHandle meshletVbo = hasMeshlets ? ctx.CreateBuffer(BufferDesc {
                                                    .usage  = BufferUsage::Storage,
                                                    .data   = std::as_bytes(std::span {packedMeshlets}),
                                                    .stride = kMeshletPackedBytes,
                                                }) :
                                                BufferHandle::Invalid;

        BufferHandle meshletVertexVbo = hasMeshlets ? ctx.CreateBuffer<BufferUsage::Storage>(std::span {meshletResult.vertices}) : BufferHandle::Invalid;
        BufferHandle meshletTriVbo    = hasMeshlets ? ctx.CreateBuffer<BufferUsage::Storage>(std::span {meshletResult.triangles}) : BufferHandle::Invalid;
        const bool   completeMeshlets = hasMeshlets && meshletVbo != BufferHandle::Invalid && meshletVertexVbo != BufferHandle::Invalid &&
                                      meshletTriVbo != BufferHandle::Invalid;

        Mesh subMesh = {
            .posBuffer           = posVbo,
            .tangentFrameBuffer  = frameVbo,
            .surfaceBuffer       = surfaceVbo,
            .skinBuffer          = BufferHandle::Invalid,
            .indexBuffer         = ibo,
            .vertexCount         = partData.VertexCount(),
            .indexCount          = partData.IndexCount(),
            .meshletBuffer       = meshletVbo,
            .meshletVertexBuffer = meshletVertexVbo,
            .meshletTriBuffer    = meshletTriVbo,
            .meshletCount        = completeMeshlets ? static_cast<uint32_t>(meshletResult.meshlets.size()) : 0u,
        };

        if (auto res = ctx.BuildMeshBLAS(subMesh); !res) [[unlikely]] {
            if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
                LogWarning("StudioModel: failed to build BLAS: {}", res.error());
            }
        }

        const Material subMaterial = ctx.CreateMaterial(ResolveStudioMaterial(ctx, vfs, partData.materialName)).value_or(Material {});

        ModelPart modelPart;
        modelPart.name            = String64(partData.materialName.c_str());
        modelPart.mesh            = subMesh;
        modelPart.defaultMaterial = subMaterial;
        modelPart.nodeIndex       = 0;
        modelPart.localTransform  = JPH::Mat44::sIdentity();
        modelPart.localMin        = partData.boundsMin;
        modelPart.localMax        = partData.boundsMax;

        const std::string assetKey = std::string(virtualPath) + "#part_" + std::to_string(prefab->parts.size());
        modelPart.meshAsset        = HashAssetID(assetKey);
        modelPart.materialAsset    = HashAssetID(assetKey + "_mat");

        const JPH::Vec3 halfExtents(
            std::max(0.01f, (partData.boundsMax.x - partData.boundsMin.x) * 0.5f),
            std::max(0.01f, (partData.boundsMax.y - partData.boundsMin.y) * 0.5f),
            std::max(0.01f, (partData.boundsMax.z - partData.boundsMin.z) * 0.5f)
        );
        modelPart.boundingRadius = halfExtents.Length();

        if (options.buildColliders) {
            JPH::BoxShapeSettings boxSettings(halfExtents);
            auto                  shapeResult = boxSettings.Create();
            if (shapeResult.IsValid()) {
                modelPart.boxCollider  = shapeResult.Get();
                modelPart.meshCollider = modelPart.boxCollider;
            }
        }

        prefab->parts.push_back(std::move(modelPart));
    }

    assetMgr.CachePrefab(hash, std::move(prefab));
    return assetMgr.GetCachedPrefab(hash);
}

} // namespace ZHLN::BSP
