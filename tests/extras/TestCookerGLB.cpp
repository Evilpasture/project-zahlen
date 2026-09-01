// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestCookerGLB.cpp
//
// The glTF container check for the offline cooker. It lived in
// tests/assets/TestCooker.cpp until reading the emitted JSON chunk back turned
// out to be the one thing in that suite an extras-free build cannot do: JSON is
// an extra. So the check moved here, next to the other JSON suites, and the
// core suite kept the container framing, the .zmet round-trip, the pak archive
// and the CLI error handling -- none of which parse a document.
//
// The fixtures are shared rather than copied; see tests/helpers/CookerFixture.hpp.

#include "TestsFramework.hpp"
#include "helpers/CookerFixture.hpp"
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <json/JSON.hpp>
#include <string>

namespace fs = std::filesystem;

struct CookerGlbTestSuite {
    CookerGlbTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(kWorkerThreadCount, kFiberCount, kFiberStackSize);
    }

    ~CookerGlbTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    CookerGlbTestSuite(const CookerGlbTestSuite&)                = delete;
    CookerGlbTestSuite& operator=(const CookerGlbTestSuite&)     = delete;
    CookerGlbTestSuite(CookerGlbTestSuite&&) noexcept            = delete;
    CookerGlbTestSuite& operator=(CookerGlbTestSuite&&) noexcept = delete;

    struct Tests {
        // --- 6. Offline GLB Container Generation (zcook glb) ---
        std::expected<void, ZHLN::Error> offline_glb_container_generation() {
            fs::path zcook = FindZcookExecutable();
            if (zcook.empty()) {
                return std::unexpected(CookerTestError::ZcookExecutableNotFound);
            }

            TempSandbox sandbox("cook_glb");
            fs::path    metaPath    = sandbox.SubPath("metadata.bin");
            fs::path    geomBinPath = sandbox.SubPath("cube_geom.bin");
            fs::path    outGlb      = sandbox.SubPath("level.glb");

            std::vector<float> rawFloats = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                            1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                            2.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
            {
                std::ofstream bf(geomBinPath, std::ios::binary);
                WriteVector(bf, rawFloats);
            }

            Metadata::Manifest manifest;
            manifest.levelName = "GlbScene";

            Metadata::Material mat;
            mat.id           = "mat_cube";
            mat.baseColor[0] = 1.0f;
            mat.metallic     = kSampleMetallic;
            mat.roughness    = 0.5f;
            manifest.materials.push_back(mat);

            Metadata::Mesh mesh;
            mesh.id         = "Mesh_Cube";
            mesh.layout     = "P3N3T2C4";
            mesh.binFile    = "cube_geom.bin";
            mesh.byteOffset = 0;
            mesh.byteLength = static_cast<uint32_t>(rawFloats.size() * sizeof(float));
            mesh.primitives.push_back({.materialId = "mat_cube", .vertexOffset = 0, .vertexCount = 3});
            manifest.meshes.push_back(mesh);

            Metadata::Node rootNode;
            rootNode.id      = "Root";
            rootNode.visible = true;
            manifest.nodes.push_back(rootNode);

            Metadata::Node cubeNode;
            cubeNode.id       = "CubeNode";
            cubeNode.parentId = "Root";
            cubeNode.meshId   = "Mesh_Cube";
            cubeNode.visible  = true;
            manifest.nodes.push_back(cubeNode);

            {
                std::ofstream                mf(metaPath, std::ios::binary);
                Metadata::BinaryStreamWriter writer(mf);
                writer.SerializeManifest(manifest);
            }

            std::string args     = std::format(R"(glb --meta "{}" -o "{}")", metaPath.string(), outGlb.string());
            int         exitCode = RunZcook(zcook, args);
            ZHLN::Test::ExpectEq(exitCode, 0);

            if (exitCode != 0) {
                return std::unexpected(CookerTestError::GLBEmissionFailed);
            }

            std::ifstream ifs(outGlb, std::ios::binary | std::ios::ate);
            auto          fileSize = ifs.tellg();
            ifs.seekg(0, std::ios::beg);

            ZHLN::Test::ExpectTrue(fileSize >= kGlbMinSize);

            uint32_t readMagic  = 0;
            uint32_t readVer    = 0;
            uint32_t readLength = 0;
            ReadValue(ifs, readMagic);
            ReadValue(ifs, readVer);
            ReadValue(ifs, readLength);

            ZHLN::Test::ExpectEq(readMagic, kGlbMagic);
            ZHLN::Test::ExpectEq(readVer, kGlbVersion);
            ZHLN::Test::ExpectEq(readLength, static_cast<uint32_t>(fileSize));

            uint32_t readJsonLen  = 0;
            uint32_t readJsonType = 0;
            ReadValue(ifs, readJsonLen);
            ReadValue(ifs, readJsonType);

            ZHLN::Test::ExpectEq(readJsonType, kGlbJsonChunkType);
            std::string readJson(readJsonLen, '\0');
            ifs.read(readJson.data(), static_cast<std::streamsize>(readJsonLen));

            auto docRes = ZHLN::ReflectJSON::Document::Parse(readJson);
            ZHLN::Test::ExpectTrue(docRes.has_value());

            if (docRes) {
                auto root = docRes->GetRoot();
                ZHLN::Test::ExpectTrue(root.GetKey("asset").has_value());
                ZHLN::Test::ExpectTrue(root.GetKey("scenes").has_value());
                ZHLN::Test::ExpectTrue(root.GetKey("meshes").has_value());
            }

            return {};
        }
    };
};

// The extras test binaries are one suite per process (see
// tests/extras/CMakeLists.txt), so this owns its own entry point.
int main() {
    return ZHLN::Test::Runner::Run<CookerGlbTestSuite>();
}
