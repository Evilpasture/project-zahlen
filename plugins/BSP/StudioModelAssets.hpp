// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "BSPGeometry.hpp"
#include "SourceVFS.hpp"
#include "SourceAsset.hpp"
#include "StudioModelLoader.hpp"
#include <Zahlen/COM/IAssetLibrary.hpp>
#include <Zahlen/COM/IGraphicsDevice.hpp>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

namespace ZHLN::BSP {

// Consumer-side policy, not a wire descriptor. Raster-only hosts may opt out of
// BLAS creation; the default preserves the native importer's ray-query path.
struct StudioAssetOptions {
    ImportOptions import {};
    bool buildBLAS = true;
};

// Source-compatible name for the shared Source texture-ownership manifest.
using StudioAsset = SourceAsset;

// No Engine, native prefab, physics shape or render-context borrow is involved.
// Upload accepts parsed CPU data, so tools and tests can author assets without
// inventing MDL/VVD/VTX containers. All descriptors are copied by RegisterPrefab.
[[nodiscard]] auto UploadStudioAsset(COM::IAssetLibrary& assets, COM::IGraphicsDevice& graphics,
    const SourceVFS& vfs, const StudioModelData& data, std::string_view virtualPath,
    const StudioAssetOptions& options = {}) -> std::expected<StudioAsset, ErrorCode>;

// As with the existing Studio importer, unreadable/malformed Source models get
// an identity-root, box-collider placeholder. Capability/upload errors are NOT
// converted into placeholders: their typed errors propagate to the host.
[[nodiscard]] auto LoadStudioAssetFromMemory(COM::IAssetLibrary& assets, COM::IGraphicsDevice& graphics,
    const SourceVFS& vfs, std::span<const std::byte> mdl, std::span<const std::byte> vvd,
    std::span<const std::byte> vtx, std::span<const std::byte> phy, std::string_view virtualPath,
    const StudioAssetOptions& options = {}) -> std::expected<StudioAsset, ErrorCode>;

[[nodiscard]] auto LoadStudioAsset(COM::IAssetLibrary& assets, COM::IGraphicsDevice& graphics,
    const SourceVFS& vfs, std::string_view virtualPath,
    const StudioAssetOptions& options = {}) -> std::expected<StudioAsset, ErrorCode>;
} // namespace ZHLN::BSP
