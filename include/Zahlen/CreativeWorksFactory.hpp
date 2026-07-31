// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "physics/Physics.hpp"
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/Types.hpp>
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

Mesh CreateTetrahedron(RenderContext& ctx);
Mesh CreatePlane(RenderContext& ctx, float extent = 10.0f, const JPH::Vec4& color = {0.6f, 0.6f, 0.6f, 1.0f});
Mesh CreateBox(RenderContext& ctx, JPH::Vec3Arg halfExtents, const JPH::Vec4& color = {0.8f, 0.4f, 0.2f, 1.0f});

[[nodiscard]] std::expected<Material, Error> CreateBasicMaterial(RenderContext& ctx, bool doubleSided = false, bool alphaBlend = false);

Mesh CreateTerrainFromData(RenderContext& ctx, int sampleCount, float worldSize, const float* heights, const float* colorsRGBA);
Mesh CreateTerrain(RenderContext& ctx, int sampleCount, float worldSize, float maxHeight, float* outHeights, TerrainType type = TerrainType::Default);

uint32_t CreateFontAtlasTexture(RenderContext& ctx);

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

    Material materialOverride = {.pipeline = PipelineHandle::Invalid};
};

// --- Low-Level RenderContext Overloads ---
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

// --- High-Level Engine Overloads (Zero Renderer Headers Required) ---
ModelPrefab* LoadModelPrefab(Engine& engine, std::string_view path);

uint32_t InstantiatePrefab(Engine& engine, const ModelPrefab& prefab, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0);

uint32_t InstantiatePrefab(Engine& engine, std::string_view path, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0);

void SetupPlayerRagdoll(Engine& engine, Entity playerEntity, std::span<const Entity> visualParts);

} // namespace ZHLN::CreativeWorksFactory
