// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <BSP/BSPGeometry.hpp>
#include <BSP/StudioModelLoader.hpp>
#include <BSP/StudioModelTypes.hpp>
#include <TestsFramework.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <cmath>
#include <cstring>
#include <vector>

namespace ZHLN {
std::string GetPoorMansStacktrace() {
    return "test_stacktrace";
}
} // namespace ZHLN

namespace {

template <typename T>
void AppendBytes(std::vector<std::byte>& buf, const T& val) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto* ptr = reinterpret_cast<const std::byte*>(&val);
    buf.insert(buf.end(), ptr, ptr + sizeof(T));
}

void AppendRawBytes(std::vector<std::byte>& buf, const void* data, size_t size) {
    const auto* ptr = static_cast<const std::byte*>(data);
    buf.insert(buf.end(), ptr, ptr + size);
}

// Builds a synthetic minimal .MDL buffer
auto BuildSyntheticMDL(
    int32_t checksum,
    std::string_view modelName,
    const std::vector<std::string>& cdTextures,
    const std::vector<std::string>& textureNames,
    int32_t numBodyParts = 1
) -> std::vector<std::byte> {
    std::vector<std::byte> buf;

    auto align4 = [](std::vector<std::byte>& b) {
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
    hdr.numcdtextures = static_cast<int32_t>(cdTextures.size());
    hdr.cdtextureindex = static_cast<int32_t>(buf.size());

    // Vector of offsets followed by strings
    const size_t offsetTablePos = buf.size();
    const size_t offsetTableSize = cdTextures.size() * sizeof(int32_t);
    buf.resize(offsetTablePos + offsetTableSize, std::byte {0});

    for (size_t i = 0; i < cdTextures.size(); ++i) {
        const int32_t strOffset = static_cast<int32_t>(buf.size());
        std::memcpy(buf.data() + offsetTablePos + i * sizeof(int32_t), &strOffset, sizeof(int32_t));
        AppendRawBytes(buf, cdTextures[i].c_str(), cdTextures[i].size() + 1);
        align4(buf);
    }

    // 2. textures
    align4(buf);
    hdr.numtextures = static_cast<int32_t>(textureNames.size());
    hdr.textureindex = static_cast<int32_t>(buf.size());

    const size_t texArrayStart = buf.size();
    const size_t texArraySize = textureNames.size() * sizeof(ZHLN::BSP::StudioTexture);
    buf.resize(texArrayStart + texArraySize, std::byte {0});

    for (size_t i = 0; i < textureNames.size(); ++i) {
        const size_t texStructPos = texArrayStart + i * sizeof(ZHLN::BSP::StudioTexture);
        const int32_t relOffset = static_cast<int32_t>(buf.size() - texStructPos);
        ZHLN::BSP::StudioTexture tex {};
        tex.sznameindex = relOffset;
        std::memcpy(buf.data() + texStructPos, &tex, sizeof(tex));
        AppendRawBytes(buf, textureNames[i].c_str(), textureNames[i].size() + 1);
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
        AppendBytes(buf, bodyPart);

        // One model
        align4(buf);
        const size_t modelStart = buf.size();
        ZHLN::BSP::StudioModel model {};
        std::strncpy(model.name, "submodel0", sizeof(model.name) - 1);
        model.nummeshes = 1;
        model.meshindex = sizeof(ZHLN::BSP::StudioModel);
        model.numvertices = 3;
        model.vertexindex = 0;
        AppendBytes(buf, model);

        // One mesh
        align4(buf);
        ZHLN::BSP::StudioMesh mesh {};
        mesh.material = 0;
        mesh.modelindex = -static_cast<int32_t>(sizeof(ZHLN::BSP::StudioModel));
        mesh.numvertices = 3;
        mesh.vertexoffset = 0;
        AppendBytes(buf, mesh);
    }

    align4(buf);
    hdr.length = static_cast<int32_t>(buf.size());
    std::memcpy(buf.data(), &hdr, sizeof(hdr));

    return buf;
}

// Builds a synthetic minimal .VVD buffer
auto BuildSyntheticVVD(
    int32_t checksum,
    const std::vector<ZHLN::BSP::StudioVertex>& vertices,
    const std::vector<ZHLN::BSP::VvdFixup>& fixups = {}
) -> std::vector<std::byte> {
    std::vector<std::byte> buf;

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
            AppendBytes(buf, f);
        }
    } else {
        hdr.numFixups = 0;
        hdr.fixupTableStart = 0;
    }

    hdr.vertexDataStart = static_cast<int32_t>(buf.size());
    for (const auto& v : vertices) {
        AppendBytes(buf, v);
    }

    hdr.tangentDataStart = 0;

    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    return buf;
}

// Builds a synthetic minimal .VTX buffer (LOD 0, 1 bodypart, 1 model, 1 mesh, 1 stripgroup)
auto BuildSyntheticVTX(
    int32_t checksum,
    const std::vector<ZHLN::BSP::VtxVertex>& vertices,
    const std::vector<uint16_t>& indices,
    bool isTriStrip = false
) -> std::vector<std::byte> {
    std::vector<std::byte> buf;

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
    AppendBytes(buf, bp);

    // Model
    const size_t modelStart = buf.size();
    ZHLN::BSP::VtxModel model {};
    model.numLODs = 1;
    model.lodOffset = sizeof(ZHLN::BSP::VtxModel);
    AppendBytes(buf, model);

    // Model LOD 0
    const size_t lodStart = buf.size();
    ZHLN::BSP::VtxModelLOD lod {};
    lod.numMeshes = 1;
    lod.meshOffset = sizeof(ZHLN::BSP::VtxModelLOD);
    lod.switchPoint = 0.0f;
    AppendBytes(buf, lod);

    // Mesh
    const size_t meshStart = buf.size();
    ZHLN::BSP::VtxMesh mesh {};
    mesh.numStripGroups = 1;
    mesh.stripGroupHeaderOffset = sizeof(ZHLN::BSP::VtxMesh);
    mesh.flags = 0;
    AppendBytes(buf, mesh);

    // StripGroup
    const size_t sgStart = buf.size();
    ZHLN::BSP::VtxStripGroup sg {};
    sg.numVerts = static_cast<int32_t>(vertices.size());
    sg.numIndices = static_cast<int32_t>(indices.size());
    sg.numStrips = 1;
    sg.flags = 0;

    // We will fill offsets after sizing
    AppendBytes(buf, sg);

    // Vertices
    while (buf.size() % 4 != 0) {
        buf.push_back(std::byte {0});
    }
    const size_t vertOfs = buf.size() - sgStart;
    for (const auto& v : vertices) {
        AppendBytes(buf, v);
    }

    // Indices
    while (buf.size() % 4 != 0) {
        buf.push_back(std::byte {0});
    }
    const size_t idxOfs = buf.size() - sgStart;
    for (uint16_t idx : indices) {
        AppendBytes(buf, idx);
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
    AppendBytes(buf, strip);

    // Patch offsets in StripGroup
    sg.vertOffset = static_cast<int32_t>(vertOfs);
    sg.indexOffset = static_cast<int32_t>(idxOfs);
    sg.stripOffset = static_cast<int32_t>(stripOfs);
    std::memcpy(buf.data() + sgStart, &sg, sizeof(sg));

    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    return buf;
}

// Builds a synthetic minimal .PHY buffer
auto BuildSyntheticPHY(int32_t checksum, int32_t solidCount = 1) -> std::vector<std::byte> {
    std::vector<std::byte> buf;

    ZHLN::BSP::PhyHeader hdr {};
    hdr.size = sizeof(ZHLN::BSP::PhyHeader);
    hdr.id = 0;
    hdr.solidCount = solidCount;
    hdr.checksum = checksum;

    buf.resize(sizeof(ZHLN::BSP::PhyHeader), std::byte {0});

    for (int32_t i = 0; i < solidCount; ++i) {
        ZHLN::BSP::PhyCompactSurface surf {};
        surf.size = sizeof(ZHLN::BSP::PhyCompactSurface);
        std::memcpy(surf.vphysicsId, "VPHY", 4);
        surf.version = 0x0100;
        surf.modelType = 0;
        surf.surfaceSize = 0;
        AppendBytes(buf, surf);
    }

    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    return buf;
}

} // namespace

struct BSPStudioModelTestSuite {
    struct Tests {
        auto studiomodel_header_parsing() -> std::expected<void, std::string> {
            const int32_t checksum = 0x12345678;
            const auto mdl = BuildSyntheticMDL(
                checksum,
                "models/props_c17/chair.mdl",
                {"models/props_c17/", "models/props_junk/"},
                {"chair01", "cushion01"}
            );

            // Create minimal matching VVD and VTX
            const auto vvd = BuildSyntheticVVD(checksum, {});
            const auto vtx = BuildSyntheticVTX(checksum, {}, {});

            const auto res = ZHLN::BSP::ParseStudioModel(mdl, vvd, vtx);
            ZHLN::Test::ExpectTrue(res.has_value());

            const auto& data = *res;
            ZHLN::Test::ExpectEq(data.checksum, checksum);
            ZHLN::Test::ExpectEq(data.name, "models/props_c17/chair.mdl");
            ZHLN::Test::ExpectEq(data.textureSearchPaths.size(), size_t {2});
            ZHLN::Test::ExpectEq(data.textureSearchPaths[0], "models/props_c17/");
            ZHLN::Test::ExpectEq(data.textureNames.size(), size_t {2});
            ZHLN::Test::ExpectEq(data.textureNames[0], "chair01");
            ZHLN::Test::ExpectEq(data.textureNames[1], "cushion01");
            ZHLN::Test::ExpectEq(data.materialNames[0], "models/props_c17/chair01");

            return {};
        }

        auto checksum_mismatch_detection() -> std::expected<void, std::string> {
            const auto mdl = BuildSyntheticMDL(0x11111111, "test.mdl", {}, {});
            const auto vvd = BuildSyntheticVVD(0x22222222, {}); // Mismatch
            const auto vtx = BuildSyntheticVTX(0x11111111, {}, {});

            const auto res = ZHLN::BSP::ParseStudioModel(mdl, vvd, vtx);
            ZHLN::Test::ExpectFalse(res.has_value());

            return {};
        }

        auto vvd_fixup_table_lod() -> std::expected<void, std::string> {
            const int32_t checksum = 0xABCDEF01;

            // 4 raw vertices
            std::vector<ZHLN::BSP::StudioVertex> rawVerts(4);
            rawVerts[0].pos[0] = 10.0f;
            rawVerts[1].pos[0] = 20.0f;
            rawVerts[2].pos[0] = 30.0f;
            rawVerts[3].pos[0] = 40.0f;

            // Fixups: only take index 2 and 3 for LOD 0
            std::vector<ZHLN::BSP::VvdFixup> fixups(2);
            fixups[0].lod = 0;
            fixups[0].sourceVertexID = 2;
            fixups[0].numVertexes = 2;

            fixups[1].lod = 1; // LOD 1 only
            fixups[1].sourceVertexID = 0;
            fixups[1].numVertexes = 2;

            const auto mdl = BuildSyntheticMDL(checksum, "test.mdl", {}, {"mat"});
            const auto vvd = BuildSyntheticVVD(checksum, rawVerts, fixups);

            // VTX referencing vertex 0 and 1 of LOD 0
            std::vector<ZHLN::BSP::VtxVertex> vtxVerts(2);
            vtxVerts[0].origMeshVertID = 0;
            vtxVerts[1].origMeshVertID = 1;
            std::vector<uint16_t> indices = {0, 1, 0};
            const auto vtx = BuildSyntheticVTX(checksum, vtxVerts, indices);

            const auto res = ZHLN::BSP::ParseStudioModel(mdl, vvd, vtx);
            ZHLN::Test::ExpectTrue(res.has_value());

            const auto& data = *res;
            ZHLN::Test::ExpectEq(data.parts.size(), size_t {1});
            ZHLN::Test::ExpectEq(data.parts[0].VertexCount(), uint32_t {2});

            // LOD 0 should have taken rawVerts[2] (pos 30) and rawVerts[3] (pos 40)
            // Converted to meters: 30 * 0.0254 = 0.762
            ZHLN::Test::ExpectInRange(data.parts[0].positions[0].position[0], 0.76f, 0.77f);
            ZHLN::Test::ExpectInRange(data.parts[0].positions[1].position[0], 1.01f, 1.02f);

            return {};
        }

        auto vtx_strip_groups_and_indices() -> std::expected<void, std::string> {
            const int32_t checksum = 0x55555555;

            std::vector<ZHLN::BSP::StudioVertex> verts(3);
            // Triangle in Source space: Z=0
            verts[0].pos[0] = 0.0f;  verts[0].pos[1] = 0.0f;  verts[0].pos[2] = 0.0f;
            verts[1].pos[0] = 10.0f; verts[1].pos[1] = 0.0f;  verts[1].pos[2] = 0.0f;
            verts[2].pos[0] = 0.0f;  verts[2].pos[1] = 10.0f; verts[2].pos[2] = 0.0f;

            std::vector<ZHLN::BSP::VtxVertex> vtxVerts(3);
            vtxVerts[0].origMeshVertID = 0;
            vtxVerts[1].origMeshVertID = 1;
            vtxVerts[2].origMeshVertID = 2;

            std::vector<uint16_t> indices = {0, 1, 2};

            const auto mdl = BuildSyntheticMDL(checksum, "tri.mdl", {"models/"}, {"wood"});
            const auto vvd = BuildSyntheticVVD(checksum, verts);
            const auto vtx = BuildSyntheticVTX(checksum, vtxVerts, indices, false); // Trilist

            const auto res = ZHLN::BSP::ParseStudioModel(mdl, vvd, vtx);
            ZHLN::Test::ExpectTrue(res.has_value());

            const auto& data = *res;
            ZHLN::Test::ExpectEq(data.parts.size(), size_t {1});
            ZHLN::Test::ExpectEq(data.parts[0].IndexCount(), uint32_t {3});
            ZHLN::Test::ExpectEq(data.parts[0].indices[0], uint32_t {0});
            ZHLN::Test::ExpectEq(data.parts[0].indices[1], uint32_t {1});
            ZHLN::Test::ExpectEq(data.parts[0].indices[2], uint32_t {2});

            return {};
        }

        auto phy_collision_parsing() -> std::expected<void, std::string> {
            const int32_t checksum = 0x77777777;

            const auto mdl = BuildSyntheticMDL(checksum, "prop.mdl", {}, {"mat"});
            const auto vvd = BuildSyntheticVVD(checksum, {});
            const auto vtx = BuildSyntheticVTX(checksum, {}, {});
            const auto phy = BuildSyntheticPHY(checksum, 2);

            const auto res = ZHLN::BSP::ParseStudioModel(mdl, vvd, vtx, phy);
            ZHLN::Test::ExpectTrue(res.has_value());

            const auto& data = *res;
            ZHLN::Test::ExpectEq(data.physicsSolids.size(), size_t {2});

            return {};
        }

        auto end_to_end_studiomodel_assembly() -> std::expected<void, std::string> {
            const int32_t checksum = 0x99999999;

            std::vector<ZHLN::BSP::StudioVertex> verts(3);
            // Source coordinates: (X=100, Y=200, Z=50), Normal=(0, 0, 1), UV=(0.5, 0.5)
            verts[0].pos[0] = 100.0f; verts[0].pos[1] = 200.0f; verts[0].pos[2] = 50.0f;
            verts[0].normal[0] = 0.0f; verts[0].normal[1] = 0.0f; verts[0].normal[2] = 1.0f;
            verts[0].texCoord[0] = 0.5f; verts[0].texCoord[1] = 0.5f;

            verts[1].pos[0] = 150.0f; verts[1].pos[1] = 200.0f; verts[1].pos[2] = 50.0f;
            verts[1].normal[0] = 0.0f; verts[1].normal[1] = 0.0f; verts[1].normal[2] = 1.0f;
            verts[1].texCoord[0] = 1.0f; verts[1].texCoord[1] = 0.5f;

            verts[2].pos[0] = 100.0f; verts[2].pos[1] = 250.0f; verts[2].pos[2] = 50.0f;
            verts[2].normal[0] = 0.0f; verts[2].normal[1] = 0.0f; verts[2].normal[2] = 1.0f;
            verts[2].texCoord[0] = 0.5f; verts[2].texCoord[1] = 1.0f;

            std::vector<ZHLN::BSP::VtxVertex> vtxVerts(3);
            vtxVerts[0].origMeshVertID = 0;
            vtxVerts[1].origMeshVertID = 1;
            vtxVerts[2].origMeshVertID = 2;

            std::vector<uint16_t> indices = {0, 1, 2};

            const auto mdl = BuildSyntheticMDL(checksum, "models/props/test.mdl", {"models/props/"}, {"test_diffuse"});
            const auto vvd = BuildSyntheticVVD(checksum, verts);
            const auto vtx = BuildSyntheticVTX(checksum, vtxVerts, indices);
            const auto phy = BuildSyntheticPHY(checksum, 1);

            ZHLN::BSP::ImportOptions options;
            options.unitScale = 0.0254f;

            const auto res = ZHLN::BSP::ParseStudioModel(mdl, vvd, vtx, phy, options);
            ZHLN::Test::ExpectTrue(res.has_value());

            const auto& data = *res;
            ZHLN::Test::ExpectEq(data.parts.size(), size_t {1});

            const auto& part = data.parts[0];
            ZHLN::Test::ExpectEq(part.materialName, "models/props/test_diffuse");
            ZHLN::Test::ExpectEq(part.VertexCount(), uint32_t {3});
            ZHLN::Test::ExpectEq(part.IndexCount(), uint32_t {3});

            // Coordinate verification: Source (100, 200, 50) -> Engine (100, 50, -200) * 0.0254
            // X = 2.54, Y = 1.27, Z = -5.08
            ZHLN::Test::ExpectInRange(part.positions[0].position[0], 2.53f, 2.55f);
            ZHLN::Test::ExpectInRange(part.positions[0].position[1], 1.26f, 1.28f);
            ZHLN::Test::ExpectInRange(part.positions[0].position[2], -5.09f, -5.07f);

            // Bounds check
            ZHLN::Test::ExpectInRange(part.boundsMin.x, 2.53f, 2.55f);
            ZHLN::Test::ExpectInRange(part.boundsMax.x, 3.80f, 3.82f);

            return {};
        }
    };
};

int main() {
    BSPStudioModelTestSuite::Tests suite;
    int                            passed = 0;
    int                            failed = 0;

    auto runTest = [&](std::string_view testName, auto testFunc) {
        ZHLN::Println("\033[32m[ RUN      ]\033[0m {}", testName);
        auto res = testFunc();
        if (res.has_value()) {
            ZHLN::Println("\033[32m[       OK ]\033[0m {}", testName);
            passed++;
        } else {
            ZHLN::Println("\033[31m[  FAILED  ]\033[0m {}", testName);
            failed++;
        }
    };

    ZHLN::Println("\033[36m==================================================\033[0m");
    ZHLN::Println("\033[36mRunning Test Suite: BSPStudioModelTestSuite\033[0m");
    ZHLN::Println("\033[36m==================================================\033[0m");

    runTest("studiomodel_header_parsing", [&] { return suite.studiomodel_header_parsing(); });
    runTest("checksum_mismatch_detection", [&] { return suite.checksum_mismatch_detection(); });
    runTest("vvd_fixup_table_lod", [&] { return suite.vvd_fixup_table_lod(); });
    runTest("vtx_strip_groups_and_indices", [&] { return suite.vtx_strip_groups_and_indices(); });
    runTest("phy_collision_parsing", [&] { return suite.phy_collision_parsing(); });
    runTest("end_to_end_studiomodel_assembly", [&] { return suite.end_to_end_studiomodel_assembly(); });

    ZHLN::Println("--------------------------------------------------");
    ZHLN::Println("Summary for BSPStudioModelTestSuite: {} Passed, {} Failed", passed, failed);
    ZHLN::Println("==================================================");

    return failed == 0 ? 0 : 1;
}
