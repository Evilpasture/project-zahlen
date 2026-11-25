// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
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
#include <optional>
#include <string_view>

namespace ZHLN {
class Engine;
class AssetManager;
class PhysicsContext;
class RenderContext;
struct ModelPrefab;

namespace ECS {
class Registry;
}
namespace PrefabFactory {

// Procedural mesh builders. A caller that passes buildBLAS is asking for a
// ray-traceable mesh, so a BLAS failure they can act on fails the call and the
// buffers are released; a device that cannot build one at all still gets its
// mesh. Nothing here logs: a mesh is data, and the caller owns the decision.
[[nodiscard]] auto BuildTetrahedronMesh(RenderContext& gfx, bool buildBLAS) -> std::expected<Mesh, ErrorCode>;
[[nodiscard]] auto BuildBoxMesh(RenderContext& gfx, FVec3 halfExtents, FVec4 color, bool buildBLAS) -> std::expected<Mesh, ErrorCode>;
[[nodiscard]] auto BuildPlaneMesh(RenderContext& gfx, float extent, FVec4 color, bool buildBLAS) -> std::expected<Mesh, ErrorCode>;
[[nodiscard]] auto BuildSphereMesh(RenderContext& gfx, float radius, FVec4 color, bool buildBLAS) -> std::expected<Mesh, ErrorCode>;
[[nodiscard]] auto BuildCylinderMesh(RenderContext& gfx, float radius, float height, FVec4 color, bool buildBLAS) -> std::expected<Mesh, ErrorCode>;
[[nodiscard]] auto BuildConeMesh(RenderContext& gfx, float radius, float height, FVec4 color, bool buildBLAS) -> std::expected<Mesh, ErrorCode>;

auto CreateBoxConfigured(Engine& engine, FVec3 halfExtents, const SpawnParams& params, bool buildBLAS) -> std::expected<Entity, ErrorCode>;

auto CreatePlaneConfigured(Engine& engine, float extent, FVec4 color, const SpawnParams& params, bool buildBLAS) -> std::expected<Entity, ErrorCode>;

auto CreateSphereConfigured(Engine& engine, float radius, const SpawnParams& params, bool buildBLAS) -> std::expected<Entity, ErrorCode>;

auto CreateCylinderConfigured(Engine& engine, float radius, float height, const SpawnParams& params, bool buildBLAS) -> std::expected<Entity, ErrorCode>;

auto CreateConeConfigured(Engine& engine, float radius, float height, const SpawnParams& params, bool buildBLAS) -> std::expected<Entity, ErrorCode>;

// Engine's device-recovery barrier and internal spawners.
void RebuildVulkanResources(RenderContext& gfx, ECS::Registry& registry);
auto CreateBoxInternal(
    RenderContext&                  gfx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    ZHLN::FVec3                     halfExtents,
    const SpawnParams&              params,
    bool                            buildBLAS = true
) -> std::expected<Entity, ErrorCode>;
auto CreateSphereInternal(
    RenderContext&                  gfx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    float                           radius,
    const SpawnParams&              params,
    bool                            buildBLAS = true
) -> std::expected<Entity, ErrorCode>;
auto CreateCylinderInternal(
    RenderContext&                  gfx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    float                           radius,
    float                           height,
    const SpawnParams&              params,
    bool                            buildBLAS = true
) -> std::expected<Entity, ErrorCode>;
auto CreateConeInternal(
    RenderContext&                  gfx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    float                           radius,
    float                           height,
    const SpawnParams&              params,
    bool                            buildBLAS = true
) -> std::expected<Entity, ErrorCode>;
auto CreatePlaneInternal(
    RenderContext&                  gfx,
    ECS::Registry&                  reg,
    ZHLN::Optional<PhysicsContext&> pc,
    float                           extent,
    ZHLN::FVec4                     color,
    const SpawnParams&              params,
    bool                            buildBLAS = true
) -> std::expected<Entity, ErrorCode>;
auto InstantiatePrefabInternal(
    RenderContext&       gfx,
    ECS::Registry&       reg,
    PhysicsContext&      pc,
    const ModelPrefab&   prefab,
    const SpawnParams&   params,
    Entity*              outBuffer,
    uint32_t             maxCount,
    void (*onRoot)(void*, Entity),
    void* rootUserdata
) -> uint32_t;
} // namespace PrefabFactory
} // namespace ZHLN
