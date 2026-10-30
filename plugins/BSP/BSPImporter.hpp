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
#include "SourceVFS.hpp"
#include "VMTParser.hpp"
#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/Engine/ModelPrefab.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Render/Types.hpp>
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

// ---------------------------------------------------------------------------
// The .vmt lighting rules
// ---------------------------------------------------------------------------
//
// A `.vmt` has one appearance, and both material passes have to agree on it:
// this file assembles brush materials, StudioModelImporter assembles prop
// materials, and a map routinely puts the same material on both (Source's
// `light_wall` lamp panel is a prop, the glow sheet under the same fixture is
// a brush). Source states that appearance in two places, and a pass that reads
// only one of them renders the material dark:
//
//   1. `$selfillum` / `$selfillummask` / `$selfillummaskscale` /
//      `$emissiveblendstrength` -- the authored self-illumination.
//   2. The shader class, plus the measured luminance of `$basetexture` for the
//      materials that author neither (a bright lamp panel with no keyvalues
//      still has to glow).
//
// `strength` is 0..1 linear and already scaled by the shader class, and `tint`
// is the chromaticity the emission is emitted in -- a red neon sign emits red,
// not a grey scaled by red.
struct VmtEmission {
    float     strength = 0.0f;
    JPH::Vec3 tint {1.0f, 1.0f, 1.0f};
};

// Reads the rules above out of one parsed material. The base texture luminance
// needs the VFS (it decodes the .vtf), which is the only reason this is not a
// pure function of the keyvalues.
[[nodiscard]] auto ResolveVmtEmission(const SourceVFS& vfs, const VMTMaterial& vmt) -> VmtEmission;

// Applies the rules above to a material description: first the shader classes
// that are unlit by construction, then the emission term. `desc.baseColor` and
// the texture maps are expected to be filled in already.
//
// Exposed (rather than file-local to the world pass) so the StudioModel pass
// -- and a test -- reach the same answer: the lamp props in a shipped map are
// `VertexLitGeneric` + `$selfillum 1`, and they stayed dark only because the
// prop pass never read those keyvalues.
auto ApplyVmtLighting(const SourceVFS& vfs, const VMTMaterial& vmt, MaterialDesc& desc) -> void;

} // namespace ZHLN::BSP
