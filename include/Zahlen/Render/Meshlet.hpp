// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Render/Vertex.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace ZHLN {

inline constexpr uint32_t kMeshletMaxVertices  = 64;
inline constexpr uint32_t kMeshletMaxTriangles = 124;
inline constexpr float    kMeshletConeWeight   = 0.5f;
inline constexpr uint32_t kMeshletsPerTaskGroup = 32;
inline constexpr uint32_t kMeshShaderGroupSize = 64;

// One meshlet as logic, not GPU wire layout: the offsets, counts and bounds
// BuildMeshlets computes. Host code -- the offline .zmesh cooker, the runtime
// importers, the tests -- carries and serializes these directly; inside
// zahlen_render, GpuPack::PackMeshlet converts them field-by-field into the
// Slang-generated GPUMeshlet at upload time.
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

    constexpr auto operator==(const MeshletDesc&) const noexcept -> bool = default;
};

static_assert(
    sizeof(MeshletDesc) == 60 && std::is_trivially_copyable_v<MeshletDesc>,
    "MeshletDesc cooked .zmesh record layout drifted"
);

struct MeshletBuildResult {
    std::vector<MeshletDesc> meshlets;
    std::vector<uint32_t>   vertices;
    std::vector<uint8_t>    triangles;

    [[nodiscard]] bool Empty() const noexcept {
        return meshlets.empty();
    }

    constexpr auto operator==(const MeshletBuildResult&) const noexcept -> bool = default;
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

[[nodiscard]] ZHLN_API MeshletBuildResult BuildMeshlets(
    std::span<const VertexPosition> positions
) noexcept;

}
