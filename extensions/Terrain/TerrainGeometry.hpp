// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "TerrainComponents.hpp"
#include <Zahlen/Error.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <expected>
#include <span>
#include <vector>
namespace ZHLN::Terrain {
enum class Error : uint8_t {
    InvalidData ZHLN_ANNOTATION(ZHLN::Description<"Invalid heightfield dimensions, samples, colors or pose."> {}) = 1,
    NotStarted  ZHLN_ANNOTATION(ZHLN::Description<"Terrain host has no engine binding."> {})
};
// Consumer-owned authoring document, never an engine component or COM ABI node.
struct Geometry {
    uint32_t                        sampleCount = 0;
    float                           worldSize   = 0;
    std::vector<float>              heights;
    std::vector<VertexPosition>     positions;
    std::vector<VertexTangentFrame> frames;
    std::vector<VertexSurface>      surfaces;
};
struct GenerateInfo {
    uint32_t    sampleCount = 128;
    float       worldSize   = 280;
    float       maxHeight   = 35;
    Type type        = Type::Default;
};
[[nodiscard]] auto FromHeights(uint32_t sampleCount, float worldSize, std::span<const float> heights, std::span<const float> colorsRGBA = {})
    -> std::expected<Geometry, ErrorCode>;
[[nodiscard]] auto Generate(const GenerateInfo& info = {}) -> std::expected<Geometry, ErrorCode>;
} // namespace ZHLN::Terrain
