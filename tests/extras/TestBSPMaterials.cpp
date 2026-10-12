// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <BSP/BSPGeometry.hpp>
#include <BSP/BSPRead.hpp>
#include <BSP/SourceVFS.hpp>
#include <BSP/VMTParser.hpp>
#include <BSP/VTFDecoder.hpp>
#include <TestsFramework.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Vertex.hpp>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ZHLN {
std::string GetPoorMansStacktrace() {
    return "test_stacktrace";
}
} // namespace ZHLN

namespace {

auto BuildSyntheticLightmapMap() -> ZHLN::BSP::BSPMap {
    ZHLN::BSP::BSPMap map;
    map.version = 20;

    // Submodel 0: World
    ZHLN::BSP::DModel worldModel {};
    worldModel.mins[0]   = -100.0f;
    worldModel.mins[1]   = -100.0f;
    worldModel.mins[2]   = 0.0f;
    worldModel.maxs[0]   = 100.0f;
    worldModel.maxs[1]   = 100.0f;
    worldModel.maxs[2]   = 100.0f;
    worldModel.firstface = 0;
    worldModel.numfaces  = 1;
    map.models.push_back(worldModel);

    // Vertex positions (16x16 face on ground, Z=0)
    map.vertices.push_back(ZHLN::BSP::DVertex {{0.0f, 0.0f, 0.0f}});
    map.vertices.push_back(ZHLN::BSP::DVertex {{256.0f, 0.0f, 0.0f}});
    map.vertices.push_back(ZHLN::BSP::DVertex {{256.0f, 256.0f, 0.0f}});
    map.vertices.push_back(ZHLN::BSP::DVertex {{0.0f, 256.0f, 0.0f}});

    // Edges
    map.edges.push_back(ZHLN::BSP::DEdge {{0, 1}});
    map.edges.push_back(ZHLN::BSP::DEdge {{1, 2}});
    map.edges.push_back(ZHLN::BSP::DEdge {{2, 3}});
    map.edges.push_back(ZHLN::BSP::DEdge {{3, 0}});

    map.surfEdges.push_back(0);
    map.surfEdges.push_back(1);
    map.surfEdges.push_back(2);
    map.surfEdges.push_back(3);

    // Plane (facing +Z)
    map.planes.push_back(ZHLN::BSP::DPlane {{0.0f, 0.0f, 1.0f}, 0.0f, 2});

    // Texture data & string table
    ZHLN::BSP::DTexData texData {};
    texData.nameStringTableID = 0;
    texData.width             = 512;
    texData.height            = 512;
    map.texDatas.push_back(texData);
    map.texNames.push_back("concrete/floor01");

    // Texture info with lightmap vectors (1 luxel per 16 units)
    ZHLN::BSP::DTexInfo texInfo {};
    texInfo.textureVecs[0][0] = 1.0f;
    texInfo.textureVecs[0][3] = 0.0f;
    texInfo.textureVecs[1][1] = 1.0f;
    texInfo.textureVecs[1][3] = 0.0f;

    // Lightmap vectors: 1/16 scale
    texInfo.lightmapVecs[0][0] = 1.0f / 16.0f;
    texInfo.lightmapVecs[0][3] = 0.0f;
    texInfo.lightmapVecs[1][1] = 1.0f / 16.0f;
    texInfo.lightmapVecs[1][3] = 0.0f;
    texInfo.texdata            = 0;
    map.texInfos.push_back(texInfo);

    // Face: 256x256 units = 16x16 luxels (mins: 0, 0, size: 16, 16)
    ZHLN::BSP::DFace face {};
    face.planenum                      = 0;
    face.firstedge                     = 0;
    face.numedges                      = 4;
    face.texinfo                       = 0;
    face.lightmapTextureMinsInLuxels[0] = 0;
    face.lightmapTextureMinsInLuxels[1] = 0;
    face.lightmapTextureSizeInLuxels[0] = 16;
    face.lightmapTextureSizeInLuxels[1] = 16;
    face.lightofs                      = 0;
    map.faces.push_back(face);

    // Entity 0: worldspawn
    ZHLN::BSP::BSPEntity ent;
    ent.keys.push_back({"classname", "worldspawn"});
    map.entities.push_back(ent);

    return map;
}

struct BSPMaterialsTestSuite {
    struct Tests {
        std::expected<void, ZHLN::ErrorCode> vfs_case_insensitive_and_search_paths() {
            // Setup a temporary directory hierarchy
            const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "zahlen_test_vfs";
            std::error_code             ec;
            std::filesystem::remove_all(tempRoot, ec);
            std::filesystem::create_directories(tempRoot / "materials" / "brick", ec);
            std::filesystem::create_directories(tempRoot / "materials" / "concrete", ec);
            std::filesystem::create_directories(tempRoot / "lightmaps", ec);

            // Create files
            {
                std::ofstream f(tempRoot / "materials" / "brick" / "brickwall003d.vmt");
                f << "\"LightmappedGeneric\"\n{\n\t\"$basetexture\" \"brick/brickwall003d\"\n}\n";
            }
            {
                std::ofstream f(tempRoot / "materials" / "brick" / "brickwall003d.vtf");
                f << "VTF_DUMMY_DATA";
            }
            {
                std::ofstream f(tempRoot / "materials" / "concrete" / "wall.png");
                f << "PNG_DUMMY_DATA";
            }
            {
                std::ofstream f(tempRoot / "lightmaps" / "gm_test_map_lightmap0.png");
                f << "LIGHTMAP_PNG_DATA";
            }

            ZHLN::BSP::SourceVFS vfs;
            vfs.AddSearchPath(tempRoot.string());

            // 1. Verify path normalization
            ZHLN::Test::ExpectEq(ZHLN::BSP::SourceVFS::NormalizePath("Materials\\Brick//BrickWall003d.vmt"), "materials/brick/brickwall003d.vmt");

            // 2. Resolve material with case-mismatch and Windows slashes
            const auto matRes = vfs.ResolveMaterial("BRICK\\BRICKWALL003D");
            ZHLN::Test::ExpectTrue(matRes.has_value());
            if (matRes) {
                ZHLN::Test::ExpectTrue(matRes->ends_with("brickwall003d.vmt"));
            }

            // 3. Resolve texture with extension omitted
            const auto texRes = vfs.ResolveTexture("brick/brickwall003d");
            ZHLN::Test::ExpectTrue(texRes.has_value());
            if (texRes) {
                ZHLN::Test::ExpectTrue(texRes->ends_with("brickwall003d.vtf"));
            }

            // 4. Resolve loose PNG texture
            const auto pngRes = vfs.ResolveTexture("concrete/wall");
            ZHLN::Test::ExpectTrue(pngRes.has_value());
            if (pngRes) {
                ZHLN::Test::ExpectTrue(pngRes->ends_with("wall.png"));
            }

            // 5. Resolve lightmap
            const auto lmRes = vfs.ResolveLightmap("maps/gm_test_map.bsp");
            ZHLN::Test::ExpectTrue(lmRes.has_value());
            if (lmRes) {
                ZHLN::Test::ExpectTrue(lmRes->ends_with("gm_test_map_lightmap0.png"));
            }

            // 6. Read file contents
            const auto fileBytes = vfs.ReadFile("materials/brick/brickwall003d.vtf");
            ZHLN::Test::ExpectTrue(fileBytes.has_value());
            if (fileBytes) {
                const std::string content(reinterpret_cast<const char*>(fileBytes->data()), fileBytes->size());
                ZHLN::Test::ExpectEq(content, "VTF_DUMMY_DATA");
            }

            std::filesystem::remove_all(tempRoot, ec);
            return {};
        }

        std::expected<void, ZHLN::ErrorCode> vmt_parser_extracts_material_properties() {
            const std::string_view vmtSource = R"(
                // Source Material Definition
                "LightmappedGeneric"
                {
                    "$basetexture" "brick/brickwall003d"
                    "$bumpmap" "brick/brickwall003d_normal"
                    "$surfaceprop" "brick"
                    "$translucent" 1
                    "$alphatest" 0
                    "$nocull" 1
                    "$color" "[0.8 0.6 0.4]"
                    "%keywords" "cstrike,de_dust"

                    "proxies"
                    {
                        "AnimatedTexture"
                        {
                            "animatedtexturevar" "$basetexture"
                        }
                    }
                }
            )";

            const auto parseRes = ZHLN::BSP::ParseVMT(vmtSource);
            ZHLN::Test::ExpectTrue(parseRes.has_value());
            if (!parseRes) {
                return std::unexpected(parseRes.error());
            }

            const auto& mat = *parseRes;
            ZHLN::Test::ExpectEq(mat.shader, "LightmappedGeneric");
            ZHLN::Test::ExpectEq(mat.baseTexture, "brick/brickwall003d");
            ZHLN::Test::ExpectEq(mat.bumpMap, "brick/brickwall003d_normal");
            ZHLN::Test::ExpectEq(mat.surfaceProp, "brick");
            ZHLN::Test::ExpectTrue(mat.isTranslucent);
            ZHLN::Test::ExpectFalse(mat.isAlphaTest);
            ZHLN::Test::ExpectTrue(mat.noCull);

            ZHLN::Test::ExpectLt(std::abs(mat.baseColor.x - 0.8f), 1e-3f);
            ZHLN::Test::ExpectLt(std::abs(mat.baseColor.y - 0.6f), 1e-3f);
            ZHLN::Test::ExpectLt(std::abs(mat.baseColor.z - 0.4f), 1e-3f);
            ZHLN::Test::ExpectLt(std::abs(mat.baseColor.w - 1.0f), 1e-3f);

            ZHLN::Test::ExpectTrue(mat.Has("$basetexture"));
            ZHLN::Test::ExpectEq(mat.Get("$surfaceprop"), "brick");
            ZHLN::Test::ExpectTrue(mat.Find("$basetexture").has_value());
            ZHLN::Test::ExpectEq(*mat.Find("$basetexture"), "brick/brickwall003d");
            ZHLN::Test::ExpectFalse(mat.Find("$nonexistent").has_value());

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> vtf_decoder_dxt1_bc1() {
            // Construct a synthetic 4x4 DXT1 VTF
            std::vector<std::byte> vtf(64 + 8);
            auto*                  bytes = reinterpret_cast<uint8_t*>(vtf.data());

            // Header
            std::memcpy(bytes + 0, "VTF\0", 4);
            const uint32_t versionMajor = 7;
            const uint32_t versionMinor = 2;
            const uint32_t headerSize   = 64;
            const uint16_t width        = 4;
            const uint16_t height       = 4;
            const uint16_t frames       = 1;
            const uint32_t format       = 13; // DXT1
            const uint8_t  mipmapCount  = 1;
            const uint32_t lowResFormat = 0xFFFFFFFF; // none

            std::memcpy(bytes + 4, &versionMajor, 4);
            std::memcpy(bytes + 8, &versionMinor, 4);
            std::memcpy(bytes + 12, &headerSize, 4);
            std::memcpy(bytes + 16, &width, 2);
            std::memcpy(bytes + 18, &height, 2);
            std::memcpy(bytes + 24, &frames, 2);
            std::memcpy(bytes + 52, &format, 4);
            std::memcpy(bytes + 56, &mipmapCount, 1);
            std::memcpy(bytes + 57, &lowResFormat, 4);

            // DXT1 block at offset 64: Solid Red
            // RGB565: Red = (31 << 11) = 0xF800
            uint8_t* block = bytes + 64;
            const uint16_t c0 = 0xF800; // Solid Red
            const uint16_t c1 = 0x0000; // Black
            const uint32_t code = 0x00000000; // all pixels index 0 (c0 = Red)

            std::memcpy(block + 0, &c0, 2);
            std::memcpy(block + 2, &c1, 2);
            std::memcpy(block + 4, &code, 4);

            const auto decoded = ZHLN::BSP::DecodeVTF(vtf);
            ZHLN::Test::ExpectTrue(decoded.has_value());
            if (!decoded) {
                return std::unexpected(decoded.error());
            }

            ZHLN::Test::ExpectEq(decoded->width, uint32_t {4});
            ZHLN::Test::ExpectEq(decoded->height, uint32_t {4});
            ZHLN::Test::ExpectEq(decoded->rgba8.size(), size_t {4 * 4 * 4});

            const auto* rgba = reinterpret_cast<const uint8_t*>(decoded->rgba8.data());
            // Check top-left pixel (should be Red: 255, 0, 0, 255)
            ZHLN::Test::ExpectEq(rgba[0], uint8_t {255});
            ZHLN::Test::ExpectEq(rgba[1], uint8_t {0});
            ZHLN::Test::ExpectEq(rgba[2], uint8_t {0});
            ZHLN::Test::ExpectEq(rgba[3], uint8_t {255});

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> vtf_decoder_dxt5_bc3() {
            // Construct a synthetic 4x4 DXT5 VTF
            std::vector<std::byte> vtf(64 + 16);
            auto*                  bytes = reinterpret_cast<uint8_t*>(vtf.data());

            // Header
            std::memcpy(bytes + 0, "VTF\0", 4);
            const uint32_t versionMajor = 7;
            const uint32_t versionMinor = 2;
            const uint32_t headerSize   = 64;
            const uint16_t width        = 4;
            const uint16_t height       = 4;
            const uint16_t frames       = 1;
            const uint32_t format       = 15; // DXT5
            const uint8_t  mipmapCount  = 1;
            const uint32_t lowResFormat = 0xFFFFFFFF; // none

            std::memcpy(bytes + 4, &versionMajor, 4);
            std::memcpy(bytes + 8, &versionMinor, 4);
            std::memcpy(bytes + 12, &headerSize, 4);
            std::memcpy(bytes + 16, &width, 2);
            std::memcpy(bytes + 18, &height, 2);
            std::memcpy(bytes + 24, &frames, 2);
            std::memcpy(bytes + 52, &format, 4);
            std::memcpy(bytes + 56, &mipmapCount, 1);
            std::memcpy(bytes + 57, &lowResFormat, 4);

            // DXT5 block at offset 64:
            // Alpha: a0 = 128, a1 = 0, index 0 for all
            uint8_t* block = bytes + 64;
            block[0] = 128; // a0
            block[1] = 0;   // a1
            std::memset(block + 2, 0, 6); // all indices = 0 (alpha = a0 = 128)

            // Color: Green (0x07E0)
            const uint16_t c0 = 0x07E0;
            const uint16_t c1 = 0x0000;
            const uint32_t code = 0x00000000; // index 0 (c0)
            std::memcpy(block + 8, &c0, 2);
            std::memcpy(block + 10, &c1, 2);
            std::memcpy(block + 12, &code, 4);

            const auto decoded = ZHLN::BSP::DecodeVTF(vtf);
            ZHLN::Test::ExpectTrue(decoded.has_value());
            if (!decoded) {
                return std::unexpected(decoded.error());
            }

            const auto* rgba = reinterpret_cast<const uint8_t*>(decoded->rgba8.data());
            // Green with 128 alpha: 0, 255, 0, 128
            ZHLN::Test::ExpectEq(rgba[0], uint8_t {0});
            ZHLN::Test::ExpectEq(rgba[1], uint8_t {255});
            ZHLN::Test::ExpectEq(rgba[2], uint8_t {0});
            ZHLN::Test::ExpectEq(rgba[3], uint8_t {128});

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> vtf_decoder_bgra8888_and_rgb888() {
            // Synthetic 2x2 BGRA8888 VTF
            std::vector<std::byte> vtf(64 + 2 * 2 * 4);
            auto*                  bytes = reinterpret_cast<uint8_t*>(vtf.data());

            std::memcpy(bytes + 0, "VTF\0", 4);
            const uint32_t versionMajor = 7;
            const uint32_t versionMinor = 2;
            const uint32_t headerSize   = 64;
            const uint16_t width        = 2;
            const uint16_t height       = 2;
            const uint16_t frames       = 1;
            const uint32_t format       = 12; // BGRA8888
            const uint8_t  mipmapCount  = 1;
            const uint32_t lowResFormat = 0xFFFFFFFF;

            std::memcpy(bytes + 4, &versionMajor, 4);
            std::memcpy(bytes + 8, &versionMinor, 4);
            std::memcpy(bytes + 12, &headerSize, 4);
            std::memcpy(bytes + 16, &width, 2);
            std::memcpy(bytes + 18, &height, 2);
            std::memcpy(bytes + 24, &frames, 2);
            std::memcpy(bytes + 52, &format, 4);
            std::memcpy(bytes + 56, &mipmapCount, 1);
            std::memcpy(bytes + 57, &lowResFormat, 4);

            // 4 pixels of Blue (B=255, G=50, R=10, A=200)
            uint8_t* p = bytes + 64;
            for (int i = 0; i < 4; ++i) {
                p[i * 4 + 0] = 255; // B
                p[i * 4 + 1] = 50;  // G
                p[i * 4 + 2] = 10;  // R
                p[i * 4 + 3] = 200; // A
            }

            const auto decoded = ZHLN::BSP::DecodeVTF(vtf);
            ZHLN::Test::ExpectTrue(decoded.has_value());
            if (!decoded) {
                return std::unexpected(decoded.error());
            }

            const auto* rgba = reinterpret_cast<const uint8_t*>(decoded->rgba8.data());
            // Should be swizzled to R=10, G=50, B=255, A=200
            ZHLN::Test::ExpectEq(rgba[0], uint8_t {10});
            ZHLN::Test::ExpectEq(rgba[1], uint8_t {50});
            ZHLN::Test::ExpectEq(rgba[2], uint8_t {255});
            ZHLN::Test::ExpectEq(rgba[3], uint8_t {200});

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> lightmap_texcoord_1_calculation() {
            const auto map      = BuildSyntheticLightmapMap();
            const auto imported = ZHLN::BSP::ImportMapGeometry(map);

            ZHLN::Test::ExpectFalse(imported.parts.empty());
            if (imported.parts.empty()) {
                return std::unexpected(ZHLN::BSP::BSPError::MalformedLump);
            }

            const auto& part = imported.parts[0];
            ZHLN::Test::ExpectGt(part.surfaces.size(), size_t {0});

            auto HalfToFloat = [](uint16_t h) -> float {
                uint32_t sign = (h >> 15) & 0x0001;
                uint32_t exp  = (h >> 10) & 0x001F;
                uint32_t mant = h & 0x03FF;
                if (exp == 0) {
                    if (mant == 0) {
                        return sign ? -0.0f : 0.0f;
                    }
                    while ((mant & 0x0400) == 0) {
                        mant <<= 1;
                        exp--;
                    }
                    exp++;
                    mant &= ~0x0400;
                } else if (exp == 31) {
                    return (mant == 0) ? (sign ? -INFINITY : INFINITY) : NAN;
                }
                exp  = exp + (127 - 15);
                mant = mant << 13;
                uint32_t val    = (sign << 31) | (exp << 23) | mant;
                float    result = 0.0f;
                std::memcpy(&result, &val, 4);
                return result;
            };

            // Inspect TEXCOORD_1 (uv1) in VertexSurface
            for (const auto& surf: part.surfaces) {
                const float u1 = HalfToFloat(surf.uv1.data & 0xFFFF);
                const float v1 = HalfToFloat((surf.uv1.data >> 16) & 0xFFFF);
                ZHLN::Test::ExpectGe(u1, 0.0f);
                ZHLN::Test::ExpectLe(u1, 1.0f);
                ZHLN::Test::ExpectGe(v1, 0.0f);
                ZHLN::Test::ExpectLe(v1, 1.0f);
            }

            return {};
        }

        std::expected<void, ZHLN::ErrorCode> end_to_end_material_and_texture_resolution() {
            const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "zahlen_e2e_mat";
            std::error_code             ec;
            std::filesystem::remove_all(tempRoot, ec);
            std::filesystem::create_directories(tempRoot / "materials" / "custom", ec);

            // Write VMT
            {
                std::ofstream f(tempRoot / "materials" / "custom" / "wall.vmt");
                f << "\"LightmappedGeneric\"\n{\n\t\"$basetexture\" \"custom/wall_diffuse\"\n}\n";
            }

            // Write VTF
            {
                std::vector<std::byte> vtf(64 + 8);
                auto*                  bytes = reinterpret_cast<uint8_t*>(vtf.data());
                std::memcpy(bytes + 0, "VTF\0", 4);
                const uint32_t verMaj = 7;
                const uint32_t verMin = 2;
                const uint32_t hdrSz  = 64;
                const uint16_t w      = 4;
                const uint16_t h      = 4;
                const uint16_t fCount = 1;
                const uint32_t fmt    = 13;
                const uint8_t  mips   = 1;
                const uint32_t lowFmt = 0xFFFFFFFF;
                std::memcpy(bytes + 4, &verMaj, 4);
                std::memcpy(bytes + 8, &verMin, 4);
                std::memcpy(bytes + 12, &hdrSz, 4);
                std::memcpy(bytes + 16, &w, 2);
                std::memcpy(bytes + 18, &h, 2);
                std::memcpy(bytes + 24, &fCount, 2);
                std::memcpy(bytes + 52, &fmt, 4);
                std::memcpy(bytes + 56, &mips, 1);
                std::memcpy(bytes + 57, &lowFmt, 4);

                std::ofstream f(tempRoot / "materials" / "custom" / "wall_diffuse.vtf", std::ios::binary);
                f.write(reinterpret_cast<const char*>(vtf.data()), vtf.size());
            }

            ZHLN::BSP::SourceVFS vfs;
            vfs.AddSearchPath(tempRoot.string());

            // 1. Resolve material
            const auto vmtPath = vfs.ResolveMaterial("custom/wall");
            ZHLN::Test::ExpectTrue(vmtPath.has_value());

            // 2. Read and parse VMT
            const auto vmtBytes = vfs.ReadFile(*vmtPath);
            ZHLN::Test::ExpectTrue(vmtBytes.has_value());
            const std::string_view vmtText(reinterpret_cast<const char*>(vmtBytes->data()), vmtBytes->size());
            const auto             matExp = ZHLN::BSP::ParseVMT(vmtText);
            ZHLN::Test::ExpectTrue(matExp.has_value());
            ZHLN::Test::ExpectEq(matExp->baseTexture, "custom/wall_diffuse");

            // 3. Resolve and decode VTF
            const auto texPath = vfs.ResolveTexture(matExp->baseTexture);
            ZHLN::Test::ExpectTrue(texPath.has_value());
            const auto texBytes = vfs.ReadFile(*texPath);
            ZHLN::Test::ExpectTrue(texBytes.has_value());
            const auto decoded = ZHLN::BSP::DecodeVTF(*texBytes);
            ZHLN::Test::ExpectTrue(decoded.has_value());
            ZHLN::Test::ExpectEq(decoded->width, uint32_t {4});
            ZHLN::Test::ExpectEq(decoded->height, uint32_t {4});

            std::filesystem::remove_all(tempRoot, ec);
            return {};
        }
    };
};

} // namespace

int main() {
    BSPMaterialsTestSuite::Tests suite;
    int                          passed = 0;
    int                          failed = 0;

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
    ZHLN::Println("\033[36mRunning Test Suite: BSPMaterialsTestSuite\033[0m");
    ZHLN::Println("\033[36m==================================================\033[0m");

    runTest("vfs_case_insensitive_and_search_paths", [&] { return suite.vfs_case_insensitive_and_search_paths(); });
    runTest("vmt_parser_extracts_material_properties", [&] { return suite.vmt_parser_extracts_material_properties(); });
    runTest("vtf_decoder_dxt1_bc1", [&] { return suite.vtf_decoder_dxt1_bc1(); });
    runTest("vtf_decoder_dxt5_bc3", [&] { return suite.vtf_decoder_dxt5_bc3(); });
    runTest("vtf_decoder_bgra8888_and_rgb888", [&] { return suite.vtf_decoder_bgra8888_and_rgb888(); });
    runTest("lightmap_texcoord_1_calculation", [&] { return suite.lightmap_texcoord_1_calculation(); });
    runTest("end_to_end_material_and_texture_resolution", [&] { return suite.end_to_end_material_and_texture_resolution(); });

    ZHLN::Println("--------------------------------------------------");
    ZHLN::Println("Summary for BSPMaterialsTestSuite: {} Passed, {} Failed", passed, failed);
    ZHLN::Println("==================================================");

    return failed == 0 ? 0 : 1;
}
