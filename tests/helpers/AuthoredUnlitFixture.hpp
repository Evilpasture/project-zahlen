// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/helpers/AuthoredUnlitFixture.hpp
//
// In-memory replacement for the retired Khronos UnlitTest.glb sample asset
// (CC-BY 4.0, (c) 2019 Analytical Graphics, Inc. -- Ed Mackey): two
// KHR_materials_unlit chamfered boxes (orange/blue, X = -/+1.2) sharing one
// POSITION/NORMAL/index buffer, assembled into GLB bytes by MakeUnlitGlb().
//
// The JSON chunk follows the TestGLTFImport.cpp convention: declarations, not
// text -- ZHLN::ReflectJSON::SerializeJSON turns each struct into JSON, so
// field names are the glTF keys and the compiler checks every value's type.
// The binary chunk is BuildChamferedBox(). The importer behaviour under test
// stays the importer's own: this only produces bytes a conformant loader must
// accept.

#pragma once

#include "helpers/ChamferedBoxMesh.hpp"
#include <json/JSONSchema.hpp>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::Test::GltfFixtures {

struct UnlitAsset {
    std::string_view version = "2.0";
};

struct UnlitScene {
    std::vector<int32_t> nodes;
};

struct UnlitAttributes {
    int32_t NORMAL   = 1;
    int32_t POSITION = 0;
};

struct UnlitPrimitive {
    UnlitAttributes attributes;
    int32_t         indices  = 2;
    int32_t         material = 0;
};

struct UnlitMesh {
    std::string_view             name;
    std::vector<UnlitPrimitive> primitives;
};

// Only baseColorFactor: metallic/roughness stay omitted so the glTF defaults
// (metallic 1, roughness 1) apply, exactly as in the retired sample. The
// importer must surface unlit through the shading model, not by rewriting
// these fallback PBR fields.
struct UnlitPbr {
    std::array<float, 4> baseColorFactor {1.0f, 1.0f, 1.0f, 1.0f};
};

// KHR_materials_unlit carries no payload: the extension object is empty.
// SerializeJSON cannot emit an empty reflected struct, so this is an empty
// map, which serializes to {}. omitEmpty must stay off for this document or
// the key would be dropped instead.
struct UnlitExtensions {
    std::map<std::string_view, int> KHR_materials_unlit;
};

struct UnlitMaterial {
    std::string_view name;
    UnlitPbr         pbrMetallicRoughness;
    UnlitExtensions  extensions;
};

struct UnlitNode {
    std::string_view     name;
    int32_t              mesh = 0;
    std::array<float, 3> translation {0.0f, 0.0f, 0.0f};
};

// min/max ride on every accessor (the spec allows them on any of them) so one
// type covers positions, normals and indices.
struct UnlitAccessor {
    int32_t            bufferView    = 0;
    int32_t            componentType = 5126;
    int32_t            count         = 0;
    std::string_view   type          = "VEC3";
    std::vector<float> min;
    std::vector<float> max;
};

struct UnlitBufferView {
    int32_t buffer     = 0;
    int32_t byteOffset = 0;
    int32_t byteLength = 0;
};

struct UnlitBuffer {
    int32_t byteLength = 0;
};

struct UnlitDocument {
    UnlitAsset                      asset;
    std::vector<std::string_view>   extensionsUsed {"KHR_materials_unlit"};
    std::vector<std::string_view>   extensionsRequired {"KHR_materials_unlit"};
    int32_t                         scene = 0;
    std::vector<UnlitScene>         scenes;
    std::vector<UnlitNode>          nodes;
    std::vector<UnlitMesh>          meshes;
    std::vector<UnlitMaterial>      materials;
    std::vector<UnlitAccessor>      accessors;
    std::vector<UnlitBufferView>    bufferViews;
    std::vector<UnlitBuffer>        buffers;
};

// Assembles a GLB container around a serialized JSON chunk and a binary chunk.
//
// Synthesizing the input is not the same as reimplementing the importer: this
// only produces bytes a conformant loader must accept, so the extension
// behaviour under test stays the importer's own.
[[nodiscard]] inline auto MakeGlb(const std::string& json, std::span<const uint8_t> bin) -> std::vector<uint8_t> {
    std::string paddedJson = json;
    while (paddedJson.size() % 4 != 0) {
        paddedJson.push_back(' ');
    }
    std::vector<uint8_t> paddedBin(bin.begin(), bin.end());
    while (paddedBin.size() % 4 != 0) {
        paddedBin.push_back(0);
    }

    std::vector<uint8_t> glb;
    auto                 append32 = [&glb](uint32_t value) {
        for (uint32_t byte = 0; byte < 4; ++byte) {
            glb.push_back(static_cast<uint8_t>((value >> (8u * byte)) & 0xFFu));
        }
    };
    auto appendBytes = [&glb](const auto& source) {
        for (const auto element: source) {
            glb.push_back(static_cast<uint8_t>(element));
        }
    };

    const size_t binChunkSize = paddedBin.empty() ? 0u : 8u + paddedBin.size();
    append32(0x46546C67u); // "glTF"
    append32(2u);
    append32(static_cast<uint32_t>(12u + 8u + paddedJson.size() + binChunkSize));
    append32(static_cast<uint32_t>(paddedJson.size()));
    append32(0x4E4F534Au); // "JSON"
    appendBytes(paddedJson);
    if (!paddedBin.empty()) {
        append32(static_cast<uint32_t>(paddedBin.size()));
        append32(0x004E4942u); // "BIN\0"
        appendBytes(paddedBin);
    }
    return glb;
}

// Positions, normals, then indices: the retired sample's layout, so the
// accessor table below reads the same way.
[[nodiscard]] inline auto ChamferedBoxBin() -> std::vector<uint8_t> {
    const ChamferedBoxMesh mesh = BuildChamferedBox();
    std::vector<uint8_t>   bin(ChamferedBoxMesh::kTotalBytes);
    std::memcpy(bin.data(), mesh.positions.data(), ChamferedBoxMesh::kPositionBytes);
    std::memcpy(bin.data() + ChamferedBoxMesh::kPositionBytes, mesh.normals.data(), ChamferedBoxMesh::kNormalBytes);
    std::memcpy(
        bin.data() + ChamferedBoxMesh::kPositionBytes + ChamferedBoxMesh::kNormalBytes, mesh.indices.data(),
        ChamferedBoxMesh::kIndexBytes
    );
    return bin;
}

[[nodiscard]] inline auto MakeUnlitGlb() -> std::vector<uint8_t> {
    const UnlitDocument document {
        .scenes   = {UnlitScene {.nodes = {0, 1}}},
        .nodes    = {UnlitNode {.name = "Orange Object", .mesh = 0, .translation = {-1.2f, 0.0f, 0.0f}},
                     UnlitNode {.name = "Blue Object", .mesh = 1, .translation = {1.2f, 0.0f, 0.0f}}},
        .meshes   = {UnlitMesh {.name = "Orange Mesh", .primitives = {UnlitPrimitive {.material = 0}}},
                     UnlitMesh {.name = "Blue Mesh", .primitives = {UnlitPrimitive {.material = 1}}}},
        .materials = {UnlitMaterial {.name              = "Orange",
                                      .pbrMetallicRoughness = {.baseColorFactor = {1.0f, 0.21763764f, 0.0f, 1.0f}}},
                      UnlitMaterial {.name              = "Blue",
                                      .pbrMetallicRoughness = {.baseColorFactor = {0.0f, 0.21763764f, 1.0f, 1.0f}}}},
        .accessors = {UnlitAccessor {.bufferView    = 0,
                                      .componentType = 5126,
                                      .count         = static_cast<int32_t>(ChamferedBoxMesh::kVertexCount),
                                      .type          = "VEC3",
                                      .min           = {-1.0f, -1.0f, -1.0f},
                                      .max           = {1.0f, 1.0f, 1.0f}},
                      UnlitAccessor {.bufferView    = 1,
                                      .componentType = 5126,
                                      .count         = static_cast<int32_t>(ChamferedBoxMesh::kVertexCount),
                                      .type          = "VEC3",
                                      .min           = {-1.0f, -1.0f, -1.0f},
                                      .max           = {1.0f, 1.0f, 1.0f}},
                      UnlitAccessor {.bufferView    = 2,
                                      .componentType = 5123,
                                      .count         = static_cast<int32_t>(ChamferedBoxMesh::kIndexCount),
                                      .type          = "SCALAR",
                                      .min           = {0.0f},
                                      .max           = {static_cast<float>(ChamferedBoxMesh::kVertexCount - 1)}}},
        .bufferViews = {UnlitBufferView {.buffer     = 0,
                                          .byteOffset = 0,
                                          .byteLength = static_cast<int32_t>(ChamferedBoxMesh::kPositionBytes)},
                        UnlitBufferView {.buffer     = 0,
                                          .byteOffset = static_cast<int32_t>(ChamferedBoxMesh::kPositionBytes),
                                          .byteLength = static_cast<int32_t>(ChamferedBoxMesh::kNormalBytes)},
                        UnlitBufferView {.buffer     = 0,
                                          .byteOffset = static_cast<int32_t>(
                                              ChamferedBoxMesh::kPositionBytes + ChamferedBoxMesh::kNormalBytes),
                                          .byteLength = static_cast<int32_t>(ChamferedBoxMesh::kIndexBytes)}},
        .buffers     = {UnlitBuffer {.byteLength = static_cast<int32_t>(ChamferedBoxMesh::kTotalBytes)}},
    };
    return MakeGlb(ZHLN::ReflectJSON::SerializeJSON(document), ChamferedBoxBin());
}

} // namespace ZHLN::Test::GltfFixtures
