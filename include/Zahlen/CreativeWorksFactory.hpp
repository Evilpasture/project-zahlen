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
auto CreateTetrahedronMesh(RenderContext& ctx) -> Mesh;
auto CreatePlaneMesh(RenderContext& ctx, float extent = 10.0f, const JPH::Vec4& color = {0.6f, 0.6f, 0.6f, 1.0f}) -> Mesh;
auto CreateBoxMesh(RenderContext& ctx, JPH::Vec3Arg halfExtents, const JPH::Vec4& color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;
auto CreateTerrainMeshFromData(RenderContext& ctx, int sampleCount, float worldSize, const float* heights, const float* colorsRGBA) -> Mesh;
auto CreateTerrainMesh(RenderContext& ctx, int sampleCount, float worldSize, float maxHeight, float* outHeights, TerrainType type = TerrainType::Default)
    -> Mesh;

struct MaterialDesc {
    // Pipeline configuration
    bool doubleSided   = false;
    bool alphaBlend    = false;
    bool additiveBlend = false;

    // PBR factors (using std::array eliminates memcpy)
    uint32_t             alphaMode   = 0;
    float                alphaCutoff = 0.5f;
    float                metallic    = 1.0f;
    float                roughness   = 1.0f;
    std::array<float, 4> baseColor   = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> emissive    = {0.0f, 0.0f, 0.0f, 1.0f};

    // Texture bindings
    TextureHandle albedoMap   = TextureHandle::Invalid;
    TextureHandle normalMap   = TextureHandle::Invalid;
    TextureHandle pbrMap      = TextureHandle::Invalid;
    TextureHandle emissiveMap = TextureHandle::Invalid;
};

[[nodiscard]] auto
    CreateBasicMaterial(RenderContext& ctx, bool doubleSided = false, bool alphaBlend = false, bool additiveBlend = false) -> std::expected<Material, Error>;

[[nodiscard]] auto CreateMaterial(RenderContext& ctx, const MaterialDesc& desc) -> std::expected<Material, Error>;

auto CreateFontAtlasTexture(RenderContext& ctx) -> TextureHandle;
auto LoadTexture(RenderContext& ctx, CreativeWorksManager& assetMgr, std::string_view path, bool isSRGB = true) -> uint32_t;

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

    // Emissive materials always shade and bloom on their own (see
    // material_model.slang / bloom_threshold_cs) -- that is the glTF and
    // Babylon.js meaning of emission: a surface term, not a light source.
    //
    // Set this to spawn an additional cheap point light ("virtual point
    // light") per emissive part so the glow also bounces onto nearby
    // geometry. Off by default: it is an approximation, it costs a light
    // per emissive part, and no other glTF viewer does it.
    bool emissiveVirtualLights = false;

    float     roughness = 0.5f;
    float     metallic  = 0.0f;
    JPH::Vec4 color     = {0.8f, 0.4f, 0.2f, -1.0f}; // alpha < 0 means fallback to material default

    Material materialOverride = {.pipeline = PipelineHandle::Invalid};
};

// --- High-Level Prefabrication Spawners (Entity Factory) ---

// Box Spawners
auto CreateBox(RenderContext& ctx, ECS::Registry& reg, PhysicsContext* pc, JPH::Vec3Arg halfExtents, const SpawnParams& params = {}) -> Entity;
auto CreateBox(Engine& engine, JPH::Vec3Arg halfExtents, const SpawnParams& params = {}) -> Entity;

// Plane Spawners
auto CreatePlane(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext*    pc,
    float              extent = 10.0f,
    const JPH::Vec4&   color  = {0.6f, 0.6f, 0.6f, 1.0f},
    const SpawnParams& params = {}
) -> Entity;
auto CreatePlane(Engine& engine, float extent = 10.0f, const JPH::Vec4& color = {0.6f, 0.6f, 0.6f, 1.0f}, const SpawnParams& params = {}) -> Entity;

// Terrain Spawners
auto CreateTerrain(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext*    pc,
    size_t             sampleCount,
    float              worldSize,
    float              maxHeight,
    TerrainType        type   = TerrainType::Default,
    const SpawnParams& params = {}
) -> Entity;
auto CreateTerrain(Engine& engine, int sampleCount, float worldSize, float maxHeight, TerrainType type = TerrainType::Default, const SpawnParams& params = {})
    -> Entity;

auto CreateTerrainFromData(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext*    pc,
    int                sampleCount,
    float              worldSize,
    const float*       heights,
    const float*       colorsRGBA,
    const SpawnParams& params = {}
) -> Entity;
auto CreateTerrainFromData(Engine& engine, int sampleCount, float worldSize, const float* heights, const float* colorsRGBA, const SpawnParams& params = {})
    -> Entity;

// --- Model Prefab Loaders ---
auto LoadModelPrefab(RenderContext& ctx, CreativeWorksManager& assetMgr, std::string_view path) -> ModelPrefab*;
auto LoadModelPrefab(Engine& engine, std::string_view path) -> ModelPrefab*;
auto LoadModelPrefabFromMemory(RenderContext& ctx, CreativeWorksManager& assetMgr, std::span<const uint8_t> bytes, std::string_view virtualPath)
    -> ModelPrefab*;
auto LoadModelPrefabFromMemory(Engine& engine, std::span<const uint8_t> bytes, std::string_view virtualPath) -> ModelPrefab*;

// --- Prefab Spawners ---
// Low-level context overload (Required by Scripting.cpp)
auto InstantiatePrefab(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext&    pc,
    const ModelPrefab& prefab,
    const SpawnParams& params,
    Entity*            outBuffer = nullptr,
    uint32_t           maxCount  = 0
) -> uint32_t;

// Engine-level convenience overloads
auto InstantiatePrefab(Engine& engine, const ModelPrefab& prefab, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0) -> uint32_t;
auto InstantiatePrefab(Engine& engine, std::string_view path, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0) -> uint32_t;
auto InstantiatePrefabFromMemory(
    Engine&                  engine,
    std::span<const uint8_t> bytes,
    std::string_view         virtualPath,
    const SpawnParams&       params,
    Entity*                  outBuffer = nullptr,
    uint32_t                 maxCount  = 0
) -> uint32_t;

void SetupPlayerRagdoll(PhysicsContext& pc, ECS::Registry& reg, Entity playerEntity, std::span<const Entity> visualParts);
void SetupPlayerRagdoll(Engine& engine, Entity playerEntity, std::span<const Entity> visualParts);
void RebuildVulkanResources(RenderContext& ctx, CreativeWorksManager& cwMgr, ECS::Registry& reg);

} // namespace ZHLN::CreativeWorksFactory
