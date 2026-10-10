// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "BSPGeometry.hpp"
#include "SourceVFS.hpp"
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <cstddef>
#include <span>
#include <string_view>

namespace ZHLN {
class RenderContext;
class AssetManager;
} // namespace ZHLN

namespace ZHLN::BSP {

// Loads a Source engine StudioModel (.mdl + .vvd + .vtx + .phy) from disk via SourceVFS,
// builds a ModelPrefab with GPU buffers, materials, and colliders, and caches it in AssetManager.
[[nodiscard]] auto LoadStudioModelPrefab(
    RenderContext&       ctx,
    AssetManager&        assetMgr,
    const SourceVFS&     vfs,
    std::string_view     path,
    const ImportOptions& options = {}
) -> ZHLN::Optional<ModelPrefab&>;

// Loads a StudioModel from in-memory byte buffers and caches it under virtualPath.
[[nodiscard]] auto LoadStudioModelPrefabFromMemory(
    RenderContext&             ctx,
    AssetManager&              assetMgr,
    const SourceVFS&           vfs,
    std::span<const std::byte> mdlBytes,
    std::span<const std::byte> vvdBytes,
    std::span<const std::byte> vtxBytes,
    std::span<const std::byte> phyBytes,
    std::string_view           virtualPath,
    const ImportOptions&       options = {}
) -> ZHLN::Optional<ModelPrefab&>;

} // namespace ZHLN::BSP
