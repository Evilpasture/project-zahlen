// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Meshlet.hpp
//
// Shared meshlet partitioning used by BOTH asset paths so that a mesh cooked
// offline by `zcook` and the same mesh imported JIT by the runtime glTF
// importer produce byte-identical GPU streams:
//
//   * tools/zcook/Transform.cpp   (offline .zmesh cooking)
//   * extras/glTF/GLTFImporter.cpp (runtime glTF/GLB import)
//
// The output is the exact memory image the task/mesh shaders read through BDA:
//
//   meshlets   : GPUMeshlet[]  (64B stride, see Zahlen/Types.hpp)
//   vertices   : uint32_t[]    unique vertex indices into the vertex pool
//   triangles  : uint8_t[]     micro-indices, 3 per primitive, local to a meshlet
//
// The triangle stream is padded to a 4-byte multiple because the mesh shader
// reads it as a `uint*` (SPIR-V storage-buffer loads of a uint8 array are not
// available without 8-bit storage on every target we support).
//
// Interface only: the implementation (src/render/Meshlet.cpp) is the tree's
// single meshoptimizer consumer, compiled into zahlen_render. Consumers of
// this header need no meshoptimizer include path and no link of their own --
// the symbols arrive through libzahlen_engine like any other ZHLN_API entry.
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
 * @brief Partitions an indexed triangle list into GPU meshlets.
 *
 * @param indices     Triangle-list index stream (must be a multiple of 3).
 * @param positions   Interleaved position stream base pointer (float3).
 * @param vertexCount Number of vertices addressable through @p positions.
 * @param posStride   Byte stride between two consecutive positions.
 *
 * Returns an empty result for degenerate input, in which case the caller must
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
