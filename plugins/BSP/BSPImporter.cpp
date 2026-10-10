// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/BSP/BSPImporter.cpp
//
// The engine-side half: file bytes -> native streams (BSPGeometry) -> GPU
// buffers, materials, colliders -> ModelPrefab -> prefab cache. The assembly
// mirrors GLTFImporter.cpp's GetOrCreateCompiledPrimitive/BuildModelPrefab
// split: one compiled part per material group, a single identity root node,
// one node per light so ModelLight's node chain carries its transform.

#include "BSPImporter.hpp"
#include "BSPRead.hpp"
#include "SourceVFS.hpp"
#include "StudioModelImporter.hpp"
#include "VMTParser.hpp"
#include "VTFDecoder.hpp"
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stb_image.h>
#include <string>
#include <vector>

namespace ZHLN::BSP {
namespace {

auto IsToolTexture(std::string_view name) -> bool {
    return name.starts_with("tools/");
}

auto LoadTextureFromBytes(RenderContext& ctx, std::string_view name, std::span<const std::byte> bytes, bool isSRGB = true) -> TextureHandle {
    // 1. Try VTF first
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "VTF\0", 4) == 0) {
        auto vtfExp = DecodeVTF(bytes);
        if (vtfExp.has_value()) {
            auto texRes = ctx.CreateTexture(name, vtfExp->rgba8, Extent2D {vtfExp->width, vtfExp->height}, isSRGB);
            if (texRes.has_value()) {
                return *texRes;
            }
        }
    }

    // 2. Try stb_image (PNG, JPG, TGA, etc.)
    int            width    = 0;
    int            height   = 0;
    int            channels = 0;
    unsigned char* pixels =
        stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (pixels) {
        const std::span<const std::byte> rgba(reinterpret_cast<const std::byte*>(pixels), static_cast<size_t>(width) * height * 4);
        auto                             texRes = ctx.CreateTexture(name, rgba, Extent2D {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}, isSRGB);
        stbi_image_free(pixels);
        if (texRes.has_value()) {
            return *texRes;
        }
    }

    return TextureHandle::Invalid;
}

auto ResolveMaterialDesc(RenderContext& ctx, const SourceVFS& vfs, const MaterialStreams& part, TextureHandle lightmapTexture) -> MaterialDesc {
    if (IsToolTexture(part.materialName)) {
        return MaterialDesc::Unlit({0.4f, 0.4f, 0.4f, 1.0f});
    }

    MaterialDesc desc = MaterialDesc::Basic({1.0f, 1.0f, 1.0f, 1.0f}, 0.8f, 0.0f, false);
    if (lightmapTexture != TextureHandle::Invalid) {
        desc.occlusionMap = lightmapTexture;
    }

    const auto vmtPath = vfs.ResolveMaterial(part.materialName);
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

    const auto& vmt  = *vmtMatExp;
    desc.baseColor   = vmt.baseColor;
    desc.doubleSided = vmt.noCull;
    if (vmt.isTranslucent) {
        desc.alphaBlend = true;
        desc.alphaMode  = 2;
    } else if (vmt.isAlphaTest) {
        desc.alphaMode   = 1;
        desc.alphaCutoff = vmt.alphaCutoff;
    }

    if (vmt.shader == "UnlitGeneric") {
        desc.unlit = true;
    }

    // Resolve base texture (albedo)
    if (!vmt.baseTexture.empty()) {
        const auto texPath = vfs.ResolveTexture(vmt.baseTexture);
        if (texPath.has_value()) {
            const auto texBytes = vfs.ReadFile(*texPath);
            if (texBytes.has_value()) {
                desc.albedoMap = LoadTextureFromBytes(ctx, vmt.baseTexture, *texBytes, true);
            }
        }
    }

    // Resolve normal/bump map
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

auto ReadWholeFile(std::string_view path) -> std::optional<std::vector<std::byte>> {
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return std::nullopt;
    }
    file.seekg(0);
    std::vector<std::byte> out(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
        return std::nullopt;
    }
    return out;
}

auto BuildModelPrefab(RenderContext& ctx, AssetManager& assetMgr, const BSPMap& map, std::string_view virtualPath, const ImportOptions& options)
    -> ZHLN::Optional<ModelPrefab&> {
    const ImportedMapData imported = ImportMapGeometry(map, options);
    if (imported.parts.empty()) {
        LogWarning("BSP Importer: no importable surfaces in {}", virtualPath);
        return std::nullopt;
    }

    auto prefab                 = std::make_unique<ModelPrefab>();
    prefab->virtualPath         = String256(virtualPath);
    prefab->emissiveFactorScale = 1.0f; // Source emission is baked lighting, not glTF factors.

    SourceVFS vfs;
    if (!options.assetRoot.empty()) {
        vfs.AddSearchPath(options.assetRoot);
    }
    const std::filesystem::path bspPath(virtualPath);
    if (bspPath.has_parent_path()) {
        vfs.AddSearchPath(bspPath.parent_path().string());
        if (bspPath.parent_path().has_parent_path()) {
            vfs.AddSearchPath(bspPath.parent_path().parent_path().string());
        }
    }

    TextureHandle lightmapTex = TextureHandle::Invalid;
    std::string   lmPath      = options.lightmapAtlasPath;
    if (lmPath.empty()) {
        const auto resolvedLm = vfs.ResolveLightmap(virtualPath);
        if (resolvedLm.has_value()) {
            lmPath = *resolvedLm;
        }
    }
    if (!lmPath.empty()) {
        const auto lmBytes = vfs.ReadFile(lmPath);
        if (lmBytes.has_value()) {
            lightmapTex = LoadTextureFromBytes(ctx, "bsp_lightmap", *lmBytes, false);
        }
    }

    // One identity root; parts hang off it directly (part.localTransform does
    // the placement), lights get their own translated nodes because ModelLight
    // resolves position through its node chain.
    prefab->nodes.push_back(ModelNode {.name = String64("bsp_root"), .parentIndex = -1, .localTransform = JPH::Mat44::sIdentity(), .hasMesh = false});

    for (const MaterialStreams& part: imported.parts) {
        const auto meshletResult = BuildMeshlets(std::span {part.indices}, std::span {part.positions});

        const BufferHandle posVbo     = ctx.CreateBuffer<BufferUsage::Vertex>(std::span {part.positions});
        const BufferHandle frameVbo   = ctx.CreateBuffer<BufferUsage::Vertex>(std::span {part.tangentFrames});
        const BufferHandle surfaceVbo = ctx.CreateBuffer<BufferUsage::Vertex>(std::span {part.surfaces});
        const BufferHandle ibo        = ctx.CreateBuffer<BufferUsage::Index>(std::span {part.indices});

        const bool hasMeshlets    = !meshletResult.Empty();
        const auto packedMeshlets = PackMeshlets(meshletResult.meshlets);

        BufferHandle meshletVbo       = hasMeshlets ? ctx.CreateBuffer(
                                                          BufferDesc {
                                                              .usage  = BufferUsage::Storage,
                                                              .data   = std::as_bytes(std::span {packedMeshlets}),
                                                              .stride = kMeshletPackedBytes,
                                                          }
                                                      ) :
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
            .vertexCount         = part.VertexCount(),
            .indexCount          = part.IndexCount(),
            .meshletBuffer       = meshletVbo,
            .meshletVertexBuffer = meshletVertexVbo,
            .meshletTriBuffer    = meshletTriVbo,
            .meshletCount        = completeMeshlets ? static_cast<uint32_t>(meshletResult.meshlets.size()) : 0u,
        };

        if (auto res = ctx.BuildMeshBLAS(subMesh); !res) [[unlikely]] {
            if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
                LogWarning("BSP Importer: failed to build mesh BLAS: {}", res.error());
            }
        }

        const Material subMaterial = ctx.CreateMaterial(ResolveMaterialDesc(ctx, vfs, part, lightmapTex)).value_or(Material {});

        ModelPart modelPart;
        modelPart.name            = String64(part.materialName.c_str());
        modelPart.mesh            = subMesh;
        modelPart.defaultMaterial = subMaterial;
        modelPart.nodeIndex       = 0;
        modelPart.localTransform  = JPH::Mat44::sIdentity();
        modelPart.localMin        = part.boundsMin;
        modelPart.localMax        = part.boundsMax;

        const float extentX      = std::max(std::abs(part.boundsMin.x), std::abs(part.boundsMax.x));
        const float extentY      = std::max(std::abs(part.boundsMin.y), std::abs(part.boundsMax.y));
        const float extentZ      = std::max(std::abs(part.boundsMin.z), std::abs(part.boundsMax.z));
        modelPart.boundingRadius = std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ);

        if (options.buildColliders) {
            modelPart.meshCollider = Physics::CreateMeshShape(part.positions.data(), part.VertexCount(), part.indices.data(), part.IndexCount());

            const JPH::Vec3 localCenter(
                (part.boundsMin.x + part.boundsMax.x) * 0.5f, (part.boundsMin.y + part.boundsMax.y) * 0.5f, (part.boundsMin.z + part.boundsMax.z) * 0.5f
            );
            const JPH::Vec3 halfExtents(
                std::max((part.boundsMax.x - part.boundsMin.x) * 0.5f, 0.01f), std::max((part.boundsMax.y - part.boundsMin.y) * 0.5f, 0.01f),
                std::max((part.boundsMax.z - part.boundsMin.z) * 0.5f, 0.01f)
            );
            const JPH::ShapeRefC baseBox = new JPH::BoxShape(halfExtents);
            modelPart.boxCollider        = new JPH::RotatedTranslatedShape(localCenter, JPH::Quat::sIdentity(), baseBox);
        }

        prefab->parts.push_back(std::move(modelPart));
    }

    for (const BspPointLight& light: imported.lights) {
        ModelNode node;
        node.name           = String64(light.name.c_str());
        node.parentIndex    = 0;
        node.localTransform = JPH::Mat44::sTranslation(light.position);
        node.hasMesh        = false;
        prefab->nodes.push_back(node);

        ModelLight modelLight;
        modelLight.name      = String64(light.name.c_str());
        modelLight.nodeIndex = static_cast<int32_t>(prefab->nodes.size()) - 1;
        modelLight.color     = light.color;
        modelLight.intensity = light.intensity;
        modelLight.type      = light.type;
        if (light.type == LightType::Spot) {
            modelLight.innerConeAngle = light.innerConeRadians;
            modelLight.outerConeAngle = light.outerConeRadians;
        }
        prefab->lights.push_back(modelLight);
    }

    // Preload static prop models referenced by the map into AssetManager
    for (const auto& sp: map.staticProps) {
        if (!sp.modelName.empty()) {
            return LoadStudioModelPrefab(ctx, assetMgr, vfs, sp.modelName, options);
        }
    }

    assetMgr.CachePrefab(HashAssetPath(virtualPath), std::move(prefab));
    return assetMgr.GetCachedPrefab(HashAssetPath(virtualPath));
}

} // namespace

auto LoadBSPPrefab(RenderContext& ctx, AssetManager& assetMgr, std::string_view path, const ImportOptions& options) -> ZHLN::Optional<ModelPrefab&> {
    const uint64_t hash   = HashAssetPath(path);
    const auto     cached = assetMgr.GetCachedPrefab(hash);
    if (cached) {
        return cached;
    }

    const auto bytes = ReadWholeFile(path);
    if (!bytes) {
        LogWarning("BSP Importer: failed to read {}", path);
        return std::nullopt;
    }
    return LoadBSPPrefabFromMemory(ctx, assetMgr, *bytes, path, options);
}

auto LoadBSPPrefabFromMemory(
    RenderContext&             ctx,
    AssetManager&              assetMgr,
    std::span<const std::byte> bytes,
    std::string_view           virtualPath,
    const ImportOptions&       options
) -> ZHLN::Optional<ModelPrefab&> {
    const uint64_t hash   = HashAssetPath(virtualPath);
    const auto     cached = assetMgr.GetCachedPrefab(hash);
    if (cached) {
        return cached;
    }

    const auto parsed = ParseBsp(bytes);
    if (!parsed) {
        LogWarning("BSP Importer: {}: {}", virtualPath, parsed.error());
        return std::nullopt;
    }
    return BuildModelPrefab(ctx, assetMgr, *parsed, virtualPath, options);
}

} // namespace ZHLN::BSP
