// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/BSP/BSPImporter.cpp
//
// The engine-side half: file bytes -> native streams (BSPGeometry) -> GPU
// buffers, materials, colliders -> ModelPrefab -> prefab cache. The assembly
// mirrors GLTFImporter.cpp's GetOrCreateCompiledPrimitive/BuildModelPrefab
// split: one compiled part per material group, a single identity root node,
// one node per light so ModelLight's node chain carries its transform.

#include "BSPImporter.hpp"
#include "BSPRead.hpp"
#include "SourceVFS.hpp"
#include "StudioModelImporter.hpp"
#include "VMTParser.hpp"
#include "VTFDecoder.hpp"
#include <array>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <stb_image.h>

namespace ZHLN::BSP {
namespace {

auto IsToolTexture(std::string_view name) -> bool {
    return name.starts_with("tools/");
}

// UnlitGeneric surfaces bypass lighting entirely (baseColor goes straight to
// the framebuffer): Source's white lamp panels (lights/white and friends)
// would land at luma 1.0 -- exactly the bloom threshold -- and turn every
// ceiling fixture into a blinding glare. Scale them to read as bright but
// sub-threshold. Missing textures would otherwise fall back to the raw
// baseColor (usually pure white) and blow out wherever direct light reaches.
inline constexpr float kUnlitDim          = 0.55f;
inline constexpr float kMissingTextureDim = 0.25f;

auto LoadTextureFromBytes(RenderContext& ctx, std::string_view name, std::span<const std::byte> bytes, bool isSRGB = true) -> TextureHandle {
    // 1. Try VTF first
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "VTF\0", 4) == 0) {
        auto vtfExp = DecodeVTF(bytes);
        if (vtfExp.has_value()) {
            auto texRes = ctx.CreateTexture(name, vtfExp->rgba8, Extent2D {vtfExp->width, vtfExp->height}, isSRGB);
            if (texRes.has_value()) {
                return *texRes;
            }
        }
    }

    // 2. Try stb_image (PNG, JPG, TGA, etc.)
    int            width    = 0;
    int            height   = 0;
    int            channels = 0;
    unsigned char* pixels   = stbi_load_from_memory(
        reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()), &width, &height, &channels, 4
    );
    if (pixels) {
        const std::span<const std::byte> rgba(reinterpret_cast<const std::byte*>(pixels), static_cast<size_t>(width) * height * 4);
        auto texRes = ctx.CreateTexture(name, rgba, Extent2D {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}, isSRGB);
        stbi_image_free(pixels);
        if (texRes.has_value()) {
            return *texRes;
        }
    }

    return TextureHandle::Invalid;
}

// One VRAD luxel: RGB mantissas sharing a signed power-of-two exponent.
// Linear value = ldexp(byte / 255, exponent). This is the on-disk unit of
// LUMP_LIGHTING_HDR (and LUMP_LIGHTING, which uses the same encoding).
struct ColorRGBExp32 {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    int8_t  exponent;
};
static_assert(sizeof(ColorRGBExp32) == 4);

[[nodiscard]] inline auto LinearToSRGB8(float linear) -> uint8_t {
    const float c = std::max(linear, 0.0f);
    const float s = (c <= 0.0031308f) ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<uint8_t>(std::clamp(s, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Engine texture ingestion is 8-bit only: RenderContext::CreateTexture always
// uploads RGBA8 and TextureUploader hardcodes four bytes per texel, so a float
// atlas is not reachable without RHI work. That forces a choice, because VRAD
// radiance above 1.0 (sunlit patches, lamp pools) clips to one flat white in
// an 8-bit target while an sRGB-encoded ramp below 1.0 is already well
// resolved. This scale is where that trade is set, and SampleBakedRadiosity
// divides it back out.
//
// Measured over a linear ramp, distinct stored codes / worst absolute error:
//
//   range          scale 1.0      scale 0.5      scale 0.25
//   0.01-0.08      56 / 0.0010    41 / 0.0014    32 / 0.0018
//   0.0-0.3       150 / 0.0022   109 / 0.0029    78 / 0.0039
//   1.0-4.0         1 / 3.0000    68 / 2.0000   119 / 0.0176
//
// So 1.0 destroys everything above 1.0 outright, while 0.25 costs the shadows
// 56 -> 32 codes -- a worst-case error of 0.0018 against values around 0.05,
// which stays invisible under an albedo multiply. 0.25 is the better trade
// for a baked-lighting map whose brights carry the readable information.
inline constexpr float kLightmapEncodeScale = 0.25f;

// Decode VRAD radiosity from BSPMap::lighting (LUMP_LIGHTING_HDR, falling back
// to LDR LUMP_LIGHTING) into the same shelf-packed atlas BSPGeometry's
// TEXCOORD_1 UVs reference -- no external *_lightmap0.png required. Each face
// owns a (W+1)x(H+1) block of ColorRGBExp32 samples at face.lightofs;
// bump-lit faces store 4 samples per luxel (flat + three directional) and the
// flat one is taken. Linear radiance is scaled down and encoded to sRGB8; the
// sampler's sRGB->linear decode round-trips the encode.
[[nodiscard]] auto BuildLightmapAtlasTexture(RenderContext& ctx, const BSPMap& map, const ImportedMapData& imported) -> TextureHandle {
    if (map.lighting.empty() || imported.lightmapOrigins.size() != map.faces.size()) {
        return TextureHandle::Invalid;
    }
    const auto atlasW = static_cast<int32_t>(imported.lightmapPageWidth);
    const auto atlasH = static_cast<int32_t>(imported.lightmapPageHeight);
    if (atlasW <= 0 || atlasH <= 0) {
        return TextureHandle::Invalid;
    }

    std::vector<uint8_t> pixels(static_cast<size_t>(atlasW) * static_cast<size_t>(atlasH) * 4, 0);
    const auto           putPixel = [&](int32_t x, int32_t y, float r, float g, float b) {
        if (x < 0 || y < 0 || x >= atlasW || y >= atlasH) {
            return;
        }
        uint8_t* px = &pixels[(static_cast<size_t>(y) * static_cast<size_t>(atlasW) + static_cast<size_t>(x)) * 4];
        px[0]       = LinearToSRGB8(r * kLightmapEncodeScale);
        px[1]       = LinearToSRGB8(g * kLightmapEncodeScale);
        px[2]       = LinearToSRGB8(b * kLightmapEncodeScale);
        px[3]       = 255;
    };

    size_t facesUnpacked = 0;
    for (size_t faceIndex = 0; faceIndex < map.faces.size(); ++faceIndex) {
        const DFace& face   = map.faces[faceIndex];
        const auto   origin = imported.lightmapOrigins[faceIndex];
        if (face.lightofs < 0 || face.lightmapTextureSizeInLuxels[0] <= 0 || face.lightmapTextureSizeInLuxels[1] <= 0 || origin[0] < 0) {
            continue;
        }
        const int32_t w = face.lightmapTextureSizeInLuxels[0] + 1;
        const int32_t h = face.lightmapTextureSizeInLuxels[1] + 1;
        const bool    bumpLit = face.texinfo >= 0 && static_cast<size_t>(face.texinfo) < map.texInfos.size() &&
                             (map.texInfos[static_cast<size_t>(face.texinfo)].flags & kSurfBumpLight) != 0;
        const size_t  sampleStride = bumpLit ? 4 : 1;
        const size_t  byteCount =
            static_cast<size_t>(w) * static_cast<size_t>(h) * sampleStride * sizeof(ColorRGBExp32);
        if (static_cast<size_t>(face.lightofs) + byteCount > map.lighting.size()) {
            continue; // truncated or corrupt lump: skip this face, not the map
        }
        const auto* luxels = reinterpret_cast<const ColorRGBExp32*>(map.lighting.data() + static_cast<size_t>(face.lightofs));
        // Decode into a scratch tile first: the gutter ring has to copy the
        // edge luxels, which are only all known once the interior is decoded.
        std::vector<std::array<float, 3>> tile(static_cast<size_t>(w) * static_cast<size_t>(h));
        for (int32_t y = 0; y < h; ++y) {
            for (int32_t x = 0; x < w; ++x) {
                const ColorRGBExp32& s = luxels[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * sampleStride];
                tile[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = {std::ldexp(s.r / 255.0f, s.exponent),
                                                                                                 std::ldexp(s.g / 255.0f, s.exponent),
                                                                                                 std::ldexp(s.b / 255.0f, s.exponent)};
            }
        }
        for (int32_t y = 0; y < h; ++y) {
            for (int32_t x = 0; x < w; ++x) {
                const auto c = tile[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)];
                putPixel(origin[0] + x, origin[1] + y, c[0], c[1], c[2]);
            }
        }
        // Duplicate the border luxels into the reserved gutter so a bilinear
        // tap just outside the cell reads this face's own edge colour.
        for (int32_t x = 0; x < w; ++x) {
            const auto top = tile[static_cast<size_t>(x)];
            const auto bot = tile[static_cast<size_t>(h - 1) * static_cast<size_t>(w) + static_cast<size_t>(x)];
            putPixel(origin[0] + x, origin[1] - 1, top[0], top[1], top[2]);
            putPixel(origin[0] + x, origin[1] + h, bot[0], bot[1], bot[2]);
        }
        for (int32_t y = 0; y < h; ++y) {
            const auto lft = tile[static_cast<size_t>(y) * static_cast<size_t>(w)];
            const auto rgt = tile[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(w - 1)];
            putPixel(origin[0] - 1, origin[1] + y, lft[0], lft[1], lft[2]);
            putPixel(origin[0] + w, origin[1] + y, rgt[0], rgt[1], rgt[2]);
        }
        {
            const auto cornerTL = tile[0];
            const auto cornerTR = tile[static_cast<size_t>(w - 1)];
            const auto cornerBL = tile[static_cast<size_t>(h - 1) * static_cast<size_t>(w)];
            const auto cornerBR = tile[static_cast<size_t>(h - 1) * static_cast<size_t>(w) + static_cast<size_t>(w - 1)];
            putPixel(origin[0] - 1, origin[1] - 1, cornerTL[0], cornerTL[1], cornerTL[2]);
            putPixel(origin[0] + w, origin[1] - 1, cornerTR[0], cornerTR[1], cornerTR[2]);
            putPixel(origin[0] - 1, origin[1] + h, cornerBL[0], cornerBL[1], cornerBL[2]);
            putPixel(origin[0] + w, origin[1] + h, cornerBR[0], cornerBR[1], cornerBR[2]);
        }
        ++facesUnpacked;
    }
    if (facesUnpacked == 0) {
        return TextureHandle::Invalid;
    }

    Log("BSP Importer: decoded {} lightmapped faces from {} lighting into a {}x{} atlas", facesUnpacked, map.lightingHDR ? "HDR" : "LDR",
        atlasW, atlasH);
    return ctx.CreateTexture("bsp_lightmap", std::span {pixels}, Extent2D {static_cast<uint32_t>(atlasW), static_cast<uint32_t>(atlasH)}, true)
        .value_or(TextureHandle::Invalid);
}

auto ResolveMaterialDesc(RenderContext& ctx, const SourceVFS& vfs, const MaterialStreams& part, TextureHandle lightmapTexture)
    -> MaterialDesc {
    if (IsToolTexture(part.materialName)) {
        return MaterialDesc::Unlit({0.4f, 0.4f, 0.4f, 1.0f});
    }

    // Neutral grey, not white: this desc is what ships when the VMT cannot be
    // found or parsed, and a pure-white fallback turns every missing material
    // into a blinding surface under direct light.
    MaterialDesc desc = MaterialDesc::Basic({0.5f, 0.5f, 0.5f, 1.0f}, 0.8f, 0.0f, false);
    if (lightmapTexture != TextureHandle::Invalid) {
        // A VRAD lightmap is incoming irradiance, so it modulates the surface
        // colour rather than being added to it. Routing it through
        // MaterialDesc::emissiveMap would add it after lighting (see
        // lighting.slang: `... + directSun + directPunctual + emissive`), which
        // washes every material towards the same grey no matter how dark or
        // saturated its albedo is. The dedicated lightmap slot is sampled in
        // material_model.slang and multiplied into the albedo instead.
        // The UV selector is not needed: SampleBakedRadiosity reads uv1
        // directly, which is where EmitVertex puts the atlas coordinates.
        desc.lightmapMap = lightmapTexture;
    }

    const auto vmtPath = vfs.ResolveMaterial(part.materialName);
    if (!vmtPath.has_value()) {
        return desc;
    }

    const auto vmtBytes = vfs.ReadFile(*vmtPath);
    if (!vmtBytes.has_value() || vmtBytes->empty()) {
        return desc;
    }

    const std::string_view vmtText(reinterpret_cast<const char*>(vmtBytes->data()), vmtBytes->size());
    const auto             vmtMatExp = ParseVMT(vmtText);
    if (!vmtMatExp.has_value()) {
        return desc;
    }

    const auto& vmt   = *vmtMatExp;
    desc.baseColor   = vmt.baseColor;
    // $nocull from the VMT, or the importer's single-sheet wall audit marking
    // this part: Source draws undercovered faces from their back side (the
    // vbsp leaffaces of the room include them), so honours match by rendering
    // the whole material group double-sided. Backface physics is handled
    // separately via reversed triangles in the collision stream.
    desc.doubleSided = vmt.noCull || part.forceDoubleSided;
    if (vmt.isTranslucent) {
        desc.alphaBlend = true;
        desc.alphaMode  = 2;
    } else if (vmt.isAlphaTest) {
        desc.alphaMode   = 1;
        desc.alphaCutoff = vmt.alphaCutoff;
    }

    if (vmt.shader == "UnlitGeneric") {
        desc.unlit = true;
        desc.baseColor.x *= kUnlitDim;
        desc.baseColor.y *= kUnlitDim;
        desc.baseColor.z *= kUnlitDim;
    }

    // Resolve base texture (albedo)
    if (!vmt.baseTexture.empty()) {
        const auto texPath = vfs.ResolveTexture(vmt.baseTexture);
        if (texPath.has_value()) {
            const auto texBytes = vfs.ReadFile(*texPath);
            if (texBytes.has_value()) {
                desc.albedoMap = LoadTextureFromBytes(ctx, vmt.baseTexture, *texBytes, true);
            }
        }
        if (desc.albedoMap == TextureHandle::Invalid) {
            // The VMT references art that is not on disk: darken the flat
            // baseColor so the surface reads as missing (dark grey) instead
            // of rendering as a pure-white wall.
            desc.baseColor.x *= kMissingTextureDim;
            desc.baseColor.y *= kMissingTextureDim;
            desc.baseColor.z *= kMissingTextureDim;
        }
    }

    // Resolve normal/bump map
    if (!vmt.bumpMap.empty()) {
        const auto bumpPath = vfs.ResolveTexture(vmt.bumpMap);
        if (bumpPath.has_value()) {
            const auto bumpBytes = vfs.ReadFile(*bumpPath);
            if (bumpBytes.has_value()) {
                desc.normalMap = LoadTextureFromBytes(ctx, vmt.bumpMap, *bumpBytes, false);
            }
        }
    }

    return desc;
}

auto ReadWholeFile(std::string_view path) -> std::optional<std::vector<std::byte>> {
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return std::nullopt;
    }
    file.seekg(0);
    std::vector<std::byte> out(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
        return std::nullopt;
    }
    return out;
}

// Fills prefab->parts from imported material streams: one compiled part per
// material group (GPU buffers, material, meshlets, BLAS, colliders), then
// folds the invisible-but-solid tool faces into the first part collider.
// Shared by the world prefab and the interactive submodel prefabs.
void AssemblePrefabParts(
    RenderContext&         ctx,
    const SourceVFS&       vfs,
    const ImportedMapData& data,
    std::string_view       prefabPath,
    const ImportOptions&   options,
    TextureHandle          lightmapTex,
    ModelPrefab&           prefab
) {
    for (const MaterialStreams& part: data.parts) {
        if (part.VertexCount() == 0 || part.IndexCount() == 0) {
            continue;
        }

        const auto meshletResult = BuildMeshlets(std::span {part.indices}, std::span {part.positions});

        const BufferHandle posVbo     = ctx.CreateBuffer<BufferUsage::Vertex>(std::span {part.positions});
        const BufferHandle frameVbo   = ctx.CreateBuffer<BufferUsage::Vertex>(std::span {part.tangentFrames});
        const BufferHandle surfaceVbo = ctx.CreateBuffer<BufferUsage::Vertex>(std::span {part.surfaces});
        const BufferHandle ibo        = ctx.CreateBuffer<BufferUsage::Index>(std::span {part.indices});

        const bool hasMeshlets    = !meshletResult.Empty();
        const auto packedMeshlets = PackMeshlets(meshletResult.meshlets);

        BufferHandle meshletVbo       = hasMeshlets ? ctx.CreateBuffer(BufferDesc {
                                                          .usage  = BufferUsage::Storage,
                                                          .data   = std::as_bytes(std::span {packedMeshlets}),
                                                          .stride = kMeshletPackedBytes,
                                                }) :
                                                      BufferHandle::Invalid;
        BufferHandle meshletVertexVbo = hasMeshlets ? ctx.CreateBuffer<BufferUsage::Storage>(std::span {meshletResult.vertices}) : BufferHandle::Invalid;
        BufferHandle meshletTriVbo    = hasMeshlets ? ctx.CreateBuffer<BufferUsage::Storage>(std::span {meshletResult.triangles}) : BufferHandle::Invalid;
        const bool   completeMeshlets = hasMeshlets && meshletVbo != BufferHandle::Invalid && meshletVertexVbo != BufferHandle::Invalid &&
                                      meshletTriVbo != BufferHandle::Invalid;

        Mesh subMesh = {
            .posBuffer           = posVbo,
            .tangentFrameBuffer  = frameVbo,
            .surfaceBuffer       = surfaceVbo,
            .skinBuffer          = BufferHandle::Invalid,
            .indexBuffer         = ibo,
            .vertexCount         = part.VertexCount(),
            .indexCount          = part.IndexCount(),
            .meshletBuffer       = meshletVbo,
            .meshletVertexBuffer = meshletVertexVbo,
            .meshletTriBuffer    = meshletTriVbo,
            .meshletCount        = completeMeshlets ? static_cast<uint32_t>(meshletResult.meshlets.size()) : 0u,
        };

        if (auto res = ctx.BuildMeshBLAS(subMesh); !res) [[unlikely]] {
            if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
                LogWarning("BSP Importer: failed to build mesh BLAS: {}", res.error());
            }
        }

        const Material subMaterial = ctx.CreateMaterial(ResolveMaterialDesc(ctx, vfs, part, lightmapTex)).value_or(Material {});

        ModelPart modelPart;
        modelPart.name            = String64(part.materialName.c_str());
        modelPart.mesh            = subMesh;
        modelPart.defaultMaterial = subMaterial;
        modelPart.nodeIndex       = 0;
        modelPart.localTransform  = JPH::Mat44::sIdentity();
        modelPart.localMin        = part.boundsMin;
        modelPart.localMax        = part.boundsMax;

        const std::string assetKey = std::string(prefabPath) + "#part_" + std::to_string(prefab.parts.size());
        modelPart.meshAsset        = HashAssetID(assetKey);
        modelPart.materialAsset    = HashAssetID(assetKey + "_mat");

        const float extentX      = std::max(std::abs(part.boundsMin.x), std::abs(part.boundsMax.x));
        const float extentY      = std::max(std::abs(part.boundsMin.y), std::abs(part.boundsMax.y));
        const float extentZ      = std::max(std::abs(part.boundsMin.z), std::abs(part.boundsMax.z));
        modelPart.boundingRadius = std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ);

        if (options.buildColliders) {
            modelPart.meshCollider = Physics::CreateMeshShape(part.positions.data(), part.VertexCount(), part.indices.data(), part.IndexCount());

            const JPH::Vec3 localCenter(
                (part.boundsMin.x + part.boundsMax.x) * 0.5f, (part.boundsMin.y + part.boundsMax.y) * 0.5f, (part.boundsMin.z + part.boundsMax.z) * 0.5f
            );
            const JPH::Vec3 halfExtents(
                std::max((part.boundsMax.x - part.boundsMin.x) * 0.5f, 0.01f), std::max((part.boundsMax.y - part.boundsMin.y) * 0.5f, 0.01f),
                std::max((part.boundsMax.z - part.boundsMin.z) * 0.5f, 0.01f)
            );
            const JPH::ShapeRefC baseBox = new JPH::BoxShape(halfExtents);
            modelPart.boxCollider        = new JPH::RotatedTranslatedShape(localCenter, JPH::Quat::sIdentity(), baseBox);

            if (modelPart.meshCollider == nullptr) {
                modelPart.meshCollider = modelPart.boxCollider;
            }
        }

        prefab.parts.push_back(std::move(modelPart));
    }

    // Invisible-but-solid tool faces (nodraw undersides, sky brushes, clip and
    // invisible walls): RenderContext::Draw would only log-and-skip an empty
    // render mesh, so instead of a dummy "physics-only" part, fold the stream
    // into the collider of the first part that already has one. A static
    // compound of {part mesh, collision-only mesh} keeps both hulls intact
    // for mesh queries and CharacterVirtual alike.
    if (options.buildColliders && data.collisionOnly.IndexCount() > 0) {
        if (JPH::ShapeRefC collisionShape = Physics::CreateMeshShape(
                data.collisionOnly.positions.data(), data.collisionOnly.VertexCount(), data.collisionOnly.indices.data(),
                data.collisionOnly.IndexCount()
            )) {
            bool folded = false;
            for (ModelPart& part: prefab.parts) {
                if (part.meshCollider == nullptr) {
                    continue;
                }
                JPH::StaticCompoundShapeSettings compoundSettings;
                compoundSettings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), part.meshCollider);
                compoundSettings.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), collisionShape);
                const auto compound = compoundSettings.Create();
                if (!compound.IsValid()) {
                    continue;
                }
                part.meshCollider = compound.Get();
                folded            = true;
                break;
            }
            if (!folded) {
                LogWarning("BSP Importer: {} collision-only tool faces could not be folded into any part collider; invisible walls are lost",
                           data.collisionOnly.indices.size() / 3);
            }
        }
    }

}

// Translates every stream (render parts and the collision-only fold) by
// offset. Used to re-base rotating doors on their hinge pivot so the entity
// transform -- placed at the pivot -- rotates the geometry about it.
void ShiftImportedGeometry(ImportedMapData& data, JPH::Vec3Arg offset) {
    const auto shiftStreams = [offset](MaterialStreams& streams) {
        for (VertexPosition& vertex: streams.positions) {
            vertex.position[0] += offset.GetX();
            vertex.position[1] += offset.GetY();
            vertex.position[2] += offset.GetZ();
        }
        streams.boundsMin.x += offset.GetX();
        streams.boundsMin.y += offset.GetY();
        streams.boundsMin.z += offset.GetZ();
        streams.boundsMax.x += offset.GetX();
        streams.boundsMax.y += offset.GetY();
        streams.boundsMax.z += offset.GetZ();
    };
    for (MaterialStreams& part: data.parts) {
        shiftStreams(part);
    }
    shiftStreams(data.collisionOnly);
    data.boundsMin.x += offset.GetX();
    data.boundsMin.y += offset.GetY();
    data.boundsMin.z += offset.GetZ();
    data.boundsMax.x += offset.GetX();
    data.boundsMax.y += offset.GetY();
    data.boundsMax.z += offset.GetZ();
}

auto BuildModelPrefab(RenderContext& ctx, AssetManager& assetMgr, const BSPMap& map, std::string_view virtualPath, const ImportOptions& options)
    -> ZHLN::Optional<ModelPrefab&> {
    // Interactive brush submodels (doors, buttons) get their own prefabs
    // below; excluding them here keeps their faces out of the static world
    // mesh and world collider (a door baked into the world keeps blocking
    // the doorway after it opens).
    const std::vector<InteractiveSubmodel> interactiveSubmodels = CollectInteractiveSubmodels(map, options);

    ImportOptions worldOptions = options;
    // Lights come from the scene description (DescribeScene -> SceneLights),
    // not from the prefab: shipping them as ModelLights too made
    // InstantiatePrefab's KHR_lights_punctual pass spawn every light a second
    // time -- and BSP-built ModelLights carry no range, which the
    // instantiator expands to 1000 m. Double intensity-200 lights with
    // kilometre range washed the whole map out.
    worldOptions.gatherLights = false;
    worldOptions.skipModels.reserve(interactiveSubmodels.size());
    for (const InteractiveSubmodel& sub: interactiveSubmodels) {
        worldOptions.skipModels.push_back(sub.modelIndex);
    }

    const ImportedMapData imported = ImportMapGeometry(map, worldOptions);
    if (imported.parts.empty()) {
        LogWarning("BSP Importer: no importable surfaces in {}", virtualPath);
        return std::nullopt;
    }

    // Face-pipeline accounting: when a wall seems to be missing in-game, this
    // line proves whether it was dropped here (and by which stage) or is
    // present and the problem is downstream of this import.
    Log(
        "BSP Importer: {} -> {} total faces -> {} render faces in {} parts ({} triangles); dropped: {} no-winding, {} degenerate, {} no-texinfo, {} tool-helper; {} collision-only; {} disp fallbacks; {} single-sheet walls forced double-sided; lightmap page {}x{} luxels",
        virtualPath, imported.faceCount + imported.stats.helperFacesDropped + imported.stats.collisionOnlyFaces + imported.stats.droppedNoWinding +
                         imported.stats.droppedDegenerate + imported.stats.droppedNoProjection,
        imported.faceCount, imported.parts.size(), imported.triangleCount, imported.stats.droppedNoWinding, imported.stats.droppedDegenerate,
        imported.stats.droppedNoProjection, imported.stats.helperFacesDropped, imported.stats.collisionOnlyFaces, imported.stats.displacementFallbacks,
        imported.stats.singleSidedSheets, static_cast<int>(imported.lightmapPageWidth), static_cast<int>(imported.lightmapPageHeight)
    );

    auto prefab                 = std::make_unique<ModelPrefab>();
    prefab->virtualPath         = String256(virtualPath);
    prefab->emissiveFactorScale = 1.0f; // Source emission is baked lighting, not glTF factors.

    SourceVFS vfs;
    if (!options.assetRoot.empty()) {
        vfs.AddSearchPath(options.assetRoot);
    }
    const std::filesystem::path bspPath(virtualPath);
    if (bspPath.has_parent_path()) {
        vfs.AddSearchPath(bspPath.parent_path().string());
        if (bspPath.parent_path().has_parent_path()) {
            vfs.AddSearchPath(bspPath.parent_path().parent_path().string());
        }
    }

    // VRAD radiosity decoded from the BSP's own lighting lumps wins: same
    // shelf-packed layout the vertex UVs were emitted against, with no
    // external *_lightmap0.png dependency. The on-disk atlas PNG remains as a
    // fallback for maps with a stripped/absent lighting lump.
    TextureHandle lightmapTex = BuildLightmapAtlasTexture(ctx, map, imported);
    if (lightmapTex == TextureHandle::Invalid) {
        std::string lmPath = options.lightmapAtlasPath;
        if (lmPath.empty()) {
            const auto resolvedLm = vfs.ResolveLightmap(virtualPath);
            if (resolvedLm.has_value()) {
                lmPath = *resolvedLm;
            }
        }
        if (!lmPath.empty()) {
            const auto lmBytes = vfs.ReadFile(lmPath);
            if (lmBytes.has_value()) {
                lightmapTex = LoadTextureFromBytes(ctx, "bsp_lightmap", *lmBytes, false);
            }
        }
    }

    // One identity root; parts hang off it directly (part.localTransform does
    // the placement), lights get their own translated nodes because ModelLight
    // resolves position through its node chain.
    prefab->nodes.push_back(ModelNode {.name = String64("bsp_root"), .parentIndex = -1, .localTransform = JPH::Mat44::sIdentity(), .hasMesh = false});

    AssemblePrefabParts(ctx, vfs, imported, virtualPath, options, lightmapTex, *prefab);

    for (const BspPointLight& light: imported.lights) {
        ModelNode node;
        node.name           = String64(light.name.c_str());
        node.parentIndex    = 0;
        node.localTransform = JPH::Mat44::sTranslation(light.position);
        node.hasMesh        = false;
        prefab->nodes.push_back(node);

        ModelLight modelLight;
        modelLight.name      = String64(light.name.c_str());
        modelLight.nodeIndex = static_cast<int32_t>(prefab->nodes.size()) - 1;
        modelLight.color     = light.color;
        modelLight.intensity = light.intensity;
        modelLight.type      = light.type;
        if (light.type == LightType::Spot) {
            modelLight.innerConeAngle = light.innerConeRadians;
            modelLight.outerConeAngle = light.outerConeRadians;
        }
        prefab->lights.push_back(modelLight);
    }

    assetMgr.CachePrefab(HashAssetPath(virtualPath), std::move(prefab));

    // One prefab per interactive submodel, cached under "<map>#model_<N>" --
    // the source BSPScene stamps on the door/button entity description.
    // Sliding doors keep their world-space vertices (the entity transform is
    // just the movement delta); rotating doors are re-based on their hinge.
    for (const InteractiveSubmodel& sub: interactiveSubmodels) {
        ImportOptions subOptions = options;
        subOptions.onlyModel    = sub.modelIndex;
        subOptions.gatherLights = false;

        ImportedMapData subImported = ImportMapGeometry(map, subOptions);
        if (subImported.parts.empty()) {
            continue; // Pure tool faces (clip/hint brushes): nothing to draw.
        }
        if (sub.rotating) {
            ShiftImportedGeometry(subImported, -sub.pivot);
        }

        const std::string subPath = std::string(virtualPath) + "#model_" + std::to_string(sub.modelIndex);
        auto              subPrefab                 = std::make_unique<ModelPrefab>();
        subPrefab->virtualPath         = String256(subPath);
        subPrefab->emissiveFactorScale = 1.0f;
        subPrefab->nodes.push_back(
            ModelNode {.name = String64("bsp_submodel_root"), .parentIndex = -1, .localTransform = JPH::Mat44::sIdentity(), .hasMesh = false}
        );
        AssemblePrefabParts(ctx, vfs, subImported, subPath, options, lightmapTex, *subPrefab);
        assetMgr.CachePrefab(HashAssetPath(subPath), std::move(subPrefab));
        Log("BSP Importer: {} -> interactive submodel prefab ({}, {} parts)", subPath, sub.rotating ? "rotating" : "sliding", subImported.parts.size());
    }

    return assetMgr.GetCachedPrefab(HashAssetPath(virtualPath));
}

} // namespace

auto LoadBSPPrefab(RenderContext& ctx, AssetManager& assetMgr, std::string_view path, const ImportOptions& options) -> ZHLN::Optional<ModelPrefab&> {
    const uint64_t hash   = HashAssetPath(path);
    const auto     cached = assetMgr.GetCachedPrefab(hash);
    if (cached) {
        return cached;
    }

    const auto bytes = ReadWholeFile(path);
    if (!bytes) {
        LogWarning("BSP Importer: failed to read {}", path);
        return std::nullopt;
    }
    return LoadBSPPrefabFromMemory(ctx, assetMgr, *bytes, path, options);
}

auto LoadBSPPrefabFromMemory(
    RenderContext&             ctx,
    AssetManager&              assetMgr,
    std::span<const std::byte> bytes,
    std::string_view           virtualPath,
    const ImportOptions&       options
) -> ZHLN::Optional<ModelPrefab&> {
    const uint64_t hash   = HashAssetPath(virtualPath);
    const auto     cached = assetMgr.GetCachedPrefab(hash);
    if (cached) {
        return cached;
    }

    const auto parsed = ParseBsp(bytes);
    if (!parsed) {
        LogWarning("BSP Importer: {}: {}", virtualPath, parsed.error());
        return std::nullopt;
    }
    return BuildModelPrefab(ctx, assetMgr, *parsed, virtualPath, options);
}

auto PreloadStaticProps(
    RenderContext&       ctx,
    AssetManager&        assetMgr,
    const BSPMap&        map,
    const ImportOptions& options
) -> size_t {
    SourceVFS vfs;
    if (!options.assetRoot.empty()) {
        vfs.AddSearchPath(options.assetRoot);
    }

    size_t loaded = 0;
    for (const auto& sp: map.staticProps) {
        if (!sp.modelName.empty()) {
            if (const auto model = LoadStudioModelPrefab(ctx, assetMgr, vfs, sp.modelName, options); model.has_value()) {
                ++loaded;
            }
        }
    }

    for (const auto& ent: map.entities) {
        const std::string_view model = ent.Find("model");
        if (model.ends_with(".mdl")) {
            if (const auto m = LoadStudioModelPrefab(ctx, assetMgr, vfs, model, options); m.has_value()) {
                ++loaded;
            }
        }
    }

    return loaded;
}

} // namespace ZHLN::BSP
