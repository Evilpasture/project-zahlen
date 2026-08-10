// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// src/gltf/GLTFImporter.hpp
#pragma once
#include <Zahlen/ModelPrefab.hpp>
#include <string_view>

namespace ZHLN {
class RenderContext;
class CreativeWorksManager;

namespace GLTF {
ModelPrefab* LoadGLBPrefab(RenderContext& ctx, CreativeWorksManager& cwMgr, std::string_view path);
void         RebuildPrefabGPUResources(RenderContext& ctx, CreativeWorksManager& cwMgr, ModelPrefab* prefab);
} // namespace GLTF
} // namespace ZHLN
