// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Vertex.hpp>
#include <array>
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

// The meshlet wire stride, in bytes: one packed record per meshlet, the number
// the cooked .zmesh format, the packer below and the shader's word protocol
// (`fetchMeshlet`: 16 words) all agree on. A documented number, not a struct
// size -- no public header names the GPU layout anymore.
inline constexpr uint32_t kMeshletPackedBytes = 64;

// One meshlet as logic, not layout: the offsets, counts and bounds BuildMeshlets
// computes, with no alignment, padding or member-offset contract. Host code --
// the cooker, the importers, the tests -- carries these; PackMeshlets turns them
// into the wire records at the submit boundary (the LightDesc/GpuPack split,
// applied to meshlets).
struct MeshletDesc {
    uint32_t vertexOffset;
    uint32_t triangleOffset;
    uint32_t vertexCount;
    uint32_t triangleCount;

    std::array<float, 3> sphereCenter;
    float                sphereRadius;

    std::array<float, 3> coneApex;
    std::array<float, 3> coneAxis;
    float                coneCutoff;
};

struct MeshletBuildResult {
    std::vector<MeshletDesc> meshlets;
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

// Packs logical descs into the wire records the mesh shaders read:
// out.size() == meshlets.size() * kMeshletPackedBytes. The record layout lives
// in src/render/GpuHandwritten.hpp; this declaration is its only public trace.
[[nodiscard]] ZHLN_API std::vector<uint8_t> PackMeshlets(std::span<const MeshletDesc> meshlets) noexcept;

}
