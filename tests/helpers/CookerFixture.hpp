// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// tests/helpers/CookerFixture.hpp
//
// Everything the zcook suites need to build a compiler input, run the cooker
// binary, and read back what it emitted: the RAII sandbox, the .zmet binary
// writer, the executable lookup, and the constants both suites assert against.
//
// It is a header because two binaries share it. tests/assets/TestCooker.cpp
// drives the cooker end to end and links the core engine alone; the glTF
// container check lives in tests/extras/TestCookerGLB.cpp, because reading the
// JSON chunk back needs extras/json. Each binary includes this exactly once, so
// the internal-linkage helpers below are defined once per process.
//
// Public headers only. src/ is banned — see tools/check_tests_public_api.py.

#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Types.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
    ZcookExecutableNotFound ZHLN_ANNOTATION(ZHLN::Description<"Could not find the 'zcook' compiler binary in the build tree.">{}) = 1,
    MetadataSerializationFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to serialize intermediate binary metadata.">{}),
    MeshCompilationFailed ZHLN_ANNOTATION(ZHLN::Description<"zcook mesh subcommand failed to produce a valid .zmesh asset.">{}),
    AnimationCompilationFailed ZHLN_ANNOTATION(ZHLN::Description<"zcook anim subcommand failed to produce a valid .zanim asset.">{}),
    TextureCookingFailed ZHLN_ANNOTATION(ZHLN::Description<"zcook tex subcommand failed to cook the texture asset.">{}),
    PakArchiveFailed ZHLN_ANNOTATION(ZHLN::Description<"zcook pak subcommand failed to compile the .pak archive.">{}),
    VfsMountVerificationFailed ZHLN_ANNOTATION(ZHLN::Description<"CreativeWorksManager failed to mount or load assets from zcook-generated .pak.">{}),
    GLBEmissionFailed ZHLN_ANNOTATION(ZHLN::Description<"zcook glb subcommand failed to produce a valid glTF 2.0 container.">{}),
    CLIErrorHandlingFailed ZHLN_ANNOTATION(ZHLN::Description<"zcook failed to return a non-zero exit code on malformed arguments.">{}),
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
// Resolved against the process working directory, which CTest sets to the
// directory holding the add_test() call. zcook itself is built at the root of
// the build tree, so the candidate list has to cover one level per nesting
// step between there and the group: tests/ is one down, tests/extras/ is two.
fs::path FindZcookExecutable() {
    std::vector<fs::path> candidates = {"zcook",
                                        "./zcook",
                                        "../zcook",
                                        "../../zcook",
                                        "bin/zcook",
                                        "zcook.exe",
                                        "./zcook.exe",
                                        "../zcook.exe",
                                        "../../zcook.exe",
                                        "bin/zcook.exe"};

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
