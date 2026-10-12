// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <BSP/BSPGeometry.hpp>
#include <BSP/BSPRead.hpp>
#include <BSP/BSPScene.hpp>
#include <BSP/BSPTypes.hpp>
#include <BSP/SourceVFS.hpp>
#include <TestsFramework.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Scene.hpp>
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

// Helper to write raw binary data into a byte vector
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

// Builds a synthetic BSP file buffer containing a LUMP_GAMELUMP with static props
auto BuildSyntheticBspWithStaticProps(
    uint16_t sprpVersion,
    const std::vector<std::string>& dictModels,
    const std::vector<uint16_t>& leafIndices,
    const std::vector<std::byte>& propDataBytes,
    int32_t propCount
) -> std::vector<std::byte> {
    std::vector<std::byte> fileBytes;

    // 1. BSP Header
    ZHLN::BSP::BspHeader header {};
    header.ident = (('P' << 24) | ('S' << 16) | ('B' << 8) | 'V');
    header.version = 20;

    // We'll place GameLump directory and sprp lump data right after the header
    const size_t headerSize = sizeof(ZHLN::BSP::BspHeader);
    fileBytes.resize(headerSize, std::byte {0});

    // 2. Build sprp lump payload
    std::vector<std::byte> sprpPayload;

    // Dictionary
    const int32_t dictCount = static_cast<int32_t>(dictModels.size());
    AppendBytes(sprpPayload, dictCount);
    for (const auto& model : dictModels) {
        char nameBuf[128] {};
        std::strncpy(nameBuf, model.c_str(), sizeof(nameBuf) - 1);
        AppendRawBytes(sprpPayload, nameBuf, sizeof(nameBuf));
    }

    // Leaf indices
    const int32_t leafCount = static_cast<int32_t>(leafIndices.size());
    AppendBytes(sprpPayload, leafCount);
    for (uint16_t leaf : leafIndices) {
        AppendBytes(sprpPayload, leaf);
    }

    // Static props
    AppendBytes(sprpPayload, propCount);
    AppendRawBytes(sprpPayload, propDataBytes.data(), propDataBytes.size());

    // 3. Game Lump Directory
    // Directory contains 2 entries: an arbitrary game lump, then 'sprp'
    const int32_t lumpCount = 2;
    std::vector<std::byte> gameLumpDir;
    AppendBytes(gameLumpDir, lumpCount);

    const size_t gameLumpDirOffset = fileBytes.size();
    const size_t gameLumpDirLen = sizeof(int32_t) + lumpCount * sizeof(ZHLN::BSP::DGameLump);

    const size_t sprpDataOffset = gameLumpDirOffset + gameLumpDirLen;
    const size_t sprpDataLen = sprpPayload.size();

    // Entry 0: Dummy game lump
    ZHLN::BSP::DGameLump dummyLump {};
    dummyLump.id = 0x11223344;
    dummyLump.flags = 0;
    dummyLump.version = 1;
    dummyLump.fileofs = 0;
    dummyLump.filelen = 0;
    AppendBytes(gameLumpDir, dummyLump);

    // Entry 1: 'sprp' lump
    ZHLN::BSP::DGameLump sprpLump {};
    sprpLump.id = ZHLN::BSP::kGameLumpStaticProps;
    sprpLump.flags = 0;
    sprpLump.version = sprpVersion;
    sprpLump.fileofs = static_cast<int32_t>(sprpDataOffset);
    sprpLump.filelen = static_cast<int32_t>(sprpDataLen);
    AppendBytes(gameLumpDir, sprpLump);

    // Write Game Lump header into BSP Header
    header.lumps[static_cast<size_t>(ZHLN::BSP::Lump::GameLump)].fileofs = static_cast<int32_t>(gameLumpDirOffset);
    header.lumps[static_cast<size_t>(ZHLN::BSP::Lump::GameLump)].filelen = static_cast<int32_t>(gameLumpDirLen);
    header.lumps[static_cast<size_t>(ZHLN::BSP::Lump::GameLump)].version = 0;

    // Copy updated header to start of buffer
    std::memcpy(fileBytes.data(), &header, sizeof(header));

    // Append game lump dir and sprp payload
    AppendRawBytes(fileBytes, gameLumpDir.data(), gameLumpDir.size());
    AppendRawBytes(fileBytes, sprpPayload.data(), sprpPayload.size());

    return fileBytes;
}

struct BSPStaticPropsTestSuite {
    struct Tests {
        auto game_lump_directory_parsing() -> std::expected<void, std::string> {
            const std::vector<std::string> dict = {"models/props_c17/chair01.mdl"};
            const std::vector<uint16_t> leafs = {1, 2, 3};
            const std::vector<std::byte> propBytes;

            const auto bsp = BuildSyntheticBspWithStaticProps(4, dict, leafs, propBytes, 0);
            const auto mapExp = ZHLN::BSP::ParseBsp(bsp);
            ZHLN::Test::ExpectTrue(mapExp.has_value());

            const auto& map = *mapExp;
            ZHLN::Test::ExpectEq(map.gameLumps.size(), size_t {2});
            ZHLN::Test::ExpectEq(map.gameLumps[0].id, 0x11223344);
            ZHLN::Test::ExpectEq(map.gameLumps[1].id, ZHLN::BSP::kGameLumpStaticProps);
            ZHLN::Test::ExpectEq(map.gameLumps[1].version, uint16_t {4});

            return {};
        }

        auto static_prop_v4_parsing() -> std::expected<void, std::string> {
            const std::vector<std::string> dict = {
                "models/props_c17/chair01.mdl",
                "models/props_junk/wood_crate001a.mdl"
            };
            const std::vector<uint16_t> leafs = {5, 10};

            // Stride 56 (v4)
            // Prop 0:
            // origin: (100.0, 200.0, 50.0)
            // angles: (0.0, 90.0, 0.0)
            // propType: 0
            // firstLeaf: 0, leafCount: 1, solid: 6, flags: 1, skin: 2
            // fadeMin: 100.0, fadeMax: 1000.0, lightingOrigin: (10.0, 20.0, 30.0)
            std::vector<std::byte> propBytes;
            {
                const float origin[3] = {100.0f, 200.0f, 50.0f};
                const float angles[3] = {0.0f, 90.0f, 0.0f};
                const uint16_t propType = 0;
                const uint16_t firstLeaf = 0;
                const uint16_t leafCount = 1;
                const uint8_t  solid = 6;
                const uint8_t  flags = 1;
                const int32_t  skin = 2;
                const float    fadeMin = 100.0f;
                const float    fadeMax = 1000.0f;
                const float    lightingOrigin[3] = {10.0f, 20.0f, 30.0f};

                AppendRawBytes(propBytes, origin, sizeof(origin));
                AppendRawBytes(propBytes, angles, sizeof(angles));
                AppendBytes(propBytes, propType);
                AppendBytes(propBytes, firstLeaf);
                AppendBytes(propBytes, leafCount);
                AppendBytes(propBytes, solid);
                AppendBytes(propBytes, flags);
                AppendBytes(propBytes, skin);
                AppendBytes(propBytes, fadeMin);
                AppendBytes(propBytes, fadeMax);
                AppendRawBytes(propBytes, lightingOrigin, sizeof(lightingOrigin));
            }
            // Prop 1:
            {
                const float origin[3] = {-50.0f, 0.0f, 25.0f};
                const float angles[3] = {10.0f, 180.0f, 0.0f};
                const uint16_t propType = 1;
                const uint16_t firstLeaf = 1;
                const uint16_t leafCount = 1;
                const uint8_t  solid = 2;
                const uint8_t  flags = 0;
                const int32_t  skin = 0;
                const float    fadeMin = 0.0f;
                const float    fadeMax = 500.0f;
                const float    lightingOrigin[3] = {0.0f, 0.0f, 0.0f};

                AppendRawBytes(propBytes, origin, sizeof(origin));
                AppendRawBytes(propBytes, angles, sizeof(angles));
                AppendBytes(propBytes, propType);
                AppendBytes(propBytes, firstLeaf);
                AppendBytes(propBytes, leafCount);
                AppendBytes(propBytes, solid);
                AppendBytes(propBytes, flags);
                AppendBytes(propBytes, skin);
                AppendBytes(propBytes, fadeMin);
                AppendBytes(propBytes, fadeMax);
                AppendRawBytes(propBytes, lightingOrigin, sizeof(lightingOrigin));
            }

            const auto bsp = BuildSyntheticBspWithStaticProps(4, dict, leafs, propBytes, 2);
            const auto mapExp = ZHLN::BSP::ParseBsp(bsp);
            ZHLN::Test::ExpectTrue(mapExp.has_value());

            const auto& map = *mapExp;
            ZHLN::Test::ExpectEq(map.staticProps.size(), size_t {2});

            // Prop 0 assertions
            const auto& p0 = map.staticProps[0];
            ZHLN::Test::ExpectEq(p0.modelName, "models/props_c17/chair01.mdl");
            ZHLN::Test::ExpectInRange(p0.origin[0], 99.99f, 100.01f);
            ZHLN::Test::ExpectInRange(p0.origin[1], 199.99f, 200.01f);
            ZHLN::Test::ExpectInRange(p0.origin[2], 49.99f, 50.01f);
            ZHLN::Test::ExpectInRange(p0.angles[1], 89.99f, 90.01f);
            ZHLN::Test::ExpectEq(p0.solid, uint8_t {6});
            ZHLN::Test::ExpectEq(p0.skin, 2);
            ZHLN::Test::ExpectInRange(p0.fadeMinDist, 99.99f, 100.01f);
            ZHLN::Test::ExpectInRange(p0.fadeMaxDist, 999.99f, 1000.01f);
            // Default diffuse modulation is pure white
            ZHLN::Test::ExpectEq(p0.diffuseModulation[0], uint8_t {255});
            ZHLN::Test::ExpectEq(p0.diffuseModulation[1], uint8_t {255});
            ZHLN::Test::ExpectEq(p0.diffuseModulation[2], uint8_t {255});
            ZHLN::Test::ExpectEq(p0.diffuseModulation[3], uint8_t {255});
            ZHLN::Test::ExpectInRange(p0.uniformScale, 0.99f, 1.01f);

            // Prop 1 assertions
            const auto& p1 = map.staticProps[1];
            ZHLN::Test::ExpectEq(p1.modelName, "models/props_junk/wood_crate001a.mdl");
            ZHLN::Test::ExpectInRange(p1.origin[0], -50.01f, -49.99f);
            ZHLN::Test::ExpectInRange(p1.angles[0], 9.99f, 10.01f);
            ZHLN::Test::ExpectEq(p1.solid, uint8_t {2});

            return {};
        }

        auto static_prop_v8_diffuse_modulation() -> std::expected<void, std::string> {
            const std::vector<std::string> dict = {"models/props_lab/light.mdl"};
            const std::vector<uint16_t> leafs = {0};

            // Version 8 (stride 68):
            // v4 base (56 bytes)
            // forcedFadeScale (4 bytes, offset 56)
            // min/max CPU/GPU levels (4 bytes, offset 60)
            // diffuseModulation Color32 (4 bytes, offset 64)
            std::vector<std::byte> propBytes;
            {
                const float origin[3] = {0.0f, 0.0f, 120.0f};
                const float angles[3] = {0.0f, 0.0f, 0.0f};
                const uint16_t propType = 0;
                const uint16_t firstLeaf = 0;
                const uint16_t leafCount = 1;
                const uint8_t  solid = 0;
                const uint8_t  flags = 0;
                const int32_t  skin = 0;
                const float    fadeMin = 50.0f;
                const float    fadeMax = 800.0f;
                const float    lightingOrigin[3] = {0.0f, 0.0f, 120.0f};
                const float    forcedFadeScale = 1.75f;
                const uint8_t  levels[4] = {1, 2, 3, 4};
                const uint8_t  modulation[4] = {255, 128, 64, 255}; // Orange tint

                AppendRawBytes(propBytes, origin, sizeof(origin));
                AppendRawBytes(propBytes, angles, sizeof(angles));
                AppendBytes(propBytes, propType);
                AppendBytes(propBytes, firstLeaf);
                AppendBytes(propBytes, leafCount);
                AppendBytes(propBytes, solid);
                AppendBytes(propBytes, flags);
                AppendBytes(propBytes, skin);
                AppendBytes(propBytes, fadeMin);
                AppendBytes(propBytes, fadeMax);
                AppendRawBytes(propBytes, lightingOrigin, sizeof(lightingOrigin));
                AppendBytes(propBytes, forcedFadeScale);
                AppendRawBytes(propBytes, levels, sizeof(levels));
                AppendRawBytes(propBytes, modulation, sizeof(modulation));
            }

            const auto bsp = BuildSyntheticBspWithStaticProps(8, dict, leafs, propBytes, 1);
            const auto mapExp = ZHLN::BSP::ParseBsp(bsp);
            ZHLN::Test::ExpectTrue(mapExp.has_value());

            const auto& map = *mapExp;
            ZHLN::Test::ExpectEq(map.staticProps.size(), size_t {1});

            const auto& p = map.staticProps[0];
            ZHLN::Test::ExpectEq(p.modelName, "models/props_lab/light.mdl");
            ZHLN::Test::ExpectInRange(p.forcedFadeScale, 1.74f, 1.76f);
            ZHLN::Test::ExpectEq(p.diffuseModulation[0], uint8_t {255});
            ZHLN::Test::ExpectEq(p.diffuseModulation[1], uint8_t {128});
            ZHLN::Test::ExpectEq(p.diffuseModulation[2], uint8_t {64});
            ZHLN::Test::ExpectEq(p.diffuseModulation[3], uint8_t {255});

            return {};
        }

        auto static_prop_v11_uniform_scale() -> std::expected<void, std::string> {
            const std::vector<std::string> dict = {"models/props_canal/boat002b.mdl"};
            const std::vector<uint16_t> leafs = {1};

            // Version 11 (stride 80):
            // v8 base (68 bytes)
            // disableX360/flags (8 bytes, offset 68)
            // uniformScale (4 bytes, offset 76)
            std::vector<std::byte> propBytes;
            {
                const float origin[3] = {200.0f, -100.0f, 0.0f};
                const float angles[3] = {0.0f, 45.0f, 0.0f};
                const uint16_t propType = 0;
                const uint16_t firstLeaf = 0;
                const uint16_t leafCount = 1;
                const uint8_t  solid = 6;
                const uint8_t  flags = 0;
                const int32_t  skin = 1;
                const float    fadeMin = 0.0f;
                const float    fadeMax = 2000.0f;
                const float    lightingOrigin[3] = {200.0f, -100.0f, 0.0f};
                const float    forcedFadeScale = 1.0f;
                const uint8_t  levels[4] = {0, 0, 0, 0};
                const uint8_t  modulation[4] = {100, 200, 255, 255}; // Cyan tint
                const uint32_t extraFlags[2] = {0, 0};
                const float    uniformScale = 3.5f;

                AppendRawBytes(propBytes, origin, sizeof(origin));
                AppendRawBytes(propBytes, angles, sizeof(angles));
                AppendBytes(propBytes, propType);
                AppendBytes(propBytes, firstLeaf);
                AppendBytes(propBytes, leafCount);
                AppendBytes(propBytes, solid);
                AppendBytes(propBytes, flags);
                AppendBytes(propBytes, skin);
                AppendBytes(propBytes, fadeMin);
                AppendBytes(propBytes, fadeMax);
                AppendRawBytes(propBytes, lightingOrigin, sizeof(lightingOrigin));
                AppendBytes(propBytes, forcedFadeScale);
                AppendRawBytes(propBytes, levels, sizeof(levels));
                AppendRawBytes(propBytes, modulation, sizeof(modulation));
                AppendRawBytes(propBytes, extraFlags, sizeof(extraFlags));
                AppendBytes(propBytes, uniformScale);
            }

            const auto bsp = BuildSyntheticBspWithStaticProps(11, dict, leafs, propBytes, 1);
            const auto mapExp = ZHLN::BSP::ParseBsp(bsp);
            ZHLN::Test::ExpectTrue(mapExp.has_value());

            const auto& map = *mapExp;
            ZHLN::Test::ExpectEq(map.staticProps.size(), size_t {1});

            const auto& p = map.staticProps[0];
            ZHLN::Test::ExpectEq(p.modelName, "models/props_canal/boat002b.mdl");
            ZHLN::Test::ExpectInRange(p.uniformScale, 3.49f, 3.51f);
            ZHLN::Test::ExpectEq(p.diffuseModulation[0], uint8_t {100});
            ZHLN::Test::ExpectEq(p.diffuseModulation[1], uint8_t {200});
            ZHLN::Test::ExpectEq(p.diffuseModulation[2], uint8_t {255});

            return {};
        }

        auto static_prop_scene_entity_conversion() -> std::expected<void, std::string> {
            ZHLN::BSP::BSPMap map;
            map.version = 20;

            // Add submodel 0 (Worldspawn) so DescribeScene is happy
            ZHLN::BSP::DModel worldModel {};
            worldModel.mins[0] = -100.0f;
            worldModel.mins[1] = -100.0f;
            worldModel.mins[2] = 0.0f;
            worldModel.maxs[0] = 100.0f;
            worldModel.maxs[1] = 100.0f;
            worldModel.maxs[2] = 100.0f;
            map.models.push_back(worldModel);

            // Add static prop
            ZHLN::BSP::StaticProp prop;
            prop.modelName = "models/props_c17/bench01.mdl";
            // Source coordinates: X=128 (East), Y=256 (North), Z=64 (Up)
            prop.origin = {128.0f, 256.0f, 64.0f};
            // Source angles: pitch=0, yaw=90, roll=0
            prop.angles = {0.0f, 90.0f, 0.0f};
            prop.solid = 6;
            prop.uniformScale = 1.5f;
            prop.diffuseModulation = {128, 64, 32, 255};
            map.staticProps.push_back(prop);

            ZHLN::BSP::ImportOptions options;
            options.unitScale = 0.0254f; // Source inches to meters
            options.buildColliders = true;

            const auto scene = ZHLN::BSP::DescribeScene(map, "maps/test_props.bsp", options);
            // Worldspawn + static prop = 2 entities
            ZHLN::Test::ExpectEq(scene.entities.size(), size_t {2});

            const auto& propEnt = scene.entities[1];
            ZHLN::Test::ExpectEq(propEnt.name, "prop_static_0");
            ZHLN::Test::ExpectEq(propEnt.shape, ZHLN::Scene::ShapeKind::Prefab);
            ZHLN::Test::ExpectEq(propEnt.source, "models/props_c17/bench01.mdl");
            ZHLN::Test::ExpectEq(propEnt.body, ZHLN::Scene::BodyKind::Static);

            // Position conversion: Source (X, Y, Z) -> Engine (X, Z, -Y) * unitScale
            // X = 128 * 0.0254 = 3.2512
            // Y = 64 * 0.0254 = 1.6256
            // Z = -256 * 0.0254 = -6.5024
            ZHLN::Test::ExpectInRange(propEnt.transform.position[0], 3.25f, 3.26f);
            ZHLN::Test::ExpectInRange(propEnt.transform.position[1], 1.62f, 1.63f);
            ZHLN::Test::ExpectInRange(propEnt.transform.position[2], -6.51f, -6.49f);

            // Uniform scale
            ZHLN::Test::ExpectInRange(propEnt.transform.scale[0], 1.49f, 1.51f);
            ZHLN::Test::ExpectInRange(propEnt.transform.scale[1], 1.49f, 1.51f);
            ZHLN::Test::ExpectInRange(propEnt.transform.scale[2], 1.49f, 1.51f);

            // Diffuse modulation color: 128/255, 64/255, 32/255, 1.0
            ZHLN::Test::ExpectInRange(propEnt.material.baseColor[0], 0.50f, 0.51f);
            ZHLN::Test::ExpectInRange(propEnt.material.baseColor[1], 0.25f, 0.26f);
            ZHLN::Test::ExpectInRange(propEnt.material.baseColor[2], 0.12f, 0.13f);
            ZHLN::Test::ExpectInRange(propEnt.material.baseColor[3], 0.99f, 1.01f);

            return {};
        }

        auto vfs_model_resolution() -> std::expected<void, std::string> {
            const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "zahlen_test_vfs_props";
            std::error_code ec;
            std::filesystem::remove_all(tempRoot, ec);
            std::filesystem::create_directories(tempRoot / "models" / "props_c17", ec);

            // Create a dummy .mdl file
            {
                std::ofstream mdlFile(tempRoot / "models" / "props_c17" / "furniturechair001a.mdl", std::ios::binary);
                mdlFile.write("IDST", 4);
            }

            ZHLN::BSP::SourceVFS vfs;
            vfs.AddSearchPath(tempRoot.string());

            // 1. Resolve with full path
            const auto res1 = vfs.ResolveModel("models/props_c17/furniturechair001a.mdl");
            ZHLN::Test::ExpectTrue(res1.has_value());

            // 2. Resolve without models/ prefix
            const auto res2 = vfs.ResolveModel("props_c17/furniturechair001a.mdl");
            ZHLN::Test::ExpectTrue(res2.has_value());

            // 3. Resolve without .mdl extension
            const auto res3 = vfs.ResolveModel("props_c17/furniturechair001a");
            ZHLN::Test::ExpectTrue(res3.has_value());

            // 4. Case-insensitivity test
            const auto res4 = vfs.ResolveModel("MODELS/Props_C17/FurnitureChair001a.MDL");
            ZHLN::Test::ExpectTrue(res4.has_value());

            std::filesystem::remove_all(tempRoot, ec);
            return {};
        }

        auto hdr_faces_lump_priority() -> std::expected<void, std::string> {
            std::vector<std::byte> fileBytes;
            fileBytes.resize(sizeof(ZHLN::BSP::BspHeader), std::byte {0});

            ZHLN::BSP::BspHeader header {};
            header.ident = (('P' << 24) | ('S' << 16) | ('B' << 8) | 'V');
            header.version = 20;

            // Dummy face in standard LUMP_FACES
            ZHLN::BSP::DFace standardFace {};
            standardFace.texinfo = 10;
            const size_t stdFaceOffset = fileBytes.size();
            AppendBytes(fileBytes, standardFace);
            const size_t stdFaceLen = sizeof(ZHLN::BSP::DFace);

            // Two faces in LUMP_FACES_HDR
            ZHLN::BSP::DFace hdrFace1 {};
            hdrFace1.texinfo = 20;
            ZHLN::BSP::DFace hdrFace2 {};
            hdrFace2.texinfo = 30;
            const size_t hdrFaceOffset = fileBytes.size();
            AppendBytes(fileBytes, hdrFace1);
            AppendBytes(fileBytes, hdrFace2);
            const size_t hdrFaceLen = sizeof(ZHLN::BSP::DFace) * 2;

            header.lumps[static_cast<size_t>(ZHLN::BSP::Lump::Faces)].fileofs = static_cast<int32_t>(stdFaceOffset);
            header.lumps[static_cast<size_t>(ZHLN::BSP::Lump::Faces)].filelen = static_cast<int32_t>(stdFaceLen);
            header.lumps[static_cast<size_t>(ZHLN::BSP::Lump::FacesHDR)].fileofs = static_cast<int32_t>(hdrFaceOffset);
            header.lumps[static_cast<size_t>(ZHLN::BSP::Lump::FacesHDR)].filelen = static_cast<int32_t>(hdrFaceLen);

            std::memcpy(fileBytes.data(), &header, sizeof(header));

            const auto mapExp = ZHLN::BSP::ParseBsp(fileBytes);
            ZHLN::Test::ExpectTrue(mapExp.has_value());
            ZHLN::Test::ExpectEq(mapExp->faces.size(), size_t {2});
            ZHLN::Test::ExpectEq(mapExp->faces[0].texinfo, int16_t {20});
            ZHLN::Test::ExpectEq(mapExp->faces[1].texinfo, int16_t {30});

            // Test fallback when FacesHDR is empty
            header.lumps[static_cast<size_t>(ZHLN::BSP::Lump::FacesHDR)].filelen = 0;
            std::memcpy(fileBytes.data(), &header, sizeof(header));

            const auto fallbackMapExp = ZHLN::BSP::ParseBsp(fileBytes);
            ZHLN::Test::ExpectTrue(fallbackMapExp.has_value());
            ZHLN::Test::ExpectEq(fallbackMapExp->faces.size(), size_t {1});
            ZHLN::Test::ExpectEq(fallbackMapExp->faces[0].texinfo, int16_t {10});

            return {};
        }
    };
};

} // namespace

int main() {
    BSPStaticPropsTestSuite::Tests suite;
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
    ZHLN::Println("\033[36mRunning Test Suite: BSPStaticPropsTestSuite\033[0m");
    ZHLN::Println("\033[36m==================================================\033[0m");

    runTest("game_lump_directory_parsing", [&] { return suite.game_lump_directory_parsing(); });
    runTest("static_prop_v4_parsing", [&] { return suite.static_prop_v4_parsing(); });
    runTest("static_prop_v8_diffuse_modulation", [&] { return suite.static_prop_v8_diffuse_modulation(); });
    runTest("static_prop_v11_uniform_scale", [&] { return suite.static_prop_v11_uniform_scale(); });
    runTest("static_prop_scene_entity_conversion", [&] { return suite.static_prop_scene_entity_conversion(); });
    runTest("vfs_model_resolution", [&] { return suite.vfs_model_resolution(); });
    runTest("hdr_faces_lump_priority", [&] { return suite.hdr_faces_lump_priority(); });

    ZHLN::Println("--------------------------------------------------");
    ZHLN::Println("Summary for BSPStaticPropsTestSuite: {} Passed, {} Failed", passed, failed);
    ZHLN::Println("==================================================");

    return failed == 0 ? 0 : 1;
}
