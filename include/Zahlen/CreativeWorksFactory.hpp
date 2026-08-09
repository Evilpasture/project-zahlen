// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <expected>
#include <span>
#include <string_view>

namespace ZHLN {
class Engine;
class RenderContext;
class CreativeWorksManager;
namespace ECS {
class Registry;
}
} // namespace ZHLN

namespace ZHLN::CreativeWorksFactory {
enum class TerrainType : uint8_t { Default = 0, Snow = 1, Desert = 2 };

// --- Low-Level GPU Geometry Builders ---
Mesh CreateTetrahedronMesh(RenderContext& ctx);
Mesh CreatePlaneMesh(RenderContext& ctx, float extent = 10.0f, const JPH::Vec4& color = {0.6f, 0.6f, 0.6f, 1.0f});
Mesh CreateBoxMesh(RenderContext& ctx, JPH::Vec3Arg halfExtents, const JPH::Vec4& color = {0.8f, 0.4f, 0.2f, 1.0f});
Mesh CreateTerrainMeshFromData(RenderContext& ctx, int sampleCount, float worldSize, const float* heights, const float* colorsRGBA);
Mesh CreateTerrainMesh(RenderContext& ctx, int sampleCount, float worldSize, float maxHeight, float* outHeights, TerrainType type = TerrainType::Default);

[[nodiscard]] std::expected<Material, Error>
    CreateBasicMaterial(RenderContext& ctx, bool doubleSided = false, bool alphaBlend = false, bool additiveBlend = false);

TextureHandle CreateFontAtlasTexture(RenderContext& ctx);
uint32_t      LoadTexture(RenderContext& ctx, CreativeWorksManager& assetMgr, std::string_view path, bool isSRGB = true);

struct SpawnParams {
    JPH::RVec3 position = JPH::RVec3::sZero();
    JPH::Quat  rotation = JPH::Quat::sIdentity();
    JPH::Vec3  scale    = JPH::Vec3::sReplicate(1.0f);

    bool     createPhysics   = false;
    bool     useBoxColliders = false;
    bool     isStaticPhysics = true;
    bool     isAnimated      = false;
    uint32_t physicsCategory = 0xFFFFFFFF;
    uint32_t physicsMask     = 0xFFFFFFFF;

    float     roughness = 0.5f;
    float     metallic  = 0.0f;
    JPH::Vec4 color     = {0.8f, 0.4f, 0.2f, -1.0f}; // alpha < 0 means fallback to material default

    Material materialOverride = {.pipeline = PipelineHandle::Invalid};
};

// --- High-Level Prefabrication Spawners (Entity Factory) ---

// Box Spawners
Entity CreateBox(RenderContext& ctx, ECS::Registry& reg, PhysicsContext* pc, JPH::Vec3Arg halfExtents, const SpawnParams& params = {});
Entity CreateBox(Engine& engine, JPH::Vec3Arg halfExtents, const SpawnParams& params = {});

// Plane Spawners
Entity CreatePlane(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext*    pc,
    float              extent = 10.0f,
    const JPH::Vec4&   color  = {0.6f, 0.6f, 0.6f, 1.0f},
    const SpawnParams& params = {}
);
Entity CreatePlane(Engine& engine, float extent = 10.0f, const JPH::Vec4& color = {0.6f, 0.6f, 0.6f, 1.0f}, const SpawnParams& params = {});

// Terrain Spawners
Entity CreateTerrain(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext*    pc,
    int                sampleCount,
    float              worldSize,
    float              maxHeight,
    TerrainType        type   = TerrainType::Default,
    const SpawnParams& params = {}
);
Entity
    CreateTerrain(Engine& engine, int sampleCount, float worldSize, float maxHeight, TerrainType type = TerrainType::Default, const SpawnParams& params = {});

Entity CreateTerrainFromData(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext*    pc,
    int                sampleCount,
    float              worldSize,
    const float*       heights,
    const float*       colorsRGBA,
    const SpawnParams& params = {}
);
Entity CreateTerrainFromData(Engine& engine, int sampleCount, float worldSize, const float* heights, const float* colorsRGBA, const SpawnParams& params = {});

// Model Prefabs
ModelPrefab* LoadModelPrefab(RenderContext& ctx, CreativeWorksManager& assetMgr, std::string_view path);

uint32_t InstantiatePrefab(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext&    pc,
    const ModelPrefab& prefab,
    const SpawnParams& params,
    Entity*            outBuffer = nullptr,
    uint32_t           maxCount  = 0
);

void SetupPlayerRagdoll(RenderContext& rc, PhysicsContext& pc, ECS::Registry& reg, Entity playerEntity, std::span<const Entity> visualParts);
void RebuildVulkanResources(RenderContext& ctx, CreativeWorksManager& cwMgr, ECS::Registry& reg);

ModelPrefab* LoadModelPrefab(Engine& engine, std::string_view path);
uint32_t     InstantiatePrefab(Engine& engine, const ModelPrefab& prefab, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0);
uint32_t     InstantiatePrefab(Engine& engine, std::string_view path, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0);
void         SetupPlayerRagdoll(Engine& engine, Entity playerEntity, std::span<const Entity> visualParts);

} // namespace ZHLN::CreativeWorksFactory
