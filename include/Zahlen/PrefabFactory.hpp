// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/PrefabFactory.hpp
//
// High-level asset factory / entity spawner. Creates Jolt colliders, ECS
// entities, GPU buffers from cached prefabs. This is the high-level spawning
// layer that belongs to src/engine, not to filesystem/VFS.

#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Entity.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/gui/FontLoader.hpp>
#include <span>
#include <string_view>

namespace ZHLN {
class Engine;
class RenderContext;
class AssetManager;
class ArticulationSystem;
namespace ECS {
class Registry;
}
} // namespace ZHLN

namespace ZHLN::PrefabFactory {

// --- Low-Level GPU Geometry Builders
auto CreateTetrahedronMesh(RenderContext& ctx) -> Mesh;
auto CreatePlaneMesh(RenderContext& ctx, float extent = 10.0f, const JPH::Vec4& color = {0.6f, 0.6f, 0.6f, 1.0f}) -> Mesh;
auto CreateBoxMesh(RenderContext& ctx, JPH::Vec3Arg halfExtents, const JPH::Vec4& color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;
auto CreateSphereMesh(RenderContext& ctx, float radius, const JPH::Vec4& color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;
auto CreateCylinderMesh(RenderContext& ctx, float radius, float height, const JPH::Vec4& color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;
auto CreateConeMesh(RenderContext& ctx, float radius, float height, const JPH::Vec4& color = {0.8f, 0.4f, 0.2f, 1.0f}) -> Mesh;

auto CreateFontAtlasTexture(RenderContext& ctx, ECS::Registry& registry) -> TextureHandle;
auto CreateFontAtlasTexture(RenderContext& ctx, ECS::Registry& registry, AssetManager& assetMgr, AssetID fontID) -> TextureHandle;
auto CreateFontAtlasTexture(RenderContext& ctx, ECS::Registry& registry, AssetManager& assetMgr, std::string_view path) -> TextureHandle;
auto CreateFontAtlasTexture(RenderContext& ctx, ECS::Registry& registry, AssetManager* assetMgr, AssetID fontID) -> TextureHandle;

auto PrimeDefaultBakedFont(AssetManager& assetMgr) -> bool;

auto LoadFontAsset(AssetManager& assetMgr, std::string_view path) -> std::expected<AssetID, ErrorCode>;

auto GetFontAsset(AssetManager& assetMgr, AssetID id) -> GUI::BakedFontAsset*;
auto GetFontAsset(AssetManager& assetMgr, std::string_view path) -> GUI::BakedFontAsset*;

auto LoadTexture(RenderContext& ctx, AssetManager& assetMgr, std::string_view path, bool isSRGB = true) -> uint32_t;

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

    bool emissiveVirtualLights = false;

    float     roughness = 0.5f;
    float     metallic  = 0.0f;
    JPH::Vec4 color     = {0.8f, 0.4f, 0.2f, -1.0f};

    Material materialOverride = {.pipeline = PipelineHandle::Invalid};
};

auto CreateBox(RenderContext& ctx, ECS::Registry& reg, PhysicsContext* pc, JPH::Vec3Arg halfExtents, const SpawnParams& params = {}) -> Entity;
auto CreateBox(Engine& engine, JPH::Vec3Arg halfExtents, const SpawnParams& params = {}) -> Entity;

auto CreatePlane(
    RenderContext&     ctx,
    ECS::Registry&     reg,
    PhysicsContext*    pc,
    float              extent = 10.0f,
    const JPH::Vec4&   color  = {0.6f, 0.6f, 0.6f, 1.0f},
    const SpawnParams& params = {}
) -> Entity;
auto CreatePlane(Engine& engine, float extent = 10.0f, const JPH::Vec4& color = {0.6f, 0.6f, 0.6f, 1.0f}, const SpawnParams& params = {}) -> Entity;

auto CreateSphere(RenderContext& ctx, ECS::Registry& reg, PhysicsContext* pc, float radius, const SpawnParams& params = {}) -> Entity;
auto CreateSphere(Engine& engine, float radius, const SpawnParams& params = {}) -> Entity;
auto CreateCylinder(RenderContext& ctx, ECS::Registry& reg, PhysicsContext* pc, float radius, float height, const SpawnParams& params = {}) -> Entity;
auto CreateCylinder(Engine& engine, float radius, float height, const SpawnParams& params = {}) -> Entity;
auto CreateCone(RenderContext& ctx, ECS::Registry& reg, PhysicsContext* pc, float radius, float height, const SpawnParams& params = {}) -> Entity;
auto CreateCone(Engine& engine, float radius, float height, const SpawnParams& params = {}) -> Entity;

auto LoadModelPrefab(RenderContext& ctx, AssetManager& assetMgr, std::string_view path) -> ModelPrefab*;
auto LoadModelPrefab(Engine& engine, std::string_view path) -> ModelPrefab*;

// `art` owns the world's joint-slot allocator and inverse bind matrices: the
// low-level form has no Engine, so the world's ArticulationSystem is named
// explicitly, the same way pc is.
auto InstantiatePrefab(
    RenderContext&      ctx,
    ECS::Registry&      reg,
    PhysicsContext&     pc,
    ArticulationSystem& art,
    const ModelPrefab&  prefab,
    const SpawnParams&  params,
    Entity*             outBuffer = nullptr,
    uint32_t            maxCount  = 0
) -> uint32_t;

auto InstantiatePrefab(Engine& engine, const ModelPrefab& prefab, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0) -> uint32_t;
auto InstantiatePrefab(Engine& engine, std::string_view path, const SpawnParams& params, Entity* outBuffer = nullptr, uint32_t maxCount = 0) -> uint32_t;

void RebuildVulkanResources(RenderContext& ctx, ECS::Registry& reg);

} // namespace ZHLN::PrefabFactory
