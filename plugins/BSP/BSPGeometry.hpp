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
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ZHLN::BSP {

// Display calibration for the whole ingested lighting field. VRAD luma, entity
// `_light` brightness and the bake fit all share one radiance scale; measured
// against the Source reference (same scene, same bands: gym floor 8.5x, wall
// band 5.5x in display-linear), this engine's tonemap chain lands that scale
// ~7x under the Source display. One gain, applied at the single brightness
// chokepoint below, lifts lights, bake VPLs and (via the same constant family
// in BSPScene) the ambient together so the Source balance is preserved.
inline constexpr float kIngestLevelGain = 7.0f;

// Source `_light` brightness (the 4th component, typically 150-300) is in
// Source lighting units, not renderer intensity. Dividing by ~30 calibrates a
// stock brightness-200 room light to engine intensity ~6.7, matching the
// physical inches->metres rescale (unitScale^2) and Source's authored
// brightness scale; kIngestLevelGain then lifts the pair to display parity.
inline constexpr float kSourceBrightnessToEngine = kIngestLevelGain / 30.0f;

// Source is Z-up and measures in inches; the engine is Y-up (glTF convention)
// and measures in metres. The default converts; unitScale lets a caller keep
// inches (1.0f) or feed a differently scaled world.
struct ImportOptions {
    bool        convertCoordinates   = true;
    float       unitScale            = 0.0254f; // Inches to meters
    bool        includeDisplacements = true;
    bool        gatherLights         = true;
    bool        buildColliders       = true;
    std::string assetRoot;

    // Self-illuminated surfaces (lamp panels, screens, signs) become area-light
    // proxies for the PBR / RTGI / DDGI pipeline. Off means the map contributes
    // nothing but its entity lights -- useful when the scene already has its own
    // emissive budget and the panels would double-count it.
    bool gatherEmissiveSurfaces = true;
    // Mirrors BSPLighting.hpp's kEmissiveMaxLights; declared here because
    // ImportOptions cannot include BSPLighting.hpp.
    uint32_t maxEmissiveLights = 192;

    // The light_environment ambient term drives the scene's sky gradient and
    // the SH irradiance the DDGI probes start from. Off keeps the engine's own
    // environment defaults.
    bool deriveAmbient = true;

    // Author the map's ambient occlusion into the scene description. The values
    // are the scene's, in the scene's units (BSPScene.cpp), not import inputs.
    bool ambientOcclusion = true;

    // Atlas faces the map's own point and spot lights may spend on shadow tiles.
    uint32_t punctualShadowFaceBudget = 24;

    // Guarantee a non-black scene: lift a black ambient to a visible floor and
    // synthesise a fill light when the map offers no light at all. See
    // BSPLighting.hpp's EnforceLightingFloor.
    bool enforceLightingFloor = true;

    // Ingest VRAD's compiled luxel radiance (LUMP_LIGHTING) as a radiance
    // field fitted into the engine's carriers -- an AmbientTerm plus a budget
    // of virtual point lights -- instead of a lightmap multiply that would
    // fight the PBR and RT pipeline. See BSPBakeField.hpp. Off restores
    // pre-bake import exactly (the lump is still parsed; it is simply unused).
    bool ingestVradBake = true;
    // VPLs kept from the bake's clusters, ranked by cluster energy with a
    // spatial quota (BSPBakeField.hpp).
    uint32_t bakeVplBudget = 384;
    // Damping of bake VPLs against the map's real lights and the DDGI pass.
    float bakeVplWeight = 0.5f;

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
};

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

    // Source's 3D-skybox scenery. Faces land here placed (SkyboxTransform),
    // and the stream is emitted as its own part per material so the instance
    // can carry DrawFlags::Background -- the layer that composites behind the
    // main world instead of racing it in the depth buffer.
    bool skyboxLayer = false;

    [[nodiscard]] auto VertexCount() const noexcept -> uint32_t {
        return static_cast<uint32_t>(positions.size());
    }
    [[nodiscard]] auto IndexCount() const noexcept -> uint32_t {
        return static_cast<uint32_t>(indices.size());
    }
};

// A light recovered from the entity lump.
//
// `range` and `radius` are the engine's own windowed inverse-square terms
// (LightDesc: 0 range = infinite / default falloff), derived by
// BSPLighting.hpp from Source's attenuation coefficients. `castShadows` /
// `startsOn` come from the entity's spawnflags and keyvalues so a mapper's
// "no shadow" or "start off" intent survives the import. `className` is kept
// for diagnostics and for deterministic naming in the scene description;
// `fromEmissive` separates the lights this pass derived from self-illuminated
// geometry from the ones an entity authored, so the audit's counters stay
// exhaustive when both end up in the same vector.
struct BspPointLight {
    std::string name;
    LightType   type = LightType::Point; // Direct reuse of engine enum

    JPH::Vec3 position {0.0f, 0.0f, 0.0f};
    JPH::Vec3 direction {0.0f, -1.0f, 0.0f};
    JPH::Vec3 color {1.0f, 1.0f, 1.0f};
    float     intensity        = 1.0f;
    float     innerConeRadians = 0.0f;
    float     outerConeRadians = 0.78539816339f; // Default 45 deg

    float  range       = 0.0f; // Metres; 0 = engine default falloff
    float  radius      = 0.0f; // Metres; 0 = point
    bool   castShadows = true;
    bool   startsOn    = true;
    int32_t spawnFlags = 0;
    // True for a light this pass derived from geometry rather than read from an
    // entity (an emissive-surface proxy, or the guarantee's fill light). The
    // audit uses it to keep "how many entity lights survived" answerable after
    // the proxies are appended to the same vector.
    bool        fromEmissive = false;
    std::string className;
};

// The light_environment ambient term: Source's `_ambient` colour and
// brightness, plus the HDR scale. This is what the engine's sky gradient and
// the SH irradiance RTGI/DDGI probes start from, so a map with no sun still
// has an irradiance floor. See BSPLighting.hpp for the reading rules.
struct AmbientTerm {
    JPH::Vec3 color {0.0f, 0.0f, 0.0f};
    float     intensity = 0.0f;
    bool      authored  = false; // a light_environment was found
};

// One self-illuminated surface after accumulation: the area-weighted centroid
// and normal of every face that uses the material, in engine space. These are
// the area-light proxies the PBR path emits and RTGI/DDGI trace. See
// BSPLighting.hpp for the material heuristics that decide a face belongs here.
struct EmissiveSurface {
    std::string materialName;
    JPH::Vec3   position {0.0f, 0.0f, 0.0f};
    JPH::Vec3   normal {0.0f, 1.0f, 0.0f};
    JPH::Vec3   color {1.0f, 1.0f, 1.0f};
    float       areaSquareMetres = 0.0f;
    float       areaSquareInches = 0.0f;
    float       strength         = 0.0f;   // authored self-illumination, 0..1
    float       fixtureDamping   = 1.0f;   // <1 when an entity light already covers this fixture
    float       reflectivityLuma = -1.0f;  // mean texdata reflectivity luma of the faces, -1 when unmeasured
    bool        noLightFlag      = false;  // vbsp set SURF_NOLIGHT on these texinfos
    uint32_t    faceCount        = 0;
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
    uint32_t emissiveFaces = 0;           // faces on a self-illuminated material (lamp panels, screens, signs)
    uint32_t emissiveSurfaces = 0;        // area-light proxies produced from them (after clustering and the cap)
    uint32_t emissiveDroppedCap = 0;      // proxies dropped because maxEmissiveLights was reached
    uint32_t bakeLuxels = 0;              // VRAD bake samples decoded by BSPBakeField
    uint32_t bakeVpls = 0;                // virtual point lights fitted from the bake's bright population
};

// Exhaustive bookkeeping for the lighting pass, mirroring ImportStats' role for
// the face pipeline: every light the entity lump offered leaves through exactly
// one counter, so a log line proves where the light went. See BSPLighting.hpp
// for the rules that produce each number.
struct LightingDiagnostics {
    uint32_t entityLightsOffered      = 0;
    uint32_t entityLightsEmitted      = 0;
    uint32_t droppedBeamCone          = 0; // point_spotlight with "No Dynamic Light"
    uint32_t droppedDisabled          = 0; // spawnflag "start disabled" / zero brightness
    uint32_t droppedBlack             = 0; // zero colour or zero brightness
    uint32_t droppedNoPosition        = 0;
    uint32_t ambientAuthored          = 0;
    uint32_t emissiveFaces            = 0;
    uint32_t emissiveSurfaces         = 0;
    uint32_t emissiveDroppedCap       = 0;
    uint32_t emissiveFixtureDamped    = 0;
    float    emissiveAreaSquareMetres = 0.0f;
    float    totalEntityIntensity     = 0.0f;
    float    totalEmissiveIntensity   = 0.0f;
    bool     floorEnforced            = false;
};

struct ImportedMapData {
    std::vector<MaterialStreams> parts;
    std::vector<BspPointLight>   lights;

    // Self-illuminated surfaces, clustered per material and grid cell, in
    // engine space. These are the area-light proxies the PBR path emits and
    // RTGI/DDGI trace; empty unless options.gatherEmissiveSurfaces is set and
    // the map actually has emissive geometry.
    std::vector<EmissiveSurface> emissive;

    // The light_environment ambient term (colour + brightness), used to seed
    // the scene's sky gradient and the SH irradiance the probes start from.
    // Zero when options.deriveAmbient is off or the map has no
    // light_environment.
    AmbientTerm ambient;

    // Where the light came from, and what the guarantee had to do.
    LightingDiagnostics lighting;

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
};

// Position transform for the options in force (Source Z-up inches -> engine
// Y-up metres by default). Exposed so scene importing converts identically.
[[nodiscard]] auto ConvertPosition(const ImportOptions& options, JPH::Vec3 in) noexcept -> JPH::Vec3;
[[nodiscard]] auto ConvertDirection(const ImportOptions& options, JPH::Vec3 in) noexcept -> JPH::Vec3;

// Rec.709 luma of the texdata reflectivity vbsp baked for a face's material.
// Source's `reflectivity` is the material's diffuse albedo in linear 0-1 and is
// the only per-material brightness signal that reaches the BSP without a .vmt,
// which is what makes it usable as an emissive gate. Exposed so the importer
// adapter can run the same probe the geometry pass does.
[[nodiscard]] auto TexDataReflectivityLuma(const BSPMap& map, const DFace& face) noexcept -> float;

// Source's 3D skybox: background scenery compiled at 1/scale size around the
// `sky_camera` entity, rendered by the engine at full size around the player's
// eye. Importing it at its raw compile coordinates leaves the whole cluster
// floating above the map (for gm_murder_drones: ~7k faces at source z 2500+,
// 78 m over the play space) and parks its fixture lights in the void. The
// transform maps skybox points into world space the way the engine draws the
// sky: world = anchor + R * (p - skyOrigin) * scale, anchored at the player
// start so the scenery surrounds the play boundaries. All source-space.
struct SkyboxTransform {
    bool  active = false;
    float origin[3] = {0.0f, 0.0f, 0.0f}; // sky_camera origin
    float anchor[3] = {0.0f, 0.0f, 0.0f}; // world point the sky_camera maps to
    float rot[9]    = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // sky_camera angles
    float scale     = 16.0f;
    // Membership: near the sky_camera in X/Y and above the z gap that separates
    // the skybox cluster from the map below it. Derived per map; see .cpp.
    float zFloor    = 0.0f;
    float xyHalf    = 4096.0f;
    // Source renders the skybox as a backdrop the world's shell occludes. The
    // engine now composites it the same way (DrawFlags::Background behind the
    // world's pixels), so geometry that lands inside the world's volume is
    // kept -- Source keeps it too. The zone is still the guard for skybox
    // LIGHT entities: without per-light layers a buried fixture would shine
    // out of the terrain into the play space, which Source's two-world split
    // never allows.
    float worldMin[3] = {0.0f, 0.0f, 0.0f};
    float worldMax[3] = {0.0f, 0.0f, 0.0f};
    float hiddenMargin = 512.0f;
};

[[nodiscard]] auto MakeSkyboxTransform(const BSPMap& map) noexcept -> SkyboxTransform;
[[nodiscard]] auto IsSkyboxPoint(const SkyboxTransform& t, const float p[3]) noexcept -> bool;
// A point in PLACED (transformed source) coordinates: inside the world volume.
// Guards skybox light entities only; geometry uses the background layer.
[[nodiscard]] auto IsSkyboxHidden(const SkyboxTransform& t, const float placed[3]) noexcept -> bool;
void TransformSkyboxPoint(const SkyboxTransform& t, const float p[3], float out[3]) noexcept;
void TransformSkyboxVector(const SkyboxTransform& t, const float v[3], float out[3]) noexcept; // rotate only
// A point already known to be skybox content: transform or copy through.
void SkyboxMapPoint(const SkyboxTransform& t, const float p[3], float out[3]) noexcept;

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

// The emissive half of the lighting heuristics as a standalone entry point.
//
// ImportMapGeometry collects the same per-face records inline while it walks the
// faces for the render stream. The scene description has no render stream to
// walk, so it calls this instead -- and it has to, because the importer
// deliberately sets gatherLights = false so the prefab does not spawn the
// entity lights a second time, and that same flag used to gate the emissive
// emission. The result was that every lamp panel, screen and sign in a Source
// map reached neither the scene nor the prefab: the emissive half of the
// heuristic was dead code in the running game.
//
// `entityLights` is what fixture damping is measured against (a panel within
// kEmissiveFixtureRadiusInches of an entity light is that light's housing), so
// pass the entity lights the scene already gathered. Name-gate strengths only:
// this layer has no VFS, so there is no .vmt / .vtf refinement here.
[[nodiscard]] auto GatherEmissiveAreaLights(
    const BSPMap& map, const ImportOptions& options, std::span<const BspPointLight> entityLights
) -> std::vector<BspPointLight>;

// Faces + displacements -> per-material native streams; entities -> lights.
// Faces whose texinfo is missing or node-only are skipped (they are tree
// partitions, not surfaces). Degenerate polygons (fewer than 3 unique corners)
// are skipped rather than producing zero-area triangles.
[[nodiscard]] auto ImportMapGeometry(const BSPMap& map, const ImportOptions& options = {}) -> ImportedMapData;

} // namespace ZHLN::BSP
