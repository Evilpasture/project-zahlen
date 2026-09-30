// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Vertex.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ZHLN::GLTF {

// Construct a tangent and bitangent sign from indexed positions, normals and
// the UV set used by the normal map when a glTF primitive omits TANGENT.
// Authored tangent attributes bypass this generator in the importer.
[[nodiscard]] auto GenerateTangents(
    std::span<const VertexPosition> positions,
    std::span<const std::array<float, 3>> normals,
    std::span<const std::array<float, 2>> texcoords,
    std::span<const uint32_t> indices
) -> std::vector<std::array<float, 4>>;

} // namespace ZHLN::GLTF
