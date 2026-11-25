// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "VFXTypes.hpp"
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <random>
#include <vector>
namespace ZHLN::Lightning {
struct GeneratedRibbon {
    std::vector<VertexPosition>     positions;
    std::vector<VertexTangentFrame> frames;
    std::vector<VertexSurface>      surfaces;
    uint32_t                        maxVertices = 0;
};

auto GenerateRibbon(FVec3 start, FVec3 end, FVec3 camera, const ConfigInfo&, std::mt19937&) -> GeneratedRibbon;
} // namespace ZHLN::Lightning
