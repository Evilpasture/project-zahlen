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
#include "BSPLighting.hpp"
#include "BSPRead.hpp"
#include "SourceVFS.hpp"
#include "StudioModelImporter.hpp"
#include "VMTParser.hpp"
#include "VTFDecoder.hpp"
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Zahlen/AssetManager.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Meshlet.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/physics/Physics.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <span>
#include <stb_image.h>
#include <string>
#include <vector>

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
inline constexpr float kUnlitDim                   = 0.55f;
inline constexpr float kMissingTextureDim          = 0.25f;
// Source's self-illumination is authored for an LDR framebuffer. Preserve its
// mask and colour, but lift it into useful HDR radiance so it survives PBR
// exposure and contributes to bloom instead of stopping at the bloom threshold.
inline constexpr float kSourceEmissionHdrScale     = 4.0f;

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
    unsigned char* pixels =
        stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (pixels) {
        const std::span<const std::byte> rgba(reinterpret_cast<const std::byte*>(pixels), static_cast<size_t>(width) * height * 4);
        auto                             texRes = ctx.CreateTexture(name, rgba, Extent2D {static_cast<uint32_t>(width), static_cast<uint32_t>(height)}, isSRGB);
        stbi_image_free(pixels);
        if (texRes.has_value()) {
            return *texRes;
        }
    }

    return TextureHandle::Invalid;
}

// A material's .vmt, resolved and parsed once per import. Source's `patch`
// shader is an include-plus-replace indirection that roughly a sixth of the
// materials in a shipped map use (every cubemap-patched surface), so the
// resolver follows it instead of treating the patch as the material.
auto LoadMaterialVmt(const SourceVFS& vfs, std::string_view materialName) -> std::optional<VMTMaterial> {
    for (int depth = 0; depth < 4; ++depth) {
        const auto vmtPath = vfs.ResolveMaterial(materialName);
        if (!vmtPath.has_value()) {
            return std::nullopt;
        }
        const auto vmtBytes = vfs.ReadFile(*vmtPath);
        if (!vmtBytes.has_value() || vmtBytes->empty()) {
            return std::nullopt;
        }
        const std::string_view vmtText(reinterpret_cast<const char*>(vmtBytes->data()), vmtBytes->size());
        const auto             vmtMatExp = ParseVMT(vmtText);
        if (!vmtMatExp.has_value()) {
            return std::nullopt;
        }
        if (vmtMatExp->shader != "patch") {
            return std::optional<VMTMaterial>(*vmtMatExp);
        }
        // `include` points at the real material; the patch's own `replace`
        // block overlays it, so merge the patch's parameters on top of the
        // parent's rather than dropping either.
        const auto include = vmtMatExp->Find("include");
        if (!include || include->empty()) {
            return std::optional<VMTMaterial>(*vmtMatExp);
        }
        std::string parent(*include);
        if (!parent.ends_with(".vmt")) {
            parent += ".vmt";
        }
        const auto parentBytes = vfs.ReadFile(parent);
        if (!parentBytes.has_value() || parentBytes->empty()) {
            return std::nullopt;
        }
        const std::string_view parentText(reinterpret_cast<const char*>(parentBytes->data()), parentBytes->size());
        const auto             parentExp = ParseVMT(parentText);
        if (!parentExp.has_value()) {
            return std::nullopt;
        }
        VMTMaterial merged = *parentExp;
        for (const auto& [key, value]: vmtMatExp->parameters) {
            if (key == "include" || key == "replace") {
                continue;
            }
            merged.parameters[key] = value;
        }
        return merged;
    }
    return std::nullopt;
}

// Measured Rec.709 luma of a material's base texture, or -1 when it cannot be
// resolved or decoded. This is the most direct evidence a surface actually
// emits: a lamp panel's VTF is a white sheet, a poster's is not.
auto TextureLuma(const SourceVFS& vfs, const VMTMaterial& vmt) -> float {
    if (vmt.baseTexture.empty()) {
        return -1.0f;
    }
    const auto texPath = vfs.ResolveTexture(vmt.baseTexture);
    if (!texPath.has_value()) {
        return -1.0f;
    }
    const auto texBytes = vfs.ReadFile(*texPath);
    if (!texBytes.has_value() || texBytes->empty()) {
        return -1.0f;
    }
    const auto decoded = DecodeVTF(*texBytes);
    if (!decoded.has_value() || decoded->rgba8.empty()) {
        return -1.0f;
    }
    const size_t pixelCount = decoded->rgba8.size() / 4;
    if (pixelCount == 0) {
        return -1.0f;
    }
    // Stride over the texture: a 2048x512 VTF is a megapixel, and the mean over
    // every 97th pixel is stable to three decimals while touching ~1% of it.
    double   sum   = 0.0;
    uint64_t count = 0;
    for (uint64_t i = 0; i < pixelCount; i += 97) {
        const size_t offset = static_cast<size_t>(i) * 4;
        const auto   r      = static_cast<double>(static_cast<uint8_t>(decoded->rgba8[offset + 0]));
        const auto   g      = static_cast<double>(static_cast<uint8_t>(decoded->rgba8[offset + 1]));
        const auto   b      = static_cast<double>(static_cast<uint8_t>(decoded->rgba8[offset + 2]));
        const auto   a      = static_cast<double>(static_cast<uint8_t>(decoded->rgba8[offset + 3])) / 255.0;
        sum += (r * 0.2126 + g * 0.7152 + b * 0.0722) / 255.0 * a;
        ++count;
    }
    return (count > 0) ? static_cast<float>(sum / static_cast<double>(count)) : -1.0f;
}

// The engine's own luminance, so the emissive ranking here and the renderer's
// light-importance sort agree on what "bright" means.
// Below this, a surface is not emitting anything the renderer can see.
constexpr float kEmissiveStrengthEpsilon = 0.02f;

// Leading scalar of a KeyValues value, or nullopt when it is not a number.
// Source writes `$selfillum 1`, `$selfillum 0.35`, `$selfillum "255 255 255
// 255"` and the texture-path form; the first token is the one that matters.
[[nodiscard]] auto ParseFloatScalar(std::string_view text) noexcept -> std::optional<float> {
    const auto* begin = text.data();
    const auto* end   = text.data() + text.size();
    while (begin < end && (*begin == ' ' || *begin == '\t' || *begin == '[' || *begin == '"')) {
        ++begin;
    }
    float value          = 0.0f;
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc {}) {
        return std::nullopt;
    }
    return value;
}

// 0..1 for a material that declares self-illumination, or -1 when it does not.
// Reads every spelling Source uses: the plain `$selfillum <0|1|scalar>` mask
// switch, the mask texture form, and the `$selfillummaskscale` multiplier that
// a lot of ported content pairs with it.
[[nodiscard]] auto SelfIllumStrength(const VMTMaterial& vmt) noexcept -> float {
    static constexpr std::string_view kKeys[] = {"$selfillum", "$selfillummask", "$selfillummaskscale", "$emissiveblendstrength"};
    float                             found   = -1.0f;
    for (const std::string_view key: kKeys) {
        const auto value = vmt.Find(key);
        if (!value || value->empty()) {
            continue;
        }
        // A texture path is a mask, not a magnitude: the surface emits, at full
        // mask strength, wherever the mask is not black.
        if (value->find('/') != std::string_view::npos || value->ends_with(".vtf")) {
            found = std::max(found, 0.0f);
            continue;
        }
        if (const auto scalar = ParseFloatScalar(*value)) {
            found = std::max(found, *scalar);
        } else {
            found = std::max(found, 1.0f);
        }
    }
    return found;
}

// Source shader names are not case-sensitive. Keep these predicates local:
// the shared strength table and these rendering decisions must use the same
// case-insensitive semantics.
[[nodiscard]] auto ShaderNameEquals(std::string_view shader, std::string_view expected) noexcept -> bool {
    if (shader.size() != expected.size()) {
        return false;
    }
    return std::equal(shader.begin(), shader.end(), expected.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

[[nodiscard]] auto ShaderNameIn(std::string_view shader, std::initializer_list<std::string_view> names) noexcept -> bool {
    return std::ranges::any_of(names, [shader](std::string_view name) { return ShaderNameEquals(shader, name); });
}

auto ResolveMaterialDesc(RenderContext& ctx, const SourceVFS& vfs, const MaterialStreams& part, const VMTMaterial* vmt) -> MaterialDesc {
    if (IsToolTexture(part.materialName)) {
        return MaterialDesc::Unlit({0.4f, 0.4f, 0.4f, 1.0f});
    }

    // Neutral grey, not white: this desc is what ships when the VMT cannot be
    // found or parsed, and a pure-white fallback turns every missing material
    // into a blinding surface under direct light.
    MaterialDesc desc = MaterialDesc::Basic({0.5f, 0.5f, 0.5f, 1.0f}, 0.8f, 0.0f, false);

    if (vmt == nullptr) {
        return desc;
    }

    const VMTMaterial& material = *vmt;
    desc.baseColor              = material.baseColor;
    // $nocull from the VMT, or the importer's single-sheet wall audit marking
    // this part: Source draws undercovered faces from their back side (the
    // vbsp leaffaces of the room include them), so honours match by rendering
    // the whole material group double-sided. Backface physics is handled
    // separately via reversed triangles in the collision stream.
    desc.doubleSided = material.noCull || part.forceDoubleSided;
    if (material.isTranslucent) {
        desc.alphaBlend = true;
        desc.alphaMode  = 2;
    } else if (material.isAlphaTest) {
        desc.alphaMode   = 1;
        desc.alphaCutoff = material.alphaCutoff;
    }

    // Resolve base texture (albedo)
    if (!material.baseTexture.empty()) {
        const auto texPath = vfs.ResolveTexture(material.baseTexture);
        if (texPath.has_value()) {
            const auto texBytes = vfs.ReadFile(*texPath);
            if (texBytes.has_value()) {
                desc.albedoMap = LoadTextureFromBytes(ctx, material.baseTexture, *texBytes, true);
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
    if (!material.bumpMap.empty()) {
        const auto bumpPath = vfs.ResolveTexture(material.bumpMap);
        if (bumpPath.has_value()) {
            const auto bumpBytes = vfs.ReadFile(*bumpPath);
            if (bumpBytes.has_value()) {
                desc.normalMap = LoadTextureFromBytes(ctx, material.bumpMap, *bumpBytes, false);
            }
        }
    }

    // Source self-illumination is normally masked. Keep that spatial detail:
    // an explicit mask wins, otherwise the base texture is the mask Source's
    // $selfillum path uses. Sampling only a constant emissive factor made whole
    // materials glow uniformly and left the authored lamp pixels dark.
    if (const auto mask = material.Find("$selfillummask"); mask && !mask->empty()) {
        if (const auto maskPath = vfs.ResolveTexture(*mask); maskPath) {
            if (const auto maskBytes = vfs.ReadFile(*maskPath); maskBytes) {
                desc.emissiveMap = LoadTextureFromBytes(ctx, *mask, *maskBytes, true);
            }
        }
    } else if (material.Find("$selfillum").has_value()) {
        desc.emissiveMap = desc.albedoMap;
    }

    // Shader class + emission, through the rules every material pass shares.
    ApplyVmtLighting(vfs, material, desc);

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

// The second stage of the emissive heuristic: the geometry pass only applied
// the material-name gate (it has no VFS), so this is where the evidence that
// actually decides the strength is read -- the .vmt's shader class and
// self-illumination keys, the measured luminance of the base .vtf, and the
// texdata reflectivity vbsp baked. Every candidate is re-probed through
// BSPLighting::ProbeEmissiveMaterial, and the ones that clear no threshold are
// dropped: a `lights/white` sheet with reflectivity (1,1,1) and no
// self-illumination is a lit surface, not a lamp, and spawning a light at its
// centroid would double the map's illumination.
//
// Runs before the prefab is built so the parts' `unlit` flag and the spawned
// lights agree on which surfaces are emitters.
auto RefineEmissiveSurfaces(const SourceVFS& vfs, std::span<EmissiveSurface> surfaces, ImportedMapData& data)
    -> std::vector<EmissiveSurface> {
    if (surfaces.empty()) {
        return {};
    }
    std::unordered_map<std::string, std::optional<VMTMaterial>> vmtCache;
    std::vector<EmissiveSurface>                                kept;
    kept.reserve(surfaces.size());

    for (EmissiveSurface& surface: surfaces) {
        const auto vmtIt = vmtCache.try_emplace(surface.materialName, std::nullopt);
        if (vmtIt.second) {
            vmtIt.first->second = LoadMaterialVmt(vfs, surface.materialName);
        }
        const VMTMaterial* vmt = vmtIt.first->second.has_value() ? &*vmtIt.first->second : nullptr;

        // Measured luminance of the material's own base texture: the most
        // direct evidence a surface emits, and it overrides the family name
        // when the .vmt declares no self-illumination of its own.
        const float luma = (vmt != nullptr && !vmt->baseTexture.empty()) ? TextureLuma(vfs, *vmt) : -1.0f;

        const EmissiveMaterialProbe probe = ProbeEmissiveMaterial(surface.materialName, surface.reflectivityLuma, vmt, surface.noLightFlag);
        if (!probe.isEmissive || probe.strength <= 0.0f) {
            continue;
        }
        surface.strength = std::max(probe.strength, (luma > 0.0f) ? luma * 0.85f : 0.0f);
        surface.color    = probe.tint;
        if (surface.reflectivityLuma < 0.0f) {
            surface.reflectivityLuma = 0.0f;
        }
        kept.push_back(std::move(surface));
    }

    data.stats.emissiveFaces = 0;
    for (const EmissiveSurface& surface: kept) {
        data.stats.emissiveFaces += surface.faceCount;
    }
    data.stats.emissiveSurfaces = static_cast<uint32_t>(kept.size());
    return kept;
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
    ModelPrefab&           prefab
) {
    // One .vmt per material group, not one per face: a map with 40 lamp panels
    // still resolves 40 identical files once. `patch` is followed through so
    // the probe sees the material the surface actually renders with.
    std::unordered_map<std::string, std::optional<VMTMaterial>> vmtCache;

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

        BufferHandle meshletVbo       = hasMeshlets ? ctx.CreateBuffer(
                                                          BufferDesc {
                                                              .usage  = BufferUsage::Storage,
                                                              .data   = std::as_bytes(std::span {packedMeshlets}),
                                                              .stride = kMeshletPackedBytes,
                                                          }
                                                      ) :
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

        const auto vmtIt = vmtCache.try_emplace(part.materialName, std::nullopt);
        if (vmtIt.second) {
            vmtIt.first->second = LoadMaterialVmt(vfs, part.materialName);
        }
        const VMTMaterial* vmt = vmtIt.first->second.has_value() ? &*vmtIt.first->second : nullptr;

        const Material subMaterial = ctx.CreateMaterial(ResolveMaterialDesc(ctx, vfs, part, vmt)).value_or(Material {});

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
                data.collisionOnly.positions.data(), data.collisionOnly.VertexCount(), data.collisionOnly.indices.data(), data.collisionOnly.IndexCount()
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
                LogWarning(
                    "BSP Importer: {} collision-only tool faces could not be folded into any part collider; invisible walls are lost",
                    data.collisionOnly.indices.size() / 3
                );
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

    ImportedMapData imported = ImportMapGeometry(map, worldOptions);
    if (imported.parts.empty()) {
        LogWarning("BSP Importer: no importable surfaces in {}", virtualPath);
        return std::nullopt;
    }

    // Face-pipeline accounting: when a wall seems to be missing in-game, this
    // line proves whether it was dropped here (and by which stage) or is
    // present and the problem is downstream of this import.
    Log("BSP Importer: {} -> {} total faces -> {} render faces in {} parts ({} triangles); dropped: {} no-winding, {} degenerate, {} no-texinfo, {} "
        "tool-helper; {} collision-only; {} disp fallbacks; {} single-sheet walls forced double-sided",
        virtualPath,
        imported.faceCount + imported.stats.helperFacesDropped + imported.stats.collisionOnlyFaces + imported.stats.droppedNoWinding +
            imported.stats.droppedDegenerate + imported.stats.droppedNoProjection,
        imported.faceCount, imported.parts.size(), imported.triangleCount, imported.stats.droppedNoWinding, imported.stats.droppedDegenerate,
        imported.stats.droppedNoProjection, imported.stats.helperFacesDropped, imported.stats.collisionOnlyFaces, imported.stats.displacementFallbacks,
        imported.stats.singleSidedSheets);

    auto prefab                 = std::make_unique<ModelPrefab>();
    prefab->virtualPath         = String256(virtualPath);
    prefab->emissiveFactorScale = 1.0f; // Keep Source emission at its authored linear scale.

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

    // Stage two of the emissive heuristic: re-probe the name-gated candidates
    // against the materials they actually render with (see
    // RefineEmissiveSurfaces). The count of candidates the probe rejected is
    // reported in the lighting line below.
    uint32_t emissiveProbeDropped = 0;
    if (worldOptions.gatherEmissiveSurfaces) {
        const size_t candidateCount = imported.emissive.size();
        imported.emissive = RefineEmissiveSurfaces(vfs, imported.emissive, imported);
        emissiveProbeDropped = static_cast<uint32_t>(candidateCount - imported.emissive.size());
    }

    // Lighting accounting: this line is the answer to "why is my scene dark?".
    // Every light the entity lump offered leaves through exactly one counter.
    const LightingDiagnostics& lighting = imported.lighting;
    Log("BSP Importer: {} lighting -> {} entity lights emitted ({} offered: {} beam cones without a dynamic light, {} disabled/black); "
        "ambient {}{}; {} emissive faces -> {} area-light proxies over {:.3f} m2 ({} dropped by the probe, {} by the cap); "
        "{} fixtures damped{}",
        virtualPath, lighting.entityLightsEmitted, lighting.entityLightsOffered, lighting.droppedBeamCone, lighting.droppedDisabled,
        lighting.ambientAuthored ? "authored" : "default",
        (imported.ambient.color.GetX() + imported.ambient.color.GetY() + imported.ambient.color.GetZ() > 0.0f) ? "" : " (zero)", lighting.emissiveFaces,
        lighting.emissiveSurfaces, lighting.emissiveAreaSquareMetres, emissiveProbeDropped, lighting.emissiveDroppedCap, lighting.emissiveFixtureDamped,
        lighting.floorEnforced ? " [floor enforced]" : "");

    // One identity root; parts hang off it directly (part.localTransform does
    // the placement), lights get their own translated nodes because ModelLight
    // resolves position through its node chain.
    prefab->nodes.push_back(ModelNode {.name = String64("bsp_root"), .parentIndex = -1, .localTransform = JPH::Mat44::sIdentity(), .hasMesh = false});

    AssemblePrefabParts(ctx, vfs, imported, virtualPath, options, *prefab);

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
    // Normalize world-space interactive submodels to their pivot; leave the
    // usual entity-local VBSP representation untouched.
    //
    // The rotating path has always done this; the sliding path did not, on the
    // theory that "the entity transform is just the movement delta". That left
    // a sliding door's transform at the identity SceneEntity default -- the map
    // origin -- and three things read that transform as the door's real
    // position:
    //
    //   * BSPInteraction reads trans->position as the door's closedPosition, so
    //     the movement system interpolated origin -> origin + movedir*travel
    //     instead of in place, and the E-to-use proximity check (2.5 m from
    //     trans->position) never fired anywhere near the actual door.
    //   * Scene::Instantiate creates the kinematic root collider at
    //     params.position, so the door's collider sat at the map origin and the
    //     player walked straight through the door where it really was.
    //
    // Re-basing costs nothing visually: the rendered vertex is
    // pivot + (worldVertex - pivot) == worldVertex, exactly as before.
    for (const InteractiveSubmodel& sub: interactiveSubmodels) {
        ImportOptions subOptions = options;
        subOptions.onlyModel     = sub.modelIndex;
        subOptions.gatherLights  = false;

        ImportedMapData subImported = ImportMapGeometry(map, subOptions);
        if (subImported.parts.empty()) {
            continue; // Pure tool faces (clip/hint brushes): nothing to draw.
        }
        // VBSP normally stores brush-entity vertices in entity-local space
        // (gm_murder_drones' door models *123..*125 are centred on zero while
        // their origins are around (-504, 1592, 184)). Older/other branches can
        // leave them in world space. Re-base only the latter: unconditionally
        // subtracting the entity pivot sends already-local render geometry back
        // to the map origin, while the fallback collider remains at the door.
        const JPH::Vec3 geometryCenter(
            (subImported.boundsMin.x + subImported.boundsMax.x) * 0.5f,
            (subImported.boundsMin.y + subImported.boundsMax.y) * 0.5f,
            (subImported.boundsMin.z + subImported.boundsMax.z) * 0.5f
        );
        if ((geometryCenter - sub.pivot).LengthSq() < geometryCenter.LengthSq()) {
            ShiftImportedGeometry(subImported, -sub.pivot);
        }

        const std::string subPath      = std::string(virtualPath) + "#model_" + std::to_string(sub.modelIndex);
        auto              subPrefab    = std::make_unique<ModelPrefab>();
        subPrefab->virtualPath         = String256(subPath);
        subPrefab->emissiveFactorScale = 1.0f;
        subPrefab->nodes.push_back(
            ModelNode {.name = String64("bsp_submodel_root"), .parentIndex = -1, .localTransform = JPH::Mat44::sIdentity(), .hasMesh = false}
        );
        AssemblePrefabParts(ctx, vfs, subImported, subPath, options, *subPrefab);
        assetMgr.CachePrefab(HashAssetPath(subPath), std::move(subPrefab));
        Log("BSP Importer: {} -> interactive submodel prefab ({}, {} parts)", subPath, sub.rotating ? "rotating" : "sliding", subImported.parts.size());
    }

    return assetMgr.GetCachedPrefab(HashAssetPath(virtualPath));
}

} // namespace

// ---------------------------------------------------------------------------
// The .vmt lighting rules (declared in BSPImporter.hpp)
// ---------------------------------------------------------------------------

auto ResolveVmtEmission(const SourceVFS& vfs, const VMTMaterial& vmt) -> VmtEmission {
    // Source maps carry their emission in three places, and a mapper only has
    // to use one of them for the surface to be dark without all of them:
    //
    //   1. `$selfillum` / `$selfillummask` / `$selfillummaskscale` /
    //      `$emissiveblendstrength` -- the authored case. `$selfillum` is
    //      either a bare 0/1 mask switch, a single scalar, or a full
    //      "r g b a" colour; all three spell the same intent, so take the
    //      scalar when there is one and fall back to a plain 1.0 mask.
    //      Self-illuminated LightmappedGeneric is not unlit (it still
    //      receives light), so the shader class scales the term rather than
    //      replacing it -- with a 0.35 floor, because a mapper who wrote
    //      `$selfillum` meant "this emits".
    //   2. The shader class itself: UnlitGeneric / Sprite / Cable / Beam /
    //      MonitorScreen are emissive by construction, whatever the keyvalues
    //      say, and they are the classes a map's lamp panels and screens use.
    //   3. The measured luminance of the base texture, for the materials that
    //      author neither. A white panel on a dark wall is the case this
    //      catches -- the mapper never wrote a keyvalue, and the panel still
    //      has to glow.
    //
    // The term is emitted in the material's own colour, and `emissiveFactorScale`
    // stays at 1.0 on the prefabs so Source's authored linear values survive to
    // the renderer unchanged.
    const float shaderStrength    = ShaderEmissiveStrength(vmt.shader);
    const float selfIllumStrength = SelfIllumStrength(vmt);

    VmtEmission emission;
    if (selfIllumStrength >= 0.0f) {
        emission.strength = selfIllumStrength * std::max(shaderStrength, 0.35f);
    } else {
        // No authored emission, so the surface emits only as much as its own
        // base texture is bright. Measured lazily: decoding a .vtf is not free
        // and the authored case above never needs it. A white panel on a dark
        // wall is the case this catches -- the mapper never wrote a keyvalue,
        // and the panel still has to glow.
        const float textureLuma = vmt.baseTexture.empty() ? -1.0f : TextureLuma(vfs, vmt);
        if (textureLuma > 0.0f) {
            emission.strength = textureLuma * shaderStrength;
        }
    }

    // Keep the term at the material's own chromaticity and let its luminance
    // set the magnitude: a red neon sign emits red, not a grey scaled by red.
    emission.tint = JPH::Vec3(vmt.baseColor.x, vmt.baseColor.y, vmt.baseColor.z);
    if (Rec709Luma(emission.tint) < 1e-4f) {
        emission.tint = JPH::Vec3(1.0f, 1.0f, 1.0f); // a black albedo is "no tint", not "black light"
    }
    return emission;
}

auto ApplyVmtLighting(const SourceVFS& vfs, const VMTMaterial& vmt, MaterialDesc& desc) -> void {
    // 1. Unlit by construction. UnlitGeneric has no lighting path at all --
    // the albedo goes straight to the framebuffer -- and Source's white lamp
    // panels (lights/white and friends) would land at luma 1.0, exactly the
    // bloom threshold, turning every ceiling fixture into a blinding glare.
    // Scale them to read bright but sub-threshold. Missing textures would
    // otherwise fall back to the raw baseColor (usually pure white) and blow
    // out wherever direct light reaches.
    if (ShaderNameEquals(vmt.shader, "UnlitGeneric")) {
        desc.unlit = true;
        desc.baseColor.x *= kUnlitDim;
        desc.baseColor.y *= kUnlitDim;
        desc.baseColor.z *= kUnlitDim;
    }

    // 2. The emission term.
    const VmtEmission emission = ResolveVmtEmission(vfs, vmt);
    if (emission.strength <= kEmissiveStrengthEpsilon) {
        return;
    }

    const float luma  = std::max(Rec709Luma(emission.tint), kEmissiveStrengthEpsilon);
    const float scale = emission.strength * kSourceEmissionHdrScale / luma;
    desc.emissive     = JPH::Float4(emission.tint.GetX() * scale, emission.tint.GetY() * scale, emission.tint.GetZ() * scale, 1.0f);

    // Emissive *and* unlit by construction: an UnlitGeneric that gets here is
    // already flagged above, and the sprite/cable/beam classes only bypass the
    // lighting model once they actually emit.
    if (ShaderNameIn(vmt.shader, {"UnlitGeneric", "Sprite", "Cable", "Beam"})) {
        desc.unlit = true;
    }
}

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

auto PreloadStaticProps(RenderContext& ctx, AssetManager& assetMgr, const BSPMap& map, const ImportOptions& options) -> size_t {
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
