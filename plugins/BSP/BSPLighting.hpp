// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// plugins/BSP/BSPLighting.hpp
//
// The lighting half of the BSP workflow: every rule that turns a compiled
// Source map into light for the engine's PBR / RTGI / DDGI pipeline when the
// map's own baked lighting (LUMP_LIGHTING / LUMP_LIGHTING_HDR, i.e. VRAD) is
// not the source of truth.
//
// Source encodes lighting in four places, and only two of them are "light
// entities":
//
//   1. LUMP_ENTITIES point lights      -- light, light_spot,
//                                         light_environment, point_spotlight,
//                                         light_dynamic,
//                                         env_projectedtexture
//   2. LUMP_TEXDATA material properties -- reflectivity, and the SURF_NOLIGHT
//                                         texinfo flag vbsp sets on
//                                         self-illuminated surfaces
//   3. The .vmt behind each material   -- $selfillum, $selfillumtint,
//                                         $selfillummaskscale, shader class
//                                         (UnlitGeneric / MonitorScreen / ...)
//   4. The .vtf behind each material   -- actual texel luminance
//
// Nothing here touches the GPU, the ECS, or an instantiator: the same
// engine-free contract as BSPGeometry.hpp. The importer adapter refines the
// material-driven parts (VMT/VTF) and hands the results to the renderer.
//
// Every heuristic is a named, documented rule with a counter in
// LightingDiagnostics, so "why is my scene dark?" is answerable from a log
// line instead of a guess.

#include "BSPGeometry.hpp"
#include "BSPRead.hpp"
#include "VMTParser.hpp"
#include <Zahlen/Math3D.hpp>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ZHLN::BSP {

// ---------------------------------------------------------------------------
// Texinfo flags (Source bspflags.h)
// ---------------------------------------------------------------------------

// SURF_NOLIGHT: vbsp sets it on every surface a mapper excluded from the
// lightmap bake. On a non-tool material that means the surface is its own light
// source -- it is the only per-surface self-illumination bit anywhere in the
// .bsp, and the strongest evidence the emissive heuristic has.
inline constexpr int32_t kSurfNolight = 0x400;

// ---------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------

// Source `_light` brightness (the 4th component, typically 15-300) is in
// Source lighting units, not renderer intensity. See BSPGeometry.hpp for the
// point/spot derivation; the sun needs its own because the engine's
// directional term is a different quantity (LightDesc documents it as lux
// against LightingSystem::GetSun's own default of 180) while the point/spot
// convention is single digits. Running a sun through the point/spot constant
// lands a stock brightness-200 sun at 6.7 -- 27x below the engine's default --
// which is the single largest reason a map whose VRAD output is unused reads
// as pitch black outdoors. Calibrate so a stock Source sun lands on the engine
// default and the mapper's authored ratio survives.
inline constexpr float kSourceSunBrightnessToEngine = 0.9f; // 200 -> 180

// Source's brightness ladder: `_light` / `_ambient`'s fourth component runs
// 0-255 over 30 units per 255, the divisor RenderSystem.cpp:439 applies before
// the ambient term becomes irradiance.
inline constexpr float kAmbientBrightnessLadder = 30.0f;

// Below this ambient irradiance (colour * brightness/30) a scene reads as black
// under PBR no matter how many point/spot lights it carries: the engine's ambient
// is EvaluateSH(N, frame.sh), and frame.sh integrates the sky gradient, so a
// black sky is a black scene. This is the rule that fires on a map whose VRAD
// output was replaced -- the light_environment is still there, but its ambient
// lane is often zero once the bake is gone.
inline constexpr float kAmbientFloorRadiance = 0.004f;

// A directional term dimmer than this makes every outdoor surface read as dusk.
inline constexpr float kSunFloorIntensity = 12.0f;

// The last-resort point light: neutral, wide, placed at the map's centre, so a
// map with literally no light anywhere still renders as a room rather than a
// void. Only ever added when the map has no light of any kind.
inline constexpr float kFillLightIntensity   = 12.0f;
inline constexpr float kFillLightRangeMetres = 40.0f;

// Equivalent Source `_light` brightness per square metre of emissive surface.
// A 0.05 m^2 white lamp panel then reads as brightness 200 -- the stock
// `light` default -- so a housing panel and the point/spot light inside it
// contribute the same order of magnitude instead of the panel being invisible
// or blinding.
inline constexpr float kEmissiveSourceBrightnessPerSquareMetre = 250.0f;

// Ceiling on a single emissive proxy's engine intensity. The flux model
// (brightness * area) is what makes a 25 m2 video wall out-shine a 32x32 lamp
// panel, but it is unbounded, and one proxy at 200+ intensity monopolises the
// renderer's intensity/distance shadow-layer sort and blows out the frame.
// A stock point/spot `light` lands at 6.67, so this is ~6 of them: clearly the
// brightest thing in the room without being the only thing visible.
inline constexpr float kEmissiveMaxLightIntensity = 40.0f;

// An emissive face this close to an entity light is almost certainly the
// housing of that light (gm_murder_drones: every lights/white and /white007
// panel sits 20-35 Source inches from one), so its emission is already
// accounted for. Damp instead of dropping: a screen that happens to share a
// room with a lamp still has to glow.
inline constexpr float kEmissiveFixtureRadiusInches = 96.0f;
inline constexpr float kEmissiveFixtureDamping       = 0.1f;

// Emissive faces are clustered on this grid (Source inches) so a fixture made
// of many coplanar sheets produces one area-light proxy, not one per sheet.
inline constexpr float kEmissiveClusterCellInches = 192.0f;

// Hard cap on emissive proxies. Ranked by strength * area, so a map with 400
// lamp sheets keeps the panels that actually matter.
inline constexpr uint32_t kEmissiveMaxLights = 192u;

// Source's own attenuation window is 1/(c + l*d + q*d^2), but the coefficients
// are authored against Source's intensity scale and cannot be converted into
// the engine's: a stock brightness-200 `_quadratic_attn 1` light lights a room
// several metres across in Source, while the same coefficients against the
// engine's scale give 18 inches. The range is therefore derived from the
// window the renderer actually evaluates -- LightDesc's
// atten = intensity/(d^2 + 1), d in metres -- except when the mapper authored a
// distance outright (`_zero_percent_distance` / `_distance` / `_fifty_percent_distance`),
// which is a real inch measurement and always wins.
inline constexpr float kAttenuationCutoff = 0.02f;

// An emissive proxy's range is a multiple of its own characteristic radius
// sqrt(area/pi): an area light reaches a few times its footprint, so a 25 m2
// wall out-reaches a 32x32 lamp panel even though the panel is brighter per
// square metre.
inline constexpr float kEmissiveRangeScale = 2.5f;

// Sanity window for any derived range, in metres. The 20 m default in the
// scene description let one closet lamp reach through five rooms of unshadowed
// concrete.
inline constexpr float kMinRangeMetres = 3.0f;
inline constexpr float kMaxRangeMetres = 24.0f;

// ---------------------------------------------------------------------------
// Light entities
// ---------------------------------------------------------------------------

// What a classname means to the lighting pass. `BeamCone` is the interesting
// one: Source's point_spotlight draws a beam sprite and *optionally* a light,
// and the light lives at the END of the beam, not at the origin.
enum class LightEntityKind : uint8_t {
    NotALight,
    Point,
    Spot,
    Sun,      // directional plus an ambient term
    BeamCone, // point_spotlight: light at origin + dir * spotlightlength
};

// Where the colour/brightness pair comes from. Source has three spellings and
// they are not interchangeable.
enum class LightValueSource : uint8_t {
    LightKey,        // `_light` (LDR) with `_lightHDR` / `_lightscaleHDR`
    RenderColor,     // `rendercolor` + `renderamt` (+ `HDRColorScale`)
    ProjectedTexture // `lightcolor` + `lightfov`
};

[[nodiscard]] auto ClassifyLightEntity(std::string_view className) noexcept -> LightEntityKind;

// True when the classname is a light class this pass knows how to read. Kept
// separate from ClassifyLightEntity so callers can ask "is this a light?"
// without caring which kind.
[[nodiscard]] auto IsLightEntity(std::string_view className) noexcept -> bool;

// Source's attenuation window for one entity: 1/(constant + linear*d +
// quadratic*d^2), all in Source units. `fiftyPercentDistance` and
// `zeroPercentDistance` are Hammer's override pair; `_distance`/`_falloff` are
// the same thing under older names. `hardFalloff` asks for a cutoff at the
// zero-percent distance rather than a smooth window.
struct SourceAttenuation {
    float constant             = 0.0f;
    float linear               = 0.0f;
    float quadratic            = 1.0f;
    float fiftyPercentDistance = 0.0f; // 0 = derive from the coefficients
    float zeroPercentDistance  = 0.0f; // 0 = derive
    bool  hardFalloff          = false;
};

[[nodiscard]] auto ParseSourceAttenuation(const BSPEntity& entity) noexcept -> SourceAttenuation;

// Metres at which the attenuation window drops below kAttenuationCutoff, given
// an already-calibrated engine intensity. Clamped to [kMinRangeMetres,
// kMaxRangeMetres] because the 20 m default in the scene description let one
// closet lamp reach through five rooms of unshadowed concrete.
[[nodiscard]] auto AttenuationRangeMetres(const SourceAttenuation& atten, float engineIntensity, float unitScale) noexcept -> float;

// The `_light` / `_lightHDR` / `_lightscaleHDR` triple. Source stores the LDR
// colour in `_light` and, when the map was compiled for HDR, an override in
// `_lightHDR` scaled by `_lightscaleHDR`; the sentinel "-1 -1 -1 1" means "no
// override, use the LDR value". `hasBrightness` is false for the legacy
// 3-component spelling ("119 49 47"), which must not silently become a
// zero-intensity light.
struct SourceLightValue {
    std::array<float, 4> rgba {255.0f, 255.0f, 255.0f, 200.0f};
    bool                 hasBrightness = true;
};

[[nodiscard]] auto ParseSourceLightValue(const BSPEntity& entity) noexcept -> SourceLightValue;

// `rendercolor` + `renderamt` (+ `HDRColorScale`): the CS:GO / GMod spelling
// point_spotlight, env_projectedtexture and the *_dynamic family use. Exported
// because it is the only way to read those entities' colour, and because a test
// has to be able to check the three spellings apart.
[[nodiscard]] auto ParseRenderColorValue(const BSPEntity& entity) noexcept -> SourceLightValue;

// Harvests every point/spot/sun light the entity lump describes. A light is
// dropped (not emitted) when it is switched off, when it is black, or when it
// is a beam cone whose "No Dynamic Light" spawnflag is set -- Source sets that
// light's scale to exactly zero, so emitting it would double a fixture that
// already has a `light`/`light_spot` beside it.
[[nodiscard]] auto GatherEntityLights(const BSPMap& map, const ImportOptions& options) -> std::vector<BspPointLight>;

// ---------------------------------------------------------------------------
// Ambient
// ---------------------------------------------------------------------------

// AmbientTerm lives in BSPGeometry.hpp (it is imported map data); the reading
// rules are here.
[[nodiscard]] auto ReadEnvironmentAmbient(const BSPMap& map) noexcept -> AmbientTerm;

// The light_environment sun direction in Source space (pitch positive down),
// or a zero vector when the map has none.
[[nodiscard]] auto EnvironmentSunDirection(const BSPMap& map) noexcept -> JPH::Vec3;

// ---------------------------------------------------------------------------
// Emissive surfaces
// ---------------------------------------------------------------------------

// What the name / texdata / VMT probe concluded about one material.
struct EmissiveMaterialProbe {
    bool      isEmissive = false;
    float     strength   = 0.0f; // authored self-illumination, 0..1
    JPH::Vec3 tint {1.0f, 1.0f, 1.0f};
};

// Name-only emissive test, used by the geometry pass (which has no VFS). A
// material family that reads as emissive by name still has to clear the
// reflectivity/strength gate before it emits -- see ProbeEmissiveMaterial.
[[nodiscard]] auto IsEmissiveMaterialName(std::string_view materialName) noexcept -> bool;

// The full probe: name family, texdata reflectivity luma, the parsed VMT when
// the importer has one, and whether vbsp set SURF_NOLIGHT on the surface's
// texinfo (which is vbsp saying "do not bake light here" -- i.e. the surface
// self-illuminates, and it is the strongest per-surface evidence in the whole
// .bsp). Returns a zero strength for materials that only
// look emissive (a pure-white, fully-reflective `lights/white` sheet is a lit
// surface, not a lamp).
[[nodiscard]] auto ProbeEmissiveMaterial(
    std::string_view materialName, float reflectivityLuma, const VMTMaterial* vmt, bool noLightFlag
) noexcept -> EmissiveMaterialProbe;

// EmissiveSurface lives in BSPGeometry.hpp (it is imported map data); the
// clustering, damping and capping rules are here.
//
// Clusters accumulated emissive faces (same material, same
// kEmissiveClusterCellInches cell), damps the ones that share a fixture with
// an entity light, ranks by strength * area and truncates to kEmissiveMaxLights.
[[nodiscard]] auto FinalizeEmissiveSurfaces(
    std::span<const EmissiveSurface> surfaces, std::span<const BspPointLight> entityLights, float unitScale, uint32_t maxLights
) -> std::vector<EmissiveSurface>;

// The area-light proxy for one emissive surface: a point light pushed just off
// the surface along its normal, with the intensity the
// equivalent-Source-brightness model gives it. Range comes from the same
// attenuation window the entity lights use.
[[nodiscard]] auto EmissiveLightFor(const EmissiveSurface& surface, const ImportOptions& options) -> BspPointLight;

// ---------------------------------------------------------------------------
// Diagnostics / guarantee
// ---------------------------------------------------------------------------

// LightingDiagnostics lives in BSPGeometry.hpp (it is imported map data); the
// rules that fill it are here.

[[nodiscard]] auto AuditLighting(
    std::span<const BspPointLight> entityLights, std::span<const EmissiveSurface> emissive, const AmbientTerm& ambient,
    const BSPMap& map
) -> LightingDiagnostics;

// The guarantee. A scene whose only light is a dim sun and whose ambient is
// black renders black, which is exactly the failure mode this pass exists to
// prevent. This lifts the ambient floor to a visible minimum and, when the map
// offers no light at all, synthesises a neutral fill light at the map's centre
// so the PBR/RTGI/DDGI pipeline always has something to trace. Returns true
// when it had to intervene.
[[nodiscard]] auto EnforceLightingFloor(
    ImportedMapData& data, const BSPMap& map, const ImportOptions& options, LightingDiagnostics& diagnostics
) -> bool;

} // namespace ZHLN::BSP
