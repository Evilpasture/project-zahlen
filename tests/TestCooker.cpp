// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/CreativeWorksManager.hpp>
#include <Zahlen/JSON.hpp>
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

namespace fs = std::filesystem;

// ============================================================================
// Constants & Enums
// ============================================================================

namespace {

constexpr size_t kMatrixElements   = 16;
constexpr size_t kVec3Elements     = 3;
constexpr size_t kVec4Elements     = 4;
constexpr size_t kGlbHeaderSize    = 12;
constexpr size_t kGlbChunkHdrSize  = 8;
constexpr size_t kGlbMinSize       = 20;
constexpr size_t kExpectedEntries3 = 3;

constexpr uint32_t kZmetVersion          = 1u;
constexpr uint32_t kMeshMagic            = 0x3048534Du; // 'MSH0'
constexpr uint32_t kMeshVersion          = 4u;
constexpr uint32_t kAnimMagic            = 0x304D4E41u; // 'ANM0'
constexpr uint32_t kAnimVersion          = 1u;
constexpr uint32_t kGlbMagic             = 0x46546C67u; // 'glTF'
constexpr uint32_t kGlbVersion           = 2u;
constexpr uint32_t kGlbJsonChunkType     = 0x4E4F534Au; // 'JSON'
constexpr uint32_t kWorkerThreadCount    = 2u;
constexpr uint32_t kFiberCount           = 32u;
constexpr size_t   kFiberStackSize       = ZHLN::kMinimumFiberStackSize;
constexpr uint8_t  kPngMagicHeader0      = 0x89u;
constexpr uint8_t  kPngMagicSub          = 0x1Au;
constexpr uint8_t  kPngHeaderLen         = 0x0Du;
constexpr uint32_t kPillarVertexCount    = 3u;
constexpr uint32_t kVertexBufferSize4096 = 4096u;

constexpr float kSampleTranslY  = 1.5f;
constexpr float kSampleMetallic = 0.2f;

} // namespace

enum class CookerTestError : uint8_t {
    ZcookExecutableNotFound[[= ZHLN::Reflect::Description("Could not find the 'zcook' compiler binary in the build tree.")]] = 1,
    MetadataSerializationFailed[[= ZHLN::Reflect::Description("Failed to serialize intermediate binary metadata.")]],
    MeshCompilationFailed[[= ZHLN::Reflect::Description("zcook mesh subcommand failed to produce a valid .zmesh asset.")]],
    AnimationCompilationFailed[[= ZHLN::Reflect::Description("zcook anim subcommand failed to produce a valid .zanim asset.")]],
    TextureCookingFailed[[= ZHLN::Reflect::Description("zcook tex subcommand failed to cook the texture asset.")]],
    PakArchiveFailed[[= ZHLN::Reflect::Description("zcook pak subcommand failed to compile the .pak archive.")]],
    VfsMountVerificationFailed[[= ZHLN::Reflect::Description("CreativeWorksManager failed to mount or load assets from zcook-generated .pak.")]],
    GLBEmissionFailed[[= ZHLN::Reflect::Description("zcook glb subcommand failed to produce a valid glTF 2.0 container.")]],
    CLIErrorHandlingFailed[[= ZHLN::Reflect::Description("zcook failed to return a non-zero exit code on malformed arguments.")]],
};

// ============================================================================
// RAII Isolation Sandbox
// ============================================================================

struct TempSandbox {
    fs::path rootPath;

    explicit TempSandbox(std::string_view testName) {
        auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        rootPath       = fs::temp_directory_path() / std::format("zhln_cooker_{}_{}", testName, timestamp);
        std::error_code ec;
        fs::create_directories(rootPath, ec);
    }

    ~TempSandbox() {
        std::error_code ec;
        fs::remove_all(rootPath, ec);
    }

    TempSandbox(const TempSandbox&)                = delete;
    TempSandbox& operator=(const TempSandbox&)     = delete;
    TempSandbox(TempSandbox&&) noexcept            = default;
    TempSandbox& operator=(TempSandbox&&) noexcept = default;

    [[nodiscard]] fs::path SubPath(std::string_view rel) const {
        fs::path        p = rootPath / rel;
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        return p;
    }
};

// ============================================================================
// Typesafe Binary Stream Helpers
// ============================================================================

namespace {

template <typename T>
inline void WriteValue(std::ostream& stream, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<char, sizeof(T)> buf {};
    std::memcpy(buf.data(), &value, sizeof(T));
    stream.write(buf.data(), static_cast<std::streamsize>(sizeof(T)));
}

template <typename T, size_t N>
inline void WriteArray(std::ostream& stream, const std::array<T, N>& arr) {
    static_assert(std::is_trivially_copyable_v<T>);
    for (const auto& item: arr) {
        WriteValue(stream, item);
    }
}

template <typename T>
inline void WriteVector(std::ostream& stream, const std::vector<T>& vec) {
    static_assert(std::is_trivially_copyable_v<T>);
    for (const auto& item: vec) {
        WriteValue(stream, item);
    }
}

template <typename T>
inline bool ReadValue(std::istream& stream, T& outValue) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::array<char, sizeof(T)> buf {};
    if (!stream.read(buf.data(), static_cast<std::streamsize>(sizeof(T)))) {
        return false;
    }
    std::memcpy(&outValue, buf.data(), sizeof(T));
    return true;
}

// Locates the compiled zcook executable across build platforms
fs::path FindZcookExecutable() {
    std::vector<fs::path> candidates = {"zcook", "./zcook", "../zcook", "bin/zcook", "zcook.exe", "./zcook.exe", "../zcook.exe", "bin/zcook.exe"};

    for (const auto& candidate: candidates) {
        std::error_code ec;
        if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec)) {
            return fs::canonical(candidate, ec);
        }
    }
    return {};
}

// Executes zcook subcommands through standard process execution and unwraps POSIX exit code
int RunZcook(const fs::path& zcookBin, std::string_view args) {
    std::string cmd    = std::format("\"{}\" {}", zcookBin.string(), args);
    int         status = std::system(cmd.c_str());
#if defined(_WIN32)
    return status;
#else
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return status;
#endif
}

} // namespace

// ============================================================================
// Metadata Serializer for Compiler Input Generation
// ============================================================================

namespace { namespace Metadata {

struct Material {
    std::string                      id;
    std::string                      albedoMap;
    std::string                      normalMap;
    std::string                      metallicRoughnessMap;
    std::string                      emissiveMap;
    std::array<float, kVec4Elements> baseColor        = {1.0f, 1.0f, 1.0f, 1.0f};
    float                            metallic         = 0.0f;
    float                            roughness        = 0.5f;
    std::array<float, kVec3Elements> emissiveFactor   = {0.0f, 0.0f, 0.0f};
    float                            emissiveStrength = 1.0f;
    bool                             doubleSided      = false;
};

struct Primitive {
    std::string materialId;
    uint32_t    vertexOffset = 0;
    uint32_t    vertexCount  = 0;
};

struct MorphTarget {
    std::string name;
    std::string binFile;
    uint32_t    byteOffset = 0;
    uint32_t    byteLength = 0;
};

struct Mesh {
    std::string              id;
    std::string              layout;
    std::string              binFile;
    uint32_t                 byteOffset = 0;
    uint32_t                 byteLength = 0;
    std::vector<Primitive>   primitives;
    std::vector<MorphTarget> morphTargets;
};

struct Node {
    std::string                        id;
    std::string                        parentId;
    bool                               visible     = true;
    std::array<float, kMatrixElements> localMatrix = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, kMatrixElements> worldMatrix = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    std::string                        meshId;
    std::string                        skinId;
    std::string                        lightId;
};

struct Light {
    std::string                      id;
    std::string                      type;
    std::array<float, kVec3Elements> color     = {1.0f, 1.0f, 1.0f};
    float                            intensity = 1.0f;
};

struct Skin {
    std::string              id;
    std::string              name;
    std::vector<std::string> joints;
    std::vector<std::string> parents;
    std::vector<float>       inverseBindMatrices;
    std::vector<float>       restPose;
};

struct AnimationSampler {
    std::string interpolation;
    uint32_t    inputOffset  = 0;
    uint32_t    inputLength  = 0;
    uint32_t    outputOffset = 0;
    uint32_t    outputLength = 0;
    std::string binFile;
};

struct AnimationChannel {
    std::string targetNodeId;
    std::string targetPath;
    uint32_t    samplerId = 0;
};

struct Animation {
    std::string                   id;
    std::string                   name;
    float                         duration = 0.0f;
    bool                          loop     = false;
    std::vector<AnimationChannel> channels;
    std::vector<AnimationSampler> samplers;
};

struct Manifest {
    std::string            levelName;
    std::vector<Material>  materials;
    std::vector<Mesh>      meshes;
    std::vector<Node>      nodes;
    std::vector<Light>     lights;
    std::vector<Skin>      skins;
    std::vector<Animation> animations;
};

class BinaryStreamWriter {
  public:
    explicit BinaryStreamWriter(std::ostream& stream): m_stream(stream) {
    }

    void WriteString(const std::string& s) {
        const auto len = static_cast<uint32_t>(s.size());
        WriteValue(len);
        if (len > 0) {
            m_stream.write(s.c_str(), static_cast<std::streamsize>(s.size()));
        }
    }

    void WriteString(std::string_view sv) {
        WriteString(std::string(sv));
    }

    template <typename T>
    void WriteValue(const T& val) {
        static_assert(std::is_trivially_copyable_v<T>);
        std::array<char, sizeof(T)> buf {};
        std::memcpy(buf.data(), &val, sizeof(T));
        m_stream.write(buf.data(), static_cast<std::streamsize>(sizeof(T)));
    }

    template <typename T, size_t N>
    void WriteArray(const std::array<T, N>& arr) {
        static_assert(std::is_trivially_copyable_v<T>);
        for (const auto& item: arr) {
            WriteValue(item);
        }
    }

    template <typename T>
    void WriteVector(const std::vector<T>& vec) {
        static_assert(std::is_trivially_copyable_v<T>);
        for (const auto& item: vec) {
            WriteValue(item);
        }
    }

    void WriteFloatVectorPrefixed(const std::vector<float>& vec) {
        const auto count = static_cast<uint32_t>(vec.size());
        WriteValue(count);
        WriteVector(vec);
    }

    void SerializeManifest(const Manifest& manifest) {
        m_stream.write("ZMET", 4);
        WriteValue(kZmetVersion);

        WriteString(manifest.levelName);

        // 1. Materials
        WriteValue(static_cast<uint32_t>(manifest.materials.size()));
        for (const auto& mat: manifest.materials) {
            WriteString(mat.id);
            WriteArray(mat.baseColor);
            WriteValue(mat.metallic);
            WriteValue(mat.roughness);
            WriteArray(mat.emissiveFactor);
            WriteValue(mat.emissiveStrength);
            WriteValue(static_cast<uint8_t>(mat.doubleSided ? 1u : 0u));
            WriteString(mat.albedoMap);
            WriteString(mat.normalMap);
            WriteString(mat.metallicRoughnessMap);
            WriteString(mat.emissiveMap);
            WriteValue(static_cast<uint8_t>(0)); // hasProcedural = 0
        }

        // 2. Meshes
        WriteValue(static_cast<uint32_t>(manifest.meshes.size()));
        for (const auto& mesh: manifest.meshes) {
            WriteString(mesh.id);
            WriteString(mesh.layout);
            WriteString(mesh.binFile);
            WriteValue(mesh.byteOffset);
            WriteValue(mesh.byteLength);

            WriteValue(static_cast<uint32_t>(mesh.primitives.size()));
            for (const auto& prim: mesh.primitives) {
                WriteString(prim.materialId);
                WriteValue(prim.vertexOffset);
                WriteValue(prim.vertexCount);
            }

            WriteValue(static_cast<uint32_t>(mesh.morphTargets.size()));
            for (const auto& target: mesh.morphTargets) {
                WriteString(target.name);
                WriteString(target.binFile);
                WriteValue(target.byteOffset);
                WriteValue(target.byteLength);
            }
        }

        // 3. Nodes
        WriteValue(static_cast<uint32_t>(manifest.nodes.size()));
        for (const auto& node: manifest.nodes) {
            WriteString(node.id);
            WriteString(node.parentId);
            WriteValue(static_cast<uint8_t>(node.visible ? 1u : 0u));
            WriteArray(node.localMatrix);
            WriteArray(node.worldMatrix);
            WriteString(node.meshId);
            WriteString(node.skinId);
            WriteString(node.lightId);
        }

        // 4. Lights
        WriteValue(static_cast<uint32_t>(manifest.lights.size()));
        for (const auto& light: manifest.lights) {
            WriteString(light.id);
            WriteString(light.type);
            WriteArray(light.color);
            WriteValue(light.intensity);
        }

        // 5. Skins
        WriteValue(static_cast<uint32_t>(manifest.skins.size()));
        for (const auto& skin: manifest.skins) {
            WriteString(skin.id);
            WriteString(skin.name);

            WriteValue(static_cast<uint32_t>(skin.joints.size()));
            for (const auto& j: skin.joints) {
                WriteString(j);
            }

            WriteValue(static_cast<uint32_t>(skin.parents.size()));
            for (const auto& p: skin.parents) {
                WriteString(p);
            }

            WriteFloatVectorPrefixed(skin.inverseBindMatrices);
            WriteFloatVectorPrefixed(skin.restPose);
        }

        // 6. Animations
        WriteValue(static_cast<uint32_t>(manifest.animations.size()));
        for (const auto& anim: manifest.animations) {
            WriteString(anim.id);
            WriteString(anim.name);
            WriteValue(anim.duration);
            WriteValue(static_cast<uint8_t>(anim.loop ? 1u : 0u));

            WriteValue(static_cast<uint32_t>(anim.channels.size()));
            for (const auto& chan: anim.channels) {
                WriteString(chan.targetNodeId);
                WriteString(chan.targetPath);
                WriteValue(chan.samplerId);
            }

            WriteValue(static_cast<uint32_t>(anim.samplers.size()));
            for (const auto& samp: anim.samplers) {
                WriteString(samp.interpolation);
                WriteValue(samp.inputOffset);
                WriteValue(samp.inputLength);
                WriteValue(samp.outputOffset);
                WriteValue(samp.outputLength);
                WriteString(samp.binFile);
            }
        }
    }

  private:
    std::ostream& m_stream;
};

}} // namespace ::Metadata

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

int main() {
    return ZHLN::Test::Runner::Run<CookerTestSuite>();
}
