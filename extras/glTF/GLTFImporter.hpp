// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// extras/glTF/GLTFImporter.hpp
//
// The glTF/GLB reader. This is an extra, not core: reading a model file means a
// container parser (cgltf), an image decoder (stb), a mesh partitioner
// (meshoptimizer) and a JSON reader for the custom members -- none of which the
// engine needs in order to run. Core reaches it only through the
// ZHLN::PrefabLoader hook, so nothing in src/ includes this header.

#include <Zahlen/ModelPrefab.hpp>
#include <span>
#include <string_view>

namespace ZHLN {
class RenderContext;
class CreativeWorksManager;

namespace GLTF {
auto LoadGLBPrefab(RenderContext& ctx, CreativeWorksManager& cwMgr, std::string_view path) -> ModelPrefab*;
auto LoadGLBPrefabFromMemory(RenderContext& ctx, CreativeWorksManager& cwMgr, std::span<const uint8_t> bytes, std::string_view virtualPath) -> ModelPrefab*;
void RebuildPrefabGPUResources(RenderContext& ctx, ModelPrefab* prefab);

/// Installs the three functions above as the engine's ZHLN::PrefabLoader
/// backend. Applications call this through ZHLN::glTF::RegisterPrefabLoader();
/// it is declared here so the importer can be used without the inspector
/// module. Idempotent.
void RegisterAsPrefabLoader() noexcept;
} // namespace GLTF
} // namespace ZHLN
