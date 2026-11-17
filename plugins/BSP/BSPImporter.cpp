// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/BSP/BSPImporter.cpp
//
// The engine-side half: file bytes -> native streams (BSPGeometry) -> GPU
// buffers, materials, colliders -> ModelPrefab -> prefab cache. The assembly
// mirrors GLTFImporter.cpp's GetOrCreateCompiledPrimitive/BuildModelPrefab
// split: one compiled part per material group, a single identity root node,
// one node per light so ModelLight's node chain carries its transform.

#include "BSPImporter.hpp"
#include "BSPPreparation.hpp"
#include "BSPMaterial.hpp"
#include "BSPLighting.hpp"
#include "BSPRead.hpp"
#include "SourceVFS.hpp"
#include "StudioModelImporter.hpp"
#include "VMTParser.hpp"
#include "VTFDecoder.hpp"
#include <Zahlen/Engine/Capabilities/AssetLibrary.hpp>
#include <Zahlen/Engine/Capabilities/GraphicsDevice.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Render/Meshlet.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Physics/Physics.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace ZHLN::BSP {
namespace {

auto LookupPrefab(AssetLibrary& assets, uint64_t hash) noexcept -> ZHLN::Optional<ModelPrefab&> {
    return assets.CachedPrefab(hash);
}

auto ReadWholeFile(std::string_view path) -> std::optional<std::vector<std::byte>> {
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!file) {
        return std::nullopt;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return std::nullopt;
    }
    file.seekg(0);
    std::vector<std::byte> out(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
        return std::nullopt;
    }
    return out;
}

// Uploads each packed VRAD page exactly once for this imported prefab. The
// pages remain linear RGBA16F: Source's RGBExp luxels are already linear
// radiance and must not take the sRGB texture path or the old VPL gain.
template <typename Gfx>
auto UploadVradLightmaps(Gfx& ctx, const VradLightmapAtlas& atlas, std::string_view prefabPath) -> std::vector<TextureID> {
    std::vector<TextureID> handles(atlas.pages.size(), TextureID::Invalid);
    for (size_t pageIndex = 0; pageIndex < atlas.pages.size(); ++pageIndex) {
        const VradLightmapPage& page = atlas.pages[pageIndex];
        if (page.width == 0 || page.height == 0 || page.rgba16f.empty()) {
            continue;
        }
        const std::string name = std::string(prefabPath) + "#vrad_lightmap_" + std::to_string(pageIndex);
        const auto uploaded = ctx.CreateTextureHDR(name, std::as_bytes(std::span {page.rgba16f}), Extent2D {page.width, page.height});
        if (!uploaded) {
            LogWarning("BSP Importer: unable to upload native VRAD lightmap page {} for {}: {}", pageIndex, prefabPath, uploaded.error());
            continue;
        }
        handles[pageIndex] = *uploaded;
    }
    return handles;
}

// Fills prefab->parts from imported material streams: one compiled part per
// material group (GPU buffers, material, meshlets, BLAS, colliders), then
// folds the invisible-but-solid tool faces into the first part collider.
// Shared by the world prefab and the interactive submodel prefabs.
template <typename Gfx>
void AssemblePrefabParts(
    Gfx&                   ctx,
    const SourceVFS&       vfs,
    const ImportedMapData& data,
    std::string_view       prefabPath,
    const ImportOptions&   options,
    ModelPrefab&           prefab
) {
    // One .vmt per material group, not one per face: a map with 40 lamp panels
    // still resolves 40 identical files once. `patch` is followed through so
    // the probe sees the material the surface actually renders with.
    std::unordered_map<std::string, std::optional<VMTMaterial>> vmtCache;
    const std::vector<TextureID> lightmapTextures = UploadVradLightmaps(ctx, data.lightmaps, prefabPath);

    for (const MaterialStreams& part: data.parts) {
        if (part.VertexCount() == 0 || part.IndexCount() == 0) {
            continue;
        }

        const BufferID   posVbo     = ctx.template CreateBuffer<BufferUsage::Vertex>(std::span {part.positions});
        const BufferID   frameVbo   = ctx.template CreateBuffer<BufferUsage::Vertex>(std::span {part.tangentFrames});
        const BufferID   surfaceVbo = ctx.template CreateBuffer<BufferUsage::Vertex>(std::span {part.surfaces});
        const BufferID   ibo        = ctx.template CreateBuffer<BufferUsage::Index>(std::span {part.indices});
        const MeshletStreams ml         = ctx.UploadMeshlets(BuildMeshlets(std::span {part.indices}, std::span {part.positions}));

        Mesh subMesh = {
            .posBuffer           = posVbo,
            .tangentFrameBuffer  = frameVbo,
            .surfaceBuffer       = surfaceVbo,
            .skinBuffer          = BufferID::Invalid,
            .indexBuffer         = ibo,
            .vertexCount         = part.VertexCount(),
            .indexCount          = part.IndexCount(),
            .meshletBuffer       = ml.meshlets,
            .meshletVertexBuffer = ml.vertices,
            .meshletTriBuffer    = ml.triangles,
            .meshletCount        = ml.count,
        };

        if (auto res = ctx.BuildMeshBLAS(subMesh); !res) [[unlikely]] {
            if (!res.error().Is(RenderFeatureError::FeatureNotSupported)) {
                LogWarning("BSP Importer: failed to build mesh BLAS: {}", res.error());
            }
        }

        const auto vmtIt = vmtCache.try_emplace(part.materialName, std::nullopt);
        if (vmtIt.second) {
            vmtIt.first->second = LoadMaterialVmt(vfs, part.materialName);
        }
        const VMTMaterial* vmt = vmtIt.first->second.has_value() ? &*vmtIt.first->second : nullptr;

        MaterialDesc materialDesc = BSPMaterial::Resolve([&](std::string_view name, std::span<const std::byte> pixels, Extent2D extent, bool srgb) {
            return ctx.CreateTexture(name, pixels, extent, srgb).value_or(TextureID::Invalid);
        }, vfs, part, vmt);
        // Only opaque/masked lit Source surfaces own a VRAD diffuse chart.
        // UnlitGeneric and blend materials keep their Source shader semantics;
        // forcing a bake through a forward-only path would make it disappear
        // from the deferred additive compositor.
        if (part.lightmapPage < lightmapTextures.size() && lightmapTextures[part.lightmapPage] != TextureID::Invalid && !materialDesc.unlit &&
            !materialDesc.alphaBlend && !materialDesc.additiveBlend && materialDesc.alphaMode != 2u) {
            materialDesc.bakedIrradianceMap   = lightmapTextures[part.lightmapPage];
            materialDesc.bakedIrradianceScale = options.sourceLightmapScale;
        }
        const Material subMaterial = ctx.CreateMaterial(materialDesc).value_or(Material {});

        ModelPart modelPart;
        modelPart.name            = String64(part.materialName.c_str());
        modelPart.mesh            = subMesh;
        modelPart.defaultMaterial = subMaterial;
        modelPart.nodeIndex       = 0;
        modelPart.localTransform  = Mat4x4::Identity();
        modelPart.localMin        = {part.boundsMin.x, part.boundsMin.y, part.boundsMin.z};
        modelPart.localMax        = {part.boundsMax.x, part.boundsMax.y, part.boundsMax.z};

        const std::string assetKey = std::string(prefabPath) + "#part_" + std::to_string(prefab.parts.size());
        modelPart.meshAsset        = HashAssetID(assetKey);
        modelPart.materialAsset    = HashAssetID(assetKey + "_mat");

        const float extentX      = std::max(std::abs(part.boundsMin.x), std::abs(part.boundsMax.x));
        const float extentY      = std::max(std::abs(part.boundsMin.y), std::abs(part.boundsMax.y));
        const float extentZ      = std::max(std::abs(part.boundsMin.z), std::abs(part.boundsMax.z));
        modelPart.boundingRadius = std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ);

        // Skybox scenery composites behind the world (DrawFlags::Background)
        // and, like Source's two-world skybox, has no solid presence in the
        // play space -- its colliders would be invisible walls at the placed
        // miniscape positions.
        if (part.skyboxLayer) {
            modelPart.drawFlags = DrawFlags::Background;
        }

        if (options.buildColliders && !part.skyboxLayer) {
            modelPart.meshCollider = MakePrefabMeshCollider(part.positions, part.indices);

            const FVec3 localCenter{
                (part.boundsMin.x + part.boundsMax.x) * 0.5f, (part.boundsMin.y + part.boundsMax.y) * 0.5f, (part.boundsMin.z + part.boundsMax.z) * 0.5f
            };
            const FVec3 halfExtents{
                std::max((part.boundsMax.x - part.boundsMin.x) * 0.5f, 0.01f), std::max((part.boundsMax.y - part.boundsMin.y) * 0.5f, 0.01f),
                std::max((part.boundsMax.z - part.boundsMin.z) * 0.5f, 0.01f)
            };
            modelPart.boxCollider = MakePrefabBoxCollider(halfExtents, localCenter);

            if (!modelPart.meshCollider) {
                modelPart.meshCollider = modelPart.boxCollider;
            }
        }

        prefab.parts.push_back(std::move(modelPart));
    }

    // Invisible-but-solid tool faces (nodraw undersides, sky brushes, clip and
    // invisible walls): RenderContext::Draw would only log-and-skip an empty
    // render mesh, so instead of a dummy "physics-only" part, fold the stream
    // into the collider of the first part that already has one. A static
    // compound of {part mesh, collision-only mesh} keeps both hulls intact
    // for mesh queries and CharacterVirtual alike.
    if (options.buildColliders && data.collisionOnly.IndexCount() > 0) {
        if (auto collisionShape = MakePrefabMeshCollider(data.collisionOnly.positions, data.collisionOnly.indices)) {
            bool folded = false;
            for (ModelPart& part: prefab.parts) {
                if (!part.meshCollider) continue;
                Physics::Shape<Physics::Compound> compound;
                AppendPrefabCollider(compound, *part.meshCollider);
                AppendPrefabCollider(compound, *collisionShape);
                part.meshCollider = Physics::AnyShape(std::move(compound));
                folded = true;
                break;
            }
            if (!folded) {
                LogWarning("BSP Importer: {} collision-only tool faces could not be folded into any part collider; invisible walls are lost",
                           data.collisionOnly.indices.size() / 3);
            }
        }
    }
}

template <typename Gfx, typename Assets>
auto BuildModelPrefab(Gfx& ctx, Assets& assetMgr, const BSPMap& map, std::string_view virtualPath, const ImportOptions& options)
    -> ZHLN::Optional<ModelPrefab&> {
    // Interactive brush submodels (doors, buttons) get their own prefabs
    // below; excluding them here keeps their faces out of the static world
    // mesh and world collider (a door baked into the world keeps blocking
    // the doorway after it opens).
    const std::vector<InteractiveSubmodel> interactiveSubmodels = CollectInteractiveSubmodels(map, options);

    ImportOptions worldOptions = options;
    // Lights come from the scene description (DescribeScene -> SceneLights),
    // not from the prefab: shipping them as ModelLights too made
    // InstantiatePrefab's KHR_lights_punctual pass spawn every light a second
    // time -- and BSP-built ModelLights carry no range, which the
    // instantiator expands to 1000 m. Double intensity-200 lights with
    // kilometre range washed the whole map out.
    worldOptions.gatherLights = false;
    worldOptions.skipModels.reserve(interactiveSubmodels.size());
    for (const InteractiveSubmodel& sub: interactiveSubmodels) {
        worldOptions.skipModels.push_back(sub.modelIndex);
    }

    ImportedMapData imported = ImportMapGeometry(map, worldOptions);
    if (imported.parts.empty()) {
        LogWarning("BSP Importer: no importable surfaces in {}", virtualPath);
        return std::nullopt;
    }

    // Face-pipeline accounting: when a wall seems to be missing in-game, this
    // line proves whether it was dropped here (and by which stage) or is
    // present and the problem is downstream of this import.
    Log("BSP Importer: {} -> {} total faces -> {} render faces in {} parts ({} triangles); dropped: {} no-winding, {} degenerate, {} no-texinfo, {} "
        "tool-helper; {} collision-only; {} disp fallbacks; {} single-sheet walls forced double-sided",
        virtualPath,
        imported.faceCount + imported.stats.helperFacesDropped + imported.stats.collisionOnlyFaces + imported.stats.droppedNoWinding +
            imported.stats.droppedDegenerate + imported.stats.droppedNoProjection,
        imported.faceCount, imported.parts.size(), imported.triangleCount, imported.stats.droppedNoWinding, imported.stats.droppedDegenerate,
        imported.stats.droppedNoProjection, imported.stats.helperFacesDropped, imported.stats.collisionOnlyFaces, imported.stats.displacementFallbacks,
        imported.stats.singleSidedSheets);
    if (imported.stats.skyboxFacesDropped > 0) {
        Log("BSP Importer: {} -> {} 3D-skybox face(s) dropped (ImportOptions::importSkybox is off)", virtualPath, imported.stats.skyboxFacesDropped);
    }
    if (worldOptions.useVradLightmaps) {
        uint64_t nativeLightmapBytes = 0;
        uint32_t largestPageWidth    = 0;
        uint32_t largestPageHeight   = 0;
        for (const VradLightmapPage& page: imported.lightmaps.pages) {
            nativeLightmapBytes += static_cast<uint64_t>(page.width) * page.height * sizeof(uint16_t) * 4u;
            if (static_cast<uint64_t>(page.width) * page.height > static_cast<uint64_t>(largestPageWidth) * largestPageHeight) {
                largestPageWidth  = page.width;
                largestPageHeight = page.height;
            }
        }
        Log("BSP Importer: {} VRAD {} -> {} face chart(s), {} linear RGBA16F page(s), largest {}x{}, {:.1f} MiB total, {} decoded luxels, {} rejected chart(s); VPL reconstruction is {}",
            virtualPath, imported.lightmaps.hdrSource ? "HDR" : "LDR", imported.stats.lightmapFaces, imported.stats.lightmapPages,
            largestPageWidth, largestPageHeight, static_cast<double>(nativeLightmapBytes) / (1024.0 * 1024.0), imported.lightmaps.luxelCount,
            imported.stats.lightmapRejectedFaces, worldOptions.ingestVradBake ? "suppressed by native lightmaps" : "disabled");
    }

    auto prefab                 = std::make_unique<ModelPrefab>();
    prefab->virtualPath         = String256(virtualPath);
    prefab->emissiveFactorScale = 1.0f; // Keep Source emission at its authored linear scale.

    SourceVFS vfs;
    if (!options.assetRoot.empty()) {
        vfs.AddSearchPath(options.assetRoot);
    }
    const std::filesystem::path bspPath(virtualPath);
    if (bspPath.has_parent_path()) {
        vfs.AddSearchPath(bspPath.parent_path().string());
        if (bspPath.parent_path().has_parent_path()) {
            vfs.AddSearchPath(bspPath.parent_path().parent_path().string());
        }
    }

    // Stage two of the emissive heuristic: re-probe the name-gated candidates
    // against the materials they actually render with (see
    // RefineEmissiveSurfaces). The count of candidates the probe rejected is
    // reported in the lighting line below.
    uint32_t emissiveProbeDropped = 0;
    if (worldOptions.gatherEmissiveSurfaces) {
        const size_t candidateCount = imported.emissive.size();
        imported.emissive = RefineEmissiveSurfaces(vfs, imported.emissive, imported);
        emissiveProbeDropped = static_cast<uint32_t>(candidateCount - imported.emissive.size());
    }

    // Lighting accounting: this line is the answer to "why is my scene dark?".
    // Every light the entity lump offered leaves through exactly one counter.
    const LightingDiagnostics& lighting = imported.lighting;
    Log("BSP Importer: {} lighting -> {} entity lights emitted ({} offered: {} beam cones without a dynamic light, {} disabled/black); "
        "ambient {}{}; {} emissive faces -> {} area-light proxies over {:.3f} m2 ({} dropped by the probe, {} by the cap); "
        "{} fixtures damped{}",
        virtualPath, lighting.entityLightsEmitted, lighting.entityLightsOffered, lighting.droppedBeamCone, lighting.droppedDisabled,
        lighting.ambientAuthored ? "authored" : "default",
        (imported.ambient.color.x + imported.ambient.color.y + imported.ambient.color.z > 0.0f) ? "" : " (zero)", lighting.emissiveFaces,
        lighting.emissiveSurfaces, lighting.emissiveAreaSquareMetres, emissiveProbeDropped, lighting.emissiveDroppedCap, lighting.emissiveFixtureDamped,
        lighting.floorEnforced ? " [floor enforced]" : "");

    // One identity root; parts hang off it directly (part.localTransform does
    // the placement), lights get their own translated nodes because ModelLight
    // resolves position through its node chain.
    prefab->nodes.push_back(ModelNode {.name = String64("bsp_root"), .parentIndex = -1, .localTransform = Mat4x4::Identity(), .hasMesh = false});

    AssemblePrefabParts(ctx, vfs, imported, virtualPath, options, *prefab);

    for (const BspPointLight& light: imported.lights) {
        ModelNode node;
        node.name           = String64(light.name.c_str());
        node.parentIndex    = 0;
        node.localTransform = CreateTranslation(FVec3 {light.position.x, light.position.y, light.position.z});
        node.hasMesh        = false;
        prefab->nodes.push_back(node);

        ModelLight modelLight;
        modelLight.name      = String64(light.name.c_str());
        modelLight.nodeIndex = static_cast<int32_t>(prefab->nodes.size()) - 1;
        modelLight.color     = light.color;
        modelLight.intensity = light.intensity;
        modelLight.range     = light.range; // 0 expands to 1000 m in PrefabFactory -- bake VPLs must keep their 3-24 m reach or every cluster holds every VPL and the per-cluster cap drops them at random.
        modelLight.type      = light.type;
        if (light.type == LightType::Spot) {
            modelLight.innerConeAngle = light.innerConeRadians;
            modelLight.outerConeAngle = light.outerConeRadians;
        }
        prefab->lights.push_back(modelLight);
    }

    assetMgr.CachePrefab(HashAssetPath(virtualPath), std::move(prefab));

    // One prefab per interactive submodel, cached under "<map>#model_<N>" --
    // the source BSPScene stamps on the door/button entity description.
    // Normalize world-space interactive submodels to their pivot; leave the
    // usual entity-local VBSP representation untouched.
    //
    // The rotating path has always done this; the sliding path did not, on the
    // theory that "the entity transform is just the movement delta". That left
    // a sliding door's transform at the identity SceneEntity default -- the map
    // origin -- and three things read that transform as the door's real
    // position:
    //
    //   * BSPInteraction reads trans->position as the door's closedPosition, so
    //     the movement system interpolated origin -> origin + movedir*travel
    //     instead of in place, and the E-to-use proximity check (2.5 m from
    //     trans->position) never fired anywhere near the actual door.
    //   * Scene::Instantiate creates the kinematic root collider at
    //     params.position, so the door's collider sat at the map origin and the
    //     player walked straight through the door where it really was.
    //
    // Re-basing costs nothing visually: the rendered vertex is
    // pivot + (worldVertex - pivot) == worldVertex, exactly as before.
    for (const InteractiveSubmodel& sub: interactiveSubmodels) {
        ImportOptions subOptions = options;
        subOptions.onlyModel        = sub.modelIndex;
        subOptions.gatherLights     = false;
        // Brush entities move at runtime; do not duplicate the world atlas or
        // treat an incidental face lightofs as static world irradiance.
        subOptions.useVradLightmaps = false;
        subOptions.ingestVradBake   = false;

        ImportedMapData subImported = ImportMapGeometry(map, subOptions);
        if (subImported.parts.empty()) {
            continue; // Pure tool faces (clip/hint brushes): nothing to draw.
        }
        // VBSP normally stores brush-entity vertices in entity-local space
        // (gm_murder_drones' door models *123..*125 are centred on zero while
        // their origins are around (-504, 1592, 184)). Older/other branches can
        // leave them in world space. Re-base only the latter: unconditionally
        // subtracting the entity pivot sends already-local render geometry back
        // to the map origin, while the fallback collider remains at the door.
        const FVec3 geometryCenter{
            (subImported.boundsMin.x + subImported.boundsMax.x) * 0.5f,
            (subImported.boundsMin.y + subImported.boundsMax.y) * 0.5f,
            (subImported.boundsMin.z + subImported.boundsMax.z) * 0.5f
        };
        if ((geometryCenter - sub.pivot).LengthSquared() < geometryCenter.LengthSquared()) {
            ShiftImportedGeometry(subImported, -sub.pivot);
        }

        const std::string subPath      = std::string(virtualPath) + "#model_" + std::to_string(sub.modelIndex);
        auto              subPrefab    = std::make_unique<ModelPrefab>();
        subPrefab->virtualPath         = String256(subPath);
        subPrefab->emissiveFactorScale = 1.0f;
        subPrefab->nodes.push_back(
            ModelNode {.name = String64("bsp_submodel_root"), .parentIndex = -1, .localTransform = Mat4x4::Identity(), .hasMesh = false}
        );
        AssemblePrefabParts(ctx, vfs, subImported, subPath, options, *subPrefab);
        assetMgr.CachePrefab(HashAssetPath(subPath), std::move(subPrefab));
        Log("BSP Importer: {} -> interactive submodel prefab ({}, {} parts)", subPath, sub.rotating ? "rotating" : "sliding", subImported.parts.size());
    }

    return LookupPrefab(assetMgr, HashAssetPath(virtualPath));
}

} // namespace

// ---------------------------------------------------------------------------
// Shared .vmt lighting rules live in VMTLighting.cpp.
// ---------------------------------------------------------------------------

auto LoadBSPPrefab(Engine& engine, std::string_view path, const ImportOptions& options) -> ZHLN::Optional<ModelPrefab&> {
    const uint64_t hash   = HashAssetPath(path);
    auto           assets = engine.Assets();
    const auto     cached = assets.CachedPrefab(hash);
    if (cached) {
        return cached;
    }

    const auto bytes = ReadWholeFile(path);
    if (!bytes) {
        LogWarning("BSP Importer: failed to read {}", path);
        return std::nullopt;
    }
    return LoadBSPPrefabFromMemory(engine, *bytes, path, options);
}

auto LoadBSPPrefabFromMemory(
    Engine&                    engine,
    std::span<const std::byte> bytes,
    std::string_view           virtualPath,
    const ImportOptions&       options
) -> ZHLN::Optional<ModelPrefab&> {
    const uint64_t hash   = HashAssetPath(virtualPath);
    auto           assets = engine.Assets();
    const auto     cached = assets.CachedPrefab(hash);
    if (cached) {
        return cached;
    }

    const auto parsed = ParseBsp(bytes);
    if (!parsed) {
        LogWarning("BSP Importer: {}: {}", virtualPath, parsed.error());
        return std::nullopt;
    }
    auto gfx = engine.Graphics();
    return BuildModelPrefab(gfx, assets, *parsed, virtualPath, options);
}

auto PreloadStaticProps(Engine& engine, const BSPMap& map, const ImportOptions& options) -> size_t {
    SourceVFS vfs;
    if (!options.assetRoot.empty()) {
        vfs.AddSearchPath(options.assetRoot);
    }

    size_t loaded = 0;
    for (const auto& sp: map.staticProps) {
        if (!sp.modelName.empty()) {
            if (const auto model = LoadStudioModelPrefab(engine, vfs, sp.modelName, options); model.has_value()) {
                ++loaded;
            }
        }
    }

    for (const auto& ent: map.entities) {
        const std::string_view model = ent.Find("model");
        if (model.ends_with(".mdl")) {
            if (const auto m = LoadStudioModelPrefab(engine, vfs, model, options); m.has_value()) {
                ++loaded;
            }
        }
    }

    return loaded;
}

} // namespace ZHLN::BSP
