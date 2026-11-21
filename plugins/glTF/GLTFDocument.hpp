// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GLTFOptions.hpp"
#include <Zahlen/COM/AssetTypes.hpp>
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/Core/ErrorCode.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Render/Meshlet.hpp>
#include <Zahlen/Render/SkeletalAnimation.hpp>
#include <Zahlen/Render/Types.hpp>
#include <Zahlen/Render/Vertex.hpp>
#include <expected>
#include <span>
#include <string>
#include <vector>
namespace ZHLN::GLTF {
enum class ImportError : uint8_t { ParseFailed = 1, BufferReadFailed, InvalidDocument };
struct TextureDocument {
    Extent2D             extent {};
    bool                 isSRGB = true;
    std::vector<uint8_t> rgba;
};
// Host-only typed binding; no synthetic GPU handle is used for a CPU image ID.
// These member pointers never cross a capability/vtable boundary.
struct TextureBinding {
    TextureID MaterialDesc::* field      = nullptr;
    uint32_t                  imageIndex = 0;
};
struct PrimitiveDocument {
    std::vector<VertexPosition>     positions;
    std::vector<VertexTangentFrame> tangentFrames;
    std::vector<VertexSurface>      surfaces;
    std::vector<VertexSkin>         skins;
    std::vector<uint32_t>           indices;
    MeshletBuildResult              meshlets;
    MaterialDesc                    material {};
    std::vector<TextureBinding>     textures;
    std::vector<float>              morphDeltas;
    uint32_t                        activeMorphCount = 0;
    float                           boundingRadius   = 1;
    FVec3                           localMin {}, localMax {};
    bool                            wantsMeshCollider = false;
};
struct PartDocument {
    String64                 name;
    uint32_t                 primitive = 0;
    int32_t                  nodeIndex = -1, skeletonIndex = -1;
    bool                     isSkinned = false;
    FVec4                    defaultMorphWeights {};
    std::vector<CSGModifier> csgModifiers;
};
struct Document {
    std::string                       virtualPath;
    ImportOptions                     options {};
    std::vector<COM::PrefabNodeInfo>  nodes;
    std::vector<Skeleton>             skeletons;
    std::vector<AnimationClip>        animations;
    std::vector<COM::PrefabLightInfo> lights;
    std::vector<TextureDocument>      textures;
    std::vector<PrimitiveDocument>    primitives; // shared source primitives uploaded once
    std::vector<PartDocument>         parts;      // one instance binding per source node
};
// All parse products own their CPU data; no cgltf pointer or input-byte borrow
// survives this call. Optional assets serves external .bin/image paths through
// VFS before disk. bytesPath is the original .gltf location for relative URIs.
[[nodiscard]] auto Decode(
    std::span<const uint8_t> bytes,
    std::string_view         virtualPath,
    std::string_view         bytesPath = {},
    ImportOptions            options   = {},
    COM::IAssetLibrary*      assets    = nullptr
) -> std::expected<Document, ErrorCode>;
[[nodiscard]] auto Read(std::string_view path, ImportOptions options = {}, COM::IAssetLibrary* assets = nullptr) -> std::expected<Document, ErrorCode>;
} // namespace ZHLN::GLTF
