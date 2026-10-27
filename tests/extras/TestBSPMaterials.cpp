// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <BSP/BSPGeometry.hpp>
#include <BSP/BSPImporter.hpp>
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

namespace {

// The suite's own error channel: every test returns an expected<void>, and the
// only failure mode is a failed expectation.
enum class MaterialsTestError : uint8_t {
    ExpectationFailed = 1,
};

auto Fail() -> std::expected<void, ZHLN::ErrorCode> {
    return std::unexpected(MaterialsTestError::ExpectationFailed);
}

auto BuildSyntheticMap() -> ZHLN::BSP::BSPMap {
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

    ZHLN::BSP::DTexInfo texInfo {};
    texInfo.textureVecs[0][0] = 1.0f;
    texInfo.textureVecs[0][3] = 0.0f;
    texInfo.textureVecs[1][1] = 1.0f;
    texInfo.textureVecs[1][3] = 0.0f;

    texInfo.texdata            = 0;
    map.texInfos.push_back(texInfo);

    // Face: 256x256 units on the world plane.
    ZHLN::BSP::DFace face {};
    face.planenum                      = 0;
    face.firstedge                     = 0;
    face.numedges                      = 4;
    face.texinfo                       = 0;
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

            // 5. Read file contents
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
                    "$envmap" "env_cubemap"
                    "$envmaptint" "[0.25 0.25 0.25]"
                    "$basealphaenvmapmask" "1"
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
            ZHLN::Test::ExpectLt(std::abs(mat.envMapTint.x - 0.25f), 1e-3f);
            ZHLN::Test::ExpectLt(std::abs(mat.envMapTint.y - 0.25f), 1e-3f);
            ZHLN::Test::ExpectLt(std::abs(mat.envMapTint.z - 0.25f), 1e-3f);
            ZHLN::Test::ExpectTrue(mat.baseAlphaEnvMapMask);

            ZHLN::Test::ExpectTrue(mat.Has("$basetexture"));
            ZHLN::Test::ExpectEq(mat.Get("$surfaceprop"), "brick");
            ZHLN::Test::ExpectTrue(mat.Find("$basetexture").has_value());
            ZHLN::Test::ExpectEq(*mat.Find("$basetexture"), "brick/brickwall003d");
            ZHLN::Test::ExpectFalse(mat.Find("$nonexistent").has_value());

            // Keys are case-insensitive and Windows path separators survive
            // tokenization for SourceVFS::NormalizePath to handle later.
            const auto windowsPath = ZHLN::BSP::ParseVMT(
                R"(VertexLitGeneric { "$BaseTexture" "models\props\de_nuke\light_wall" "$Color2" "[0.2 0.4 0.6]" })");
            ZHLN::Test::ExpectTrue(windowsPath.has_value());
            if (windowsPath) {
                ZHLN::Test::ExpectEq(windowsPath->baseTexture, "models\\props\\de_nuke\\light_wall");
                ZHLN::Test::ExpectEq(*windowsPath->Find("$BASETEXTURE"), "models\\props\\de_nuke\\light_wall");
                ZHLN::Test::ExpectFalse(windowsPath->baseAlphaEnvMapMask);
                ZHLN::Test::ExpectLt(std::abs(windowsPath->envMapTint.x - 1.0f), 1e-3f);
                ZHLN::Test::ExpectTrue(windowsPath->Find("$color2").has_value());
            }

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

        std::expected<void, ZHLN::ErrorCode> bsp_geometry_leaves_secondary_uv_zero() {
            const auto map      = BuildSyntheticMap();
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

            // VertexSurface retains TEXCOORD_1 for glTF; BSP surfaces now
            // leave the secondary UV set unused.
            for (const auto& surf: part.surfaces) {
                const float u1 = HalfToFloat(surf.uv1.data & 0xFFFF);
                const float v1 = HalfToFloat((surf.uv1.data >> 16) & 0xFFFF);
                ZHLN::Test::ExpectEq(u1, 0.0f);
                ZHLN::Test::ExpectEq(v1, 0.0f);
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

        // A prop material has to reach the renderer with the same emission the
        // world material pass would give it. Source puts its lamps on props --
        // a `wall_light`, a ceiling panel, an emergency light are all
        // StudioModels whose material is `VertexLitGeneric` + `$selfillum 1` --
        // and the StudioModel pass used to drop the term on the floor, so every
        // fixture in a shipped map came up as plain, dark, lit albedo.
        std::expected<void, ZHLN::ErrorCode> vmt_emission_reaches_prop_materials() {
            const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "zahlen_selfillum";
            std::error_code             ec;
            std::filesystem::remove_all(tempRoot, ec);
            std::filesystem::create_directories(tempRoot / "materials" / "models" / "props" / "de_nuke", ec);
            std::filesystem::create_directories(tempRoot / "materials" / "lights", ec);

            // 1. Source's `wall_light` panel: the exact keyvalues the map ships.
            {
                std::ofstream f(tempRoot / "materials" / "models" / "props" / "de_nuke" / "light_wall.vmt");
                f << "VertexLitGeneric\n{\n\t\"$basetexture\" \"models/props/de_nuke/light_wall\"\n\t\"$selfillum\" \"1\"\n}\n";
            }
            // 2. The same thing with the shader class spelled lowercase: shader
            //    names are not case-sensitive in Source, and content does this.
            {
                std::ofstream f(tempRoot / "materials" / "models" / "props" / "de_nuke" / "wall_lamp_lower.vmt");
                f << "vertexlitgeneric\n{\n\t\"$basetexture\" \"models/props/de_nuke/light_wall\"\n\t\"$selfillum\" \"1\"\n}\n";
            }
            // 3. A white lamp panel that authors nothing: the shader class alone
            //    has to carry it (UnlitGeneric is emissive by construction).
            {
                std::ofstream f(tempRoot / "materials" / "lights" / "white_panel.vmt");
                f << "UnlitGeneric\n{\n\t\"$basetexture\" \"lights/white_panel\"\n}\n";
            }
            // 4. A panel with no emission keyvalues of its own: the measured
            //    luminance of the base texture is what makes it glow.
            {
                std::ofstream f(tempRoot / "materials" / "lights" / "panel_lit.vmt");
                f << "VertexLitGeneric\n{\n\t\"$basetexture\" \"lights/white_panel\"\n}\n";
            }
            // 5. A prop that authors nothing and has no art on disk: it must
            //    stay dark rather than silently emitting on a mangled name.
            {
                std::ofstream f(tempRoot / "materials" / "models" / "props" / "de_nuke" / "concrete.vmt");
                f << "VertexLitGeneric\n{\n\t\"$surfaceprop\" \"concrete\"\n}\n";
            }
            // 6. The lamp material a map actually ships: UnlitGeneric plus
            //    `$selfillum 1`, which is what `lights/white` and `/white007`
            //    are -- every recessed strip and wall sconce in gm_murder_drones.
            {
                std::ofstream f(tempRoot / "materials" / "lights" / "walkway_strip.vmt");
                f << "UnlitGeneric\n{\n\t\"$basetexture\" \"lights/white_panel\"\n\t\"$selfillum\" \"1\"\n}\n";
            }

            // 7. A Source prop with a tinted, base-alpha-masked envmap.
            {
                std::ofstream f(tempRoot / "materials" / "models" / "props" / "de_nuke" / "chair_env.vmt");
                f << R"(VertexLitGeneric
{
    "$envmap" "env_cubemap"
    "$envmaptint" "[0.25 0.25 0.25]"
    "$basealphaenvmapmask" "1"
})";
            }
            // 8. A texlight whose art is not on disk: the emission factor would
            //    apply with no texture to modulate it, so it has to be dropped
            //    and the surface left at the missing-texture grey.
            {
                std::ofstream f(tempRoot / "materials" / "lights" / "orphan_panel.vmt");
                f << "UnlitGeneric\n{\n\t\"$basetexture\" \"lights/not_on_disk\"\n\t\"$selfillum\" \"1\"\n}\n";
            }

            // One 4x4 BC1 `lights/white_panel.vtf`: white endpoints, so every
            // sampled texel is 255 and the measured luma is 1.0.
            {
                std::vector<std::byte> vtf(64 + 8, std::byte {0});
                auto*                  bytes = reinterpret_cast<uint8_t*>(vtf.data());
                std::memcpy(bytes + 0, "VTF\0", 4);
                const uint32_t verMaj = 7;
                const uint32_t verMin = 2;
                const uint32_t hdrSz  = 64;
                const uint16_t w      = 4;
                const uint16_t h      = 4;
                const uint16_t fCount = 1;
                const uint32_t fmt    = 13; // BC1 / DXT1
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
                const uint8_t block[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00};
                std::memcpy(bytes + 64, block, sizeof(block));

                std::ofstream f(tempRoot / "materials" / "lights" / "white_panel.vtf", std::ios::binary);
                f.write(reinterpret_cast<const char*>(vtf.data()), static_cast<std::streamsize>(vtf.size()));
            }

            ZHLN::BSP::SourceVFS vfs;
            vfs.AddSearchPath(tempRoot.string());

            struct Parsed {
                bool                   ok = false;
                ZHLN::BSP::VMTMaterial material;
            };
            auto parseMaterial = [&](std::string_view name) -> Parsed {
                Parsed out;
                const auto vmtPath = vfs.ResolveMaterial(name);
                if (!vmtPath.has_value()) {
                    return out;
                }
                const auto bytes = vfs.ReadFile(*vmtPath);
                if (!bytes.has_value()) {
                    return out;
                }
                const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
                if (const auto parsed = ZHLN::BSP::ParseVMT(text); parsed.has_value()) {
                    out.material = *parsed;
                    out.ok       = true;
                }
                return out;
            };

            // The prop envmap material must retain its tint and base-alpha mask
            // in the deferred material description without opting into the
            // KHR specular extension's forward-only route.
            {
                const Parsed parsed = parseMaterial("models/props/de_nuke/chair_env");
                if (!ZHLN::Test::ExpectTrue(parsed.ok)) {
                    return Fail();
                }
                ZHLN::MaterialDesc desc = ZHLN::MaterialDesc::Basic();
                ZHLN::BSP::ApplyVmtLighting(vfs, parsed.material, desc);
                if (!ZHLN::Test::ExpectTrue(desc.environmentReflectionEnabled) || !ZHLN::Test::ExpectTrue(desc.environmentReflectionAlphaMask) ||
                    !ZHLN::Test::ExpectEq(desc.alphaMode, uint32_t {0}) || !ZHLN::Test::ExpectFalse(desc.specularEnabled) ||
                    !ZHLN::Test::ExpectInRange(desc.environmentReflectionFactor, 0.249f, 0.251f) ||
                    !ZHLN::Test::ExpectInRange(desc.roughness, 0.599f, 0.601f) || !ZHLN::Test::ExpectInRange(desc.metallic, 0.0f, 0.0f)) {
                    return Fail();
                }
            }

            // `$selfillum 1` on VertexLitGeneric: full self-illumination scaled
            // by the shader class (0.55) and lifted into HDR radiance so it
            // survives PBR exposure next to a baked albedo (0.55 * 4), emitted
            // untinted. The material is still lit: a lamp housing receives
            // light like anything else.
            for (const std::string_view name: {"models/props/de_nuke/light_wall", "models/props/de_nuke/wall_lamp_lower"}) {
                const Parsed parsed = parseMaterial(name);
                if (!ZHLN::Test::ExpectTrue(parsed.ok)) {
                    return Fail();
                }
                const auto emission = ZHLN::BSP::ResolveVmtEmission(vfs, parsed.material);
                if (!ZHLN::Test::ExpectInRange(emission.strength, 0.549f, 0.551f) ||
                    !ZHLN::Test::ExpectInRange(emission.tint.GetX(), 0.999f, 1.001f) ||
                    !ZHLN::Test::ExpectInRange(emission.tint.GetY(), 0.999f, 1.001f) ||
                    !ZHLN::Test::ExpectInRange(emission.tint.GetZ(), 0.999f, 1.001f)) {
                    return Fail();
                }

                ZHLN::MaterialDesc desc = ZHLN::MaterialDesc::Basic();
                ZHLN::BSP::ApplyVmtLighting(vfs, parsed.material, desc);
                if (!ZHLN::Test::ExpectInRange(desc.emissive.x, 2.199f, 2.201f) ||
                    !ZHLN::Test::ExpectInRange(desc.emissive.y, 2.199f, 2.201f) ||
                    !ZHLN::Test::ExpectInRange(desc.emissive.z, 2.199f, 2.201f) ||
                    !ZHLN::Test::ExpectInRange(desc.emissive.w, 0.999f, 1.001f) || !ZHLN::Test::ExpectEq(desc.unlit, false)) {
                    return Fail();
                }
            }

            // An UnlitGeneric panel is a texlight: unlit (no lighting path at
            // all), albedo dimmed off the bloom threshold, and the emission
            // term carried at full HDR strength from its own texel luminance
            // (1.0 here). The panel used to come out with the dim and no
            // emission at all, which is what turned every recessed strip in an
            // imported map into a flat grey tile.
            {
                const Parsed parsed = parseMaterial("lights/white_panel");
                if (!ZHLN::Test::ExpectTrue(parsed.ok)) {
                    return Fail();
                }
                ZHLN::MaterialDesc desc = ZHLN::MaterialDesc::Basic();
                ZHLN::BSP::ApplyVmtLighting(vfs, parsed.material, desc);
                if (!ZHLN::Test::ExpectEq(desc.unlit, true) || !ZHLN::Test::ExpectInRange(desc.emissive.x, 3.99f, 4.01f) ||
                    !ZHLN::Test::ExpectInRange(desc.baseColor.x, 0.549f, 0.551f)) {
                    return Fail();
                }
            }

            // A texlight -- UnlitGeneric plus `$selfillum 1`, the map's own
            // `lights/white` -- keeps the unlit path and the dimmed albedo, and
            // now also carries the same HDR emission a lit self-illuminated
            // surface gets. The dropped term is what left every walkway strip
            // in an imported map as a flat grey tile beside the light pool it
            // was casting.
            {
                const Parsed parsed = parseMaterial("lights/walkway_strip");
                if (!ZHLN::Test::ExpectTrue(parsed.ok)) {
                    return Fail();
                }
                const auto emission = ZHLN::BSP::ResolveVmtEmission(vfs, parsed.material);
                if (!ZHLN::Test::ExpectInRange(emission.strength, 0.999f, 1.001f)) {
                    return Fail();
                }
                ZHLN::MaterialDesc desc = ZHLN::MaterialDesc::Basic();
                ZHLN::BSP::ApplyVmtLighting(vfs, parsed.material, desc);
                if (!ZHLN::Test::ExpectEq(desc.unlit, true) || !ZHLN::Test::ExpectInRange(desc.emissive.x, 3.99f, 4.01f) ||
                    !ZHLN::Test::ExpectInRange(desc.emissive.y, 3.99f, 4.01f) ||
                    !ZHLN::Test::ExpectInRange(desc.emissive.z, 3.99f, 4.01f) ||
                    !ZHLN::Test::ExpectInRange(desc.baseColor.x, 0.549f, 0.551f)) {
                    return Fail();
                }
            }

            // A texlight whose texture does not resolve emits nothing: with no
            // picture to modulate it, the factor alone would paint a solid HDR
            // blob where a missing asset lives.
            {
                const Parsed parsed = parseMaterial("lights/orphan_panel");
                if (!ZHLN::Test::ExpectTrue(parsed.ok)) {
                    return Fail();
                }
                ZHLN::MaterialDesc desc = ZHLN::MaterialDesc::Basic();
                ZHLN::BSP::ApplyVmtLighting(vfs, parsed.material, desc);
                if (!ZHLN::Test::ExpectEq(desc.unlit, true) || !ZHLN::Test::ExpectInRange(desc.emissive.x, 0.0f, 0.0f) ||
                    !ZHLN::Test::ExpectInRange(desc.emissive.y, 0.0f, 0.0f) ||
                    !ZHLN::Test::ExpectInRange(desc.emissive.z, 0.0f, 0.0f)) {
                    return Fail();
                }
            }

            // A lit shader with a bright panel and no keyvalues: the texture
            // luminance carries it, at the VertexLitGeneric scale.
            {
                const Parsed parsed = parseMaterial("lights/panel_lit");
                if (!ZHLN::Test::ExpectTrue(parsed.ok)) {
                    return Fail();
                }
                const auto emission = ZHLN::BSP::ResolveVmtEmission(vfs, parsed.material);
                if (!ZHLN::Test::ExpectInRange(emission.strength, 0.549f, 0.551f)) {
                    return Fail();
                }
            }

            // Nothing authored and no art: no emission, and not unlit either.
            {
                const Parsed parsed = parseMaterial("models/props/de_nuke/concrete");
                if (!ZHLN::Test::ExpectTrue(parsed.ok)) {
                    return Fail();
                }
                const auto emission = ZHLN::BSP::ResolveVmtEmission(vfs, parsed.material);
                if (!ZHLN::Test::ExpectInRange(emission.strength, 0.0f, 0.0f)) {
                    return Fail();
                }

                ZHLN::MaterialDesc desc = ZHLN::MaterialDesc::Basic();
                ZHLN::BSP::ApplyVmtLighting(vfs, parsed.material, desc);
                if (!ZHLN::Test::ExpectInRange(desc.emissive.x, 0.0f, 0.0f) || !ZHLN::Test::ExpectEq(desc.unlit, false)) {
                    return Fail();
                }
            }

            std::filesystem::remove_all(tempRoot, ec);
            return {};
        }
    };
};

} // namespace
