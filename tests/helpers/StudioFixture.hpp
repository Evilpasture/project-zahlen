// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "BinaryWriter.hpp"
#include <BSP/StudioModelTypes.hpp>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>
namespace ZHLN::Test::StudioFixture {
using ZHLN::Test::BinaryWriter;

// Builds a synthetic minimal .MDL buffer
inline auto BuildSyntheticMDL(
    int32_t                                 checksum,
    std::string_view                        modelName,
    std::initializer_list<std::string_view> cdTextures,
    std::initializer_list<std::string_view> textureNames,
    int32_t                                 numBodyParts = 1
) -> std::vector<std::byte> {
    BinaryWriter buf;
    const std::span<const std::string_view> cdTexSpan  {cdTextures.begin(), cdTextures.size()};
    const std::span<const std::string_view> texNameSpan {textureNames.begin(), textureNames.size()};

    auto align4 = [](auto& b) {
        while (b.size() % 4 != 0) {
            b.push_back(std::byte {0});
        }
    };

    ZHLN::BSP::StudioHdr hdr {};
    hdr.id = ZHLN::BSP::kStudioHdrMagic;
    hdr.version = 48;
    hdr.checksum = checksum;
    std::strncpy(hdr.name, modelName.data(), sizeof(hdr.name) - 1);
    hdr.hull_min[0] = -10.0f;
    hdr.hull_min[1] = -10.0f;
    hdr.hull_min[2] = 0.0f;
    hdr.hull_max[0] = 10.0f;
    hdr.hull_max[1] = 10.0f;
    hdr.hull_max[2] = 40.0f;

    // Placeholder for header
    buf.resize(sizeof(ZHLN::BSP::StudioHdr), std::byte {0});

    // 1. cdtextures
    hdr.numcdtextures = static_cast<int32_t>(cdTexSpan.size());
    hdr.cdtextureindex = static_cast<int32_t>(buf.size());

    // Vector of offsets followed by strings
    const size_t offsetTablePos = buf.size();
    const size_t offsetTableSize = cdTexSpan.size() * sizeof(int32_t);
    buf.resize(offsetTablePos + offsetTableSize, std::byte {0});

    for (size_t i = 0; i < cdTexSpan.size(); ++i) {
        const int32_t strOffset = static_cast<int32_t>(buf.size());
        std::memcpy(buf.data() + offsetTablePos + i * sizeof(int32_t), &strOffset, sizeof(int32_t));
        buf.append(std::span<const char> {cdTexSpan[i].data(), cdTexSpan[i].size()});
        buf.push_back(std::byte {0});
        align4(buf);
    }

    // 2. textures
    align4(buf);
    hdr.numtextures = static_cast<int32_t>(texNameSpan.size());
    hdr.textureindex = static_cast<int32_t>(buf.size());

    const size_t texArrayStart = buf.size();
    const size_t texArraySize = texNameSpan.size() * sizeof(ZHLN::BSP::StudioTexture);
    buf.resize(texArrayStart + texArraySize, std::byte {0});

    for (size_t i = 0; i < texNameSpan.size(); ++i) {
        const size_t texStructPos = texArrayStart + i * sizeof(ZHLN::BSP::StudioTexture);
        const int32_t relOffset = static_cast<int32_t>(buf.size() - texStructPos);
        ZHLN::BSP::StudioTexture tex {};
        tex.sznameindex = relOffset;
        std::memcpy(buf.data() + texStructPos, &tex, sizeof(tex));
        buf.append(std::span<const char> {texNameSpan[i].data(), texNameSpan[i].size()});
        buf.push_back(std::byte {0});
        align4(buf);
    }

    // 3. bodyparts
    align4(buf);
    hdr.numbodyparts = numBodyParts;
    hdr.bodypartindex = static_cast<int32_t>(buf.size());

    for (int32_t bp = 0; bp < numBodyParts; ++bp) {
        align4(buf);
        const size_t bpStart = buf.size();
        ZHLN::BSP::StudioBodyPart bodyPart {};
        bodyPart.sznameindex = 0;
        bodyPart.nummodels = 1;
        bodyPart.base = 1;
        bodyPart.modelindex = sizeof(ZHLN::BSP::StudioBodyPart);
        buf.append_object(bodyPart);

        // One model
        align4(buf);
        const size_t modelStart = buf.size();
        ZHLN::BSP::StudioModel model {};
        std::strncpy(model.name, "submodel0", sizeof(model.name) - 1);
        model.nummeshes = 1;
        model.meshindex = sizeof(ZHLN::BSP::StudioModel);
        model.numvertices = 3;
        model.vertexindex = 0;
        buf.append_object(model);

        // One mesh
        align4(buf);
        ZHLN::BSP::StudioMesh mesh {};
        mesh.material = 0;
        mesh.modelindex = -static_cast<int32_t>(sizeof(ZHLN::BSP::StudioModel));
        mesh.numvertices = 3;
        mesh.vertexoffset = 0;
        buf.append_object(mesh);
    }

    align4(buf);
    hdr.length = static_cast<int32_t>(buf.size());
    std::memcpy(buf.data(), &hdr, sizeof(hdr));

    return std::move(buf).take();
}

// Builds a synthetic minimal .VVD buffer
inline auto BuildSyntheticVVD(
    int32_t                                    checksum,
    std::span<const ZHLN::BSP::StudioVertex>   vertices,
    std::span<const ZHLN::BSP::VvdFixup>       fixups = {}
) -> std::vector<std::byte> {
    BinaryWriter buf;

    ZHLN::BSP::VvdHeader hdr {};
    hdr.id = ZHLN::BSP::kVvdMagic;
    hdr.version = 4;
    hdr.checksum = checksum;
    hdr.numLODs = 1;
    hdr.numLODVertexes[0] = static_cast<int32_t>(vertices.size());

    buf.resize(sizeof(ZHLN::BSP::VvdHeader), std::byte {0});

    if (!fixups.empty()) {
        hdr.numFixups = static_cast<int32_t>(fixups.size());
        hdr.fixupTableStart = static_cast<int32_t>(buf.size());
        for (const auto& f : fixups) {
            buf.append_object(f);
        }
    } else {
        hdr.numFixups = 0;
        hdr.fixupTableStart = 0;
    }

    hdr.vertexDataStart = static_cast<int32_t>(buf.size());
    for (const auto& v : vertices) {
        buf.append_object(v);
    }

    hdr.tangentDataStart = 0;

    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    return std::move(buf).take();
}

// Builds a synthetic minimal .VTX buffer (LOD 0, 1 bodypart, 1 model, 1 mesh, 1 stripgroup)
inline auto BuildSyntheticVTX(
    int32_t                                 checksum,
    std::span<const ZHLN::BSP::VtxVertex>   vertices,
    std::span<const uint16_t>               indices,
    bool                                    isTriStrip = false
) -> std::vector<std::byte> {
    BinaryWriter buf;

    ZHLN::BSP::VtxHeader hdr {};
    hdr.version = ZHLN::BSP::kVtxVersion;
    hdr.vertCacheSize = 512;
    hdr.maxBonesPerStrip = 512;
    hdr.maxBonesPerTri = 512;
    hdr.maxBonesPerVert = 3;
    hdr.checkSum = checksum;
    hdr.numLODs = 1;
    hdr.materialReplacementListOffset = 0;
    hdr.numBodyParts = 1;
    hdr.bodyPartOffset = sizeof(ZHLN::BSP::VtxHeader);

    buf.resize(sizeof(ZHLN::BSP::VtxHeader), std::byte {0});

    // BodyPart
    const size_t bpStart = buf.size();
    ZHLN::BSP::VtxBodyPart bp {};
    bp.numModels = 1;
    bp.modelOffset = sizeof(ZHLN::BSP::VtxBodyPart);
    buf.append_object(bp);

    // Model
    const size_t modelStart = buf.size();
    ZHLN::BSP::VtxModel model {};
    model.numLODs = 1;
    model.lodOffset = sizeof(ZHLN::BSP::VtxModel);
    buf.append_object(model);

    // Model LOD 0
    const size_t lodStart = buf.size();
    ZHLN::BSP::VtxModelLOD lod {};
    lod.numMeshes = 1;
    lod.meshOffset = sizeof(ZHLN::BSP::VtxModelLOD);
    lod.switchPoint = 0.0f;
    buf.append_object(lod);

    // Mesh
    const size_t meshStart = buf.size();
    ZHLN::BSP::VtxMesh mesh {};
    mesh.numStripGroups = 1;
    mesh.stripGroupHeaderOffset = sizeof(ZHLN::BSP::VtxMesh);
    mesh.flags = 0;
    buf.append_object(mesh);

    // StripGroup
    const size_t sgStart = buf.size();
    ZHLN::BSP::VtxStripGroup sg {};
    sg.numVerts = static_cast<int32_t>(vertices.size());
    sg.numIndices = static_cast<int32_t>(indices.size());
    sg.numStrips = 1;
    sg.flags = 0;

    // We will fill offsets after sizing
    buf.append_object(sg);

    // Vertices
    while (buf.size() % 4 != 0) {
        buf.push_back(std::byte {0});
    }
    const size_t vertOfs = buf.size() - sgStart;
    for (const auto& v : vertices) {
        buf.append_object(v);
    }

    // Indices
    while (buf.size() % 4 != 0) {
        buf.push_back(std::byte {0});
    }
    const size_t idxOfs = buf.size() - sgStart;
    for (uint16_t idx : indices) {
        buf.append_object(idx);
    }

    // Strips
    while (buf.size() % 4 != 0) {
        buf.push_back(std::byte {0});
    }
    const size_t stripOfs = buf.size() - sgStart;
    ZHLN::BSP::VtxStrip strip {};
    strip.numIndices = static_cast<int32_t>(indices.size());
    strip.indexOffset = 0;
    strip.numVerts = static_cast<int32_t>(vertices.size());
    strip.vertOffset = 0;
    strip.numBones = 1;
    strip.flags = isTriStrip ? 0x02 : 0x01;
    buf.append_object(strip);

    // Patch offsets in StripGroup
    sg.vertOffset = static_cast<int32_t>(vertOfs);
    sg.indexOffset = static_cast<int32_t>(idxOfs);
    sg.stripOffset = static_cast<int32_t>(stripOfs);
    std::memcpy(buf.data() + sgStart, &sg, sizeof(sg));

    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    return std::move(buf).take();
}

// Builds a synthetic minimal .PHY buffer
inline auto BuildSyntheticPHY(int32_t checksum, int32_t solidCount = 1) -> std::vector<std::byte> {
    BinaryWriter buf;

    ZHLN::BSP::PhyHeader hdr {};
    hdr.size = sizeof(ZHLN::BSP::PhyHeader);
    hdr.id = 0;
    hdr.solidCount = solidCount;
    hdr.checksum = checksum;

    buf.resize(sizeof(ZHLN::BSP::PhyHeader), std::byte {0});

    for (int32_t i = 0; i < solidCount; ++i) {
        ZHLN::BSP::PhyCompactSurface surf {};
        surf.size = static_cast<int32_t>(sizeof(ZHLN::BSP::PhyCompactSurface) - sizeof(int32_t));
        std::memcpy(surf.vphysicsId, "VPHY", 4);
        surf.version = 0x0100;
        surf.modelType = 0;
        surf.surfaceSize = 0;
        buf.append_object(surf);
    }

    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    return std::move(buf).take();
}

} // namespace ZHLN::Test::StudioFixture
