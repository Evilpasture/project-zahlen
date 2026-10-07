// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "GpuHandwritten.hpp"
#include <Zahlen/Meshlet.hpp>
#include <cstring>
#include <meshoptimizer.h>

namespace ZHLN {

MeshletBuildResult BuildMeshlets(std::span<const uint32_t> indices, const float* positions, size_t vertexCount, size_t posStride) noexcept {
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

    const meshopt_Meshlet& last = raw[meshletCount - 1];
    meshletVertices.resize(last.vertex_offset + last.vertex_count);
    meshletTriangles.resize(last.triangle_offset + (last.triangle_count * 3));

    out.meshlets.resize(meshletCount);
    out.triangles.reserve(meshletTriangles.size() + meshletCount * 3);

    for (size_t i = 0; i < meshletCount; ++i) {
        const meshopt_Meshlet& m = raw[i];

        meshopt_optimizeMeshlet(&meshletVertices[m.vertex_offset], &meshletTriangles[m.triangle_offset], m.triangle_count, m.vertex_count);

        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            &meshletVertices[m.vertex_offset], &meshletTriangles[m.triangle_offset], m.triangle_count, positions, vertexCount, posStride
        );

        const uint32_t alignedOffset = static_cast<uint32_t>((out.triangles.size() + 3u) & ~size_t {3u});
        out.triangles.resize(alignedOffset, 0u);
        out.triangles.insert(
            out.triangles.end(), meshletTriangles.begin() + static_cast<ptrdiff_t>(m.triangle_offset),
            meshletTriangles.begin() + static_cast<ptrdiff_t>(m.triangle_offset) + (static_cast<ptrdiff_t>(m.triangle_count) * 3)
        );

        out.meshlets[i] = MeshletDesc {
            .vertexOffset   = m.vertex_offset,
            .triangleOffset = alignedOffset,
            .vertexCount    = m.vertex_count,
            .triangleCount  = m.triangle_count,
            .sphereCenter   = {bounds.center[0], bounds.center[1], bounds.center[2]},
            .sphereRadius   = bounds.radius,
            .coneApex       = {bounds.cone_apex[0], bounds.cone_apex[1], bounds.cone_apex[2]},
            .coneAxis       = {bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2]},
            .coneCutoff     = bounds.cone_cutoff,
        };
    }

    out.vertices.assign(meshletVertices.begin(), meshletVertices.end());

    out.triangles.resize((out.triangles.size() + 3u) & ~size_t {3u}, 0u);

    return out;
}

MeshletBuildResult BuildMeshlets(std::span<const uint32_t> indices, std::span<const VertexPosition> positions) noexcept {
    if (positions.empty()) {
        return {};
    }
    return BuildMeshlets(indices, &positions[0].position[0], positions.size(), sizeof(VertexPosition));
}

std::vector<uint8_t> PackMeshlets(std::span<const MeshletDesc> meshlets) noexcept {
    std::vector<uint8_t> out(meshlets.size() * sizeof(GPUMeshlet));
    for (size_t i = 0; i < meshlets.size(); ++i) {
        const MeshletDesc& d = meshlets[i];
        const GPUMeshlet   g {
            .vertexOffset   = d.vertexOffset,
            .triangleOffset = d.triangleOffset,
            .vertexCount    = d.vertexCount,
            .triangleCount  = d.triangleCount,
            .sphereCenter   = {d.sphereCenter[0], d.sphereCenter[1], d.sphereCenter[2]},
            .sphereRadius   = d.sphereRadius,
            .coneApex       = {d.coneApex[0], d.coneApex[1], d.coneApex[2]},
            .coneAxis       = {d.coneAxis[0], d.coneAxis[1], d.coneAxis[2]},
            .coneCutoff     = d.coneCutoff,
            ._pad           = 0,
        };
        std::memcpy(out.data() + i * sizeof(GPUMeshlet), &g, sizeof(GPUMeshlet));
    }
    return out;
}

}
