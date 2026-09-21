// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// include/Zahlen/Meshlet.hpp
//
// Shared meshlet partitioning used by BOTH asset paths -- tools/zcook/Transform.cpp (offline
// .zmesh cooking) and extras/glTF/GLTFImporter.cpp (runtime import) -- so a mesh cooked offline
// and the same mesh imported JIT produce byte-identical GPU streams. The output is the exact
// memory image the task/mesh shaders read through BDA:
//
//   meshlets   : GPUMeshlet[]  (64B stride, declared below)
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
#include <Zahlen/Vertex.hpp> // VertexPosition, for the convenience overload
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ZHLN {

// Partitioning limits. These match the guaranteed VK_EXT_mesh_shader minimums
// (maxMeshOutputVertices >= 256, maxMeshOutputPrimitives >= 256) with plenty of
// headroom, and are also the meshoptimizer build parameters used by the cooker
// and the runtime glTF importer, so cooked and JIT meshlets stay identical.
inline constexpr uint32_t kMeshletMaxVertices  = 64;
inline constexpr uint32_t kMeshletMaxTriangles = 124; // multiple of 4 (meshoptimizer recommendation)
inline constexpr float    kMeshletConeWeight   = 0.5f;
// Meshlets handled by one task-shader workgroup (one payload slot each).
inline constexpr uint32_t kMeshletsPerTaskGroup = 32;
// Threads per mesh-shader workgroup (one vertex per thread, 2 prims per thread).
inline constexpr uint32_t kMeshShaderGroupSize = 64;

// 64-byte meshlet descriptor. basic_task / basic_mesh index it through
// a raw BDA pointer, so this layout is the authoritative GPU type.
//
// Hand-written rather than reflected from Slang, unlike the generated GPU
// structs in <Zahlen/Render/GpuLayout.hpp>: the shaders read it as a raw word
// protocol -- slang's fetchMeshlet seats coneAxis at byte 44, which no
// std140/std430 declaration of consecutive float3s can spell (Slang seats it at
// 48), so no declaration-derived spelling of it would be the layout they
// actually read. See tools/zshader/GpuTypes.cpp.
//
// It lives beside BuildMeshlets rather than with the renderer's types because
// this header is the contract the offline cooker and the runtime importer
// share: both must produce these bytes, and neither should include the
// renderer's descriptors (or Jolt) to do it.
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
