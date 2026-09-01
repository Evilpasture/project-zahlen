// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// extras/glTF/GLTFImporter.hpp
//
// The glTF/GLB reader. This is an extra, not core: reading a model file means a
// container parser (cgltf), an image decoder (stb), a mesh partitioner
// (meshoptimizer) and a JSON reader for the custom members -- none of which the
// engine needs in order to run.
//
// There is no registration step and nothing in src/ includes this header. These
// functions build the plain ZHLN::ModelPrefab that the ECS already describes and
// cache it under HashCreativeWorkPath(path); from then on Core's
// CreativeWorksFactory::LoadModelPrefab(path) -- a lookup in that same cache --
// returns it. The importer depends on Core, Core depends on the prefab cache,
// and no function pointer is installed anywhere.

#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <span>
#include <string_view>

namespace ZHLN {
class Engine;
class RenderContext;
class CreativeWorksManager;

namespace GLTF {
auto LoadGLBPrefab(RenderContext& ctx, CreativeWorksManager& cwMgr, std::string_view path) -> ModelPrefab*;
auto LoadGLBPrefabFromMemory(RenderContext& ctx, CreativeWorksManager& cwMgr, std::span<const uint8_t> bytes, std::string_view virtualPath) -> ModelPrefab*;
void RebuildPrefabGPUResources(RenderContext& ctx, ModelPrefab* prefab);

/// Import straight from a byte buffer and spawn it in one call. Core has no
/// equivalent because reading bytes is the importer's job.
auto InstantiatePrefabFromMemory(
    Engine&                          engine,
    std::span<const uint8_t>         bytes,
    std::string_view                 virtualPath,
    const CreativeWorksFactory::SpawnParams& params,
    Entity*                          outBuffer = nullptr,
    uint32_t                         maxCount  = 0
) -> uint32_t;

/// Re-imports every cached prefab and re-registers its meshes and materials
/// under the asset keys the ECS instances were built against. This is the
/// device-lost half of importing: after a VkDevice is recreated the GPU handles
/// in a ModelPrefab are dead, and recovering them means reading the .glb again.
void RebuildCachedPrefabs(RenderContext& ctx, CreativeWorksManager& cwMgr);

/// Subscribes RebuildCachedPrefabs to the engine's device-lost notification.
///
/// Call it once after creating the Engine, next to the first import. This is a
/// subscription, not a backend: it tells core that imported models hold GPU
/// resources core cannot recreate, and core calls back once it has rebuilt its
/// own. An application that never imports a model needs no call at all, and
/// registering twice would rebuild twice.
void InstallDeviceLostHandler(Engine& engine);
} // namespace GLTF
} // namespace ZHLN
