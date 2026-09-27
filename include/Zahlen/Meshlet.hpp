// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Vertex.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ZHLN {

inline constexpr uint32_t kMeshletMaxVertices  = 64;
inline constexpr uint32_t kMeshletMaxTriangles = 124;
inline constexpr float    kMeshletConeWeight   = 0.5f;
inline constexpr uint32_t kMeshletsPerTaskGroup = 32;
inline constexpr uint32_t kMeshShaderGroupSize = 64;

struct alignas(16) GPUMeshlet {
    uint32_t vertexOffset;
    uint32_t triangleOffset;
    uint32_t vertexCount;
    uint32_t triangleCount;

    float sphereCenter[3];
    float sphereRadius;

    float    coneApex[3];
    float    coneAxis[3];
    float    coneCutoff;
    uint32_t _pad;
};
static_assert(sizeof(GPUMeshlet) == 64);
static_assert(alignof(GPUMeshlet) == 16);

struct MeshletBuildResult {
    std::vector<GPUMeshlet> meshlets;
    std::vector<uint32_t>   vertices;
    std::vector<uint8_t>    triangles;

    [[nodiscard]] bool Empty() const noexcept {
        return meshlets.empty();
    }
};

[[nodiscard]] ZHLN_API MeshletBuildResult BuildMeshlets(
    std::span<const uint32_t> indices,
    const float*              positions,
    size_t                    vertexCount,
    size_t                    posStride
) noexcept;

[[nodiscard]] ZHLN_API MeshletBuildResult BuildMeshlets(
    std::span<const uint32_t>       indices,
    std::span<const VertexPosition> positions
) noexcept;

}
