// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/CookerFixture.hpp"
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Vertex.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <sys/wait.h>
#endif

// ============================================================================
// Asset Cooker Tests (zero-boilerplate namespace collection)
// ============================================================================

namespace ZHLN::Test {
namespace {

// --- 1. Locate zcook Compiler Executable ---
std::expected<void, ZHLN::ErrorCode> test_locate_zcook_compiler_binary(CookerSession& session) {
            const fs::path& zcook = session.ZcookPath();
            ZHLN::Test::Expect(!zcook.empty());

            if (zcook.empty()) {
                ZHLN::Println("    [Compiler Error] Could not find 'zcook' in current build directory!");
                return std::unexpected(CookerTestError::ZcookExecutableNotFound);
            }

            ZHLN::Println("    [Found Compiler] Path: {}", zcook.string());
            return {};
        }

        // --- 2. Offline Mesh Compilation (zcook mesh) ---
        std::expected<void, ZHLN::ErrorCode> test_offline_mesh_compilation_and_header_verification(CookerFixture& cooker) {
            if (cooker.zcook.empty()) {
                return std::unexpected(CookerTestError::ZcookExecutableNotFound);
            }

            fs::path metaPath    = cooker.SubPath("level_metadata.bin");
            fs::path rawGeomPath = cooker.SubPath("cube_geom.bin");
            fs::path outZmesh    = cooker.SubPath("cube.zmesh");

            // Write 3-vertex raw triangle (stride 13: v_idx, px, py, pz, nx, ny, nz, u, v, r, g, b, a)
            std::vector<float> rawFloats = {0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                            1.0f, 1.0f,  0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                            2.0f, 0.0f,  1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
            {
                std::ofstream bf(rawGeomPath, std::ios::binary);
                WriteVector(bf, rawFloats);
            }

            Metadata::Manifest manifest;
            manifest.levelName = "TestLevel";

            Metadata::Mesh mesh;
            mesh.id         = "Mesh_Pillar";
            mesh.layout     = "P3N3T2C4";
            mesh.binFile    = "cube_geom.bin";
            mesh.byteOffset = 0;
            mesh.byteLength = kVertexBufferSize4096;
            mesh.primitives.push_back({.materialId = "mat_stone", .vertexOffset = 0, .vertexCount = kPillarVertexCount});
            manifest.meshes.push_back(mesh);

            {
                std::ofstream                mf(metaPath, std::ios::binary);
                Metadata::BinaryStreamWriter writer(mf);
                writer.SerializeManifest(manifest);
            }

            std::string args =
                std::format(R"(mesh --meta "{}" --id "Mesh_Pillar" -i "{}" -o "{}")", metaPath.string(), rawGeomPath.string(), outZmesh.string());
            int exitCode = cooker.Run(args);
            ZHLN::Test::Expect(exitCode) == 0;

            if (exitCode != 0) {
                return std::unexpected(CookerTestError::MeshCompilationFailed);
            }

            std::ifstream ifs(outZmesh, std::ios::binary | std::ios::ate);
            auto          fileSize = ifs.tellg();
            ifs.seekg(0, std::ios::beg);

            ZHLN::CookedMeshHeader header {};
            ReadValue(ifs, header);

            ZHLN::Test::ExpectEq(header.magic, kMeshMagic);
            ZHLN::Test::ExpectEq(header.version, kMeshVersion);
            ZHLN::Test::ExpectEq(header.vertexCount, 3u);
            ZHLN::Test::ExpectEq(header.indexCount, 3u);
            ZHLN::Test::ExpectEq(header.hasSkin, 0u);
            ZHLN::Test::ExpectEq(header.boundingBoxMin[0], -1.0f);
            ZHLN::Test::ExpectEq(header.boundingBoxMax[0], 1.0f);

            // A single triangle must still produce exactly one meshlet.
            ZHLN::Test::ExpectEq(header.meshletCount, 1u);
            ZHLN::Test::ExpectEq(header.meshletVertexCount, 3u);
            ZHLN::Test::ExpectEq(header.meshletTriByteCount, 4u); // 3 micro-indices, padded to 4B

            size_t expectedSize = sizeof(ZHLN::CookedMeshHeader) + (header.vertexCount * sizeof(ZHLN::VertexPosition)) +
                                  (header.vertexCount * sizeof(ZHLN::VertexTangentFrame)) + (header.vertexCount * sizeof(ZHLN::VertexSurface)) +
                                  (header.indexCount * sizeof(uint32_t)) + (header.meshletCount * sizeof(ZHLN::MeshletDesc)) +
                                  (header.meshletVertexCount * sizeof(uint32_t)) + header.meshletTriByteCount;

            ZHLN::Test::ExpectEq(static_cast<size_t>(fileSize), expectedSize);

            // The total byte count remains 20 bytes per vertex after the
            // split. Check the actual on-disk ordering, not just file size.
            ifs.seekg(sizeof(ZHLN::CookedMeshHeader) + header.vertexCount * sizeof(ZHLN::VertexPosition), std::ios::beg);
            std::array<ZHLN::VertexTangentFrame, 3> frames {};
            std::array<ZHLN::VertexSurface, 3>      surfaces {};
            ifs.read(reinterpret_cast<char*>(frames.data()), sizeof(frames));
            ifs.read(reinterpret_cast<char*>(surfaces.data()), sizeof(surfaces));
            if (!ifs || frames[0].normal.data != ZHLN::Math::PackNormal(0.0f, 1.0f, 0.0f).data || surfaces[1].uv.data != ZHLN::Math::PackUV(1.0f, 0.0f).data ||
                surfaces[0].color.data != ZHLN::Math::PackColor(1.0f, 1.0f, 1.0f, 1.0f).data || surfaces[2].uv1.data != 0) {
                return std::unexpected(CookerTestError::MeshCompilationFailed);
            }
            return {};
        }

        // --- 3. Offline Animation Compilation (zcook anim) ---
        std::expected<void, ZHLN::ErrorCode> test_offline_animation_compilation_and_hashing(CookerFixture& cooker) {
            if (cooker.zcook.empty()) {
                return std::unexpected(CookerTestError::ZcookExecutableNotFound);
            }

            fs::path metaPath = cooker.SubPath("metadata.bin");
            fs::path animBin  = cooker.SubPath("anim_payload.bin");
            fs::path outZanim = cooker.SubPath("walk.zanim");

            std::vector<float> timeKeys   = {0.0f, 0.5f, 1.0f};
            std::vector<float> translKeys = {0.0f, 0.0f, 0.0f, 0.0f, kSampleTranslY, 0.0f, 0.0f, 0.0f, 0.0f};

            {
                std::ofstream bf(animBin, std::ios::binary);
                WriteVector(bf, timeKeys);
                WriteVector(bf, translKeys);
            }

            Metadata::Manifest manifest;
            manifest.levelName = "AnimLevel";

            Metadata::Animation anim;
            anim.id       = "Anim_Walk";
            anim.name     = "WalkCycle";
            anim.duration = 1.0f;
            anim.loop     = true;
            anim.channels.push_back({.targetNodeId = "Bone_Spine", .targetPath = "translation", .samplerId = 0});
            anim.samplers.push_back(
                {.interpolation = "LINEAR",
                 .inputOffset   = 0,
                 .inputLength   = static_cast<uint32_t>(timeKeys.size() * sizeof(float)),
                 .outputOffset  = static_cast<uint32_t>(timeKeys.size() * sizeof(float)),
                 .outputLength  = static_cast<uint32_t>(translKeys.size() * sizeof(float)),
                 .binFile       = "anim_payload.bin"}
            );
            manifest.animations.push_back(anim);

            {
                std::ofstream                mf(metaPath, std::ios::binary);
                Metadata::BinaryStreamWriter writer(mf);
                writer.SerializeManifest(manifest);
            }

            std::string args     = std::format(R"(anim --meta "{}" --id "Anim_Walk" -o "{}")", metaPath.string(), outZanim.string());
            int         exitCode = cooker.Run(args);
            ZHLN::Test::Expect(exitCode) == 0;

            if (exitCode != 0) {
                return std::unexpected(CookerTestError::AnimationCompilationFailed);
            }

            std::ifstream          ifs(outZanim, std::ios::binary);
            ZHLN::CookedAnimHeader readHeader {};
            ReadValue(ifs, readHeader);

            ZHLN::Test::ExpectEq(readHeader.duration, 1.0f);
            ZHLN::Test::ExpectEq(readHeader.loop, 1u);
            ZHLN::Test::ExpectEq(readHeader.trackCount, 1u);

            ZHLN::CookedAnimTrack readTrack {};
            ReadValue(ifs, readTrack);

            ZHLN::Test::ExpectEq(readTrack.targetNodeHash, ZHLN::HashAssetPath("Bone_Spine"));
            ZHLN::Test::ExpectEq(readTrack.pathType, 0u); // 0 = Translation
            ZHLN::Test::ExpectEq(readTrack.keyCount, 3u);

            return {};
        }

        // --- 4. Offline Texture Cooking (zcook tex) ---
        std::expected<void, ZHLN::ErrorCode> test_offline_texture_cooking_passthrough(CookerFixture& cooker) {
            if (cooker.zcook.empty()) {
                return std::unexpected(CookerTestError::ZcookExecutableNotFound);
            }

            fs::path inTex  = cooker.SubPath("albedo.png");
            fs::path outTex = cooker.SubPath("albedo.ztex");

            std::vector<uint8_t> dummyPng = {kPngMagicHeader0, 'P', 'N', 'G', '\r', '\n', kPngMagicSub, '\n', 0x00, 0x00, 0x00, kPngHeaderLen};
            {
                std::ofstream ofs(inTex, std::ios::binary);
                WriteVector(ofs, dummyPng);
            }

            std::string args     = std::format(R"(tex -i "{}" -o "{}")", inTex.string(), outTex.string());
            int         exitCode = cooker.Run(args);
            ZHLN::Test::Expect(exitCode) == 0;

            if (exitCode != 0) {
                return std::unexpected(CookerTestError::TextureCookingFailed);
            }

            std::ifstream ifs(outTex, std::ios::binary | std::ios::ate);
            auto          size = ifs.tellg();
            ZHLN::Test::ExpectEq(static_cast<size_t>(size), dummyPng.size());

            return {};
        }

        // --- 5. Offline PAK Archive Creation (zcook pak) & Engine VFS Mount ---
        std::expected<void, ZHLN::ErrorCode> test_offline_pak_archive_creation_and_vfs_mount(CookerFixture& cooker) {
            if (cooker.zcook.empty()) {
                return std::unexpected(CookerTestError::ZcookExecutableNotFound);
            }

            fs::path pakPath      = cooker.SubPath("data/base.pak");
            fs::path manifestPath = cooker.SubPath("build_assets/manifest.txt");

            fs::path f1 = cooker.SubPath("build_assets/player.zmesh");
            fs::path f2 = cooker.SubPath("build_assets/albedo.ztex");
            fs::path f3 = cooker.SubPath("build_assets/walk.zanim");

            std::string content1 = "MSH0_SAMPLE_MESH_PAYLOAD";
            std::string content2 = "TEX0_SAMPLE_IMAGE_PAYLOAD";
            std::string content3 = "ANM0_SAMPLE_TRACK_PAYLOAD";

            {
                std::ofstream(f1, std::ios::binary).write(content1.data(), static_cast<std::streamsize>(content1.size()));
                std::ofstream(f2, std::ios::binary).write(content2.data(), static_cast<std::streamsize>(content2.size()));
                std::ofstream(f3, std::ios::binary).write(content3.data(), static_cast<std::streamsize>(content3.size()));
            }

            {
                std::ofstream mfs(manifestPath);
                mfs << "models/player.zmesh=" << f1.string() << "\n";
                mfs << "textures/albedo.ztex=" << f2.string() << "\n";
                mfs << "anims/walk.zanim=" << f3.string() << "\n";
            }

            std::string args     = std::format(R"(pak -o "{}" -i "{}")", pakPath.string(), manifestPath.string());
            int         exitCode = cooker.Run(args);
            ZHLN::Test::Expect(exitCode) == 0;

            if (exitCode != 0) {
                return std::unexpected(CookerTestError::PakArchiveFailed);
            }

            std::ifstream   ifs(pakPath, std::ios::binary);
            ZHLN::FS::PakHeader diskHeader {};
            ReadValue(ifs, diskHeader);

            ZHLN::Test::ExpectEq(std::string_view(diskHeader.magic, 4), "ZPAK");
            ZHLN::Test::ExpectEq(diskHeader.entryCount, static_cast<uint32_t>(kExpectedEntries3));

            ZHLN::AssetManager cwMgr;
            bool                       mounted = cwMgr.MountPak(pakPath.string());
            ZHLN::Test::ExpectTrue(mounted);

            // Fetch Mesh
            ZHLN::AssetLoadRequest reqMesh {.assetID = ZHLN::HashAssetPath("models/player.zmesh")};
            bool                          loadedMesh = cwMgr.LoadSync(reqMesh);
            ZHLN::Test::ExpectTrue(loadedMesh);
            ZHLN::Test::ExpectEq(reqMesh.outSize, content1.size());
            ZHLN::Test::ExpectEq(std::string_view(static_cast<const char*>(reqMesh.outData), reqMesh.outSize), content1);
            cwMgr.FreeMemory(reqMesh);

            // Fetch Texture
            ZHLN::AssetLoadRequest reqTex {.assetID = ZHLN::HashAssetPath("textures/albedo.ztex")};
            bool                          loadedTex = cwMgr.LoadSync(reqTex);
            ZHLN::Test::ExpectTrue(loadedTex);
            ZHLN::Test::ExpectEq(reqTex.outSize, content2.size());
            ZHLN::Test::ExpectEq(std::string_view(static_cast<const char*>(reqTex.outData), reqTex.outSize), content2);
            cwMgr.FreeMemory(reqTex);

            return {};
        }


        // --- 7. Compiler CLI Error Handling & Input Validation ---
        ZHLN_ANNOTATION(ZHLN::Test::Cases {
            ZHLN::Test::NamedCase {"invalid_command", 0},
            ZHLN::Test::NamedCase {"mesh_missing_flags", 1},
            ZHLN::Test::NamedCase {"anim_missing_id", 2},
            ZHLN::Test::NamedCase {"pak_missing_manifest", 3},
            ZHLN::Test::NamedCase {"glb_missing_meta", 4},
        })
        std::expected<void, ZHLN::ErrorCode> test_zcook_cli_subcommand_error_handling(CookerFixture& cooker, int variant) {
            if (cooker.zcook.empty()) {
                return std::unexpected(CookerTestError::ZcookExecutableNotFound);
            }

            std::string args;
            switch (variant) {
                case 0:
                    args = "invalid_command";
                    break;
                case 1:
                    args = std::format(R"(mesh --meta "{}")", cooker.SubPath("missing_meta.bin").string());
                    break;
                case 2:
                    args = std::format(R"(anim -o "{}")", cooker.SubPath("out.zanim").string());
                    break;
                case 3:
                    args = std::format(
                        R"(pak -o "{}" -i "{}")",
                        cooker.SubPath("out.pak").string(),
                        cooker.SubPath("non_existent_manifest.txt").string()
                    );
                    break;
                default:
                    args = std::format(R"(glb -o "{}")", cooker.SubPath("out.glb").string());
                    break;
            }
            const int rc = cooker.Run(args);
            ZHLN::Test::Expect(rc) == 1;
            return {};
        }

} // namespace
} // namespace ZHLN::Test
