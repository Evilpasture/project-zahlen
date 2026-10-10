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

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/physics/Physics.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace ZHLN::BSP {
namespace {

auto IsToolTexture(std::string_view name) -> bool {
    return name.starts_with("tools/");
}

auto MaterialFor(const MaterialStreams& part) -> MaterialDesc {
    // TODO(BSP): VMT/VTF decoding for real materials. Until then every surface
    // comes up as a stable neutral PBR material keyed by its texdata name;
    // tool textures (nodraw, triggers, ...) render unlit gray so they read as
    // non-surfaces in the editor preview.
    if (IsToolTexture(part.materialName)) {
        return MaterialDesc::Unlit({0.4f, 0.4f, 0.4f, 1.0f});
    }
    return MaterialDesc::Basic({0.65f, 0.62f, 0.58f, 1.0f}, 0.8f, 0.0f, true);
}

auto ReadWholeFile(std::string_view path, std::vector<std::byte>& out) -> bool {
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return false;
    }
    file.seekg(0);
    out.resize(static_cast<size_t>(size));
    return static_cast<bool>(file.read(reinterpret_cast<char*>(out.data()), size));
}

auto BuildModelPrefab(RenderContext& ctx, AssetManager& assetMgr, const BSPMap& map, std::string_view virtualPath, const ImportOptions& options)
    -> ZHLN::Optional<ModelPrefab&> {
    const MarshalledMap marshalled = MarshallMap(map, options.marshalling);
    if (marshalled.parts.empty()) {
        LogWarning("BSP Importer: no marshallable surfaces in {}", virtualPath);
        return std::nullopt;
    }

    auto prefab = std::make_unique<ModelPrefab>();
    prefab->virtualPath        = String256(virtualPath);
    prefab->emissiveFactorScale = 1.0f; // Source emission is baked lighting, not glTF factors.

    // One identity root; parts hang off it directly (part.localTransform does
    // the placement), lights get their own translated nodes because ModelLight
    // resolves position through its node chain.
    prefab->nodes.push_back(ModelNode {.name = String64("bsp_root"), .parentIndex = -1, .localTransform = JPH::Mat44::sIdentity(), .hasMesh = false});

    for (const MaterialStreams& part: marshalled.parts) {
        const auto meshletResult = BuildMeshlets(std::span {part.indices}, std::span {part.positions});

        const BufferHandle posVbo     = ctx.CreateBuffer<BufferUsage::Vertex>(std::span {part.positions});
        const BufferHandle frameVbo   = ctx.CreateBuffer<BufferUsage::Vertex>(std::span {part.tangentFrames});
        const BufferHandle surfaceVbo = ctx.CreateBuffer<BufferUsage::Vertex>(std::span {part.surfaces});
        const BufferHandle ibo        = ctx.CreateBuffer<BufferUsage::Index>(std::span {part.indices});

        const bool hasMeshlets    = !meshletResult.Empty();
        const auto packedMeshlets = PackMeshlets(meshletResult.meshlets);

        BufferHandle meshletVbo = hasMeshlets ? ctx.CreateBuffer(
                                                    BufferDesc {
                                                        .usage = BufferUsage::Storage,
                                                        .data  = std::as_bytes(std::span {packedMeshlets}),
                                                        .stride = kMeshletPackedBytes,
                                                    }
                                                ) :
                                                BufferHandle::Invalid;
        BufferHandle meshletVertexVbo = hasMeshlets ? ctx.CreateBuffer<BufferUsage::Storage>(std::span {meshletResult.vertices}) : BufferHandle::Invalid;
        BufferHandle meshletTriVbo    = hasMeshlets ? ctx.CreateBuffer<BufferUsage::Storage>(std::span {meshletResult.triangles}) : BufferHandle::Invalid;
        const bool   completeMeshlets =
            hasMeshlets && meshletVbo != BufferHandle::Invalid && meshletVertexVbo != BufferHandle::Invalid && meshletTriVbo != BufferHandle::Invalid;

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

        const Material subMaterial = ctx.CreateMaterial(MaterialFor(part)).value_or(Material {});

        ModelPart modelPart;
        modelPart.name           = String64(part.materialName.c_str());
        modelPart.mesh           = subMesh;
        modelPart.defaultMaterial = subMaterial;
        modelPart.nodeIndex      = 0;
        modelPart.localTransform = JPH::Mat44::sIdentity();
        modelPart.localMin       = {part.boundsMin[0], part.boundsMin[1], part.boundsMin[2]};
        modelPart.localMax       = {part.boundsMax[0], part.boundsMax[1], part.boundsMax[2]};

        const float extentX = std::max(std::abs(part.boundsMin[0]), std::abs(part.boundsMax[0]));
        const float extentY = std::max(std::abs(part.boundsMin[1]), std::abs(part.boundsMax[1]));
        const float extentZ = std::max(std::abs(part.boundsMin[2]), std::abs(part.boundsMax[2]));
        modelPart.boundingRadius = std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ);

        if (options.buildColliders) {
            modelPart.meshCollider =
                Physics::CreateMeshShape(part.positions.data(), part.VertexCount(), part.indices.data(), part.IndexCount());

            const JPH::Vec3 localCenter(
                (part.boundsMin[0] + part.boundsMax[0]) * 0.5f,
                (part.boundsMin[1] + part.boundsMax[1]) * 0.5f,
                (part.boundsMin[2] + part.boundsMax[2]) * 0.5f
            );
            const JPH::Vec3 halfExtents(
                std::max((part.boundsMax[0] - part.boundsMin[0]) * 0.5f, 0.01f),
                std::max((part.boundsMax[1] - part.boundsMin[1]) * 0.5f, 0.01f),
                std::max((part.boundsMax[2] - part.boundsMin[2]) * 0.5f, 0.01f)
            );
            const JPH::ShapeRefC baseBox = new JPH::BoxShape(halfExtents);
            modelPart.boxCollider        = new JPH::RotatedTranslatedShape(localCenter, JPH::Quat::sIdentity(), baseBox);
        }

        prefab->parts.push_back(std::move(modelPart));
    }

    for (const BspPointLight& light: marshalled.lights) {
        ModelNode node;
        node.name           = String64(light.name.c_str());
        node.parentIndex    = 0;
        node.localTransform = JPH::Mat44::sTranslation(JPH::Vec3(light.position[0], light.position[1], light.position[2]));
        node.hasMesh        = false;
        prefab->nodes.push_back(node);

        ModelLight modelLight;
        modelLight.name      = String64(light.name.c_str());
        modelLight.nodeIndex = static_cast<int32_t>(prefab->nodes.size()) - 1;
        modelLight.color     = JPH::Vec3(light.color[0], light.color[1], light.color[2]);
        modelLight.intensity = light.intensity;
        switch (light.kind) {
            case BspPointLight::Kind::Spot:
                modelLight.type           = LightType::Spot;
                modelLight.innerConeAngle = light.innerConeRadians;
                modelLight.outerConeAngle = light.outerConeRadians;
                break;
            case BspPointLight::Kind::Directional:
                modelLight.type = LightType::Directional;
                break;
            case BspPointLight::Kind::Point:
            default:
                modelLight.type = LightType::Point;
                break;
        }
        prefab->lights.push_back(modelLight);
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

    std::vector<std::byte> bytes;
    if (!ReadWholeFile(path, bytes)) {
        LogWarning("BSP Importer: failed to read {}", path);
        return std::nullopt;
    }
    return LoadBSPPrefabFromMemory(ctx, assetMgr, bytes, path, options);
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

    const ParseResult parsed = ParseBsp(bytes);
    if (!parsed.ok) {
        LogWarning("BSP Importer: {}: {}", virtualPath, parsed.error);
        return std::nullopt;
    }
    return BuildModelPrefab(ctx, assetMgr, parsed.map, virtualPath, options);
}

} // namespace ZHLN::BSP
