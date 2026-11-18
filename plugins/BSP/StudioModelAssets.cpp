// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "StudioModelAssets.hpp"
#include "AssetUpload.hpp"
#include "StudioMaterial.hpp"
#include <Zahlen/Core/AssetID.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>

namespace ZHLN::BSP {
namespace {

auto ValidPath(std::string_view path) -> bool {
    return !path.empty() && path.size() < 256 && path.find('\0') == std::string_view::npos;
}

auto RegisterStudioParts(COM::IAssetLibrary& assets, std::string_view path,
                         std::span<const COM::PrefabPartInfo> parts) -> COM::Result<uint64_t> {
    const std::array<COM::PrefabNodeInfo, 1> nodes {{
        {.name = String64("root"), .parentIndex = -1, .localTransform = Mat4x4::Identity(), .hasMesh = false}
    }};
    return assets.RegisterPrefab(COM::InChain(COM::PrefabRegistrationInfo {
        .virtualPath = path, .emissiveFactorScale = 1.0f, .nodes = nodes, .parts = parts
    }));
}

auto Placeholder(COM::IAssetLibrary& assets, std::string_view path, bool colliders)
    -> std::expected<StudioAsset, ErrorCode> {
    if (!ValidPath(path)) return std::unexpected(COM::Error::InvalidArgument);
    const auto hash = assets.PrefabHash(path);
    if (assets.HasPrefab(hash)) return StudioAsset {.hash = hash};
    const auto key = std::string(path) + "#fallback";
    COM::PrefabPartInfo part {.name = String64("placeholder"),
        .meshAsset = assets.AssetHash(std::string_view(key)), .materialAsset = assets.AssetHash(std::string_view(key + "_mat")),
        .nodeIndex = 0, .boundingRadius = 0.433f,
        .localMin = {-0.25f, -0.25f, -0.25f}, .localMax = {0.25f, 0.25f, 0.25f},
        .hasBoxCollider = colliders, .boxHalfExtents = {0.25f, 0.25f, 0.25f}, .useBoxForMeshCollider = colliders};
    const auto registered = RegisterStudioParts(assets, path, std::span(&part, 1));
    if (!registered.HasValue()) return std::unexpected(registered.error);
    return StudioAsset {.hash = registered.value};
}

} // namespace

auto UploadStudioAsset(COM::IAssetLibrary& assets, COM::IGraphicsDevice& graphics,
    const SourceVFS& vfs, const StudioModelData& data, std::string_view virtualPath,
    const StudioAssetOptions& options) -> std::expected<StudioAsset, ErrorCode> {
    if (!ValidPath(virtualPath)) return std::unexpected(COM::Error::InvalidArgument);
    const auto hash = assets.PrefabHash(virtualPath);
    if (assets.HasPrefab(hash)) return StudioAsset {.hash = hash};

    // Validate the complete CPU input before allocating any GPU resources.
    for (const auto& source: data.parts) {
        if (source.positions.empty() || source.indices.empty()) continue;
        if (source.positions.size() > UINT32_MAX || source.indices.size() > UINT32_MAX ||
            source.tangentFrames.size() != source.positions.size() || source.surfaces.size() != source.positions.size() ||
            source.indices.size() % 3 != 0) return std::unexpected(COM::Error::InvalidArgument);
        for (const auto index: source.indices)
            if (index >= source.positions.size()) return std::unexpected(COM::Error::InvalidArgument);
        if (!std::isfinite(source.boundsMin.x) || !std::isfinite(source.boundsMin.y) || !std::isfinite(source.boundsMin.z) ||
            !std::isfinite(source.boundsMax.x) || !std::isfinite(source.boundsMax.y) || !std::isfinite(source.boundsMax.z) ||
            source.boundsMin.x > source.boundsMax.x || source.boundsMin.y > source.boundsMax.y || source.boundsMin.z > source.boundsMax.z)
            return std::unexpected(COM::Error::InvalidArgument);
    }

    AssetAuthoring::Upload upload {graphics};
    std::vector<COM::PrefabPartInfo> parts;
    for (const auto& source: data.parts) {
        if (source.positions.empty() || source.indices.empty()) continue;
        const auto uploaded = graphics.UploadMesh(COM::MeshStreamsDesc {
            .positions = source.positions, .tangentFrames = source.tangentFrames,
            .surfaces = source.surfaces, .indices32 = source.indices
        }, options.buildBLAS);
        if (!uploaded.HasValue()) return std::unexpected(uploaded.error);
        upload.meshes.push_back(uploaded.value);

        std::optional<ErrorCode> textureError;
        const auto desc = StudioMaterial::Resolve(
            [&](std::string_view name, std::span<const std::byte> bytes, Extent2D extent, bool srgb) -> TextureID {
                if (textureError) return TextureID::Invalid;
                const auto texture = graphics.CreateTexture(std::string_view(std::string(virtualPath) + "#studio_" + std::string(name)), bytes, extent, srgb);
                if (!texture.HasValue()) { textureError = texture.error; return TextureID::Invalid; }
                if (std::find(upload.textures.begin(), upload.textures.end(), texture.value) == upload.textures.end())
                    upload.textures.push_back(texture.value);
                return texture.value;
            }, vfs, source.materialName);
        if (textureError) return std::unexpected(*textureError);
        const auto material = graphics.CreateMaterial(desc);
        if (!material.HasValue()) return std::unexpected(material.error);

        const auto key = std::string(virtualPath) + "#part_" + std::to_string(parts.size());
        const FVec3 half {
            std::max(0.01f, (source.boundsMax.x - source.boundsMin.x) * 0.5f),
            std::max(0.01f, (source.boundsMax.y - source.boundsMin.y) * 0.5f),
            std::max(0.01f, (source.boundsMax.z - source.boundsMin.z) * 0.5f)
        };
        COM::PrefabPartInfo part {.name = String64(source.materialName.c_str()),
            .meshAsset = assets.AssetHash(std::string_view(key)), .materialAsset = assets.AssetHash(std::string_view(key + "_mat")),
            .mesh = uploaded.value, .defaultMaterial = material.value, .nodeIndex = 0,
            .boundingRadius = half.Length(),
            .localMin = {source.boundsMin.x, source.boundsMin.y, source.boundsMin.z},
            .localMax = {source.boundsMax.x, source.boundsMax.y, source.boundsMax.z},
            .hasBoxCollider = options.import.buildColliders, .boxHalfExtents = half,
            .useBoxForMeshCollider = options.import.buildColliders};
        parts.push_back(part);
    }
    const auto registered = RegisterStudioParts(assets, virtualPath, parts);
    if (!registered.HasValue()) return std::unexpected(registered.error);
    upload.committed = true;
    return StudioAsset {.hash = registered.value, .textures = std::move(upload.textures)};
}

auto LoadStudioAssetFromMemory(COM::IAssetLibrary& assets, COM::IGraphicsDevice& graphics,
    const SourceVFS& vfs, std::span<const std::byte> mdl, std::span<const std::byte> vvd,
    std::span<const std::byte> vtx, std::span<const std::byte> phy, std::string_view virtualPath,
    const StudioAssetOptions& options) -> std::expected<StudioAsset, ErrorCode> {
    if (!ValidPath(virtualPath)) return std::unexpected(COM::Error::InvalidArgument);
    const auto hash = assets.PrefabHash(virtualPath);
    if (assets.HasPrefab(hash)) return StudioAsset {.hash = hash};
    const auto parsed = ParseStudioModel(mdl, vvd, vtx, phy, options.import);
    if (!parsed) return Placeholder(assets, virtualPath, options.import.buildColliders);
    return UploadStudioAsset(assets, graphics, vfs, *parsed, virtualPath, options);
}

auto LoadStudioAsset(COM::IAssetLibrary& assets, COM::IGraphicsDevice& graphics,
    const SourceVFS& vfs, std::string_view virtualPath, const StudioAssetOptions& options)
    -> std::expected<StudioAsset, ErrorCode> {
    if (!ValidPath(virtualPath)) return std::unexpected(COM::Error::InvalidArgument);
    const auto hash = assets.PrefabHash(virtualPath);
    if (assets.HasPrefab(hash)) return StudioAsset {.hash = hash};
    const auto resolved = vfs.ResolveModel(virtualPath);
    if (!resolved) return Placeholder(assets, virtualPath, options.import.buildColliders);
    const auto mdl = vfs.ReadFile(*resolved);
    if (!mdl) return Placeholder(assets, virtualPath, options.import.buildColliders);
    auto base = *resolved;
    if (base.ends_with(".mdl")) base.resize(base.size() - 4);
    const auto vvd = vfs.ReadFile(base + ".vvd");
    const auto vtx = vfs.ReadFile(base + (vfs.Exists(base + ".dx90.vtx") ? ".dx90.vtx" : ".vtx"));
    const auto phy = vfs.ReadFile(base + ".phy");
    if (!vvd || !vtx) return Placeholder(assets, virtualPath, options.import.buildColliders);
    return LoadStudioAssetFromMemory(assets, graphics, vfs, *mdl, *vvd, *vtx,
        phy ? std::span<const std::byte>(*phy) : std::span<const std::byte> {}, virtualPath, options);
}
} // namespace ZHLN::BSP
