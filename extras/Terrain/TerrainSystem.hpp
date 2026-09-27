// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Terrain/TerrainSystem.hpp
#pragma once

#include "TerrainComponents.hpp"
#include <Zahlen/Common.h>
#include <Zahlen/Components.hpp>
#include <Zahlen/ecs/SystemParameters.hpp>

namespace ZHLN {

class Engine;
class RenderContext;

namespace ECS {
class SystemGraph;
} // namespace ECS

namespace Terrain {

// Owns the terrain slot table and lazy-bakes GPU meshes/materials for
// TerrainComponent entities. Moved out of core: it is the bookkeeping half
// of the procedural terrain feature, not engine substrate.
class TerrainSystem {
  public:
    TerrainSystem()  = default;
    ~TerrainSystem() = default;

    TerrainSystem(const TerrainSystem&)            = delete;
    TerrainSystem& operator=(const TerrainSystem&) = delete;
    TerrainSystem(TerrainSystem&&)                 = default;
    TerrainSystem& operator=(TerrainSystem&&)      = default;

    static void Update(ECS::Query<const TerrainComponent, Components::MeshComponent&> query,
                       ECS::ResMut<RenderContext> render);

    static TerrainHandle      RegisterTerrainData(TerrainData data) noexcept;
    static const TerrainData* GetTerrainData(TerrainHandle handle) noexcept;
    static void               UnregisterTerrainData(TerrainHandle handle) noexcept;

    static float SampleHeightAt(const Engine& engine, float worldX, float worldZ) noexcept;
};

// Composition-root entry point: registers TerrainComponent and contributes
// the TerrainSystem update-graph node through the engine's extension seam
// (replayed on every graph rebuild, so it survives scene resets).
void Install(Engine& engine);

} // namespace Terrain
} // namespace ZHLN
