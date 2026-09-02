// tools/zcook/Transform.hpp
// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "IR.hpp"
#include "Zahlen/Types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace ZHLN {

struct CompiledMesh {
    std::vector<VertexPosition>        positions;
    std::vector<VertexAttributes>      attributes;
    std::vector<VertexSkin>            skins;
    std::vector<uint32_t>              indices;
    std::vector<Compiler::IRPrimitive> primitives;
    std::vector<uint32_t>              originalVertexIndices;

    // VK_EXT_mesh_shader streams (see include/Zahlen/Meshlet.hpp). Empty when
    // the mesh is degenerate, in which case consumers fall back to the classic
    // indexed draw path.
    std::vector<GPUMeshlet> meshlets;
    std::vector<uint32_t>   meshletVertices;
    std::vector<uint8_t>    meshletTriangles;

    float minB[3]   = {1e30f, 1e30f, 1e30f};
    float maxB[3]   = {-1e30f, -1e30f, -1e30f};
    bool  isSkinned = false;
};

CompiledMesh CompileRawMesh(const Compiler::IRMesh& mesh, const std::string& binPath);
} // namespace ZHLN
