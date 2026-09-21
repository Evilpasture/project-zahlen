// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Terrain/TerrainFactory.hpp
//
// Procedural terrain content generation: the FBM/warp/ridge noise heightmap
// generator, its slope-aware tinting, and the entity spawners that combine a
// baked heightmap mesh with the core heightfield collider. Moved out of
// CreativeWorksFactory/MeshBuilder because it is sample-world content
// creation, not engine substrate -- core keeps CreateHeightFieldShape and the
// generic mesh/meshlet plumbing.
#pragma once

#include "TerrainComponents.hpp"
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <Zahlen/Render/Types.hpp>

namespace ZHLN {
class Engine;
class RenderContext;
class PhysicsContext;

namespace Terrain {

// Builds a heightfield mesh (positions + packed attributes + meshlets + BLAS)
// from caller-owned height/color arrays. No noise, no tinting -- pure
// heightmap tessellation.
auto CreateTerrainMeshFromData(RenderContext& ctx, int sampleCount, float worldSize, const float* heights, const float* colorsRGBA) -> Mesh;

// Generates a heightmap with the terrain noise (FBM with domain warp and
// ridged components for Snow, plain FBM otherwise), tints it by slope and
// altitude, and builds the mesh. `outHeights` receives sampleCount*sampleCount
// heights for physics.
auto CreateTerrainMesh(RenderContext& ctx, int sampleCount, float worldSize, float maxHeight, float* outHeights, TerrainType type = TerrainType::Default)
    -> Mesh;

// Spawns a terrain entity from caller-provided height/color data: mesh,
// material, TerrainComponent, and (optionally) a static heightfield body.
auto CreateTerrainFromData(
    RenderContext&                             ctx,
    ECS::Registry&                             reg,
    PhysicsContext*                            pc,
    int                                        sampleCount,
    float                                      worldSize,
    const float*                               heights,
    const float*                               colorsRGBA,
    const CreativeWorksFactory::SpawnParams& params = {}
) -> Entity;
auto CreateTerrainFromData(Engine& engine, int sampleCount, float worldSize, const float* heights, const float* colorsRGBA, const CreativeWorksFactory::SpawnParams& params = {})
    -> Entity;

// Spawns a procedurally generated terrain entity (noise heights baked through
// CreateTerrainMesh, then the same wiring as CreateTerrainFromData).
auto CreateTerrain(
    RenderContext&                             ctx,
    ECS::Registry&                             reg,
    PhysicsContext*                            pc,
    size_t                                     sampleCount,
    float                                      worldSize,
    float                                      maxHeight,
    TerrainType                                type   = TerrainType::Default,
    const CreativeWorksFactory::SpawnParams& params = {}
) -> Entity;
auto CreateTerrain(Engine& engine, int sampleCount, float worldSize, float maxHeight, TerrainType type = TerrainType::Default, const CreativeWorksFactory::SpawnParams& params = {})
    -> Entity;

} // namespace Terrain
} // namespace ZHLN
