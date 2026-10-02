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
// cache it under HashAssetPath(path); from then on Core's
// PrefabFactory::LoadModelPrefab(path) -- a lookup in that same cache --
// returns it. The importer depends on Core, Core depends on the prefab cache,
// and no function pointer is installed anywhere.

#include <Zahlen/Core/Optional.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/ModelPrefab.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace ZHLN {
class Engine;
class RenderContext;
class AssetManager;

namespace GLTF {

enum class CapabilityKind : uint8_t { Core, Extension };
enum class CapabilitySupport : uint8_t { Supported, Partial };

struct Capability {
    std::string_view name;
    CapabilityKind kind;
    CapabilitySupport support = CapabilitySupport::Supported;
    std::string_view limitation = {};
};

// What the importer actually consumes (not everything cgltf can parse).
// "Supported" means the data is routed to our renderer, not that its output
// is pixel-identical to a reference glTF viewer. Partial capabilities emit a
// yellow warning. Undeclared extensions are *not* implicitly supported.
static constexpr std::array kCapabilities {
    Capability {"glTF 2.0 nodes, transforms and meshes", CapabilityKind::Core},
    Capability {"TRIANGLES primitives", CapabilityKind::Core},
    Capability {"pbrMetallicRoughness", CapabilityKind::Core},
    Capability {"TEXCOORD_0/1", CapabilityKind::Core},
    Capability {"COLOR_0", CapabilityKind::Core},
    Capability {"JOINTS_0/WEIGHTS_0", CapabilityKind::Core},
    Capability {"POSITION morph targets (first four)", CapabilityKind::Core},
    Capability {"STEP animation", CapabilityKind::Core},
    Capability {"LINEAR animation", CapabilityKind::Core, CapabilitySupport::Partial, "playback eases between keys"},
    Capability {"KHR_mesh_quantization", CapabilityKind::Extension},
    Capability {"KHR_texture_transform", CapabilityKind::Extension},
    Capability {"KHR_materials_unlit", CapabilityKind::Extension},
    Capability {"KHR_materials_emissive_strength", CapabilityKind::Extension},
    Capability {"KHR_materials_clearcoat", CapabilityKind::Extension},
    Capability {"KHR_materials_transmission", CapabilityKind::Extension},
    Capability {"KHR_materials_ior", CapabilityKind::Extension},
    Capability {"KHR_materials_volume", CapabilityKind::Extension, CapabilitySupport::Partial, "attenuation color/distance are ignored"},
    Capability {"KHR_materials_iridescence", CapabilityKind::Extension},
    Capability {"KHR_materials_sheen", CapabilityKind::Extension},
    Capability {"KHR_materials_anisotropy", CapabilityKind::Extension},
};

// Validation is advisory: even an unsupported *required* extension does not
// veto an otherwise readable model. E.g. KHR_materials_pbrSpecularGlossiness
// without a core PBR fallback renders with the default gray material. Parse
// failures and cgltf buffer-loading failures still fail the import.
// The engine's default presentation uses a low exposure and boosts imported
// emission accordingly. A glTF fidelity renderer instead sets this to 1.0f:
// emissiveFactor * KHR_materials_emissive_strength then stays in glTF's
// authored linear units. Emissive texture sampling remains sRGB -> linear in
// either mode. A virtualPath identifies one prefab AND one set of options;
// reusing a cached path with different options is rejected.
struct ImportOptions {
    float emissiveFactorScale = kGLTFEmissiveDisplayScale;
    // Preserve authored detail up to this edge length. The default keeps the
    // engine's existing texture memory budget; fidelity stills can opt into
    // higher resolution without changing other clients' imports.
    uint32_t maxTextureDimension = kGLTFDefaultMaxTextureDimension;
};

auto LoadGLBPrefab(RenderContext& ctx, AssetManager& cwMgr, std::string_view path, ImportOptions options = {}) -> ZHLN::Optional<ModelPrefab&>;

// Like LoadGLBPrefab, but consumes bytes already read by the caller. Split
// .gltf assets store geometry and textures in external .bin/image files whose
// URIs only resolve relative to the .gltf's own location; `bytesPath` is that
// on-disk location (leave empty when the bytes came from a network fetch of a
// self-contained .glb). It is used only to resolve external resources, never
// to re-read the file.
auto LoadGLBPrefabFromMemory(
    RenderContext&           ctx,
    AssetManager&            cwMgr,
    std::span<const uint8_t> bytes,
    std::string_view         virtualPath,
    std::string_view         bytesPath = {},
    ImportOptions            options = {}
) -> ZHLN::Optional<ModelPrefab&>;
void RebuildPrefabGPUResources(RenderContext& ctx, ModelPrefab* prefab);

// Import straight from a byte buffer and spawn it in one call. Core has no
// equivalent because reading bytes is the importer's job.
auto InstantiatePrefabFromMemory(
    Engine&                          engine,
    std::span<const uint8_t>         bytes,
    std::string_view                 virtualPath,
    const PrefabFactory::SpawnParams& params,
    Entity*                          outBuffer = nullptr,
    uint32_t                         maxCount  = 0
) -> uint32_t;

// Re-imports every cached prefab and re-registers its meshes and materials
// under the asset keys the ECS instances were built against. This is the
// device-lost half of importing: after a VkDevice is recreated the GPU handles
// in a ModelPrefab are dead, and recovering them means reading the .glb again.
void RebuildCachedPrefabs(RenderContext& ctx, AssetManager& cwMgr);

// Subscribes RebuildCachedPrefabs to the engine's device-lost notification.
//
// Call it once after creating the Engine, next to the first import. This is a
// subscription, not a backend: it tells core that imported models hold GPU
// resources core cannot recreate, and core calls back once it has rebuilt its
// own. An application that never imports a model needs no call at all, and
// registering twice would rebuild twice.
void InstallDeviceLostHandler(Engine& engine);
} // namespace GLTF
} // namespace ZHLN
