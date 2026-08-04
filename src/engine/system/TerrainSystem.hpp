// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>

namespace ZHLN {

class Engine;

class ZHLN_API TerrainSystem {
  public:
    TerrainSystem()  = default;
    ~TerrainSystem() = default;

    TerrainSystem(const TerrainSystem&)            = delete;
    TerrainSystem& operator=(const TerrainSystem&) = delete;
    TerrainSystem(TerrainSystem&&)                 = default;
    TerrainSystem& operator=(TerrainSystem&&)      = default;

    /**
     * @brief Processes TerrainComponents:
     *  - Generates GPU meshes and materials on-demand for uninitialized or invalidated terrain assets.
     *  - Synchronizes terrain material properties (roughness/metallic) with the RenderContext.
     */
    void Update(Engine& engine, float dt);

    /**
     * @brief Performs a fast bilinear height query on active terrains at world coordinate (x, z).
     * @return The interpolated height in world units, or 0.0f if no terrain covers the coordinate.
     */
    static float SampleHeightAt(const Engine& engine, float worldX, float worldZ) noexcept;
};

} // namespace ZHLN
