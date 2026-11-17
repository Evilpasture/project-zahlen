// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/ECS/Entity.hpp>
#include <Zahlen/Engine/SpawnParams.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Render/Types.hpp>
#include <cstdint>
#include <expected>
#include <string_view>

namespace ZHLN {
class Engine;
class ArticulationContext;
class AssetManager;
class GraphicsDevice;
class PhysicsContext;
class RenderContext;
struct FontAtlas;
struct ModelPrefab;

namespace ECS {
class Registry;
}
namespace PrefabFactory {
auto BuildTetrahedronMesh(GraphicsDevice gfx, bool buildBLAS) -> Mesh;
auto BuildBoxMesh(GraphicsDevice gfx, FVec3 halfExtents, FVec4 color, bool buildBLAS) -> Mesh;
auto BuildPlaneMesh(GraphicsDevice gfx, float extent, FVec4 color, bool buildBLAS) -> Mesh;
auto BuildSphereMesh(GraphicsDevice gfx, float radius, FVec4 color, bool buildBLAS) -> Mesh;
auto BuildCylinderMesh(GraphicsDevice gfx, float radius, float height, FVec4 color, bool buildBLAS) -> Mesh;
auto BuildConeMesh(GraphicsDevice gfx, float radius, float height, FVec4 color, bool buildBLAS) -> Mesh;

// The cache-parameterized forms: engine-internal, which is why they are not in
// include/. A host reaches the same work through Engine (fonts) or through the
// spawned-prefab entry points (models).
auto CreateFontAtlas(RenderContext& ctx, AssetManager& assetMgr, AssetID fontID) -> FontAtlas;
auto LoadFontAsset(AssetManager& assetMgr, std::string_view path) -> std::expected<AssetID, ErrorCode>;

auto CreateBoxConfigured(Engine& engine, FVec3 halfExtents, const SpawnParams& params, bool buildBLAS) -> Entity;

auto CreatePlaneConfigured(Engine& engine, float extent, FVec4 color, const SpawnParams& params, bool buildBLAS) -> Entity;

auto CreateSphereConfigured(Engine& engine, float radius, const SpawnParams& params, bool buildBLAS) -> Entity;

auto CreateCylinderConfigured(Engine& engine, float radius, float height, const SpawnParams& params, bool buildBLAS) -> Entity;

auto CreateConeConfigured(Engine& engine, float radius, float height, const SpawnParams& params, bool buildBLAS) -> Entity;

// Engine's device-recovery barrier and internal spawners.
void RebuildVulkanResources(GraphicsDevice gfx, ECS::Registry& registry);
auto CreateBoxInternal(
    GraphicsDevice                  gfx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    ZHLN::FVec3                     halfExtents,
    const SpawnParams&              params,
    bool                            buildBLAS = true
) -> Entity;
auto CreateSphereInternal(
    GraphicsDevice                  gfx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    float                           radius,
    const SpawnParams&              params,
    bool                            buildBLAS = true
) -> Entity;
auto CreateCylinderInternal(
    GraphicsDevice                  gfx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    float                           radius,
    float                           height,
    const SpawnParams&              params,
    bool                            buildBLAS = true
) -> Entity;
auto CreateConeInternal(
    GraphicsDevice                  gfx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    float                           radius,
    float                           height,
    const SpawnParams&              params,
    bool                            buildBLAS = true
) -> Entity;
auto CreatePlaneInternal(
    GraphicsDevice                  gfx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    float                           extent,
    ZHLN::FVec4                     color,
    const SpawnParams&              params,
    bool                            buildBLAS = true
) -> Entity;
auto InstantiatePrefabInternal(
    GraphicsDevice       gfx,
    ECS::Registry&       reg,
    PhysicsContext&      pc,
    ArticulationContext& art,
    const ModelPrefab&   prefab,
    const SpawnParams&   params,
    Entity*              outBuffer,
    uint32_t             maxCount,
    void (*onRoot)(void*, Entity),
    void* rootUserdata
) -> uint32_t;
} // namespace PrefabFactory
} // namespace ZHLN
