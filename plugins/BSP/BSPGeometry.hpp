// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// plugins/BSP/BSPGeometry.hpp
//
// The import step: BSP lumps in, Zahlen-native vertex streams out. The
// streams are exactly the cooked-mesh payload layout the renderer already
// consumes -- VertexPosition / VertexTangentFrame / VertexSurface / indices
// (see <Zahlen/Vertex.hpp>) -- grouped per material the way the glTF importer
// groups primitives. Nothing here touches the GPU, the ECS, or an instantiator;
// the importer adapter uploads these streams and Scene::Instantiate /
// PrefabFactory::InstantiatePrefab take it from there.
//
// Engine-free by design: this layer is buildable without the full renderer tree.

#include "BSPRead.hpp"
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Vertex.hpp>
#include <array>
#include <Zahlen/Render/Handles.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace ZHLN::BSP {

// Source `_light` brightness (the 4th component, typically 150-300) is a
// VRAD radiosity unit, not a renderer intensity. Passed through raw, every
// point/spot light lands ~30x too hot (intensity 200 against an engine
// convention of single digits for punctual lights and 3-28 for suns) and
// blows the scene out to white. Dividing by ~30 calibrates a stock
// brightness-200 room light to engine intensity ~6.7, matching the physical
// inches->metres rescale (unitScale^2) plus VRAD's luxel gain.
inline constexpr float kSourceBrightnessToEngine = 1.0f / 30.0f;

// Source is Z-up and measures in inches; the engine is Y-up (glTF convention)
// and measures in metres. The default converts; unitScale lets a caller keep
// inches (1.0f) or feed a differently scaled world.
struct ImportOptions {
    bool        convertCoordinates   = true;
    float       unitScale            = 0.0254f; // Inches to meters
    bool        includeDisplacements = true;
    bool        gatherLights         = true;
    // When true, DescribeScene skips light entities that are VRAD baking
    // markers: unnamed and style 0. Those lights are already in the baked
    // lightmap; respawning them in real time double-lights the map and
    // floods the shading clusters. Named/styled lights stay: they are
    // switchable or animated at runtime.
    bool        onlyInteractiveLights = false;
    bool        buildColliders       = true;
    std::string assetRoot;
    std::string lightmapAtlasPath;

    // Restrict the import to one submodel (index into BSPMap::models); -1
    // imports every model not listed in skipModels. Interactive brush
    // entities (func_door, func_button, ...) get their own prefab per
    // submodel so they can animate instead of being baked into the world.
    int32_t onlyModel = -1;

    // Submodel indices to exclude entirely: their faces are imported
    // separately, so they must not also land in the static world mesh (and
    // its collider -- a door baked into the world keeps blocking after it
    // opens).
    std::vector<int32_t> skipModels;

    // Baked-lightmap atlas handle to route into studio-model materials so
    // static props sample the prop-ambient cell (rows 0..3 of the atlas) via
    // their (0,0) lightmap UVs. Invalid keeps props unlit by the bake.
    TextureHandle lightmapTexture = TextureHandle::Invalid;
};

// Rows 0..3 of the lightmap atlas are reserved for the prop-ambient cell:
// the map's mean baked radiosity, which studio props sample through their
// (0,0) lightmap UVs (see BuildLightmapAtlasTexture / ResolveStudioMaterial).
inline constexpr int32_t kPropAmbientRows = 4;

// Padding reserved on each side of every packed lightmap face cell. One
// luxel would suffice for the bilinear sample at a face border, but uv1 is a
// PackedHalf2: near u = 1 the half float quantises to ~1/2048 of UV space,
// i.e. up to one atlas texel at 4096, so an edge tap can land ~1.5 texels
// outside the cell once bilinear centre offset is added. Two luxels of
// own-edge colour absorb that error instead of letting it read a neighbour's
// light (and three luxels per side no longer fits this map's 38k faces).
inline constexpr int32_t kLightmapGutter = 2;

// One material group: the concatenated, de-indexed-then-reindexed streams of
// every face that uses one texdata name.
struct MaterialStreams {
    std::string materialName;

    std::vector<VertexPosition>     positions;
    std::vector<VertexTangentFrame> tangentFrames;
    std::vector<VertexSurface>      surfaces;
    std::vector<uint32_t>           indices;

    JPH::Float3 boundsMin {0.0f, 0.0f, 0.0f};
    JPH::Float3 boundsMax {0.0f, 0.0f, 0.0f};

    // Set when at least one face in this group has no opposite-facing twin:
    // a single-sheet wall whose only geometry faces AWAY from the reachable
    // side (Source still draws such faces -- vbsp's leaffaces list them in the
    // leaf on their back side). The whole material group renders double-sided
    // so those walls stop being invisible from the room side.
    bool forceDoubleSided = false;

    [[nodiscard]] auto VertexCount() const noexcept -> uint32_t {
        return static_cast<uint32_t>(positions.size());
    }
    [[nodiscard]] auto IndexCount() const noexcept -> uint32_t {
        return static_cast<uint32_t>(indices.size());
    }
};

// A light recovered from the entity lump.
struct BspPointLight {
    std::string name;
    LightType   type = LightType::Point; // Direct reuse of engine enum

    JPH::Vec3 position {0.0f, 0.0f, 0.0f};
    JPH::Vec3 direction {0.0f, -1.0f, 0.0f};
    JPH::Vec3 color {1.0f, 1.0f, 1.0f};
    float     intensity        = 1.0f;
    float     innerConeRadians = 0.0f;
    float     outerConeRadians = 0.78539816339f; // Default 45 deg
};

// Bookkeeping for the face pipeline. Every face in every model leaves through
// exactly one of these counters, so an importer log line can prove where
// geometry went when a wall seems to be "missing": the counts are exhaustive
// over the model face ranges.
struct ImportStats {
    uint32_t helperFacesDropped = 0;      // tools/ & compile-flag helpers dropped by design (fog, trigger, skip...)
    uint32_t collisionOnlyFaces = 0;      // invisible-but-solid faces kept in the physics hull
    uint32_t droppedNoWinding = 0;        // surfedge/edge/vertex indices out of range
    uint32_t droppedDegenerate = 0;       // fewer than 3 unique corners
    uint32_t droppedNoProjection = 0;     // texinfo/texdata index problems (includes tree-only kTexInfoNode faces)
    uint32_t displacementFallbacks = 0;   // invalid disp records rendered as flat quads instead of being lost
    uint32_t singleSidedSheets = 0;       // faces with no opposite-facing twin; forced double-sided + backside collision
};

// texinfo flag: face compiled with bumpmapping, so VRAD stored 4 ColorRGBExp32
// luxels per lightmap sample instead of 1. The importer needs it to stride
// through LUMP_LIGHTING correctly.
inline constexpr int32_t kSurfBumpLight = 0x800;

struct ImportedMapData {
    std::vector<MaterialStreams> parts;
    std::vector<BspPointLight>   lights;

    // Invisible-but-solid tool faces (toolsnodraw, toolsskybox, toolsinvisible,
    // toolsclip...): Source strips these from the visual mesh but keeps them in
    // the physics hull, so they arrive here under the same split -- positions
    // and indices only, never rendered, folded into the world collider by the
    // importer adapter. Fog/trigger/hint/skip classes (invisible AND non-solid)
    // do not appear anywhere: they are compile-time/volumetric helpers, not
    // geometry. Empty unless options.buildColliders is set.
    MaterialStreams collisionOnly;

    JPH::Float3 boundsMin {0.0f, 0.0f, 0.0f};
    JPH::Float3 boundsMax {0.0f, 0.0f, 0.0f};
    uint32_t    faceCount         = 0;
    uint32_t    displacementCount = 0;
    uint32_t    triangleCount     = 0;
    ImportStats stats;

    // Lightmap atlas page extent, in luxels: max(mins + size) over all faces.
    // Source lightmaps live in one huge page-local coordinate space, and the
    // baked atlas images community tools dump next to the map use that same
    // origin -- the absorption in EmitVertex maps global luxel UVs straight
    // into atlas space. Zero when no face carries a lightmap.
    float lightmapPageWidth  = 0.0f;
    float lightmapPageHeight = 0.0f;

    // Per-face atlas cell origin in luxels, indexed by face index, from the
    // same shelf pack that produced the TEXCOORD_1 UVs above. {-1, -1} marks
    // a face with no lightmap. The importer walks this alongside
    // BSPMap::lighting to unpack VRAD radiosity into an atlas texture.
    std::vector<std::array<int32_t, 2>> lightmapOrigins;
};

// Position transform for the options in force (Source Z-up inches -> engine
// Y-up metres by default). Exposed so scene importing converts identically.
[[nodiscard]] auto ConvertPosition(const ImportOptions& options, JPH::Vec3 in) noexcept -> JPH::Vec3;
[[nodiscard]] auto ConvertDirection(const ImportOptions& options, JPH::Vec3 in) noexcept -> JPH::Vec3;

// One interactive brush submodel (func_door / func_button family) recovered
// from the entity lump: its model index, whether the entity rotates about a
// hinge, and the engine-space pivot that rotation happens around (the
// authored origin keyvalue when present, otherwise the model bounds centre).
// The importer and the scene description both consume this list so they
// agree on which submodels leave the static world and where their pivots are.
struct InteractiveSubmodel {
    int32_t   modelIndex = -1;
    bool      rotating   = false;
    JPH::Vec3 pivot {0.0f, 0.0f, 0.0f};
};

[[nodiscard]] auto CollectInteractiveSubmodels(const BSPMap& map, const ImportOptions& options) -> std::vector<InteractiveSubmodel>;

// Faces + displacements -> per-material native streams; entities -> lights.
// Faces whose texinfo is missing or node-only are skipped (they are tree
// partitions, not surfaces). Degenerate polygons (fewer than 3 unique corners)
// are skipped rather than producing zero-area triangles.
[[nodiscard]] auto ImportMapGeometry(const BSPMap& map, const ImportOptions& options = {}) -> ImportedMapData;

} // namespace ZHLN::BSP
