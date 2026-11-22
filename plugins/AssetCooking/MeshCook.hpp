// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// AssetCooking/MeshCook.hpp
//
// The mesh baker: interleaved loop-vertex floats in, `.zmesh` bytes out.
// Importer-agnostic by construction -- the input is spans, not a Blender
// manifest and not a file. Whoever can produce triangle loops in the
// documented interleaving (Assimp, cgltf, a procedural generator, or the
// zcook CLI's Blender-IR bridge) can cook a mesh.
//
// Loop-vertex layout (all floats, triangle loops, no index buffer):
//   [vertexId, px py pz, nx ny nz, u v, r g b a]                 (13 floats)
//   ... plus [j0 j1 j2 j3, w0 w1 w2 w3] when `skinned` is set    (21 floats)
// Cooking deduplicates loops into a vertex pool, computes tangent frames,
// and builds meshlets over the result.

#include <AssetCooking/CookError.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Render/Meshlet.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace ZHLN::AssetCooking {

struct MeshPrimitive {
    std::string materialId;
    uint32_t    vertexOffset = 0; // first loop vertex, in loops
    uint32_t    vertexCount  = 0; // loop vertices in this primitive
};

struct RawMeshInput {
    std::span<const float>           loopVertices;
    bool                             skinned = false;
    std::span<const MeshPrimitive>   primitives;
};

struct CompiledMesh {
    std::vector<VertexPosition>       positions;
    std::vector<VertexTangentFrame>   tangentFrames;
    std::vector<VertexSurface>        surfaces;
    std::vector<VertexSkin>           skins;
    std::vector<uint32_t>             indices;
    std::vector<MeshPrimitive>        primitives;
    std::vector<uint32_t>             originalVertexIndices;

    // VK_EXT_mesh_shader streams (see include/Zahlen/Render/Meshlet.hpp). Empty when
    // the mesh is degenerate, in which case consumers fall back to the classic
    // indexed draw path.
    MeshletBuildResult meshlets;

    float minB[3]   = {1e30f, 1e30f, 1e30f};
    float maxB[3]   = {-1e30f, -1e30f, -1e30f};
    bool  isSkinned = false;
};

// Dedupe + tangent frames + meshlets. An input with no loops compiles to an
// empty mesh (all streams empty), mirroring a missing source on disk.
[[nodiscard]] auto CompileMesh(const RawMeshInput& input) -> CompiledMesh;

// Serializes a compiled mesh into `.zmesh` bytes (header + SoA streams).
[[nodiscard]] auto SerializeCookedMesh(const CompiledMesh& mesh) -> std::vector<std::byte>;

// Compile + serialize in one call; the baker consumers actually want.
[[nodiscard]] auto CookMesh(const RawMeshInput& input) -> std::expected<std::vector<std::byte>, ErrorCode>;

} // namespace ZHLN::AssetCooking
