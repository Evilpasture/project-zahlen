// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// plugins/BSP/BSPImporter.hpp
//
// The Source engine BSP reader, in the same shape as <glTF/GLTFImporter.hpp>:
// there is no registration step and nothing in src/ includes this header.
// These functions import the map into the plain ZHLN::ModelPrefab the ECS
// already describes -- native vertex streams, materials, colliders, lights --
// and cache it under HashAssetPath(path); from then on Core's
// PrefabFactory::LoadModelPrefab(path) returns it and PrefabFactory::
// InstantiatePrefab / Scene::Instantiate do the rest.
//
// Scope: mainline Source formats (BSP versions 19-21). World and brush-model
// geometry, displacements, entity lights. Material *textures* are not decoded
// yet (VMT/VTF is its own format family) -- materials come up as stable
// default PBR materials named after their texdata.

#include "BSPGeometry.hpp"
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

// Reads path from disk and imports it. A cached prefab from an earlier call is
// returned as-is (the cache key is the virtual path, like the glTF importer).
[[nodiscard]] auto
    LoadBSPPrefab(RenderContext& ctx, AssetManager& assetMgr, std::string_view path, const ImportOptions& options = {}) -> ZHLN::Optional<ModelPrefab&>;

// Like LoadBSPPrefab, but consumes bytes already read by the caller. The
// virtual path names the cache entry; geometry converts per options.
[[nodiscard]] auto LoadBSPPrefabFromMemory(
    RenderContext&             ctx,
    AssetManager&              assetMgr,
    std::span<const std::byte> bytes,
    std::string_view           virtualPath,
    const ImportOptions&       options = {}
) -> ZHLN::Optional<ModelPrefab&>;

// Preloads all StudioModel static props referenced by the map into AssetManager.
// Returns the count of successfully resolved and cached prefabs.
auto PreloadStaticProps(
    RenderContext&       ctx,
    AssetManager&        assetMgr,
    const BSPMap&        map,
    const ImportOptions& options = {}
) -> size_t;

} // namespace ZHLN::BSP
