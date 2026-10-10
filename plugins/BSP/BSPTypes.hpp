// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// plugins/BSP/BSPTypes.hpp
//
// On-disk Source engine BSP structures for the mainline formats (HL2 episodes,
// CS:S, Portal 1/2, L4D/2, CS:GO-era: BSP versions 19-21). Field order and
// widths follow the format byte-for-byte; exotic forks (Titanfall's XOR'd
// lumps, Vindictus/VTMB/Strata struct variants, static-prop game lump
// versions) are deliberately out of scope here.
//
// The structs are packed and declared in file order so that a C++ member's
// offset *is* its file offset. That identity is what lets the generic
// field-wise reader in BSPRead.hpp read any of these in one code path
// instead of one hand-written reader per lump, per version.

#include <cstdint>

namespace ZHLN::BSP {

// 'VBSP' as a little-endian int32.
inline constexpr int32_t kBspIdent      = 0x50534256;
inline constexpr int32_t kBspVersionMin = 19;
inline constexpr int32_t kBspVersionMax = 21;

// Texinfo index meaning "this face belongs to a node, not a surface".
inline constexpr int32_t kTexInfoNode = -1;

enum class Lump : uint32_t {
    Entities                    = 0,
    Planes                      = 1,
    TexData                     = 2,
    Vertexes                    = 3,
    Visibility                  = 4,
    Nodes                       = 5,
    TexInfo                     = 6,
    Faces                       = 7,
    Lighting                    = 8,
    Occlusion                   = 9,
    Leafs                       = 10,
    FaceIds                     = 11,
    Edges                       = 12,
    SurfEdges                   = 13,
    Models                      = 14,
    WorldLights                 = 15,
    LeafFaces                   = 16,
    LeafBrushes                 = 17,
    Brushes                     = 18,
    BrushSides                  = 19,
    Areas                       = 20,
    AreaPortals                 = 21,
    DispInfo                    = 26,
    OriginalFaces               = 27,
    PhysDisp                    = 28,
    PhysCollide                 = 29,
    VertNormals                 = 30,
    VertNormalIndices           = 31,
    DispLightmapAlphas          = 32,
    DispVerts                   = 33,
    DispLightmapSamplePositions = 34,
    GameLump                    = 35,
    LeafWaterData               = 36,
    Primitives                  = 37,
    PrimVerts                   = 38,
    PrimIndices                 = 39,
    PakFile                     = 40,
    ClipPortalVerts             = 41,
    Cubemaps                    = 42,
    TexDataStringData           = 43,
    TexDataStringTable          = 44,
    Overlays                    = 45,
    LeafMinDistToWater          = 46,
    FaceMacroTextureInfo        = 47,
    DispTris                    = 48,
    FacesHDR                    = 58,
    Count                       = 64
};

#pragma pack(push, 1)

struct LumpEntry {
    int32_t fileofs;
    int32_t filelen;
    int32_t version;
    int32_t fourCC;
};

struct BspHeader {
    int32_t   ident;
    int32_t   version;
    LumpEntry lumps[static_cast<uint32_t>(Lump::Count)];
};

// LUMP_PLANES
struct DPlane {
    float   normal[3];
    float   dist;
    int32_t type;
};

// LUMP_VERTEXES
struct DVertex {
    float point[3];
};

// LUMP_EDGES
struct DEdge {
    uint16_t v[2];
};

// LUMP_MODELS
struct DModel {
    float   mins[3];
    float   maxs[3];
    float   origin[3];
    int32_t headnode;
    int32_t firstface;
    int32_t numfaces;
};

// LUMP_TEXINFO: [s/t][xyz offset], projections in texels / luxels.
struct DTexInfo {
    float   textureVecs[2][4];
    float   lightmapVecs[2][4];
    int32_t flags;
    int32_t texdata;
};

// LUMP_TEXDATA
struct DTexData {
    float   reflectivity[3];
    int32_t nameStringTableID;
    int32_t width;
    int32_t height;
    int32_t viewWidth;
    int32_t viewHeight;
};

// LUMP_FACES: the mainline (version 19-21) face. On-disk order after origFace
// is firstPrimID, numPrims -- the classic header spells the pair the other way
// round, so keep this order when porting from memory.
struct DFace {
    uint16_t planenum;
    int8_t   side;
    int8_t   onNode;
    int32_t  firstedge;
    int16_t  numedges;
    int16_t  texinfo;
    int16_t  dispinfo;
    uint16_t surfaceFogVolumeID;
    uint8_t  styles[4];
    int32_t  lightofs;
    float    area;
    int32_t  lightmapTextureMinsInLuxels[2];
    int32_t  lightmapTextureSizeInLuxels[2];
    int32_t  origFace;
    uint16_t firstPrimID;
    uint16_t numPrims;
    int32_t  smoothingGroups;
};

// LUMP_DISPINFO (the mainline flavour, 176 bytes).
struct DDispInfo {
    float    startPosition[3];
    int32_t  dispVertStart;
    int32_t  dispTriStart;
    int32_t  power;
    int32_t  minTess;
    float    smoothingAngle;
    int32_t  contents;
    uint16_t mapFace;
    uint16_t padding;
    int32_t  lightmapAlphaStart;
    int32_t  lightmapSamplePositionStart;
    uint8_t  neighbors[88];
    int32_t  allowedVerts[10];
};

// LUMP_DISP_VERTS
struct DDispVert {
    float vec[3];
    float dist;
    float alpha;
};

// LUMP_NODES (mainline v0).
struct DNode {
    int32_t  planenum;
    int32_t  children[2];
    int16_t  mins[3];
    int16_t  maxs[3];
    uint16_t firstface;
    uint16_t numfaces;
    int16_t  area;
    uint16_t padding;
};

// LUMP_LEAFS v0 (30 bytes). v1 appends two padding bytes (DLeafV1 in the
// reference implementation); the reader consumes the lump version field to
// pick the stride and keeps this struct as the payload.
struct DLeaf {
    int32_t  contents;
    int16_t  cluster;
    int16_t  areaFlags;
    int16_t  mins[3];
    int16_t  maxs[3];
    uint16_t firstleafface;
    uint16_t numleaffaces;
    uint16_t firstleafbrush;
    uint16_t numleafbrushes;
    int16_t  leafWaterDataID;
};

// LUMP_BRUSHES
struct DBrush {
    int32_t firstside;
    int32_t numsides;
    int32_t contents;
};

// LUMP_BRUSHSIDES
struct DBrushSide {
    uint16_t planenum;
    int16_t  texinfo;
    int16_t  dispinfo;
    int16_t  bevel;
};

// LUMP_OVERLAYS
struct DOverlay {
    int32_t  id;
    int16_t  texinfo;
    uint16_t faceCountAndRenderOrder;
    int32_t  ofaces[64];
    float    u[2];
    float    v[2];
    float    uvpoints[4][3];
    float    origin[3];
    float    basisNormal[3];
};

// LUMP_GAMELUMP (Lump 35): dgamelump_t directory entry (16 bytes)
struct DGameLump {
    int32_t  id;      // FourCC (e.g. 'sprp' 0x70727073)
    uint16_t flags;   // Flags (bit 0: compressed with LZMA)
    uint16_t version; // Game lump version
    int32_t  fileofs; // Absolute byte offset from start of BSP file
    int32_t  filelen; // Byte length of game lump data
};

#pragma pack(pop)

// Game lump identifiers
inline constexpr int32_t kGameLumpStaticProps    = 0x70727073; // 'sprp' (little-endian)
inline constexpr int32_t kGameLumpStaticPropsAlt = 0x73707270; // 'prps' (big-endian)

static_assert(sizeof(LumpEntry) == 16);
static_assert(sizeof(BspHeader) == 4 + 4 + 16 * 64);
static_assert(sizeof(DPlane) == 20);
static_assert(sizeof(DVertex) == 12);
static_assert(sizeof(DEdge) == 4);
static_assert(sizeof(DModel) == 48);
static_assert(sizeof(DTexInfo) == 72);
static_assert(sizeof(DTexData) == 32);
static_assert(sizeof(DFace) == 56);
static_assert(sizeof(DDispInfo) == 176);
static_assert(sizeof(DDispVert) == 20);
static_assert(sizeof(DNode) == 32);
static_assert(sizeof(DLeaf) == 30);
static_assert(sizeof(DBrush) == 12);
static_assert(sizeof(DBrushSide) == 8);
static_assert(sizeof(DOverlay) == 352);
static_assert(sizeof(DGameLump) == 16);

} // namespace ZHLN::BSP
