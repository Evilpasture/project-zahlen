// src/gltf/GLTFImporter.hpp
#pragma once

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
} // namespace GLTF
} // namespace ZHLN
