// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include <BSP/BSPGeometry.hpp>
#include <BSP/StudioModelLoader.hpp>
#include <BSP/StudioModelTypes.hpp>
#include <TestsFramework.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include "helpers/StudioFixture.hpp"
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using ZHLN::Test::StudioFixture::BuildSyntheticMDL;
using ZHLN::Test::StudioFixture::BuildSyntheticVVD;
using ZHLN::Test::StudioFixture::BuildSyntheticVTX;
using ZHLN::Test::StudioFixture::BuildSyntheticPHY;

} // namespace

namespace {

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

} // namespace
