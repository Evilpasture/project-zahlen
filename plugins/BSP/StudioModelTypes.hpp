// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace ZHLN::BSP {

// FourCC and version identifiers for StudioModel files
inline constexpr int32_t kStudioHdrMagic = ('I' | ('D' << 8) | ('S' << 16) | ('T' << 24)); // 0x54534449 "IDST"
inline constexpr int32_t kVvdMagic       = ('I' | ('D' << 8) | ('S' << 16) | ('V' << 24)); // 0x56534449 "IDSV"
inline constexpr int32_t kVtxVersion     = 7;                                               // VTX version 7
inline constexpr int32_t kPhyVphyMagic   = ('V' | ('P' << 8) | ('H' << 16) | ('Y' << 24)); // 0x59485056 "VPHY"

#pragma pack(push, 1)

// ============================================================================
// .MDL (Model Header & Metadata)
// ============================================================================

struct StudioHdr {
    int32_t id;          // 'IDST'
    int32_t version;     // 44, 48, 49
    int32_t checksum;
    char    name[64];
    int32_t length;      // Data length

    float   eyeposition[3];
    float   illumposition[3];
    float   hull_min[3];
    float   hull_max[3];
    float   view_bbmin[3];
    float   view_bbmax[3];

    int32_t flags;

    int32_t numbones;
    int32_t boneindex;

    int32_t numbonecontrollers;
    int32_t bonecontrollerindex;

    int32_t numhitboxsets;
    int32_t hitboxsetindex;

    int32_t numlocalanim;
    int32_t localanimindex;

    int32_t numlocalseq;
    int32_t localseqindex;

    int32_t activitylistversion;
    int32_t eventsindexed;

    int32_t numtextures;
    int32_t textureindex;

    int32_t numcdtextures;
    int32_t cdtextureindex;

    int32_t numskinref;
    int32_t numskinfamilies;
    int32_t skinindex;

    int32_t numbodyparts;
    int32_t bodypartindex;

    int32_t numlocalattachments;
    int32_t localattachmentindex;
};
static_assert(sizeof(StudioHdr) == 248);

struct StudioTexture {
    int32_t sznameindex; // relative to this struct
    int32_t flags;
    int32_t used;
    int32_t unused1;
    int32_t material;    // IMaterial* pointer slot on 32-bit disk layout
    int32_t clientmaterial;
    int32_t unused[10];
};
static_assert(sizeof(StudioTexture) == 64);

struct StudioBodyPart {
    int32_t sznameindex; // relative to this struct
    int32_t nummodels;
    int32_t base;
    int32_t modelindex;  // relative to this struct
};
static_assert(sizeof(StudioBodyPart) == 16);

struct StudioModel {
    char    name[64];
    int32_t type;
    float   boundingradius;
    int32_t nummeshes;
    int32_t meshindex;   // relative to this struct
    int32_t numvertices; // total vertices in this submodel
    int32_t vertexindex; // vertex offset into VVD
    int32_t tangentsindex;
    int32_t numattachments;
    int32_t attachmentindex;
    int32_t numeyeballs;
    int32_t eyeballindex;
    int32_t pVertexData; // void*
    int32_t pTangentData;// void*
    int32_t unused[8];
};
static_assert(sizeof(StudioModel) == 148);

struct StudioMesh {
    int32_t material;     // index into StudioTexture array
    int32_t modelindex;   // relative offset back to StudioModel
    int32_t numvertices;
    int32_t vertexoffset; // vertex offset relative to model's vertexindex
    int32_t numflexes;
    int32_t flexindex;
    int32_t materialtype;
    int32_t materialparam;
    int32_t meshid;
    float   center[3];
    int32_t pModelvertexdata;
    int32_t numLODvertices[8];
    int32_t unused[8];
};
static_assert(sizeof(StudioMesh) == 116);

struct StudioBone {
    int32_t sznameindex; // relative to this struct
    int32_t parent;      // parent bone index (-1 = root)
    int32_t bonecontroller[6];
    float   pos[3];
    float   quat[4];
    float   rot[3];
    float   posscale[3];
    float   rotscale[3];
    float   poseToBone[3][4];
    float   qAlignment[4];
    int32_t flags;
    int32_t proctype;
    int32_t procindex;
    int32_t physicsbone;
    int32_t surfacepropidx;
    int32_t contents;
    int32_t unused[8];
};
static_assert(sizeof(StudioBone) == 216);

// ============================================================================
// .VVD (Valve Vertex Data)
// ============================================================================

struct VvdHeader {
    int32_t id;                  // 'IDSV' = 0x56534449
    int32_t version;             // 4
    int32_t checksum;
    int32_t numLODs;
    int32_t numLODVertexes[8];
    int32_t numFixups;
    int32_t fixupTableStart;
    int32_t vertexDataStart;
    int32_t tangentDataStart;
};
static_assert(sizeof(VvdHeader) == 64);

struct VvdFixup {
    int32_t lod;
    int32_t sourceVertexID;
    int32_t numVertexes;
};
static_assert(sizeof(VvdFixup) == 12);

struct StudioBoneWeight {
    float   weight[3];
    int8_t  bone[3];
    uint8_t numbones;
};
static_assert(sizeof(StudioBoneWeight) == 16);

struct StudioVertex {
    StudioBoneWeight boneWeights; // 16 bytes
    float            pos[3];      // 12 bytes
    float            normal[3];   // 12 bytes
    float            texCoord[2]; // 8 bytes
};
static_assert(sizeof(StudioVertex) == 48);

// ============================================================================
// .VTX (Valve Triangle Strip & Index Data)
// ============================================================================

struct VtxHeader {
    int32_t  version;
    int32_t  vertCacheSize;
    uint16_t maxBonesPerStrip;
    uint16_t maxBonesPerTri;
    int32_t  maxBonesPerVert;
    int32_t  checkSum;
    int32_t  numLODs;
    int32_t  materialReplacementListOffset;
    int32_t  numBodyParts;
    int32_t  bodyPartOffset;
};
static_assert(sizeof(VtxHeader) == 36);

struct VtxBodyPart {
    int32_t numModels;
    int32_t modelOffset;
};
static_assert(sizeof(VtxBodyPart) == 8);

struct VtxModel {
    int32_t numLODs;
    int32_t lodOffset;
};
static_assert(sizeof(VtxModel) == 8);

struct VtxModelLOD {
    int32_t numMeshes;
    int32_t meshOffset;
    float   switchPoint;
};
static_assert(sizeof(VtxModelLOD) == 12);

struct VtxMesh {
    int32_t numStripGroups;
    int32_t stripGroupHeaderOffset;
    uint8_t flags;
};
static_assert(sizeof(VtxMesh) == 9);

struct VtxStripGroup {
    int32_t numVerts;
    int32_t vertOffset;
    int32_t numIndices;
    int32_t indexOffset;
    int32_t numStrips;
    int32_t stripOffset;
    uint8_t flags;
};
static_assert(sizeof(VtxStripGroup) == 25);

struct VtxStrip {
    int32_t  numIndices;
    int32_t  indexOffset;
    int32_t  numVerts;
    int32_t  vertOffset;
    int16_t  numBones;
    uint8_t  flags;
    int32_t  numBoneStateChanges;
    int32_t  boneStateChangeOffset;
};
static_assert(sizeof(VtxStrip) == 27);

struct VtxVertex {
    uint8_t  boneWeightIndex[3];
    uint8_t  numBones;
    uint16_t origMeshVertID;
    int8_t   boneID[3];
};
static_assert(sizeof(VtxVertex) == 9);

// ============================================================================
// .PHY (Physics Collision Data)
// ============================================================================

struct PhyHeader {
    int32_t size;        // sizeof(PhyHeader) = 16
    int32_t id;          // 0
    int32_t solidCount;  // number of solids
    int32_t checksum;
};
static_assert(sizeof(PhyHeader) == 16);

struct PhyCompactSurface {
    int32_t size;             // size of surface data
    char    vphysicsId[4];    // "VPHY"
    int16_t version;          // 0x0100
    int16_t modelType;
    int32_t surfaceSize;
    float   dragAxisAreas[3];
    int32_t axisMapSize;
};
static_assert(sizeof(PhyCompactSurface) == 32);

#pragma pack(pop)

} // namespace ZHLN::BSP
