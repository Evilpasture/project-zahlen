// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extensions/Terrain/TerrainSystem.hpp
#pragma once

#include "TerrainComponents.hpp"
#include <Zahlen/Common.h>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/GameModule.hpp>
#include <Zahlen/ECS/SystemParameters.hpp>

namespace ZHLN {

class Engine;
class RenderContext;
namespace ECS {
class EntityCommandBuffer;
}

namespace Terrain {

// Explicit views for sampling and lifecycle passes. Terrain slot storage is
// shared outside the registry, so callers of the typed entry points must also
// isolate sampling from slot retirement/reclamation and cleanup from other
// accesses to the affected components. The Engine adapters run at their
// existing externally ordered phases.
using TerrainHeightQuery = ECS::Query<const TerrainComponent, const Components::TransformComponent>;
using TerrainReleaseQuery = ECS::Query<TerrainComponent&>;
using TerrainCleanupQuery = ECS::Query<TerrainComponent&, const Components::PendingDestroy>;

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

    static void Update(
        ECS::Query<const TerrainComponent, const Components::PendingDestroy, Components::MeshComponent&, Components::OwnedMeshComponent&> query,
        ECS::ResMut<RenderContext> render,
        ECS::EntityCommandBuffer&  ecb
    );

    static TerrainID      RegisterTerrainData(TerrainData data) noexcept;
    static const TerrainData* GetTerrainData(TerrainID handle) noexcept;
    static void               UnregisterTerrainData(TerrainID handle) noexcept;

    // Explicit ownership operations for TerrainComponent's heightmap slot.
    static void Attach(Engine& engine, Entity entity, TerrainComponent component);
    static void Detach(Engine& engine, Entity entity);
    static void ReleaseTerrainDataWithQuery(TerrainReleaseQuery query);
    static void RegisterCleanup(Engine& engine);
    static void CleanupWithQuery(TerrainCleanupQuery query, bool all);
    static void Cleanup(Engine& engine, bool all);

    static float SampleHeightAtWithQuery(TerrainHeightQuery query, float worldX, float worldZ) noexcept;
};

// Composition-root entry point: registers TerrainComponent and contributes
// the TerrainSystem update-graph node through the engine's extension seam
// (replayed on every graph rebuild, so it survives scene resets).
// The module as a value: what the host installs, and what a host that wants to
// report a placement failure installs itself instead of calling Install.
auto Describe() -> Module;

void Install(Engine& engine);

} // namespace Terrain
} // namespace ZHLN
