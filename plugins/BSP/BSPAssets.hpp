// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "BSPGeometry.hpp"
#include "SourceAsset.hpp"
#include "SourceVFS.hpp"
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace ZHLN::BSP {
struct AssetOptions {
    ImportOptions import {};
    bool          buildBLAS = true;
};

// CPU documents belong to the importing host/tool, never Core. The world and
// each moving brush are separate assets; upload one document per transaction.
struct AssetDocument {
    std::string     virtualPath;
    ImportedMapData geometry;
};
struct PreparedAssets {
    // Owns embedded pak bytes; optionally borrows an IAssetLibrary capability.
    SourceVFS                     source;
    std::vector<AssetDocument> prefabs;      // world first, then moving brushes
    std::vector<std::string>      studioModels; // explicit Studio asset import requests
};

// CPU preparation: exclude moving brushes from the world, retain their cache
// keys, rebase world-space doors to their pivots, disable their static bake and
// avoid duplicating lights that BSPScene describes separately. Source material
// policy, skybox placement, displacement and VRAD decoding remain shared.
[[nodiscard]] auto PrepareAssets(const Map& map, SourceVFS source, std::string_view virtualPath, const ImportOptions& options = {})
    -> std::expected<PreparedAssets, ErrorCode>;
[[nodiscard]] auto DecodeAssets(std::span<const std::byte> bytes, SourceVFS source, std::string_view virtualPath, const ImportOptions& options = {})
    -> std::expected<PreparedAssets, ErrorCode>;
[[nodiscard]] auto
    ReadAssets(SourceVFS source, std::string_view virtualPath, const ImportOptions& options = {}) -> std::expected<PreparedAssets, ErrorCode>;

// One copied prefab registration. Success transfers meshes to the cache and
// returns the caller-owned texture manifest; failure retires unpublished GPU
// allocations. Collision cache entries are copied engine data, owned until
// ClearCache even if a later registration fails. Upload documents explicitly so
// a failed submodel never hides ownership of earlier successful imports.
[[nodiscard]] auto UploadAsset(
    COM::IAssetLibrary&     assets,
    COM::IGraphicsDevice&   graphics,
    const SourceVFS&        source,
    const AssetDocument& document,
    const AssetOptions&  options = {}
) -> std::expected<SourceAsset, ErrorCode>;
} // namespace ZHLN::BSP
