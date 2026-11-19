// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <random>
#include <vector>
namespace ZHLN::Lightning {
struct ConfigInfo {
    float eta = 2, peakCurrentKA = 45, timeDilation = 1, ribbonWidth = .8f;
    int   subdivisions = 5;
    float soundVolume = 10, emissiveIntensity = 8000;
};
struct GeneratedRibbon {
    std::vector<VertexPosition>     positions;
    std::vector<VertexTangentFrame> frames;
    std::vector<VertexSurface>      surfaces;
    uint32_t                        maxVertices = 0;
};

auto GenerateRibbon(FVec3 start, FVec3 end, FVec3 camera, const ConfigInfo&, std::mt19937&) -> GeneratedRibbon;
} // namespace ZHLN::Lightning
