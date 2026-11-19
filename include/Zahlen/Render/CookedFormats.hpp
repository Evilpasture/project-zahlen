// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Zahlen/Render/CookedFormats.hpp
//
// Wire layout of cooked texture, mesh and animation containers shared between
// the engine asset loader and the offline cooker (utils/zcook). Pure POD.

#include <cstdint>

namespace ZHLN {

#pragma pack(push, 1)

struct CookedTextureHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t mipLevels;
    uint32_t vkFormat;
    uint32_t dataSize;
};

// Version 8 payload: positions[vertexCount], tangentFrames[vertexCount],
// surfaces[vertexCount], optional skins[vertexCount], indices[indexCount],
// then the three logical meshlet streams (MeshletDesc[meshletCount],
// meshletVertices[meshletVertexCount], meshletTriangles[meshletTriByteCount]).
// All vertices use independent SoA buffers.
struct CookedMeshHeader {
    uint32_t magic;
    uint32_t version;
    float    boundingBoxMin[3];
    float    boundingBoxMax[3];
    uint32_t vertexCount;
    uint32_t indexCount;
    uint32_t hasSkin;
    uint32_t meshletCount;
    uint32_t meshletVertexCount;
    uint32_t meshletTriByteCount;
};

struct CookedAnimHeader {
    uint32_t magic;
    uint32_t version;
    float    duration;
    uint32_t loop;
    uint32_t trackCount;
};

struct CookedAnimTrack {
    uint64_t targetNodeHash;
    uint32_t pathType;
    uint32_t keyCount;
    uint32_t timeOffset;
    uint32_t valueOffset;
};

#pragma pack(pop)

} // namespace ZHLN
