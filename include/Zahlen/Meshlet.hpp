// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Meshlet.hpp
//
// Shared meshlet partitioning used by BOTH asset paths so that a mesh cooked
// offline by `zcook` and the same mesh imported JIT by the runtime glTF
// importer produce byte-identical GPU streams:
//
//   * src/zcook/Transform.cpp     (offline .zmesh cooking)
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

#pragma once

#include <Zahlen/Types.hpp>
#include <cstdint>
#include <cstring>
#include <meshoptimizer.h>
#include <span>
#include <vector>

namespace ZHLN {

struct MeshletBuildResult {
    std::vector<GPUMeshlet> meshlets;
    std::vector<uint32_t>   vertices;  // unique vertex indices
    std::vector<uint8_t>    triangles; // 3 micro-indices per primitive

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
[[nodiscard]] inline MeshletBuildResult
    BuildMeshlets(std::span<const uint32_t> indices, const float* positions, size_t vertexCount, size_t posStride) noexcept {
    MeshletBuildResult out;

    if (indices.size() < 3 || positions == nullptr || vertexCount == 0 || posStride < sizeof(float) * 3) {
        return out;
    }

    const size_t maxMeshlets = meshopt_buildMeshletsBound(indices.size(), kMeshletMaxVertices, kMeshletMaxTriangles);
    if (maxMeshlets == 0) {
        return out;
    }

    std::vector<meshopt_Meshlet> raw(maxMeshlets);
    std::vector<unsigned int>    meshletVertices(maxMeshlets * kMeshletMaxVertices);
    std::vector<unsigned char>   meshletTriangles(maxMeshlets * kMeshletMaxTriangles * 3);

    const size_t meshletCount = meshopt_buildMeshlets(
        raw.data(), meshletVertices.data(), meshletTriangles.data(), indices.data(), indices.size(), positions, vertexCount, posStride, kMeshletMaxVertices,
        kMeshletMaxTriangles, kMeshletConeWeight
    );

    if (meshletCount == 0) {
        return out;
    }

    // meshopt_buildMeshlets over-allocates; trim to the tail of the last meshlet.
    const meshopt_Meshlet& last = raw[meshletCount - 1];
    meshletVertices.resize(last.vertex_offset + last.vertex_count);
    meshletTriangles.resize(last.triangle_offset + (last.triangle_count * 3));

    out.meshlets.resize(meshletCount);
    out.triangles.reserve(meshletTriangles.size() + meshletCount * 3);

    for (size_t i = 0; i < meshletCount; ++i) {
        const meshopt_Meshlet& m = raw[i];

        // Per-meshlet optimisation improves both vertex-cache behaviour and the
        // tightness of the cone that the task shader tests against.
        meshopt_optimizeMeshlet(&meshletVertices[m.vertex_offset], &meshletTriangles[m.triangle_offset], m.triangle_count, m.vertex_count);

        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            &meshletVertices[m.vertex_offset], &meshletTriangles[m.triangle_offset], m.triangle_count, positions, vertexCount, posStride
        );

        // meshoptimizer packs micro-index runs back to back, but the mesh
        // shader loads them as 32-bit words (no 8-bit storage requirement), so
        // re-emit every meshlet at a 4-byte aligned offset.
        const uint32_t alignedOffset = static_cast<uint32_t>((out.triangles.size() + 3u) & ~size_t {3u});
        out.triangles.resize(alignedOffset, 0u);
        out.triangles.insert(
            out.triangles.end(), meshletTriangles.begin() + static_cast<ptrdiff_t>(m.triangle_offset),
            meshletTriangles.begin() + static_cast<ptrdiff_t>(m.triangle_offset) + (static_cast<ptrdiff_t>(m.triangle_count) * 3)
        );

        out.meshlets[i] = GPUMeshlet {
            .vertexOffset   = m.vertex_offset,
            .triangleOffset = alignedOffset,
            .vertexCount    = m.vertex_count,
            .triangleCount  = m.triangle_count,
            .sphereCenter   = {bounds.center[0], bounds.center[1], bounds.center[2]},
            .sphereRadius   = bounds.radius,
            .coneApex       = {bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2]},
            .coneAxis       = {bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2]},
            .coneCutoff     = bounds.cone_cutoff,
            ._pad           = 0,
        };
    }

    out.vertices.assign(meshletVertices.begin(), meshletVertices.end());

    // Tail padding so the last word-sized load stays in bounds.
    out.triangles.resize((out.triangles.size() + 3u) & ~size_t {3u}, 0u);

    return out;
}

/// Convenience overload for the engine's packed VertexPosition stream.
[[nodiscard]] inline MeshletBuildResult BuildMeshlets(std::span<const uint32_t> indices, std::span<const VertexPosition> positions) noexcept {
    if (positions.empty()) {
        return {};
    }
    return BuildMeshlets(indices, &positions[0].position[0], positions.size(), sizeof(VertexPosition));
}

} // namespace ZHLN
