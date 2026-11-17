// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Terrain/TerrainFactory.cpp
#include "TerrainFactory.hpp"
#include "TerrainGeometry.hpp"
#include "TerrainSystem.hpp"
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/Capabilities/Entities.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Capabilities/Simulation.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <Zahlen/Render/Meshlet.hpp>
#include <Zahlen/Render/Render.hpp>
#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace ZHLN::Terrain {

namespace {

template <typename Gfx>
auto UploadNativeGeometry(Gfx&& ctx, const Geometry& geometry) -> Mesh {
    const auto& positions     = geometry.positions;
    const auto& tangentFrames = geometry.frames;
    const auto& surfaces      = geometry.surfaces;
    BufferID posVbo     = ctx.template CreateBuffer<BufferUsage::Vertex>(std::span {positions});
    BufferID frameVbo   = ctx.template CreateBuffer<BufferUsage::Vertex>(std::span {tangentFrames});
    BufferID surfaceVbo = ctx.template CreateBuffer<BufferUsage::Vertex>(std::span {surfaces});

    const MeshletStreams ml = ctx.UploadMeshlets(BuildMeshlets(positions));
    auto finalMesh = Mesh {
        .posBuffer           = posVbo,
        .tangentFrameBuffer  = frameVbo,
        .surfaceBuffer       = surfaceVbo,
        .skinBuffer          = BufferID::Invalid,
        .indexBuffer         = BufferID::Invalid,
        .vertexCount         = static_cast<uint32_t>(positions.size()),
        .indexCount          = 0,
        .meshletBuffer       = ml.meshlets,
        .meshletVertexBuffer = ml.vertices,
        .meshletTriBuffer    = ml.triangles,
        .meshletCount        = ml.count,
    };
    auto res = ctx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::LogWarning("CreateTerrainMesh: Failed to build mesh BLAS: {}", res.error());
        }
    }
    return finalMesh;
}

template <typename Gfx>
auto BuildTerrainMeshFromDataImpl(Gfx&& ctx, int n, float size, const float* heights, const float* colors) -> Mesh {
    if (n < 2 || !heights)
        return {};
    const size_t count = size_t(n) * n;
    auto         geometry =
        FromHeights(static_cast<uint32_t>(n), size, {heights, count}, colors ? std::span<const float> {colors, count * 4} : std::span<const float> {});
    return geometry ? UploadNativeGeometry(ctx, *geometry) : Mesh {};
}
template <typename Gfx>
auto BuildTerrainMeshImpl(Gfx&& ctx, int n, float size, float height, float* out, TerrainType type) -> Mesh {
    auto geometry = Generate({.sampleCount = static_cast<uint32_t>(n), .worldSize = size, .maxHeight = height, .type = type});
    if (!geometry)
        return {};
    if (out)
        std::ranges::copy(geometry->heights, out);
    return UploadNativeGeometry(ctx, *geometry);
}

} // namespace

auto CreateTerrainMeshFromData(RenderContext& ctx, int sampleCount, float worldSize, const float* heights, const float* colorsRGBA) -> Mesh {
    return BuildTerrainMeshFromDataImpl(ctx, sampleCount, worldSize, heights, colorsRGBA);
}

auto CreateTerrainMesh(RenderContext& ctx, int sampleCount, float worldSize, float maxHeight, float* outHeights, TerrainType type) -> Mesh {
    return BuildTerrainMeshImpl(ctx, sampleCount, worldSize, maxHeight, outHeights, type);
}

auto CreateTerrainFromData(
    RenderContext&                    ctx,
    ECS::Registry&                    reg,
    PhysicsContext*                   pc,
    int                               sampleCount,
    float                             worldSize,
    const float*                      heights,
    const float*                      colorsRGBA,
    const PrefabFactory::SpawnParams& params
) -> Entity {
    Entity e = reg.Create();

    Mesh mesh = CreateTerrainMeshFromData(ctx, sampleCount, worldSize, heights, colorsRGBA);

    Material mat;
    if (params.materialOverride.pipeline != PipelineID::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res        = ctx.CreateMaterial(MaterialDesc::Basic());
        mat                 = mat_res.value_or(Material {});
        mat.roughnessFactor = 0.85f;
        mat.metallicFactor  = 0.05f;
    }

    AssetID    meshAsset = HashAssetID("prefab_terraindata_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_terraindata_mat_" + std::to_string(e.index));

    ctx.RegisterGPUMesh(meshAsset, mesh);
    ctx.RegisterGPUMaterial(matAsset, mat);

    TerrainData tData {.sampleCount = static_cast<uint32_t>(sampleCount), .worldSize = worldSize, .maxHeight = 35.0f, .heights = {}, .colors = {}};
    if (heights != nullptr) {
        tData.heights.assign(heights, heights + (static_cast<ptrdiff_t>(sampleCount * sampleCount)));
    }
    if (colorsRGBA != nullptr) {
        tData.colors.assign(colorsRGBA, colorsRGBA + (static_cast<ptrdiff_t>(sampleCount * sampleCount * 4)));
    }
    TerrainID tID  = TerrainSystem::RegisterTerrainData(std::move(tData));
    // ECS render transforms use float positions; physics below retains the
    // original double-precision spawn position.
    const FVec3 position {static_cast<float>(params.position.x), static_cast<float>(params.position.y), static_cast<float>(params.position.z)};
    const auto worldMat = ComposeTransform(position, params.rotation, params.scale);

    reg.Add(e, Components::NameComponent {.name = String64("TerrainData_" + std::to_string(e.index))});
    reg.Add(e, Components::TransformComponent {.position = position, .rotation = params.rotation, .scale = params.scale});
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = worldSize * 1.5f});
    reg.Add(e, Components::OwnedMeshComponent {.meshAsset = meshAsset, .mesh = mesh});
    reg.Add(e, Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor});
    reg.Add(
        e, TerrainComponent {
               .sampleCount   = static_cast<uint32_t>(sampleCount),
               .worldSize     = worldSize,
               .maxHeight     = 35.0f,
               .roughness     = mat.roughnessFactor,
               .metallic      = mat.metallicFactor,
               .terrainID = tID
           }
    );

    if (params.createPhysics && pc != nullptr && heights != nullptr) {
        auto shape = Physics::CreateHeightFieldShape(heights, sampleCount, worldSize);
        // FIXED: Used pc->CreateRigidBody
        auto body = pc->CreateRigidBody(shape, params.position, params.rotation.AsVec4(), JPH::EMotionType::Static, Layers::ID::NON_MOVING, 0, 0xFFFFFFFF, 0xFFFFFFFF);
        reg.Add(e, Components::PhysicsComponent {.physicsID = body, .isStatic = true});
    }

    return e;
}

auto CreateTerrainFromData(
    Engine&                           engine,
    int                               sampleCount,
    float                             worldSize,
    const float*                      heights,
    const float*                      colorsRGBA,
    const PrefabFactory::SpawnParams& params
) -> Entity {
    TerrainSystem::RegisterCleanup(engine);
    auto   entities = engine.Entities();
    auto   gfx      = engine.Graphics();
    Entity e        = entities.Create();

    Mesh mesh = BuildTerrainMeshFromDataImpl(gfx, sampleCount, worldSize, heights, colorsRGBA);

    Material mat;
    if (params.materialOverride.pipeline != PipelineID::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res        = gfx.CreateMaterial(MaterialDesc::Basic());
        mat                 = mat_res.value_or(Material {});
        mat.roughnessFactor = 0.85f;
        mat.metallicFactor  = 0.05f;
    }

    AssetID    meshAsset = HashAssetID("prefab_terraindata_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_terraindata_mat_" + std::to_string(e.index));

    gfx.RegisterMesh(meshAsset, mesh);
    gfx.RegisterMaterial(matAsset, mat);

    TerrainData tData {.sampleCount = static_cast<uint32_t>(sampleCount), .worldSize = worldSize, .maxHeight = 35.0f, .heights = {}, .colors = {}};
    if (heights != nullptr) {
        tData.heights.assign(heights, heights + (static_cast<ptrdiff_t>(sampleCount * sampleCount)));
    }
    if (colorsRGBA != nullptr) {
        tData.colors.assign(colorsRGBA, colorsRGBA + (static_cast<ptrdiff_t>(sampleCount * sampleCount * 4)));
    }
    TerrainID   tID      = TerrainSystem::RegisterTerrainData(std::move(tData));
    const FVec3 position {static_cast<float>(params.position.x), static_cast<float>(params.position.y), static_cast<float>(params.position.z)};
    const auto  worldMat = ComposeTransform(position, params.rotation, params.scale);

    entities.Attach(
        e,
        Components::NameComponent {.name = String64("TerrainData_" + std::to_string(e.index))},
        Components::TransformComponent {.position = position, .rotation = params.rotation, .scale = params.scale},
        Components::WorldTransformComponent {.world = worldMat, .previous = worldMat},
        Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = worldSize * 1.5f},
        Components::OwnedMeshComponent {.meshAsset = meshAsset, .mesh = mesh},
        Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor},
        TerrainComponent {
            .sampleCount = static_cast<uint32_t>(sampleCount),
            .worldSize   = worldSize,
            .maxHeight   = 35.0f,
            .roughness   = mat.roughnessFactor,
            .metallic    = mat.metallicFactor,
            .terrainID   = tID
        }
    );

    if (params.createPhysics && heights != nullptr && sampleCount >= 2) {
        const size_t count = static_cast<size_t>(sampleCount) * static_cast<size_t>(sampleCount);
        const auto   body  = engine.Physics().CreateRigidBody(
            Physics::Shape<Physics::HeightField> {std::span<const float> {heights, count}, static_cast<uint32_t>(sampleCount), worldSize},
            params.position, params.rotation.AsVec4(), Physics::MotionType::Static, Physics::ObjectLayer::NonMoving
        );
        entities.Attach(e, Components::PhysicsComponent {.physicsID = body, .isStatic = true});
    }

    return e;
}

auto CreateTerrain(
    RenderContext&                    ctx,
    ECS::Registry&                    reg,
    PhysicsContext*                   pc,
    size_t                            sampleCount,
    float                             worldSize,
    float                             maxHeight,
    TerrainType                       type,
    const PrefabFactory::SpawnParams& params
) -> Entity {
    Entity e = reg.Create();

    TerrainData tData {.sampleCount = static_cast<uint32_t>(sampleCount), .worldSize = worldSize, .maxHeight = maxHeight, .heights = {}, .colors = {}};
    tData.heights.resize(sampleCount * sampleCount);

    Mesh mesh = CreateTerrainMesh(ctx, sampleCount, worldSize, maxHeight, tData.heights.data(), type);

    Material mat;
    if (params.materialOverride.pipeline != PipelineID::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res        = ctx.CreateMaterial(MaterialDesc::Basic());
        mat                 = mat_res.value_or(Material {});
        mat.roughnessFactor = 0.85f;
        mat.metallicFactor  = 0.05f;
    }

    AssetID    meshAsset = HashAssetID("prefab_terrain_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_terrain_mat_" + std::to_string(e.index));

    ctx.RegisterGPUMesh(meshAsset, mesh);
    ctx.RegisterGPUMaterial(matAsset, mat);

    TerrainID tID  = TerrainSystem::RegisterTerrainData(std::move(tData));
    // ECS render transforms use float positions; physics below retains the
    // original double-precision spawn position.
    const FVec3 position {static_cast<float>(params.position.x), static_cast<float>(params.position.y), static_cast<float>(params.position.z)};
    const auto worldMat = ComposeTransform(position, params.rotation, params.scale);

    reg.Add(e, Components::NameComponent {.name = String64("Terrain_" + std::to_string(e.index))});
    reg.Add(e, Components::TransformComponent {.position = position, .rotation = params.rotation, .scale = params.scale});
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = worldSize * 1.5f});
    reg.Add(e, Components::OwnedMeshComponent {.meshAsset = meshAsset, .mesh = mesh});
    reg.Add(e, Components::PBRComponent {.roughness = 0.85f, .metallic = 0.05f});
    reg.Add(
        e, TerrainComponent {
               .sampleCount   = static_cast<uint32_t>(sampleCount),
               .worldSize     = worldSize,
               .maxHeight     = maxHeight,
               .roughness     = 0.85f,
               .metallic      = 0.05f,
               .terrainID = tID
           }
    );

    if (params.createPhysics && pc != nullptr) {
        const TerrainData* stored = TerrainSystem::GetTerrainData(tID);
        if (stored != nullptr && !stored->heights.empty()) {
            auto shape = Physics::CreateHeightFieldShape(stored->heights.data(), sampleCount, worldSize);
            auto body =
                pc->CreateRigidBody(shape, params.position, params.rotation.AsVec4(), JPH::EMotionType::Static, Layers::ID::NON_MOVING, 0, 0xFFFFFFFF, 0xFFFFFFFF);
            reg.Add(e, Components::PhysicsComponent {.physicsID = body, .isStatic = true});
        }
    }

    return e;
}

auto CreateTerrain(Engine& engine, int sampleCount, float worldSize, float maxHeight, TerrainType type, const PrefabFactory::SpawnParams& params) -> Entity {
    TerrainSystem::RegisterCleanup(engine);
    auto   entities = engine.Entities();
    auto   gfx      = engine.Graphics();
    Entity e        = entities.Create();

    TerrainData tData {.sampleCount = static_cast<uint32_t>(sampleCount), .worldSize = worldSize, .maxHeight = maxHeight, .heights = {}, .colors = {}};
    tData.heights.resize(static_cast<size_t>(sampleCount) * static_cast<size_t>(sampleCount));

    Mesh mesh = BuildTerrainMeshImpl(gfx, sampleCount, worldSize, maxHeight, tData.heights.data(), type);

    Material mat;
    if (params.materialOverride.pipeline != PipelineID::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res        = gfx.CreateMaterial(MaterialDesc::Basic());
        mat                 = mat_res.value_or(Material {});
        mat.roughnessFactor = 0.85f;
        mat.metallicFactor  = 0.05f;
    }

    AssetID    meshAsset = HashAssetID("prefab_terrain_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_terrain_mat_" + std::to_string(e.index));

    gfx.RegisterMesh(meshAsset, mesh);
    gfx.RegisterMaterial(matAsset, mat);

    TerrainID   tID      = TerrainSystem::RegisterTerrainData(std::move(tData));
    const FVec3 position {static_cast<float>(params.position.x), static_cast<float>(params.position.y), static_cast<float>(params.position.z)};
    const auto  worldMat = ComposeTransform(position, params.rotation, params.scale);

    entities.Attach(
        e,
        Components::NameComponent {.name = String64("Terrain_" + std::to_string(e.index))},
        Components::TransformComponent {.position = position, .rotation = params.rotation, .scale = params.scale},
        Components::WorldTransformComponent {.world = worldMat, .previous = worldMat},
        Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = worldSize * 1.5f},
        Components::OwnedMeshComponent {.meshAsset = meshAsset, .mesh = mesh},
        Components::PBRComponent {.roughness = 0.85f, .metallic = 0.05f},
        TerrainComponent {
            .sampleCount = static_cast<uint32_t>(sampleCount),
            .worldSize   = worldSize,
            .maxHeight   = maxHeight,
            .roughness   = 0.85f,
            .metallic    = 0.05f,
            .terrainID   = tID
        }
    );

    if (params.createPhysics) {
        const TerrainData* stored = TerrainSystem::GetTerrainData(tID);
        if (stored != nullptr && !stored->heights.empty()) {
            const auto body = engine.Physics().CreateRigidBody(
                Physics::Shape<Physics::HeightField> {std::span<const float> {stored->heights}, static_cast<uint32_t>(sampleCount), worldSize},
                params.position, params.rotation.AsVec4(), Physics::MotionType::Static, Physics::ObjectLayer::NonMoving
            );
            entities.Attach(e, Components::PhysicsComponent {.physicsID = body, .isStatic = true});
        }
    }

    return e;
}

} // namespace ZHLN::Terrain
