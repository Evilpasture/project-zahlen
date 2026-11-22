// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Cook.hpp"
#include <AssetCooking/AnimCook.hpp>
#include <AssetCooking/MeshCook.hpp>
#include <AssetCooking/PakWriter.hpp>
#include "BinaryReader.hpp"
#include "GLB.hpp"
#include <AssetCooking/RadianceDecoder.hpp>
#include <AssetCooking/RadianceEncoder.hpp>
// The cook formats themselves -- CookedMeshHeader, CookedAnimHeader, the pak
// records -- live in this engine-internal header. zcook is a build-tree tool
// that links the engine's objects, so it reads them; the header is not exported,
// which is what keeps them out of the API.
#include <Zahlen/FileSystem/VFS.hpp>
#include <Zahlen/Render/CookedFormats.hpp>
#include <Zahlen/Threading/Mutex.hpp>
#include <Zahlen/Error.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Render/Meshlet.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <print>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace ZHLN {


int CookMesh(int argc, char** argv) {
    std::string metaPath, meshId, outPath, inPath;
    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--meta" && i + 1 < argc)
            metaPath = argv[++i];
        else if (arg == "--id" && i + 1 < argc)
            meshId = argv[++i];
        else if (arg == "-i" && i + 1 < argc)
            inPath = argv[++i];
        else if (arg == "-o" && i + 1 < argc)
            outPath = argv[++i];
    }

    if (metaPath.empty() || meshId.empty() || inPath.empty() || outPath.empty()) {
        std::println(stderr, "[zcook] ERROR: Missing arguments for mesh subcommand.");
        return 1;
    }

    // Safely patch the extension incase the build system passed .json instead of .bin
    std::string binMetaPath = fs::path(metaPath).replace_extension(".bin").string();

    Compiler::BinaryReader reader(binMetaPath);
    auto                   manifestResult = reader.Parse();
    if (!manifestResult) {
        std::println(stderr, "[zcook] ERROR: {}", manifestResult.error());
        return 1;
    }
    Compiler::IRManifest manifest = std::move(*manifestResult);

    auto it = std::ranges::find_if(manifest.meshes, [&](const auto& m) { return m.id == meshId; });
    if (it == manifest.meshes.end()) {
        std::println(stderr, "[zcook] ERROR: Mesh '{}' not found in manifest.", meshId);
        return 1;
    }

    // Disk + Blender-IR adaptation stays in the CLI; the baker is
    // AssetCooking::CookMesh over in-memory spans.
    FILE* bf = std::fopen(inPath.c_str(), "rb");
    if (bf == nullptr) {
        std::println(stderr, "[zcook] ERROR: Failed to open intermediate bin file '{}'", inPath);
        return 1;
    }
    std::fseek(bf, 0, SEEK_END);
    const long binSize = std::ftell(bf);
    std::fseek(bf, 0, SEEK_SET);
    std::vector<float> rawFloats(binSize > 0 ? static_cast<size_t>(binSize) / sizeof(float) : 0);
    if (!rawFloats.empty()) {
        std::fread(rawFloats.data(), sizeof(float), rawFloats.size(), bf);
    }
    std::fclose(bf);

    std::vector<AssetCooking::MeshPrimitive> primitives;
    primitives.reserve(it->primitives.size());
    for (const auto& prim: it->primitives) {
        primitives.push_back({prim.materialId, prim.vertexOffset, prim.vertexCount});
    }

    const auto cooked = AssetCooking::CookMesh(AssetCooking::RawMeshInput {
        .loopVertices = rawFloats,
        .skinned      = it->layout.contains("_J4W4"),
        .primitives   = primitives,
    });
    if (!cooked) {
        std::println(stderr, "[zcook] ERROR: Mesh cooking failed: {}", ZHLN::Error(cooked.error()).Message());
        return 1;
    }

    fs::create_directories(fs::path(outPath).parent_path());
    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (out == nullptr) {
        std::println(stderr, "[zcook] ERROR: Failed to open '{}' for writing.", outPath);
        return 1;
    }
    std::fwrite(cooked->data(), 1, cooked->size(), out);
    std::fclose(out);
    return 0;
}

int CookTexture(int argc, char** argv) {
    std::string inPath, outPath;
    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-i" && i + 1 < argc)
            inPath = argv[++i];
        else if (arg == "-o" && i + 1 < argc)
            outPath = argv[++i];
    }
    if (inPath.empty() || outPath.empty()) {
        std::println(stderr, "[zcook] ERROR: Missing arguments for texture subcommand.");
        return 1;
    }

    FILE* in = std::fopen(inPath.c_str(), "rb");
    if (!in) {
        std::println(stderr, "[zcook] ERROR: Failed to open '{}' for reading.", inPath);
        return 1;
    }
    std::fseek(in, 0, SEEK_END);
    long size = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    if (size < 0) {
        std::println(stderr, "[zcook] ERROR: Failed to read '{}'.", inPath);
        std::fclose(in);
        return 1;
    }
    std::vector<char> fileData(static_cast<size_t>(size));
    std::fread(fileData.data(), 1, static_cast<size_t>(size), in);
    std::fclose(in);

    fs::create_directories(fs::path(outPath).parent_path());
    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) {
        std::println(stderr, "[zcook] ERROR: Failed to open '{}' for writing.", outPath);
        return 1;
    }

    // Non-.hdr stays a verbatim copy (offline_texture_cooking_passthrough).
    // .hdr becomes prepared ZRD2 (original sky, sunless IBL, optional sun).
    // A host can explicitly decode source or cooked assets with AssetCooking.
    const auto ext = fs::path(inPath).extension().string();
    const bool isHdr = ext == ".hdr" || ext == ".HDR";
    if (isHdr) {
        const auto decoded = AssetCooking::DecodeRadiance(std::span<const std::byte>(reinterpret_cast<const std::byte*>(fileData.data()), static_cast<size_t>(size)));
        if (!decoded) {
            const ZHLN::Error err = decoded.error();
            std::println(stderr, "[zcook] ERROR: HDR decode failed: {} ({})", err.Message(), err.Name());
            std::fclose(out);
            return 1;
        }
        const auto cooked = AssetCooking::EncodeCookedRadiance(*decoded);
        if (cooked.empty() || std::fwrite(cooked.data(), 1, cooked.size(), out) != cooked.size()) {
            std::println(stderr, "[zcook] ERROR: Could not write prepared HDR '{}'", outPath);
            std::fclose(out);
            return 1;
        }
        std::fclose(out);
        return 0;
    }

    std::fwrite(fileData.data(), 1, size, out);
    std::fclose(out);
    return 0;
}

int CookAnimation(int argc, char** argv) {
    std::string metaPath;
    std::string outPath;
    std::string animId;
    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--meta" && i + 1 < argc) {
            metaPath = argv[++i];
        } else if (arg == "--id" && i + 1 < argc) {
            animId = argv[++i];
        } else if (arg == "-o" && i + 1 < argc) {
            outPath = argv[++i];
        }
    }
    if (metaPath.empty() || animId.empty() || outPath.empty()) {
        std::println(stderr, "[zcook] ERROR: Missing arguments for animation subcommand.");
        return 1;
    }

    std::string            binMetaPath = fs::path(metaPath).replace_extension(".bin").string();
    Compiler::BinaryReader reader(binMetaPath);
    auto                   manifestResult = reader.Parse();
    if (!manifestResult) {
        std::println(stderr, "[zcook] ERROR: {}", manifestResult.error());
        return 1;
    }
    Compiler::IRManifest manifest = std::move(*manifestResult);

    auto it = std::ranges::find_if(manifest.animations, [&](const auto& a) { return a.id == animId; });
    if (it == manifest.animations.end()) {
        std::println(stderr, "[zcook] ERROR: Animation '{}' not found in manifest.", animId);
        return 1;
    }

    const auto& anim        = *it;
    std::string levelFolder = fs::path(metaPath).parent_path().string();

    // Sampler payloads are read here (Blender-IR bridge); the baker itself
    // only sees in-memory times/values.
    struct OwnedTrack {
        AssetCooking::AnimTrackInput         input;
        std::vector<float>                   times;
        std::vector<std::byte>               values;
    };
    std::vector<OwnedTrack>              owned;
    std::vector<AssetCooking::AnimTrackInput> tracks;

    bool payloadFailed = false;
    auto readPayload = [&](std::string_view binFile, uint32_t offset, uint32_t length) -> std::vector<std::byte> {
        std::string fullBinPath = std::format("{}/{}", levelFolder, binFile);
        FILE*       bf          = std::fopen(fullBinPath.c_str(), "rb");
        if (!bf) {
            payloadFailed = true;
            return {};
        }
        std::vector<std::byte> bytes(length);
        std::fseek(bf, offset, SEEK_SET);
        if (length > 0) {
            bytes.resize(std::fread(bytes.data(), 1, length, bf));
        }
        std::fclose(bf);
        return bytes;
    };

    for (const auto& channel: anim.channels) {
        if (channel.samplerId >= anim.samplers.size()) {
            continue;
        }
        const auto& sampler = anim.samplers[channel.samplerId];

        uint32_t pathType = 0;
        if (channel.targetPath == "translation") {
            pathType = AssetCooking::kAnimPathTranslation;
        } else if (channel.targetPath == "rotation") {
            pathType = AssetCooking::kAnimPathRotation;
        } else if (channel.targetPath == "scale") {
            pathType = AssetCooking::kAnimPathScale;
        } else {
            continue;
        }

        OwnedTrack track;
        track.values = readPayload(sampler.binFile, sampler.outputOffset, sampler.outputLength);
        const auto timeBytes = readPayload(sampler.binFile, sampler.inputOffset, sampler.inputLength);
        // std::byte storage read as floats goes through memcpy (alignment and
        // strict aliasing), and a partial tail float is dropped, not read.
        track.times.resize(timeBytes.size() / sizeof(float));
        if (!track.times.empty()) {
            std::memcpy(track.times.data(), timeBytes.data(), track.times.size() * sizeof(float));
        }
        track.input = {
            .targetNodeHash = HashAssetPath(channel.targetNodeId),
            .pathType       = pathType,
            .times          = track.times,
            .values         = track.values,
        };
        owned.push_back(std::move(track));
    }
    if (payloadFailed) {
        std::println(stderr, "[zcook] ERROR: Failed to open an animation payload .bin under '{}'", levelFolder);
        return 1;
    }
    tracks.reserve(owned.size());
    for (const auto& track: owned) {
        tracks.push_back(track.input);
    }

    const auto bytes = AssetCooking::CookAnimation({.duration = anim.duration, .loop = anim.loop, .tracks = tracks});
    fs::create_directories(fs::path(outPath).parent_path());
    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (out == nullptr) {
        std::println(stderr, "[zcook] ERROR: Failed to open '{}' for writing.", outPath);
        return 1;
    }
    std::fwrite(bytes.data(), 1, bytes.size(), out);
    std::fclose(out);
    return 0;
}

int PackArchive(int argc, char** argv) {
    std::string outPath, manifestPath;
    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-o" && i + 1 < argc)
            outPath = argv[++i];
        else if (arg == "-i" && i + 1 < argc)
            manifestPath = argv[++i];
    }
    if (outPath.empty() || manifestPath.empty()) {
        std::println(stderr, "[zcook] ERROR: Missing arguments for pack subcommand.");
        return 1;
    }

    std::ifstream ifs(manifestPath);
    if (!ifs.is_open()) {
        std::println(stderr, "[zcook] ERROR: Failed to open '{}' for reading.", manifestPath);
        return 1;
    }

    // Manifest + file loading is the CLI's job; the archive layout itself is
    // AssetCooking::PackArchive, shared with in-memory consumers.
    struct LoadedFile {
        std::string            virtualPath;
        std::vector<std::byte> data;
    };
    std::vector<LoadedFile> loaded;
    std::string             line;
    size_t                  failed = 0;
    while (std::getline(ifs, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        auto pos = line.find('=');
        if (pos == std::string::npos)
            continue;
        LoadedFile file {.virtualPath = line.substr(0, pos)};
        FILE*      f = std::fopen(line.substr(pos + 1).c_str(), "rb");
        if (f == nullptr) {
            ++failed;
            continue;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0) {
            file.data.resize(static_cast<size_t>(size));
            file.data.resize(std::fread(file.data.data(), 1, file.data.size(), f));
        }
        std::fclose(f);
        loaded.push_back(std::move(file));
    }
    ifs.close();

    std::vector<AssetCooking::PakInput> inputs;
    inputs.reserve(loaded.size());
    for (const auto& file: loaded) {
        inputs.push_back({file.virtualPath, file.data});
    }
    const auto bytes = AssetCooking::PackArchive(inputs);

    fs::create_directories(fs::path(outPath).parent_path());
    FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) {
        std::println(stderr, "[zcook] ERROR: Failed to open '{}' for writing.", outPath);
        return 1;
    }
    std::fwrite(bytes.data(), 1, bytes.size(), out);
    std::fclose(out);

    std::println("[zcook] Successfully packed {}/{} assets into '{}'", loaded.size(), loaded.size() + failed, outPath);
    (void)failed;
    return 0;
}

int CookGLB(int argc, char** argv) {
    std::string metaPath, outPath;
    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--meta" && i + 1 < argc) {
            metaPath = argv[++i];
        } else if (arg == "-o" && i + 1 < argc) {
            outPath = argv[++i];
        }
    }
    if (metaPath.empty() || outPath.empty()) {
        std::println(stderr, "[zcook] ERROR: Missing arguments for glb subcommand.");
        return 1;
    }

    std::string            binMetaPath = fs::path(metaPath).replace_extension(".bin").string();
    Compiler::BinaryReader reader(binMetaPath);
    auto                   manifestResult = reader.Parse();
    if (!manifestResult) {
        std::println(stderr, "[zcook] ERROR: {}", manifestResult.error());
        return 1;
    }
    Compiler::IRManifest manifest = std::move(*manifestResult);

    std::string levelFolder = fs::path(metaPath).parent_path().string();
    if (!GLB::EmitGLB(manifest, levelFolder, outPath)) {
        std::println(stderr, "[zcook] ERROR: Failed to generate GLB.");
        return 1;
    }
    return 0;
}

} // namespace ZHLN
