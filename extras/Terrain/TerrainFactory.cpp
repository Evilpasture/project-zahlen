// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Terrain/TerrainFactory.cpp
#include "TerrainFactory.hpp"
#include "TerrainSystem.hpp"
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace ZHLN::Terrain {

namespace {

/// Meshlet partitioning for terrain meshes. Lifted from the renderer's
/// MeshBuilder helper (which stays core-private): the terrain generator needs
/// its meshes on the mesh pipeline exactly like before the move, and the only
/// ingredients are the public BuildMeshlets + RenderContext buffer API.
void AttachTerrainMeshlets(RenderContext& ctx, Mesh& mesh, std::span<const VertexPosition> positions, std::span<const uint32_t> indices) {
    if (positions.empty()) {
        return;
    }

    std::vector<uint32_t> sequential;
    if (indices.empty()) {
        sequential.resize(positions.size());
        for (uint32_t i = 0; i < sequential.size(); ++i) {
            sequential[i] = i;
        }
        indices = sequential;
    }

    // Triangle lists only: anything else has no meshlet representation.
    if (indices.size() < 3 || (indices.size() % 3) != 0) {
        return;
    }

    const auto built = BuildMeshlets(indices, positions);
    if (built.Empty()) {
        return;
    }

    // Storage, not vertex data: the task/mesh shaders reach these only through
    // their device address, they are never bound to the input assembler.
    mesh.meshletBuffer       = ctx.CreateStorageBuffer(built.meshlets.data(), built.meshlets.size() * sizeof(GPUMeshlet), sizeof(GPUMeshlet));
    mesh.meshletVertexBuffer = ctx.CreateStorageBuffer(built.vertices.data(), built.vertices.size() * sizeof(uint32_t), sizeof(uint32_t));
    mesh.meshletTriBuffer    = ctx.CreateStorageBuffer(built.triangles.data(), built.triangles.size(), sizeof(uint8_t));

    if (mesh.meshletBuffer == BufferHandle::Invalid || mesh.meshletVertexBuffer == BufferHandle::Invalid || mesh.meshletTriBuffer == BufferHandle::Invalid) {
        mesh.meshletBuffer       = BufferHandle::Invalid;
        mesh.meshletVertexBuffer = BufferHandle::Invalid;
        mesh.meshletTriBuffer    = BufferHandle::Invalid;
        mesh.meshletCount        = 0;
        return;
    }

    mesh.meshletCount = static_cast<uint32_t>(built.meshlets.size());
}

} // namespace

auto CreateTerrainMeshFromData(RenderContext& ctx, int sampleCount, float worldSize, const float* heights, const float* colorsRGBA) -> Mesh {
    float halfSize = worldSize / 2.0f;
    float dx       = worldSize / (sampleCount - 1);
    float dz       = worldSize / (sampleCount - 1);

    auto get_height = [&](int x, int z) -> float {
        x = std::clamp(x, 0, sampleCount - 1);
        z = std::clamp(z, 0, sampleCount - 1);
        return heights[x + z * sampleCount];
    };

    auto get_normal = [&](int x, int z) -> JPH::Vec3 {
        float hL = get_height(x - 1, z);
        float hR = get_height(x + 1, z);
        float hD = get_height(x, z - 1);
        float hU = get_height(x, z + 1);
        return JPH::Vec3(hL - hR, 2.0f * dx, hD - hU).Normalized();
    };

    std::vector<VertexPosition>   positions;
    std::vector<VertexAttributes> attributes;
    size_t                        quadCount = static_cast<size_t>(sampleCount - 1) * (sampleCount - 1);
    positions.reserve(quadCount * 6);
    attributes.reserve(quadCount * 6);

    for (int z = 0; z < sampleCount - 1; ++z) {
        for (int x = 0; x < sampleCount - 1; ++x) {
            int idxA = x + z * sampleCount;
            int idxB = (x + 1) + z * sampleCount;
            int idxC = x + (z + 1) * sampleCount;
            int idxD = (x + 1) + (z + 1) * sampleCount;

            float ax  = -halfSize + x * dx;
            float az  = -halfSize + z * dz;
            float bx  = -halfSize + (x + 1) * dx;
            float bz  = -halfSize + z * dz;
            float cx  = -halfSize + x * dx;
            float cz  = -halfSize + (z + 1) * dz;
            float dx_ = -halfSize + (x + 1) * dx;
            float dz_ = -halfSize + (z + 1) * dz;

            JPH::Vec3 nA = get_normal(x, z);
            JPH::Vec3 nB = get_normal(x + 1, z);
            JPH::Vec3 nC = get_normal(x, z + 1);
            JPH::Vec3 nD = get_normal(x + 1, z + 1);

            auto fetch_color = [&](int idx) -> PackedRGBA8 {
                if (colorsRGBA == nullptr) {
                    return Math::PackColor(0.8f, 0.8f, 0.8f, 1.0f);
                }
                const float* c = &colorsRGBA[static_cast<ptrdiff_t>(idx * 4)];
                return Math::PackColor(c[0], c[1], c[2], c[3]);
            };

            VertexPosition   posA {{ax, heights[idxA], az}};
            VertexAttributes attrA {
                .normal  = Math::PackNormal(nA.GetX(), nA.GetY(), nA.GetZ()),
                .tangent = Math::PackNormal(1, 0, 0, 1),
                .uv      = Math::PackUV(static_cast<float>(x) / sampleCount, static_cast<float>(z) / sampleCount),
                .color   = fetch_color(idxA)
            };

            VertexPosition   posB {{bx, heights[idxB], bz}};
            VertexAttributes attrB {
                .normal  = Math::PackNormal(nB.GetX(), nB.GetY(), nB.GetZ()),
                .tangent = Math::PackNormal(1, 0, 0, 1),
                .uv      = Math::PackUV(static_cast<float>(x + 1) / sampleCount, static_cast<float>(z) / sampleCount),
                .color   = fetch_color(idxB)
            };

            VertexPosition   posC {{cx, heights[idxC], cz}};
            VertexAttributes attrC {
                .normal  = Math::PackNormal(nC.GetX(), nC.GetY(), nC.GetZ()),
                .tangent = Math::PackNormal(1, 0, 0, 1),
                .uv      = Math::PackUV(static_cast<float>(x) / sampleCount, static_cast<float>(z + 1) / sampleCount),
                .color   = fetch_color(idxC)
            };

            VertexPosition   posD {{dx_, heights[idxD], dz_}};
            VertexAttributes attrD {
                .normal  = Math::PackNormal(nD.GetX(), nD.GetY(), nD.GetZ()),
                .tangent = Math::PackNormal(1, 0, 0, 1),
                .uv      = Math::PackUV(static_cast<float>(x + 1) / sampleCount, static_cast<float>(z + 1) / sampleCount),
                .color   = fetch_color(idxD)
            };

            positions.push_back(posA);
            attributes.push_back(attrA);
            positions.push_back(posC);
            attributes.push_back(attrC);
            positions.push_back(posB);
            attributes.push_back(attrB);
            positions.push_back(posB);
            attributes.push_back(attrB);
            positions.push_back(posC);
            attributes.push_back(attrC);
            positions.push_back(posD);
            attributes.push_back(attrD);
        }
    }

    BufferHandle posVbo  = ctx.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
    BufferHandle attrVbo = ctx.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));

    Mesh finalMesh {.posBuffer = posVbo, .attrBuffer = attrVbo, .vertexCount = static_cast<uint32_t>(positions.size())};
    AttachTerrainMeshlets(ctx, finalMesh, positions, {});
    if (auto res = ctx.BuildMeshBLAS(finalMesh); !res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::Log("WARNING: CreateTerrainMeshFromData: Failed to build mesh BLAS: {}", ZHLN::Error(res.error()).Message());
        }
    }
    return finalMesh;
}

auto CreateTerrainMesh(RenderContext& ctx, int sampleCount, float worldSize, float maxHeight, float* outHeights, TerrainType type) -> Mesh {
    auto noise = [&](float x, float y) -> float {
        float ix = std::floor(x);
        float iy = std::floor(y);
        float fx = x - ix;
        float fy = y - iy;
        float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
        float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
        return Math::Lerp(
            Math::Lerp(Math::Hash(ix, iy), Math::Hash(ix + 1.0f, iy), ux), Math::Lerp(Math::Hash(ix, iy + 1.0f), Math::Hash(ix + 1.0f, iy + 1.0f), ux), uy
        );
    };

    auto get_height = [&](float x, float z) -> float {
        if (type == TerrainType::Snow) {
            float tx = x * 0.012f;
            float tz = z * 0.012f;

            float warpX = noise(tx + 1.2f, tz + 3.4f);
            float warpZ = noise(tx + 5.6f, tz + 7.8f);

            float val = 0.0f;
            float amp = 0.5f;
            float wx  = tx + warpX * 0.7f;
            float wz  = tz + warpZ * 0.7f;

            for (int i = 0; i < 5; i++) {
                val += amp * noise(wx, wz);
                wx *= 2.05f;
                wz *= 2.05f;
                amp *= 0.48f;
            }

            float ridge = 1.0f - std::abs(noise(tx * 2.2f, tz * 2.2f) * 2.0f - 1.0f);
            ridge *= ridge;

            return (std::pow(val, 1.2f) * 0.75f + ridge * 0.25f) * maxHeight;
        }
        float val  = 0.0f;
        float amp  = 0.5f;
        float freq = 0.015f;
        float tx   = x * freq;
        float tz   = z * freq;
        for (int i = 0; i < 4; i++) {
            val += amp * noise(tx, tz);
            tx *= 2.1f;
            tz *= 2.15f;
            amp *= 0.45f;
        }
        return std::pow(val, 1.4f) * maxHeight;
    };

    float halfSize = worldSize / 2.0f;
    float dx       = worldSize / (sampleCount - 1);
    float dz       = worldSize / (sampleCount - 1);

    for (int z = 0; z < sampleCount; ++z) {
        for (int x = 0; x < sampleCount; ++x) {
            float posX                        = -halfSize + x * dx;
            float posZ                        = -halfSize + z * dz;
            outHeights[x + (z * sampleCount)] = get_height(posX, posZ);
        }
    }

    std::vector<VertexPosition>   positions;
    std::vector<VertexAttributes> attributes;
    positions.reserve(static_cast<size_t>((sampleCount - 1)) * (sampleCount - 1) * 6);
    attributes.reserve(static_cast<size_t>((sampleCount - 1)) * (sampleCount - 1) * 6);

    auto get_normal = [&](int x, int z) -> JPH::Vec3 {
        float     posX = -halfSize + x * dx;
        float     posZ = -halfSize + z * dz;
        float     hL   = (x > 0) ? outHeights[(x - 1) + z * sampleCount] : get_height(posX - dx, posZ);
        float     hR   = (x < sampleCount - 1) ? outHeights[(x + 1) + z * sampleCount] : get_height(posX + dx, posZ);
        float     hD   = (z > 0) ? outHeights[x + (z - 1) * sampleCount] : get_height(posX, posZ - dz);
        float     hU   = (z < sampleCount - 1) ? outHeights[x + (z + 1) * sampleCount] : get_height(posX, posZ + dz);
        JPH::Vec3 normal(hL - hR, 2.0f * dx, hD - hU);
        return normal.Normalized();
    };

    for (int z = 0; z < sampleCount - 1; ++z) {
        for (int x = 0; x < sampleCount - 1; ++x) {
            int idxA = x + z * sampleCount;
            int idxB = (x + 1) + z * sampleCount;
            int idxC = x + (z + 1) * sampleCount;
            int idxD = (x + 1) + (z + 1) * sampleCount;

            float ax  = -halfSize + x * dx;
            float az  = -halfSize + z * dz;
            float bx  = -halfSize + (x + 1) * dx;
            float bz  = -halfSize + z * dz;
            float cx  = -halfSize + x * dx;
            float cz  = -halfSize + (z + 1) * dz;
            float dx_ = -halfSize + (x + 1) * dx;
            float dz_ = -halfSize + (z + 1) * dz;

            float ay = outHeights[idxA];
            float by = outHeights[idxB];
            float cy = outHeights[idxC];
            float dy = outHeights[idxD];

            JPH::Vec3 nA = get_normal(x, z);
            JPH::Vec3 nB = get_normal(x + 1, z);
            JPH::Vec3 nC = get_normal(x, z + 1);
            JPH::Vec3 nD = get_normal(x + 1, z + 1);

            auto get_color = [&](float y, JPH::Vec3 normal) -> PackedRGBA8 {
                float slope = normal.GetY();
                float normY = y / maxHeight;

                if (type == TerrainType::Snow) {
                    if (slope < 0.60f) {
                        return Math::PackColor(0.22f, 0.25f, 0.30f, 1.0f);
                    }
                    if (slope < 0.72f) {
                        float t = (slope - 0.60f) / 0.12f;
                        float r = 0.22f + t * (0.88f - 0.22f);
                        float g = 0.25f + t * (0.93f - 0.25f);
                        float b = 0.30f + t * (0.98f - 0.30f);
                        return Math::PackColor(r, g, b, 1.0f);
                    }
                    if (normY > 0.70f) {
                        return Math::PackColor(0.97f, 0.98f, 1.00f, 1.0f);
                    }
                    if (normY < 0.15f) {
                        return Math::PackColor(0.75f, 0.88f, 0.96f, 1.0f);
                    }
                    float snowVar = 0.90f + 0.06f * std::sin(y * 0.4f);
                    return Math::PackColor(snowVar * 0.95f, snowVar * 0.98f, snowVar, 1.0f);
                }
                if (slope < 0.65f) {
                    return Math::PackColor(0.35f, 0.32f, 0.29f, 1.0f);
                }
                if (normY > 0.75f) {
                    return Math::PackColor(0.95f, 0.95f, 0.98f, 1.0f);
                }
                if (normY < 0.12f) {
                    return Math::PackColor(0.72f, 0.64f, 0.48f, 1.0f);
                }
                float greenVar = 0.4f + 0.12f * std::sin(y * 0.5f);
                return Math::PackColor(0.12f, greenVar, 0.08f, 1.0f);
            };

            VertexPosition   posA  = {{ax, ay, az}};
            VertexAttributes attrA = {
                .normal  = Math::PackNormal(nA.GetX(), nA.GetY(), nA.GetZ()),
                .tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
                .uv      = Math::PackUV(static_cast<float>(x) / sampleCount, static_cast<float>(z) / sampleCount),
                .color   = get_color(ay, nA)
            };

            VertexPosition   vB    = {{bx, by, bz}};
            VertexAttributes attrB = {
                .normal  = Math::PackNormal(nB.GetX(), nB.GetY(), nB.GetZ()),
                .tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
                .uv      = Math::PackUV(static_cast<float>(x + 1) / sampleCount, static_cast<float>(z) / sampleCount),
                .color   = get_color(by, nB)
            };

            VertexPosition   vC    = {{cx, cy, cz}};
            VertexAttributes attrC = {
                .normal  = Math::PackNormal(nC.GetX(), nC.GetY(), nC.GetZ()),
                .tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
                .uv      = Math::PackUV(static_cast<float>(x) / sampleCount, static_cast<float>(z + 1) / sampleCount),
                .color   = get_color(cy, nC)
            };

            VertexPosition   vD    = {{dx_, dy, dz_}};
            VertexAttributes attrD = {
                .normal  = Math::PackNormal(nD.GetX(), nD.GetY(), nD.GetZ()),
                .tangent = Math::PackNormal(1.0f, 0.0f, 0.0f, 1.0f),
                .uv      = Math::PackUV(static_cast<float>(x + 1) / sampleCount, static_cast<float>(z + 1) / sampleCount),
                .color   = get_color(dy, nD)
            };

            positions.push_back(posA);
            attributes.push_back(attrA);

            positions.push_back(vC);
            attributes.push_back(attrC);

            positions.push_back(vB);
            attributes.push_back(attrB);

            positions.push_back(vB);
            attributes.push_back(attrB);

            positions.push_back(vC);
            attributes.push_back(attrC);

            positions.push_back(vD);
            attributes.push_back(attrD);
        }
    }

    BufferHandle posVbo  = ctx.CreateVertexBuffer(positions.data(), positions.size() * sizeof(VertexPosition));
    BufferHandle attrVbo = ctx.CreateVertexBuffer(attributes.data(), attributes.size() * sizeof(VertexAttributes));

    auto finalMesh = Mesh {
        .posBuffer   = posVbo,
        .attrBuffer  = attrVbo,
        .skinBuffer  = BufferHandle::Invalid,
        .indexBuffer = BufferHandle::Invalid,
        .vertexCount = static_cast<uint32_t>(positions.size()),
        .indexCount  = 0
    };
    AttachTerrainMeshlets(ctx, finalMesh, positions, {});
    auto res = ctx.BuildMeshBLAS(finalMesh);
    if (!res) [[unlikely]] {
        if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
            ZHLN::Log("WARNING: CreateTerrainMesh: Failed to build mesh BLAS: {}", ZHLN::Error(res.error()).Message());
        }
    }
    return finalMesh;
}

auto CreateTerrainFromData(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext*    pc,
    int                sampleCount,
    float              worldSize,
    const float*       heights,
    const float*       colorsRGBA,
    const CreativeWorksFactory::SpawnParams& params
) -> Entity {
    Entity e = reg.Create();

    Mesh mesh = CreateTerrainMeshFromData(ctx, sampleCount, worldSize, heights, colorsRGBA);

    Material mat;
    if (params.materialOverride.pipeline != PipelineHandle::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res        = ctx.CreateBasicMaterial(false, false, false);
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
    TerrainHandle tHandle  = TerrainSystem::RegisterTerrainData(std::move(tData));
    JPH::Mat44    worldMat = Math::CreateTransform(JPH::Vec3(params.position), params.rotation, params.scale);

    reg.Add(e, Components::NameComponent {.name = String64("TerrainData_" + std::to_string(e.index))});
    reg.Add(e, Components::TransformComponent {.position = JPH::Vec3(params.position), .rotation = params.rotation, .scale = params.scale});
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = worldSize * 1.5f});
    reg.Add(e, Components::PBRComponent {.roughness = mat.roughnessFactor, .metallic = mat.metallicFactor});
    reg.Add(
        e, TerrainComponent {
               .sampleCount   = static_cast<uint32_t>(sampleCount),
               .worldSize     = worldSize,
               .maxHeight     = 35.0f,
               .roughness     = mat.roughnessFactor,
               .metallic      = mat.metallicFactor,
               .terrainHandle = tHandle
           }
    );

    if (params.createPhysics && pc != nullptr && heights != nullptr) {
        auto shape = Physics::CreateHeightFieldShape(heights, sampleCount, worldSize);
        // FIXED: Used pc->CreateRigidBody
        auto body = pc->CreateRigidBody(shape, params.position, params.rotation, JPH::EMotionType::Static, Layers::ID::NON_MOVING, 0, 0xFFFFFFFF, 0xFFFFFFFF, e);
        reg.Add(e, Components::PhysicsComponent {.physicsHandle = body, .isStatic = true});
    }

    return e;
}

auto CreateTerrainFromData(Engine& engine, int sampleCount, float worldSize, const float* heights, const float* colorsRGBA, const CreativeWorksFactory::SpawnParams& params)
    -> Entity {
    return CreateTerrainFromData(
        engine.GetRenderContext(), engine.GetRegistry(), &engine.GetPhysicsContext(), sampleCount, worldSize, heights, colorsRGBA, params
    );
}

auto CreateTerrain(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext*    pc,
    size_t             sampleCount,
    float              worldSize,
    float              maxHeight,
    TerrainType        type,
    const CreativeWorksFactory::SpawnParams& params
) -> Entity {
    Entity e = reg.Create();

    TerrainData tData {.sampleCount = static_cast<uint32_t>(sampleCount), .worldSize = worldSize, .maxHeight = maxHeight, .heights = {}, .colors = {}};
    tData.heights.resize(sampleCount * sampleCount);

    Mesh mesh = CreateTerrainMesh(ctx, sampleCount, worldSize, maxHeight, tData.heights.data(), type);

    Material mat;
    if (params.materialOverride.pipeline != PipelineHandle::Invalid) {
        mat = params.materialOverride;
    } else {
        auto mat_res        = ctx.CreateBasicMaterial(false, false, false);
        mat                 = mat_res.value_or(Material {});
        mat.roughnessFactor = 0.85f;
        mat.metallicFactor  = 0.05f;
    }

    AssetID    meshAsset = HashAssetID("prefab_terrain_mesh_" + std::to_string(e.index));
    MaterialID matAsset  = HashAssetID("prefab_terrain_mat_" + std::to_string(e.index));

    ctx.RegisterGPUMesh(meshAsset, mesh);
    ctx.RegisterGPUMaterial(matAsset, mat);

    TerrainHandle tHandle  = TerrainSystem::RegisterTerrainData(std::move(tData));
    JPH::Mat44    worldMat = Math::CreateTransform(JPH::Vec3(params.position), params.rotation, params.scale);

    reg.Add(e, Components::NameComponent {.name = String64("Terrain_" + std::to_string(e.index))});
    reg.Add(e, Components::TransformComponent {.position = JPH::Vec3(params.position), .rotation = params.rotation, .scale = params.scale});
    reg.Add(e, Components::WorldTransformComponent {.world = worldMat, .previous = worldMat});

    reg.Add(e, Components::MeshComponent {.meshAsset = meshAsset, .materialAsset = matAsset, .cullRadius = worldSize * 1.5f});
    reg.Add(e, Components::PBRComponent {.roughness = 0.85f, .metallic = 0.05f});
    reg.Add(
        e, TerrainComponent {
               .sampleCount   = static_cast<uint32_t>(sampleCount),
               .worldSize     = worldSize,
               .maxHeight     = maxHeight,
               .roughness     = 0.85f,
               .metallic      = 0.05f,
               .terrainHandle = tHandle
           }
    );

    if (params.createPhysics && pc != nullptr) {
        const TerrainData* stored = TerrainSystem::GetTerrainData(tHandle);
        if (stored != nullptr && !stored->heights.empty()) {
            auto shape = Physics::CreateHeightFieldShape(stored->heights.data(), sampleCount, worldSize);
            auto body  = pc->CreateRigidBody(shape, params.position, params.rotation, JPH::EMotionType::Static, Layers::ID::NON_MOVING, 0, 0xFFFFFFFF, 0xFFFFFFFF, e);
            reg.Add(e, Components::PhysicsComponent {.physicsHandle = body, .isStatic = true});
        }
    }

    return e;
}

auto CreateTerrain(Engine& engine, int sampleCount, float worldSize, float maxHeight, TerrainType type, const CreativeWorksFactory::SpawnParams& params) -> Entity {
    return CreateTerrain(engine.GetRenderContext(), engine.GetRegistry(), &engine.GetPhysicsContext(), sampleCount, worldSize, maxHeight, type, params);
}

} // namespace ZHLN::Terrain
