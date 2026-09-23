// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// extras/Terrain/TerrainComponents.hpp
//
// Procedural terrain: the heightmap store, the ECS component and the handle
// type. Core keeps the generic pieces this is built on (the heightfield
// collider in PhysicsContext, meshlet/indexed rendering in RenderContext);
// the snow/mountain generator and its bookkeeping live here.
#pragma once

#include <Zahlen/Core/Array.hpp>
#include <cstdint>

namespace ZHLN::Terrain {

enum class TerrainHandle : uint64_t { Invalid = 0 };
static_assert(sizeof(TerrainHandle) == 8);

enum class TerrainType : uint8_t { Default = 0, Snow = 1, Desert = 2 };

// Heightmap + coloring owned by the terrain slot table (TerrainSystem).
// The ECS component below references one of these by handle; the buffers
// outlive registry clears and are retired through UnregisterTerrainData.
struct TerrainData {
    uint32_t           sampleCount = 128;
    float              worldSize   = 280.0f;
    float              maxHeight   = 35.0f;
    ZHLN::Array<float> heights;
    ZHLN::Array<float> colors;
};

struct TerrainComponent {
    uint32_t      sampleCount   = 128;
    float         worldSize     = 280.0f;
    float         maxHeight     = 35.0f;
    float         roughness     = 0.85f;
    float         metallic      = 0.05f;
    TerrainHandle terrainHandle = TerrainHandle::Invalid;
};

} // namespace ZHLN::Terrain
