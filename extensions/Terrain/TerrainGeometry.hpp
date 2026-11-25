// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TerrainTypes.hpp"
#include <Zahlen/Render/Vertex.hpp>
#include <expected>
#include <span>
#include <vector>
namespace ZHLN::Terrain {
// Consumer-owned authoring document, never an engine component or COM ABI node.
struct Geometry {
    uint32_t                        sampleCount = 0;
    float                           worldSize   = 0;
    std::vector<float>              heights;
    std::vector<VertexPosition>     positions;
    std::vector<VertexTangentFrame> frames;
    std::vector<VertexSurface>      surfaces;
};
[[nodiscard]] auto FromHeights(uint32_t sampleCount, float worldSize, std::span<const float> heights, std::span<const float> colorsRGBA = {})
    -> std::expected<Geometry, ErrorCode>;
[[nodiscard]] auto Generate(const GenerateInfo& info = {}) -> std::expected<Geometry, ErrorCode>;
} // namespace ZHLN::Terrain
