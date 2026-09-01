// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include "helpers/CookerFixture.hpp"
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Types.hpp>
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
// Test Suite Implementation
// ============================================================================

struct CookerTestSuite {
    CookerTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(kWorkerThreadCount, kFiberCount, kFiberStackSize);
    }

    ~CookerTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    CookerTestSuite(const CookerTestSuite&)                = delete;
    CookerTestSuite& operator=(const CookerTestSuite&)     = delete;
    CookerTestSuite(CookerTestSuite&&) noexcept            = delete;
    CookerTestSuite& operator=(CookerTestSuite&&) noexcept = delete;

    struct Tests {
        // --- 1. Locate zcook Compiler Executable ---
        std::expected<void, ZHLN::Error> locate_zcook_compiler_binary() {
            fs::path zcook = FindZcookExecutable();
            ZHLN::Test::ExpectFalse(zcook.empty());

            if (zcook.empty()) {
                ZHLN::Println("    [Compiler Error] Could not find 'zcook' in current build directory!");
                return std::unexpected(CookerTestError::ZcookExecutableNotFound);
            }

            ZHLN::Println("    [Found Compiler] Path: {}", zcook.string());
            return {};
        }

        // --- 2. Offline Mesh Compilation (zcook mesh) ---
        std::expected<void, ZHLN::Error> offline_mesh_compilation_and_header_verification() {
            fs::path zcook = FindZcookExecutable();
            if (zcook.empty()) {
                return std::unexpected(CookerTestError::ZcookExecutableNotFound);
            }

            TempSandbox sandbox("cook_mesh");
            fs::path    metaPath    = sandbox.SubPath("level_metadata.bin");
            fs::path    rawGeomPath = sandbox.SubPath("cube_geom.bin");
            fs::path    outZmesh    = sandbox.SubPath("cube.zmesh");

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
            int exitCode = RunZcook(zcook, args);
            ZHLN::Test::ExpectEq(exitCode, 0);

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
                                  (header.vertexCount * sizeof(ZHLN::VertexAttributes)) + (header.indexCount * sizeof(uint32_t)) +
                                  (header.meshletCount * sizeof(ZHLN::GPUMeshlet)) + (header.meshletVertexCount * sizeof(uint32_t)) +
                                  header.meshletTriByteCount;

            ZHLN::Test::ExpectEq(static_cast<size_t>(fileSize), expectedSize);
            return {};
        }

        // --- 3. Offline Animation Compilation (zcook anim) ---
        std::expected<void, ZHLN::Error> offline_animation_compilation_and_hashing() {
            fs::path zcook = FindZcookExecutable();
            if (zcook.empty()) {
                return std::unexpected(CookerTestError::ZcookExecutableNotFound);
            }

            TempSandbox sandbox("cook_anim");
            fs::path    metaPath = sandbox.SubPath("metadata.bin");
            fs::path    animBin  = sandbox.SubPath("anim_payload.bin");
            fs::path    outZanim = sandbox.SubPath("walk.zanim");

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
            int         exitCode = RunZcook(zcook, args);
            ZHLN::Test::ExpectEq(exitCode, 0);

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

            ZHLN::Test::ExpectEq(readTrack.targetNodeHash, ZHLN::HashCreativeWorkPath("Bone_Spine"));
            ZHLN::Test::ExpectEq(readTrack.pathType, 0u); // 0 = Translation
            ZHLN::Test::ExpectEq(readTrack.keyCount, 3u);

            return {};
        }

        // --- 4. Offline Texture Cooking (zcook tex) ---
        std::expected<void, ZHLN::Error> offline_texture_cooking_passthrough() {
            fs::path zcook = FindZcookExecutable();
            if (zcook.empty()) {
                return std::unexpected(CookerTestError::ZcookExecutableNotFound);
            }

            TempSandbox sandbox("cook_tex");
            fs::path    inTex  = sandbox.SubPath("albedo.png");
            fs::path    outTex = sandbox.SubPath("albedo.ztex");

            std::vector<uint8_t> dummyPng = {kPngMagicHeader0, 'P', 'N', 'G', '\r', '\n', kPngMagicSub, '\n', 0x00, 0x00, 0x00, kPngHeaderLen};
            {
                std::ofstream ofs(inTex, std::ios::binary);
                WriteVector(ofs, dummyPng);
            }

            std::string args     = std::format(R"(tex -i "{}" -o "{}")", inTex.string(), outTex.string());
            int         exitCode = RunZcook(zcook, args);
            ZHLN::Test::ExpectEq(exitCode, 0);

            if (exitCode != 0) {
                return std::unexpected(CookerTestError::TextureCookingFailed);
            }

            std::ifstream ifs(outTex, std::ios::binary | std::ios::ate);
            auto          size = ifs.tellg();
            ZHLN::Test::ExpectEq(static_cast<size_t>(size), dummyPng.size());

            return {};
        }

        // --- 5. Offline PAK Archive Creation (zcook pak) & Engine VFS Mount ---
        std::expected<void, ZHLN::Error> offline_pak_archive_creation_and_vfs_mount() {
            fs::path zcook = FindZcookExecutable();
            if (zcook.empty()) {
                return std::unexpected(CookerTestError::ZcookExecutableNotFound);
            }

            TempSandbox sandbox("cook_pak");
            fs::path    pakPath      = sandbox.SubPath("data/base.pak");
            fs::path    manifestPath = sandbox.SubPath("build_assets/manifest.txt");

            fs::path f1 = sandbox.SubPath("build_assets/player.zmesh");
            fs::path f2 = sandbox.SubPath("build_assets/albedo.ztex");
            fs::path f3 = sandbox.SubPath("build_assets/walk.zanim");

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
            int         exitCode = RunZcook(zcook, args);
            ZHLN::Test::ExpectEq(exitCode, 0);

            if (exitCode != 0) {
                return std::unexpected(CookerTestError::PakArchiveFailed);
            }

            std::ifstream   ifs(pakPath, std::ios::binary);
            ZHLN::PakHeader diskHeader {};
            ReadValue(ifs, diskHeader);

            ZHLN::Test::ExpectEq(std::string_view(diskHeader.magic, 4), "ZPAK");
            ZHLN::Test::ExpectEq(diskHeader.entryCount, static_cast<uint32_t>(kExpectedEntries3));

            ZHLN::CreativeWorksManager cwMgr;
            bool                       mounted = cwMgr.MountPak(pakPath.string());
            ZHLN::Test::ExpectTrue(mounted);

            // Fetch Mesh
            ZHLN::CreativeWorkLoadRequest reqMesh {.assetID = ZHLN::HashCreativeWorkPath("models/player.zmesh")};
            bool                          loadedMesh = cwMgr.LoadSync(reqMesh);
            ZHLN::Test::ExpectTrue(loadedMesh);
            ZHLN::Test::ExpectEq(reqMesh.outSize, content1.size());
            ZHLN::Test::ExpectEq(std::string_view(static_cast<const char*>(reqMesh.outData), reqMesh.outSize), content1);
            cwMgr.FreeCreativeWorkMemory(reqMesh);

            // Fetch Texture
            ZHLN::CreativeWorkLoadRequest reqTex {.assetID = ZHLN::HashCreativeWorkPath("textures/albedo.ztex")};
            bool                          loadedTex = cwMgr.LoadSync(reqTex);
            ZHLN::Test::ExpectTrue(loadedTex);
            ZHLN::Test::ExpectEq(reqTex.outSize, content2.size());
            ZHLN::Test::ExpectEq(std::string_view(static_cast<const char*>(reqTex.outData), reqTex.outSize), content2);
            cwMgr.FreeCreativeWorkMemory(reqTex);

            return {};
        }


        // --- 7. Compiler CLI Error Handling & Input Validation ---
        std::expected<void, ZHLN::Error> zcook_cli_subcommand_error_handling() {
            fs::path zcook = FindZcookExecutable();
            if (zcook.empty()) {
                return std::unexpected(CookerTestError::ZcookExecutableNotFound);
            }

            TempSandbox sandbox("cli_errors");
            fs::path    badMetaPath = sandbox.SubPath("missing_meta.bin");
            fs::path    badOutPath  = sandbox.SubPath("out.pak");
            fs::path    badManPath  = sandbox.SubPath("non_existent_manifest.txt");

            // Test 1: Unsupported subcommand
            int rc1 = RunZcook(zcook, "invalid_command");
            ZHLN::Test::ExpectEq(rc1, 1);

            // Test 2: Cook mesh missing required flags
            std::string args2 = std::format(R"(mesh --meta "{}")", badMetaPath.string());
            int         rc2   = RunZcook(zcook, args2);
            ZHLN::Test::ExpectEq(rc2, 1);

            // Test 3: Cook anim missing ID
            std::string args3 = std::format(R"(anim -o "{}")", sandbox.SubPath("out.zanim").string());
            int         rc3   = RunZcook(zcook, args3);
            ZHLN::Test::ExpectEq(rc3, 1);

            // Test 4: Pack missing manifest
            std::string args4 = std::format(R"(pak -o "{}" -i "{}")", badOutPath.string(), badManPath.string());
            int         rc4   = RunZcook(zcook, args4);
            ZHLN::Test::ExpectEq(rc4, 1);

            // Test 5: Cook GLB missing metadata
            std::string args5 = std::format(R"(glb -o "{}")", sandbox.SubPath("out.glb").string());
            int         rc5   = RunZcook(zcook, args5);
            ZHLN::Test::ExpectEq(rc5, 1);

            return {};
        }
    };
};

// Exported for the assets group binary (RunAssetsTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunCookerSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<CookerTestSuite>();
}

