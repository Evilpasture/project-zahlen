// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Core/Array.hpp>
#include <Zahlen/Types.hpp>

namespace ZHLN {

class Engine;

struct TerrainData {
    uint32_t           sampleCount = 128;
    float              worldSize   = 280.0f;
    float              maxHeight   = 35.0f;
    ZHLN::Array<float> heights;
    ZHLN::Array<float> colors;
};

class ZHLN_API TerrainSystem {
  public:
    TerrainSystem()  = default;
    ~TerrainSystem() = default;

    TerrainSystem(const TerrainSystem&)            = delete;
    TerrainSystem& operator=(const TerrainSystem&) = delete;
    TerrainSystem(TerrainSystem&&)                 = default;
    TerrainSystem& operator=(TerrainSystem&&)      = default;

    void Update(Engine& engine, float dt);

    static TerrainHandle      RegisterTerrainData(TerrainData data) noexcept;
    static const TerrainData* GetTerrainData(TerrainHandle handle) noexcept;
    static void               UnregisterTerrainData(TerrainHandle handle) noexcept;

    static float SampleHeightAt(const Engine& engine, float worldX, float worldZ) noexcept;
};

} // namespace ZHLN
