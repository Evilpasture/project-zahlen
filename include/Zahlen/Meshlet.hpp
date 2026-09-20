// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Meshlet.hpp
//
// Shared meshlet partitioning used by BOTH asset paths -- tools/zcook/Transform.cpp (offline
// .zmesh cooking) and extras/glTF/GLTFImporter.cpp (runtime import) -- so a mesh cooked offline
// and the same mesh imported JIT produce byte-identical GPU streams. The output is the exact
// memory image the task/mesh shaders read through BDA:
//
//   meshlets   : GPUMeshlet[]  (64B stride, see Zahlen/Types.hpp)
//   vertices   : uint32_t[]    unique vertex indices into the vertex pool
//   triangles  : uint8_t[]     micro-indices, 3 per primitive, local to a meshlet
//
// The triangle stream is padded to a 4-byte multiple because the mesh shader reads it as a
// `uint*`: SPIR-V storage-buffer loads of a uint8 array need 8-bit storage, which not every
// target we support has.
//
// Interface only -- the implementation (src/render/Meshlet.cpp) is the tree's single
// meshoptimizer consumer, so consumers of this header need no meshoptimizer include path or
// link of their own.
#pragma once

#include <Zahlen/Common.h>
#include <Zahlen/Types.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ZHLN {

struct MeshletBuildResult {
    std::vector<GPUMeshlet> meshlets;
    std::vector<uint32_t>   vertices;  // unique vertex indices
    std::vector<uint8_t>    triangles; // 3 micro-indices per primitive (4-byte padded)

    [[nodiscard]] bool Empty() const noexcept {
        return meshlets.empty();
    }
};

/**
 * Partitions an indexed triangle list into GPU meshlets. @p indices must be a multiple of 3,
 * @p positions is the interleaved float3 stream base and @p posStride the byte stride between
 * two positions. Returns an empty result for degenerate input, in which case the caller must
 * keep using the classic vertex/index draw path.
 */
[[nodiscard]] ZHLN_API MeshletBuildResult BuildMeshlets(
    std::span<const uint32_t> indices,
    const float*              positions,
    size_t                    vertexCount,
    size_t                    posStride
) noexcept;

/**
 * @brief Convenience overload for the engine's packed VertexPosition stream.
 */
[[nodiscard]] ZHLN_API MeshletBuildResult BuildMeshlets(
    std::span<const uint32_t>       indices,
    std::span<const VertexPosition> positions
) noexcept;

} // namespace ZHLN
