// Copyright (C) 2026 Evilpasture | housemaster@evilpasture.org
// SPDX-License-Identifier: GPL-3.0-or-later
#include "BSPAssets.hpp"
#include "AssetUpload.hpp"
#include "BSPMaterial.hpp"
#include "BSPPreparation.hpp"
#include <Zahlen/Core/AssetID.hpp>
#include <Zahlen/Math/Geometry2D.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <optional>
#include <unordered_map>

namespace ZHLN::BSP {
namespace {
auto ValidAssetPath(std::string_view path) -> bool {
    return !path.empty() && path.size() < 256 && path.find('\0') == std::string_view::npos;
}
auto ValidStreams(const MaterialStreams& part, bool render) -> bool {
    if (part.positions.size() > UINT32_MAX || part.indices.size() > UINT32_MAX || part.indices.size() % 3 != 0)
        return false;
    if (render && (part.tangentFrames.size() != part.positions.size() || part.surfaces.size() != part.positions.size()))
        return false;
    for (const auto& vertex: part.positions)
        for (const float lane: vertex.position)
            if (!std::isfinite(lane))
                return false;
    for (const auto index: part.indices)
        if (index >= part.positions.size())
            return false;
    return true;
}
auto HasTriangles(const MaterialStreams& part) -> bool {
    for (size_t i = 0; i < part.indices.size(); i += 3)
        if (part.indices[i] != part.indices[i + 1] && part.indices[i] != part.indices[i + 2] && part.indices[i + 1] != part.indices[i + 2])
            return true;
    return false;
}
auto CacheStreams(COM::IAssetLibrary& assets, CollisionAssetID id, const MaterialStreams& part) -> COM::Result<void> {
    std::vector<FVec3> vertices;
    vertices.reserve(part.positions.size());
    for (const auto& v: part.positions)
        vertices.push_back({v.position[0], v.position[1], v.position[2]});
    return assets.CacheCollisionMesh(COM::InChain(COM::CollisionMeshInfo {.id = id, .vertices = vertices, .indices = part.indices}));
}
} // namespace

auto PrepareAssets(const Map& map, SourceVFS source, std::string_view virtualPath, const ImportOptions& options)
    -> std::expected<PreparedAssets, ErrorCode> {
    if (!ValidAssetPath(virtualPath) || !std::isfinite(options.unitScale) || options.unitScale <= 0)
        return std::unexpected(COM::Error::InvalidArgument);
    if (!options.assetRoot.empty())
        source.AddSearchPath(options.assetRoot);
    const std::filesystem::path path(virtualPath);
    if (path.has_parent_path()) {
        source.AddSearchPath(path.parent_path().string());
        if (path.parent_path().has_parent_path())
            source.AddSearchPath(path.parent_path().parent_path().string());
    }
    const auto moving         = CollectInteractiveSubmodels(map, options);
    auto       worldOptions   = options;
    worldOptions.gatherLights = false; // BSP scene owns the authored entity lights.
    for (const auto& sub: moving)
        worldOptions.skipModels.push_back(sub.modelIndex);
    auto world = ImportMapGeometry(map, worldOptions);
    if (world.parts.empty())
        return std::unexpected(COM::Error::InvalidArgument);
    if (worldOptions.gatherEmissiveSurfaces)
        world.emissive = RefineEmissiveSurfaces(source, world.emissive, world);
    PreparedAssets result {.source = std::move(source)};
    result.prefabs.push_back({std::string(virtualPath), std::move(world)});
    for (const auto& sub: moving) {
        auto subOptions             = options;
        subOptions.onlyModel        = sub.modelIndex;
        subOptions.gatherLights     = false;
        subOptions.useVradLightmaps = false;
        subOptions.ingestVradBake   = false;
        auto geometry               = ImportMapGeometry(map, subOptions);
        if (geometry.parts.empty())
            continue;
        const FVec3 center {
            (geometry.boundsMin.x + geometry.boundsMax.x) * 0.5f, (geometry.boundsMin.y + geometry.boundsMax.y) * 0.5f,
            (geometry.boundsMin.z + geometry.boundsMax.z) * 0.5f
        };
        if ((center - sub.pivot).LengthSquared() < center.LengthSquared())
            ShiftImportedGeometry(geometry, -sub.pivot);
        auto key = std::string(virtualPath) + "#model_" + std::to_string(sub.modelIndex);
        if (!ValidAssetPath(key))
            return std::unexpected(COM::Error::InvalidArgument);
        result.prefabs.push_back({std::move(key), std::move(geometry)});
    }
    const auto request = [&](std::string_view name) {
        if (!name.empty() && std::find(result.studioModels.begin(), result.studioModels.end(), name) == result.studioModels.end())
            result.studioModels.emplace_back(name);
    };
    for (const auto& prop: map.staticProps)
        request(prop.modelName);
    for (const auto& entity: map.entities) {
        const auto name = entity.Find("model");
        if (name.ends_with(".mdl"))
            request(name);
    }
    return result;
}

auto DecodeAssets(std::span<const std::byte> bytes, SourceVFS source, std::string_view virtualPath, const ImportOptions& options)
    -> std::expected<PreparedAssets, ErrorCode> {
    const auto parsed = Parse(bytes);
    if (!parsed)
        return std::unexpected(parsed.error());
    // Parse has validated every lump range; the SourceVFS keeps its own pak
    // copy, so neither the caller's bytes nor the parser result is borrowed.
    Header header {};
    std::memcpy(&header, bytes.data(), sizeof(header));
    const auto& pak = header.lumps[static_cast<size_t>(Lump::PakFile)];
    if (pak.filelen > 0)
        source.SetPakfileData(bytes.subspan(static_cast<size_t>(pak.fileofs), static_cast<size_t>(pak.filelen)));
    return PrepareAssets(*parsed, std::move(source), virtualPath, options);
}

auto ReadAssets(SourceVFS source, std::string_view virtualPath, const ImportOptions& options) -> std::expected<PreparedAssets, ErrorCode> {
    if (!ValidAssetPath(virtualPath))
        return std::unexpected(COM::Error::InvalidArgument);
    if (!options.assetRoot.empty())
        source.AddSearchPath(options.assetRoot);
    const auto bytes = source.ReadFile(virtualPath);
    if (!bytes)
        return std::unexpected(Error::ShortRead);
    return DecodeAssets(*bytes, std::move(source), virtualPath, options);
}

auto UploadAsset(
    COM::IAssetLibrary&     assets,
    COM::IGraphicsDevice&   graphics,
    const SourceVFS&        source,
    const AssetDocument& document,
    const AssetOptions&  options
) -> std::expected<SourceAsset, ErrorCode> {
    const auto& path     = document.virtualPath;
    const auto& geometry = document.geometry;
    if (!ValidAssetPath(path))
        return std::unexpected(COM::Error::InvalidArgument);
    const auto hash = assets.PrefabHash(std::string_view(path));
    if (assets.HasPrefab(hash))
        return SourceAsset {.hash = hash};
    if (!std::isfinite(options.import.sourceLightmapScale))
        return std::unexpected(COM::Error::InvalidArgument);
    for (const auto& part: geometry.parts) {
        if (!ValidStreams(part, true))
            return std::unexpected(COM::Error::InvalidArgument);
        if (!std::isfinite(part.boundsMin.x) || !std::isfinite(part.boundsMin.y) || !std::isfinite(part.boundsMin.z) || !std::isfinite(part.boundsMax.x) ||
            !std::isfinite(part.boundsMax.y) || !std::isfinite(part.boundsMax.z) || part.boundsMin.x > part.boundsMax.x ||
            part.boundsMin.y > part.boundsMax.y || part.boundsMin.z > part.boundsMax.z)
            return std::unexpected(COM::Error::InvalidArgument);
    }
    if (!ValidStreams(geometry.collisionOnly, false))
        return std::unexpected(COM::Error::InvalidArgument);
    for (const auto& page: geometry.lightmaps.pages) {
        if (page.rgba16f.empty())
            continue;
        if (!page.width || !page.height || uint64_t(page.width) * page.height != page.rgba16f.size() / 4 || page.rgba16f.size() % 4)
            return std::unexpected(COM::Error::InvalidArgument);
    }

    AssetAuthoring::Upload upload {graphics};
    std::vector<TextureID> lightmaps(geometry.lightmaps.pages.size(), TextureID::Invalid);
    for (size_t i = 0; i < lightmaps.size(); ++i) {
        const auto& page = geometry.lightmaps.pages[i];
        if (page.rgba16f.empty())
            continue;
        const auto name    = path + "#vrad_lightmap_" + std::to_string(i);
        const auto texture = graphics.CreateTexture(
            COM::InChain(
                COM::TextureCreateInfo {
                    .name   = std::string_view(name),
                    .pixels = std::as_bytes(std::span(page.rgba16f)),
                    .extent = {page.width, page.height},
                    .isSRGB = false,
                    .isHDR  = true
                }
            )
        );
        if (!texture.HasValue())
            return std::unexpected(texture.error);
        lightmaps[i] = texture.value;
        upload.textures.push_back(texture.value);
    }
    std::vector<COM::PrefabPartInfo>                            parts;
    std::vector<const MaterialStreams*>                         authored;
    std::unordered_map<std::string, std::optional<VMTMaterial>> materials;
    for (const auto& part: geometry.parts) {
        if (part.positions.empty() || part.indices.empty())
            continue;
        const auto mesh = graphics.UploadMesh(
            COM::MeshStreamsDesc {.positions = part.positions, .tangentFrames = part.tangentFrames, .surfaces = part.surfaces, .indices32 = part.indices},
            options.buildBLAS
        );
        if (!mesh.HasValue())
            return std::unexpected(mesh.error);
        upload.meshes.push_back(mesh.value);
        auto [vmt, inserted] = materials.try_emplace(part.materialName);
        if (inserted)
            vmt->second = LoadMaterialVmt(source, part.materialName);
        std::optional<ErrorCode> textureError;
        auto                     desc = BSPMaterial::Resolve(
            [&](std::string_view name, std::span<const std::byte> pixels, Extent2D extent, bool srgb) -> TextureID {
                if (textureError)
                    return TextureID::Invalid;
                const auto key     = path + "#source_" + std::string(name) + (srgb ? "#srgb" : "#linear");
                const auto texture = graphics.CreateTexture(std::string_view(key), pixels, extent, srgb);
                if (!texture.HasValue()) {
                    textureError = texture.error;
                    return TextureID::Invalid;
                }
                if (std::find(upload.textures.begin(), upload.textures.end(), texture.value) == upload.textures.end())
                    upload.textures.push_back(texture.value);
                return texture.value;
            },
            source, part, vmt->second ? &*vmt->second : nullptr
        );
        if (textureError)
            return std::unexpected(*textureError);
        if (part.lightmapPage < lightmaps.size() && lightmaps[part.lightmapPage] != TextureID::Invalid && !desc.unlit && !desc.alphaBlend &&
            !desc.additiveBlend && desc.alphaMode != 2u) {
            desc.bakedIrradianceMap   = lightmaps[part.lightmapPage];
            desc.bakedIrradianceScale = options.import.sourceLightmapScale;
        }
        const auto material = graphics.CreateMaterial(desc);
        if (!material.HasValue())
            return std::unexpected(material.error);
        const auto  key = path + "#part_" + std::to_string(parts.size());
        const FVec3 half {
            std::max((part.boundsMax.x - part.boundsMin.x) * 0.5f, 0.01f), std::max((part.boundsMax.y - part.boundsMin.y) * 0.5f, 0.01f),
            std::max((part.boundsMax.z - part.boundsMin.z) * 0.5f, 0.01f)
        };
        const FVec3 center {
            (part.boundsMin.x + part.boundsMax.x) * 0.5f, (part.boundsMin.y + part.boundsMax.y) * 0.5f, (part.boundsMin.z + part.boundsMax.z) * 0.5f
        };
        const bool collider = options.import.buildColliders && !part.skyboxLayer;
        parts.push_back(
            COM::PrefabPartInfo {
                .name            = String64(part.materialName.c_str()),
                .meshAsset       = assets.AssetHash(std::string_view(key)),
                .materialAsset   = assets.AssetHash(std::string_view(key + "_mat")),
                .mesh            = mesh.value,
                .defaultMaterial = material.value,
                .nodeIndex       = 0,
                .boundingRadius  = std::hypot(
                    std::max(std::abs(part.boundsMin.x), std::abs(part.boundsMax.x)), std::max(std::abs(part.boundsMin.y), std::abs(part.boundsMax.y)),
                    std::max(std::abs(part.boundsMin.z), std::abs(part.boundsMax.z))
                ),
                .localMin              = part.boundsMin,
                .localMax              = part.boundsMax,
                .collision             = collider && HasTriangles(part) ? HashCollisionAssetID(key + "#collision") : CollisionAssetID::Invalid,
                .hasBoxCollider        = collider,
                .boxHalfExtents        = half,
                .boxCenter             = center,
                .useBoxForMeshCollider = collider && !HasTriangles(part),
                .drawFlags             = part.skyboxLayer ? DrawFlags::Background : DrawFlags::None
            }
        );
        authored.push_back(&part);
    }
    if (parts.empty())
        return std::unexpected(COM::Error::InvalidArgument);
    // Collisions are committed only after all GPU allocation/material work has
    // succeeded. CacheCollisionMesh copies each stream; no caller memory escapes.
    for (size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].collision == CollisionAssetID::Invalid)
            continue;
        const auto cached = CacheStreams(assets, parts[i].collision, *authored[i]);
        if (!cached.HasValue())
            return std::unexpected(cached.error);
    }
    if (options.import.buildColliders && HasTriangles(geometry.collisionOnly)) {
        const auto solid = std::find_if(parts.begin(), parts.end(), [](const auto& part) { return part.hasBoxCollider; });
        if (solid != parts.end()) {
            const auto id     = HashCollisionAssetID(path + "#collision_only");
            const auto cached = CacheStreams(assets, id, geometry.collisionOnly);
            if (!cached.HasValue())
                return std::unexpected(cached.error);
            solid->additionalMeshCollision = id;
        }
    }
    std::vector<COM::PrefabNodeInfo>  nodes {{.name = String64("bsp_root")}};
    std::vector<COM::PrefabLightInfo> lights;
    for (const auto& light: geometry.lights) {
        nodes.push_back({.name = String64(light.name.c_str()), .parentIndex = 0, .localTransform = CreateTranslation(light.position)});
        lights.push_back(
            {.name           = String64(light.name.c_str()),
             .nodeIndex      = static_cast<int32_t>(nodes.size() - 1),
             .type           = light.type,
             .color          = light.color,
             .intensity      = light.intensity,
             .range          = light.range,
             .innerConeAngle = light.type == LightType::Spot ? light.innerConeRadians : 0,
             .outerConeAngle = light.type == LightType::Spot ? light.outerConeRadians : 0}
        );
    }
    const auto registered = assets.RegisterPrefab(
        COM::InChain(
            COM::PrefabRegistrationInfo {.virtualPath = std::string_view(path), .emissiveFactorScale = 1.0f, .nodes = nodes, .parts = parts, .lights = lights}
        )
    );
    if (!registered.HasValue())
        return std::unexpected(registered.error);
    upload.committed = true;
    return SourceAsset {.hash = registered.value, .textures = std::move(upload.textures)};
}
} // namespace ZHLN::BSP
