// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/extras/TestBSPLighting.cpp
//
// The lighting heuristics, on synthetic maps. Nothing here needs a .bsp on
// disk: the rules under test are keyvalue-driven (classname families, the
// `_light` colour/brightness ladder, the attenuation cutoffs, the material
// name families, the fixture-damping radius), and every one of them has a
// measurable boundary that is easier to pin down with a hand-built entity than
// with a real map.
//
// The map that motivated this -- a Source map whose VRAD lightmap lump was
// replaced, leaving every baked surface black -- is used as the shape of the
// fixtures: `point_spotlight` entities that carry the "No Dynamic Light"
// spawnflag, `light` entities whose `_light` has only three components, and
// self-illuminated materials named after the fixtures they render.

#include <BSP/BSPGeometry.hpp>
#include <BSP/BSPLighting.hpp>
#include <BSP/BSPRead.hpp>
#include <TestsFramework.hpp>
#include <Zahlen/Common.h>
#include <Zahlen/Log.hpp>
#include <cmath>
#include <string>
#include <vector>

namespace ZHLN {
std::string GetPoorMansStacktrace() {
    return "test_stacktrace";
}
} // namespace ZHLN

namespace {

using ZHLN::BSP::AmbientTerm;
using ZHLN::BSP::BSPEntity;
using ZHLN::BSP::BSPMap;
using ZHLN::BSP::EmissiveMaterialProbe;
using ZHLN::BSP::EmissiveSurface;
using ZHLN::BSP::ImportOptions;
using ZHLN::BSP::ImportedMapData;
using ZHLN::BSP::LightEntityKind;
using ZHLN::BSP::LightingDiagnostics;
using ZHLN::BSP::SourceAttenuation;
using ZHLN::BSP::SourceLightValue;

// Source spawnflag bits, from point_spotlight.cpp.
constexpr int32_t kSfStartOn             = 1;
constexpr int32_t kSfNoDynamicLight      = 2;

// The suite's own error channel: every test returns an expected<void>, and the
// only failure mode is a failed expectation.
enum class LightingTestError : uint8_t {
    ExpectationFailed = 1,
};

auto Fail() -> std::expected<void, ZHLN::ErrorCode> {
    return std::unexpected(LightingTestError::ExpectationFailed);
}

auto MakeEntity(std::initializer_list<std::pair<std::string, std::string>> kv) -> BSPEntity {
    BSPEntity entity;
    for (const auto& [key, value]: kv) {
        entity.keys.emplace_back(key, value);
    }
    return entity;
}

auto MakeLight(std::string_view className, std::string_view light = "255 255 255 200", std::string_view origin = "0 0 0") -> BSPEntity {
    return MakeEntity({
        {"classname", std::string(className)},
        {"origin", std::string(origin)},
        {"_light", std::string(light)},
    });
}

struct BSPLightingTestSuite {
    // ---- classification -------------------------------------------------
    // The classname table is the first thing that can go wrong: a family the
    // old code did not know is a family that renders black.
    [[nodiscard]] auto classify_light_entity_families() const -> std::expected<void, ZHLN::ErrorCode> {
        if (!ZHLN::Test::ExpectEq(ZHLN::BSP::ClassifyLightEntity("light"), LightEntityKind::Point) ||
            !ZHLN::Test::ExpectEq(ZHLN::BSP::ClassifyLightEntity("light_spot"), LightEntityKind::Spot) ||
            !ZHLN::Test::ExpectEq(ZHLN::BSP::ClassifyLightEntity("light_environment"), LightEntityKind::Sun) ||
            !ZHLN::Test::ExpectEq(ZHLN::BSP::ClassifyLightEntity("light_dynamic"), LightEntityKind::Point) ||
            !ZHLN::Test::ExpectEq(ZHLN::BSP::ClassifyLightEntity("env_projectedtexture"), LightEntityKind::Spot) ||
            !ZHLN::Test::ExpectEq(ZHLN::BSP::ClassifyLightEntity("point_spotlight"), LightEntityKind::BeamCone) ||
            !ZHLN::Test::ExpectEq(ZHLN::BSP::ClassifyLightEntity("prop_dynamic"), LightEntityKind::NotALight) ||
            !ZHLN::Test::ExpectEq(ZHLN::BSP::ClassifyLightEntity("ambient_generic"), LightEntityKind::NotALight) ||
            !ZHLN::Test::ExpectEq(ZHLN::BSP::ClassifyLightEntity("info_lighting"), LightEntityKind::NotALight) ||
            !ZHLN::Test::ExpectEq(ZHLN::BSP::ClassifyLightEntity(""), LightEntityKind::NotALight)) {
            return Fail();
        }
        return {};
    }

    // ---- colour / brightness --------------------------------------------
    // A `_light` with three components is a colour with an implied full
    // brightness, not a black light. This is the single most common reason a
    // replaced-VRAD map imports dark: 19 of this map's 1226 point lights use
    // the three-component form.
    [[nodiscard]] auto parse_source_light_value_defaults_brightness() const -> std::expected<void, ZHLN::ErrorCode> {
        const SourceLightValue three = ZHLN::BSP::ParseSourceLightValue(MakeLight("light", "119 49 47"));
        if (!ZHLN::Test::ExpectInRange(three.rgba[0], 119.0f, 119.0f) || !ZHLN::Test::ExpectInRange(three.rgba[1], 49.0f, 49.0f) ||
            !ZHLN::Test::ExpectInRange(three.rgba[2], 47.0f, 47.0f) ||
            // No brightness lane: Source's own default of 200, not a zero.
            !ZHLN::Test::ExpectInRange(three.rgba[3], 200.0f, 200.0f) || !ZHLN::Test::ExpectTrue(!three.hasBrightness)) {
            return Fail();
        }

        const SourceLightValue four = ZHLN::BSP::ParseSourceLightValue(MakeLight("light", "82 139 226 20"));
        if (!ZHLN::Test::ExpectInRange(four.rgba[3], 20.0f, 20.0f)) {
            return Fail();
        }

        // The HDR spelling wins when it is authored (this map ships
        // `_lightHDR "-1 -1 -1 1"`, i.e. "same as LDR").
        const SourceLightValue hdr = ZHLN::BSP::ParseSourceLightValue(MakeEntity({
            {"classname", "light"},
            {"origin", "0 0 0"},
            {"_light", "82 139 226 20"},
            {"_lightHDR", "-1 -1 -1 1"},
            {"_lightscaleHDR", "1"},
        }));
        if (!ZHLN::Test::ExpectInRange(hdr.rgba[3], 20.0f, 20.0f)) {
            return Fail();
        }

        // rendercolor / renderamt is the second spelling, read through its own
        // parser: a 3-component rendercolor is full brightness, and
        // `HDRColorScale` multiplies the brightness lane.
        const SourceLightValue render = ZHLN::BSP::ParseRenderColorValue(MakeEntity({
            {"classname", "point_spotlight"},
            {"origin", "0 0 0"},
            {"rendercolor", "255 240 200"},
            {"renderamt", "200"},
            {"HDRColorScale", "0.5"},
        }));
        if (!ZHLN::Test::ExpectInRange(render.rgba[0], 255.0f, 255.0f) || !ZHLN::Test::ExpectInRange(render.rgba[1], 240.0f, 240.0f) ||
            !ZHLN::Test::ExpectInRange(render.rgba[2], 200.0f, 200.0f) ||
            // `HDRColorScale` multiplies the brightness lane: 200 * 0.5.
            !ZHLN::Test::ExpectInRange(render.rgba[3], 99.0f, 101.0f)) {
            return Fail();
        }

        // A light that says nothing about its colour at all still has to come
        // back as white-at-200, the engine's own default pair.
        const SourceLightValue bare = ZHLN::BSP::ParseSourceLightValue(MakeLight("light", ""));
        if (!ZHLN::Test::ExpectInRange(bare.rgba[0], 255.0f, 255.0f) || !ZHLN::Test::ExpectInRange(bare.rgba[3], 200.0f, 200.0f)) {
            return Fail();
        }
        return {};
    }

    // ---- attenuation ----------------------------------------------------
    // Source's `_quadratic_attn 1` with everything else zero is the map's
    // actual configuration: pure inverse square, no authored cutoff. The range
    // then has to be derived from the brightness, because leaving it at the
    // engine's 1000 m default is what washes a whole level out.
    [[nodiscard]] auto attenuation_range_from_brightness() const -> std::expected<void, ZHLN::ErrorCode> {
        const SourceAttenuation quadratic = ZHLN::BSP::ParseSourceAttenuation(MakeLight("light", "255 255 255 200"));
        if (!ZHLN::Test::ExpectInRange(quadratic.constant, 0.0f, 0.0f) || !ZHLN::Test::ExpectInRange(quadratic.linear, 0.0f, 0.0f) ||
            !ZHLN::Test::ExpectInRange(quadratic.quadratic, 1.0f, 1.0f)) {
            return Fail();
        }

        // 200 * 1/30 = 6.67 engine intensity; at the 0.02 cutoff that is
        // sqrt(6.67/0.02 - 1) = 18.2 m of throw, well inside the 24 m ceiling.
        const float range = ZHLN::BSP::AttenuationRangeMetres(quadratic, 200.0f * (1.0f / 30.0f), 1.0f / 39.37f);
        if (!ZHLN::Test::ExpectInRange(range, 15.0f, 24.0f)) {
            return Fail();
        }

        // A dim light must not out-throw a bright one.
        const float dim = ZHLN::BSP::AttenuationRangeMetres(quadratic, 20.0f * (1.0f / 30.0f), 1.0f / 39.37f);
        if (!ZHLN::Test::ExpectTrue(dim < range) || !ZHLN::Test::ExpectTrue(dim >= 3.0f)) {
            return Fail();
        }

        // An authored cutoff wins outright: `_zero_percent_distance` is Source's
        // own "this light stops here".
        const SourceAttenuation authored = ZHLN::BSP::ParseSourceAttenuation(MakeEntity({
            {"classname", "light"},
            {"origin", "0 0 0"},
            {"_light", "255 255 255 200"},
            {"_zero_percent_distance", "256"},
        }));
        const float authoredRange = ZHLN::BSP::AttenuationRangeMetres(authored, 200.0f * (1.0f / 30.0f), 1.0f / 39.37f);
        if (!ZHLN::Test::ExpectInRange(authoredRange, 6.0f, 7.0f)) { // 256 in * 0.0254 m/in
            return Fail();
        }
        return {};
    }

    // ---- entity harvesting ----------------------------------------------
    // The map's 743 `point_spotlight` entities all carry "No Dynamic Light":
    // they are beam sprites that sit behind a co-located `light_spot`, and
    // VRAD's own worldlights lump contains none of them. Harvesting them as
    // punctual lights would double the map's illumination.
    [[nodiscard]] auto gather_entity_lights_skips_disabled_beam_cones() const -> std::expected<void, ZHLN::ErrorCode> {
        BSPMap map;
        map.version = 20;
        map.entities.push_back(MakeLight("light", "255 255 255 200", "0 0 64"));
        map.entities.push_back(MakeLight("light_spot", "255 255 255 200", "128 0 64"));
        map.entities.push_back(MakeEntity({
            {"classname", "point_spotlight"},
            {"origin", "128 0 40"},
            {"spawnflags", "3"},
            {"spotlightlength", "50"},
            {"rendercolor", "255 240 200"},
            {"renderamt", "200"},
        }));
        map.entities.push_back(MakeEntity({
            {"classname", "point_spotlight"},
            {"origin", "256 0 40"},
            {"spawnflags", "1"}, // Start On, no "No Dynamic Light": a real light
            {"spotlightlength", "50"},
            {"rendercolor", "255 240 200"},
            {"renderamt", "200"},
        }));
        map.entities.push_back(MakeLight("prop_dynamic", "255 255 255 200", "512 0 64"));
        map.entities.push_back(MakeEntity({{"classname", "light"}, {"origin", "640 0 64"}, {"_light", "0 0 0 0"}}));

        const ImportOptions       options;
        const std::vector<ZHLN::BSP::BspPointLight> lights = ZHLN::BSP::GatherEntityLights(map, options);

        // light, light_spot, and the one point_spotlight whose beam does carry a
        // light. The "No Dynamic Light" cone, the non-light, and the black
        // light are all gone.
        if (!ZHLN::Test::ExpectEq(lights.size(), 3u)) {
            return Fail();
        }
        bool sawPoint = false;
        bool sawSpot  = false;
        bool sawSun   = false;
        for (const ZHLN::BSP::BspPointLight& light: lights) {
            if (light.type == ZHLN::LightType::Point) {
                sawPoint = true;
            }
            if (light.type == ZHLN::LightType::Spot) {
                sawSpot = true;
                // The cone width must not become the throw distance.
                if (!ZHLN::Test::ExpectTrue(light.range > 1.0f) || !ZHLN::Test::ExpectTrue(light.outerConeRadians > 0.0f)) {
                    return Fail();
                }
            }
            if (light.type == ZHLN::LightType::Sun) {
                sawSun = true;
            }
        }
        if (!ZHLN::Test::ExpectTrue(sawPoint) || !ZHLN::Test::ExpectTrue(sawSpot) || !ZHLN::Test::ExpectTrue(!sawSun)) {
            return Fail();
        }
        return {};
    }

    // The beam cone's light lives at the END of the beam, not at the origin:
    // Source places it there and the entity's origin is only the emitter's
    // position. Getting this wrong puts a light 50 inches behind every lamp.
    [[nodiscard]] auto gather_entity_lights_places_beam_cone_at_beam_end() const -> std::expected<void, ZHLN::ErrorCode> {
        BSPMap map;
        map.version = 20;
        map.entities.push_back(MakeEntity({
            {"classname", "point_spotlight"},
            {"origin", "0 0 0"},
            {"spawnflags", "1"}, // Start On, no "No Dynamic Light"
            {"angles", "0 0 0"},
            {"spotlightlength", "96"},
            {"rendercolor", "255 255 255"},
            {"renderamt", "200"},
        }));

        const ImportOptions                            options;
        const std::vector<ZHLN::BSP::BspPointLight>    lights = ZHLN::BSP::GatherEntityLights(map, options);
        if (!ZHLN::Test::ExpectEq(lights.size(), 1u)) {
            return Fail();
        }
        // 96 inches straight down the -Z of Source space (pitch 0) is 96 inches
        // of +Y in engine space: 2.44 m.
        const float metres = lights[0].position.Length();
        if (!ZHLN::Test::ExpectInRange(metres, 2.3f, 2.6f)) {
            return Fail();
        }
        return {};
    }

    // ---- environment ----------------------------------------------------
    // The sun's direction and the ambient term are the two things an outdoor
    // section cannot be lit without, and both come out of the single
    // light_environment entity.
    [[nodiscard]] auto environment_sun_and_ambient() const -> std::expected<void, ZHLN::ErrorCode> {
        BSPMap map;
        map.version = 20;
        map.entities.push_back(MakeEntity({
            {"classname", "light_environment"},
            {"origin", "-271.787 -1984 -512"},
            {"angles", "-62 87 0"},
            {"pitch", "-62"},
            {"_light", "82 139 226 20"},
            {"_ambient", "64 95 166 10"},
        }));

        const AmbientTerm ambient = ZHLN::BSP::ReadEnvironmentAmbient(map);
        if (!ZHLN::Test::ExpectTrue(ambient.authored)) {
            return Fail();
        }
        // The colour is linear 0-1 and the brightness stays in Source's 0-255
        // lane; the /30 ladder is the caller's job (BSPScene does it), so
        // `_ambient "64 95 166 10"` reads back as (64, 95, 166)/255 and 10.
        if (!ZHLN::Test::ExpectInRange(ambient.color.GetX(), 0.245f, 0.255f) ||
            !ZHLN::Test::ExpectInRange(ambient.color.GetY(), 0.365f, 0.378f) ||
            !ZHLN::Test::ExpectInRange(ambient.color.GetZ(), 0.645f, 0.655f) ||
            !ZHLN::Test::ExpectInRange(ambient.intensity, 9.9f, 10.1f)) {
            return Fail();
        }

        // pitch -62, yaw 87 -> a sun that points up and to the side; the
        // direction has to be a unit vector in engine space.
        const JPH::Vec3 sun = ZHLN::BSP::EnvironmentSunDirection(map);
        if (!ZHLN::Test::ExpectInRange(sun.Length(), 0.999f, 1.001f) ||
            // pitch -62: the sun sits above the horizon in Source's Z-up space.
            !ZHLN::Test::ExpectTrue(sun.GetZ() > 0.4f)) {
            return Fail();
        }

        // A map with no light_environment gets a usable fallback rather than a
        // zero direction that silently deletes the sun: a 45-degree elevation
        // sun, pointing up in Source's Z-up space.
        const BSPMap empty;
        const JPH::Vec3 fallback = ZHLN::BSP::EnvironmentSunDirection(empty);
        if (!ZHLN::Test::ExpectInRange(fallback.Length(), 0.999f, 1.001f) || !ZHLN::Test::ExpectTrue(fallback.GetZ() > 0.5f) ||
            !ZHLN::Test::ExpectTrue(fallback.GetX() != 0.0f || fallback.GetY() != 0.0f)) {
            return Fail();
        }
        if (!ZHLN::Test::ExpectTrue(!ZHLN::BSP::ReadEnvironmentAmbient(empty).authored)) {
            return Fail();
        }
        return {};
    }

    // ---- material families ----------------------------------------------
    // The name gate is what makes an emissive surface findable at all: Source
    // has no per-material "emissive" bit in the .bsp, so the material's name
    // is the signal that survives.
    [[nodiscard]] auto is_emissive_material_name_families() const -> std::expected<void, ZHLN::ErrorCode> {
        if (!ZHLN::Test::ExpectTrue(ZHLN::BSP::IsEmissiveMaterialName("lights/white")) ||
            !ZHLN::Test::ExpectTrue(ZHLN::BSP::IsEmissiveMaterialName("lights/white007")) ||
            !ZHLN::Test::ExpectTrue(ZHLN::BSP::IsEmissiveMaterialName("effects/combinedisplay001b")) ||
            !ZHLN::Test::ExpectTrue(ZHLN::BSP::IsEmissiveMaterialName("murder drones/doorssign")) ||
            !ZHLN::Test::ExpectTrue(ZHLN::BSP::IsEmissiveMaterialName("neon/glowstrip")) ||
            !ZHLN::Test::ExpectTrue(ZHLN::BSP::IsEmissiveMaterialName("models/props/led_panel")) ||
            !ZHLN::Test::ExpectFalse(ZHLN::BSP::IsEmissiveMaterialName("concrete/concretefloor003a")) ||
            !ZHLN::Test::ExpectFalse(ZHLN::BSP::IsEmissiveMaterialName("metal/metalwall005b")) ||
            !ZHLN::Test::ExpectFalse(ZHLN::BSP::IsEmissiveMaterialName(""))) {
            return Fail();
        }
        return {};
    }

    // A `lights/white` sheet with reflectivity (1,1,1) is a lit surface, not a
    // lamp: Source's own reflectivity for that material is full white, and the
    // texture it references is not in the map's pak. The probe has to damp it
    // instead of trusting the name.
    [[nodiscard]] auto probe_emissive_material_damps_near_white() const -> std::expected<void, ZHLN::ErrorCode> {
        const EmissiveMaterialProbe white = ZHLN::BSP::ProbeEmissiveMaterial("lights/white", 1.0f, nullptr, false);
        if (!ZHLN::Test::ExpectTrue(white.isEmissive) || !ZHLN::Test::ExpectTrue(white.strength > 0.0f) ||
            !ZHLN::Test::ExpectTrue(white.strength < 0.5f)) {
            return Fail();
        }

        // A genuinely dark-authored emitter (reflectivity (0,0,0) means the
        // material does not reflect -- the map's `LIGHTS/WHITE007` case) keeps
        // its full family strength.
        const EmissiveMaterialProbe dark = ZHLN::BSP::ProbeEmissiveMaterial("lights/white007", 0.0f, nullptr, false);
        if (!ZHLN::Test::ExpectTrue(dark.strength > white.strength)) {
            return Fail();
        }

        // A display panel with a real albedo is the strong case: neither the
        // white nor the black damping applies.
        const EmissiveMaterialProbe display = ZHLN::BSP::ProbeEmissiveMaterial("effects/combinedisplay002a", 0.16f, nullptr, false);
        if (!ZHLN::Test::ExpectTrue(display.strength > 0.5f)) {
            return Fail();
        }

        // Ordinary materials never pass, whatever the family words look like.
        if (!ZHLN::Test::ExpectTrue(!ZHLN::BSP::ProbeEmissiveMaterial("concrete/concretefloor003a", 0.5f, nullptr, false).isEmissive)) {
            return Fail();
        }

        // vbsp's SURF_NOLIGHT flag is the strongest per-surface evidence in the
        // .bsp: it is vbsp saying "do not bake light here", which is exactly
        // "this surface is its own light". A near-white `sign` material that
        // would otherwise be damped to a tenth of its family strength keeps the
        // full strength once the flag is set.
        const EmissiveMaterialProbe damped = ZHLN::BSP::ProbeEmissiveMaterial("murder drones/communitysign", 0.83f, nullptr, false);
        const EmissiveMaterialProbe flagged = ZHLN::BSP::ProbeEmissiveMaterial("murder drones/communitysign", 0.83f, nullptr, true);
        if (!ZHLN::Test::ExpectTrue(flagged.isEmissive) || !ZHLN::Test::ExpectTrue(damped.isEmissive) ||
            !ZHLN::Test::ExpectTrue(flagged.strength > damped.strength * 3.0f)) {
            return Fail();
        }
        return {};
    }

    // ---- clustering and the cap ------------------------------------------
    // 402 faces on one material is one light, not 402: the centroid and normal
    // are area-weighted, and the cap keeps a pathological map from emitting
    // more proxies than the renderer can sort.
    [[nodiscard]] auto finalize_emissive_surfaces_clusters_and_caps() const -> std::expected<void, ZHLN::ErrorCode> {
        std::vector<EmissiveSurface> in;
        for (int i = 0; i < 8; ++i) {
            EmissiveSurface surface;
            surface.materialName       = "lights/white";
            surface.areaSquareMetres   = 1.0f;
            surface.areaSquareInches   = 1544.0f;
            surface.faceCount          = 1;
            surface.strength           = 0.5f;
            // All eight faces share a centroid: one cluster.
            surface.position           = JPH::Vec3(2.0f, 1.0f, 0.0f);
            surface.normal             = JPH::Vec3(0.0f, 1.0f, 0.0f);
            in.push_back(surface);
        }
        for (int i = 0; i < 4; ++i) {
            EmissiveSurface surface;
            surface.materialName     = "lights/white";
            surface.areaSquareMetres = 1.0f;
            surface.faceCount        = 1;
            surface.strength         = 0.5f;
            // Far from the first group, and from each other: four more
            // clusters. (The +1 keeps them off the coincident group's own
            // cell, which the clusterer would otherwise merge them into.)
            surface.position         = JPH::Vec3(40.0f + 20.0f * static_cast<float>(i), 1.0f, 0.0f);
            surface.normal           = JPH::Vec3(0.0f, 1.0f, 0.0f);
            in.push_back(surface);
        }

        const ImportOptions                        options;
        const std::vector<EmissiveSurface>         clustered = ZHLN::BSP::FinalizeEmissiveSurfaces(in, {}, 1.0f / 39.37f, 64u);
        if (!ZHLN::Test::ExpectEq(clustered.size(), 5u)) {
            return Fail();
        }
        // The eight coincident faces collapse into one surface that keeps the
        // whole group's area and face count.
        for (const EmissiveSurface& surface: clustered) {
            if (surface.position.GetX() < 3.0f) {
                if (!ZHLN::Test::ExpectEq(surface.faceCount, 8u) || !ZHLN::Test::ExpectInRange(surface.areaSquareMetres, 7.9f, 8.1f)) {
                    return Fail();
                }
            }
        }

        // The cap: 300 distinct clusters with a cap of 8 leaves 8, and the
        // dropped count is reported rather than silently truncating.
        std::vector<EmissiveSurface> many;
        for (int i = 0; i < 300; ++i) {
            EmissiveSurface surface;
            surface.materialName     = "lights/white";
            surface.areaSquareMetres = 1.0f;
            surface.faceCount        = 1;
            surface.strength         = 0.5f;
            surface.position         = JPH::Vec3(2.0f + 20.0f * static_cast<float>(i), 1.0f, 0.0f);
            surface.normal           = JPH::Vec3(0.0f, 1.0f, 0.0f);
            many.push_back(surface);
        }
        const std::vector<EmissiveSurface> capped = ZHLN::BSP::FinalizeEmissiveSurfaces(many, {}, 1.0f / 39.37f, 8u);
        if (!ZHLN::Test::ExpectEq(capped.size(), 8u)) {
            return Fail();
        }
        return {};
    }

    // ---- fixture damping -------------------------------------------------
    // A lamp panel with a `light` entity 24 inches away is the housing around
    // that light, not a second light: emitting a proxy at its centroid
    // doubles the illumination of every fixture in the map.
    [[nodiscard]] auto finalize_emissive_surfaces_damps_nearby_entity_lights() const -> std::expected<void, ZHLN::ErrorCode> {
        ZHLN::BSP::BspPointLight entity;
        entity.type     = ZHLN::LightType::Point;
        entity.position = JPH::Vec3(0.0f, 0.0f, 0.0f);

        std::vector<EmissiveSurface> in;
        EmissiveSurface nearSurface;
        nearSurface.materialName     = "lights/white";
        nearSurface.areaSquareMetres = 1.0f;
        nearSurface.faceCount        = 1;
        nearSurface.strength         = 0.5f;
        // 24 inches from the entity light: inside the 96-inch (2.44 m) fixture
        // radius, and far enough away in X to be its own cluster.
        nearSurface.position         = JPH::Vec3(0.0f, 0.61f, 0.0f);
        nearSurface.normal           = JPH::Vec3(0.0f, 1.0f, 0.0f);
        in.push_back(nearSurface);

        EmissiveSurface farSurface;
        farSurface.materialName     = "lights/white";
        farSurface.areaSquareMetres = 1.0f;
        farSurface.faceCount        = 1;
        farSurface.strength         = 0.5f;
        farSurface.position         = JPH::Vec3(30.0f, 0.61f, 0.0f);
        farSurface.normal           = JPH::Vec3(0.0f, 1.0f, 0.0f);
        in.push_back(farSurface);

        const std::vector<EmissiveSurface> out = ZHLN::BSP::FinalizeEmissiveSurfaces(in, {entity}, 1.0f / 39.37f, 64u);
        if (!ZHLN::Test::ExpectEq(out.size(), 2u)) {
            return Fail();
        }
        for (const EmissiveSurface& surface: out) {
            if (surface.position.GetX() > 1.0f) {
                // The far cluster keeps its full strength.
                if (!ZHLN::Test::ExpectInRange(surface.fixtureDamping, 1.0f, 1.0f)) {
                    return Fail();
                }
            } else if (!ZHLN::Test::ExpectTrue(surface.fixtureDamping < 0.5f)) {
                return Fail();
            }
        }
        return {};
    }

    // ---- the proxy light ------------------------------------------------
    // A 0.05 m2 panel has to come out brighter than a 5 m2 wall: the intensity
    // is the brightness-per-square-metre times the area, so the renderer's
    // intensity/distance sort treats a small bright panel and a large dim one
    // in the right order.
    [[nodiscard]] auto emissive_light_for_scales_with_area() const -> std::expected<void, ZHLN::ErrorCode> {
        EmissiveSurface small;
        small.materialName     = "effects/combinedisplay002a";
        small.areaSquareMetres = 0.05f;
        small.strength         = 0.9f;
        small.position         = JPH::Vec3(1.0f, 2.0f, 3.0f);
        small.normal           = JPH::Vec3(0.0f, 1.0f, 0.0f);

        EmissiveSurface large = small;
        large.areaSquareMetres = 5.0f;
        large.strength        = 0.35f;

        const ImportOptions            options;
        const ZHLN::BSP::BspPointLight smallLight = ZHLN::BSP::EmissiveLightFor(small, options);
        const ZHLN::BSP::BspPointLight largeLight = ZHLN::BSP::EmissiveLightFor(large, options);

        if (!ZHLN::Test::ExpectTrue(smallLight.intensity > 0.0f) || !ZHLN::Test::ExpectTrue(largeLight.intensity > 0.0f)) {
            return Fail();
        }
        // The proxy's intensity is the surface's total emitted flux, so the big
        // dim wall out-shines the small bright panel: 5 m2 * 0.35 against
        // 0.05 m2 * 0.9. That is the point of modelling it as an area light --
        // the renderer's intensity/distance sort then treats a wall and a lamp
        // panel as different kinds of emitter, which is what they are.
        if (!ZHLN::Test::ExpectTrue(largeLight.intensity > smallLight.intensity)) {
            return Fail();
        }
        // The range follows the footprint instead, so the wall reaches further
        // than the panel even though the panel is brighter per square metre.
        if (!ZHLN::Test::ExpectTrue(largeLight.range > smallLight.range)) {
            return Fail();
        }
        // The proxy is offset off the surface along its normal, never buried in
        // the geometry it is lighting.
        if (!ZHLN::Test::ExpectTrue(smallLight.position.GetY() > small.position.GetY())) {
            return Fail();
        }
        return {};
    }

    // ---- the guarantee ---------------------------------------------------
    // This is the whole point: an empty scene must never come back empty. The
    // guarantee adds an ambient floor and, when the map genuinely has no
    // emitter at all, a fill light -- so "I replaced VRAD" degrades to "dim"
    // rather than to "black".
    [[nodiscard]] auto enforce_lighting_floor_never_returns_black() const -> std::expected<void, ZHLN::ErrorCode> {
        BSPMap map;
        map.version = 20;
        map.entities.push_back(MakeEntity({{"classname", "worldspawn"}}));

        ImportedMapData data;
        LightingDiagnostics diagnostics;
        const ImportOptions options;

        // Nothing at all: the floor fires and the scene is no longer black.
        const bool intervened = ZHLN::BSP::EnforceLightingFloor(data, map, options, diagnostics);
        if (!ZHLN::Test::ExpectTrue(intervened) || !ZHLN::Test::ExpectTrue(diagnostics.floorEnforced)) {
            return Fail();
        }
        if (!ZHLN::Test::ExpectTrue(data.ambient.authored) ||
            !ZHLN::Test::ExpectTrue(data.ambient.color.GetX() + data.ambient.color.GetY() + data.ambient.color.GetZ() > 0.0f) ||
            !ZHLN::Test::ExpectTrue(data.lights.empty() || data.lights[0].intensity > 0.0f)) {
            return Fail();
        }

        // A map that already has lights and an authored ambient is left alone:
        // the guarantee is a floor, not a re-light.
        ImportedMapData lit;
        ZHLN::BSP::BspPointLight light;
        light.type      = ZHLN::LightType::Point;
        light.position  = JPH::Vec3(0.0f, 2.0f, 0.0f);
        light.intensity = 5.0f;
        light.range     = 8.0f;
        lit.lights.push_back(light);
        lit.ambient.authored  = true;
        lit.ambient.color     = JPH::Vec3(0.1f, 0.1f, 0.1f);
        // Above the ambient floor: irradiance = luma * brightness/30 must
        // exceed kAmbientFloorRadiance, so a brightness of 10 is the minimum.
        lit.ambient.intensity = 10.0f;

        LightingDiagnostics litDiagnostics;
        const bool litIntervened = ZHLN::BSP::EnforceLightingFloor(lit, map, options, litDiagnostics);
        if (!ZHLN::Test::ExpectTrue(!litIntervened) || !ZHLN::Test::ExpectTrue(litDiagnostics.floorEnforced == false)) {
            return Fail();
        }
        if (!ZHLN::Test::ExpectEq(lit.lights.size(), 1u)) {
            return Fail();
        }
        return {};
    }

    // ---- audit exhaustiveness --------------------------------------------
    // Every light the entity lump offered leaves through exactly one counter.
    // If these stop summing, the log line stops being able to explain a dark
    // scene and this test is the thing that notices.
    [[nodiscard]] auto audit_lighting_counters_are_exhaustive() const -> std::expected<void, ZHLN::ErrorCode> {
        BSPMap map;
        map.version = 20;
        for (int i = 0; i < 10; ++i) {
            map.entities.push_back(MakeLight("light", "255 255 255 200", "0 0 64"));
        }
        for (int i = 0; i < 3; ++i) {
            map.entities.push_back(MakeLight("light_spot", "255 255 255 200", "0 0 64"));
        }
        for (int i = 0; i < 5; ++i) {
            map.entities.push_back(MakeEntity({
                {"classname", "point_spotlight"},
                {"origin", "0 0 40"},
                {"spawnflags", "3"},
                {"spotlightlength", "50"},
            }));
        }
        map.entities.push_back(MakeLight("prop_dynamic", "255 255 255 200", "0 0 64"));
        map.entities.push_back(MakeLight("light", "0 0 0 0", "0 0 64")); // black: suppressed
        map.entities.push_back(MakeLight("light", "255 255 255 0", "0 0 64")); // zero brightness: suppressed

        const ImportOptions                            options;
        const std::vector<ZHLN::BSP::BspPointLight>    lights = ZHLN::BSP::GatherEntityLights(map, options);
        const LightingDiagnostics                      audit  = ZHLN::BSP::AuditLighting(lights, {}, AmbientTerm {}, map);

        // 10 light + 3 light_spot + 5 point_spotlight + 2 more `light`.
        if (!ZHLN::Test::ExpectEq(audit.entityLightsOffered, 20u)) {
            return Fail();
        }
        if (!ZHLN::Test::ExpectEq(audit.entityLightsEmitted, lights.size())) {
            return Fail();
        }
        if (!ZHLN::Test::ExpectEq(audit.droppedBeamCone, 5u)) {
            return Fail();
        }
        // offered - beam cones - emitted == everything else that was suppressed
        const uint32_t suppressed = audit.entityLightsOffered - audit.droppedBeamCone - audit.entityLightsEmitted;
        if (!ZHLN::Test::ExpectEq(suppressed, 2u)) {
            return Fail();
        }
        return {};
    }

    // ---- options ---------------------------------------------------------
    // The switches exist so a caller can trade the extra light count for the
    // extra import time; each one has to actually turn something off.
    [[nodiscard]] auto options_gate_each_stage() const -> std::expected<void, ZHLN::ErrorCode> {
        BSPMap map;
        map.version = 20;
        map.entities.push_back(MakeLight("light", "255 255 255 200", "0 0 64"));
        map.entities.push_back(MakeEntity({
            {"classname", "light_environment"},
            {"origin", "0 0 0"},
            {"angles", "-62 87 0"},
            {"_light", "82 139 226 20"},
            {"_ambient", "64 95 166 10"},
        }));
        // One light with an authored cutoff, so the unit scale has something
        // to move: a derived range is in engine metres and does not scale.
        map.entities.push_back(MakeEntity({
            {"classname", "light"},
            {"origin", "0 0 64"},
            {"_light", "255 255 255 200"},
            {"_zero_percent_distance", "256"},
        }));

        // The switches are ImportOptions fields; the defaults have to be "on",
        // or a caller who never touches them gets the dark import back.
        const ImportOptions defaults;
        if (!ZHLN::Test::ExpectTrue(defaults.gatherLights) || !ZHLN::Test::ExpectTrue(defaults.deriveAmbient) ||
            !ZHLN::Test::ExpectTrue(defaults.gatherEmissiveSurfaces) || !ZHLN::Test::ExpectTrue(defaults.enforceLightingFloor) ||
            !ZHLN::Test::ExpectEq(defaults.maxEmissiveLights, 192u)) {
            return Fail();
        }

        // The harvest helpers themselves are pure functions of the map: the
        // gate is ImportMapGeometry's, and turning the flags off there is what
        // skips the work (see ImportMapGeometry). What the helpers do guarantee
        // is that they read the same answer whatever the other options say.
        ImportOptions tweaked;
        tweaked.unitScale             = 1.0f / 52.0f;
        tweaked.convertCoordinates    = false;
        tweaked.gatherLights          = false;
        tweaked.deriveAmbient         = false;
        tweaked.gatherEmissiveSurfaces = false;
        tweaked.enforceLightingFloor  = false;
        // One `light`, one `light_environment`, one `light` with an authored
        // cutoff.
        if (!ZHLN::Test::ExpectEq(ZHLN::BSP::GatherEntityLights(map, tweaked).size(), 3u) ||
            !ZHLN::Test::ExpectTrue(ZHLN::BSP::ReadEnvironmentAmbient(map).authored)) {
            return Fail();
        }

        // The unit scale moves authored distances: `_zero_percent_distance
        // 256` is 6.5 m at Source's 39.37 inches per metre and 13.0 m at half
        // that. A derived range is already in engine metres and does not move.
        const ImportOptions            fineOptions;
        const std::vector<ZHLN::BSP::BspPointLight> fine   = ZHLN::BSP::GatherEntityLights(map, fineOptions);
        ImportOptions                             coarse = fineOptions;
        coarse.unitScale                                        = 1.0f / 19.685f;
        const std::vector<ZHLN::BSP::BspPointLight> scaled = ZHLN::BSP::GatherEntityLights(map, coarse);
        if (!ZHLN::Test::ExpectEq(fine.size(), scaled.size())) {
            return Fail();
        }
        bool sawScaled = false;
        bool sawFixed  = false;
        for (size_t i = 0; i < fine.size(); ++i) {
            if (fine[i].range > 0.0f && std::abs(scaled[i].range - fine[i].range) > 0.01f) {
                // The authored cutoff doubled.
                if (!ZHLN::Test::ExpectTrue(scaled[i].range > fine[i].range * 1.9f)) {
                    return Fail();
                }
                sawScaled = true;
            } else {
                sawFixed = true;
            }
        }
        if (!ZHLN::Test::ExpectTrue(sawScaled) || !ZHLN::Test::ExpectTrue(sawFixed)) {
            return Fail();
        }
        return {};
    }
};

} // namespace

auto main() -> int {
    BSPLightingTestSuite suite;

    int passed = 0;
    int failed = 0;

    auto runTest = [&](std::string_view testName, auto testFunc) {
        ZHLN::Println("\033[32m[ RUN      ]\033[0m {}", testName);
        auto res = testFunc();
        if (res.has_value()) {
            ZHLN::Println("\033[32m[       OK ]\033[0m {}", testName);
            passed++;
        } else {
            ZHLN::Println("\033[31m[  FAILED  ]\033[0m {}", testName);
            failed++;
        }
    };

    ZHLN::Println("\033[36m==================================================\033[0m");
    ZHLN::Println("\033[36mRunning Test Suite: BSPLightingTestSuite\033[0m");
    ZHLN::Println("\033[36m==================================================\033[0m");

    runTest("classify_light_entity_families", [&] { return suite.classify_light_entity_families(); });
    runTest("parse_source_light_value_defaults_brightness", [&] { return suite.parse_source_light_value_defaults_brightness(); });
    runTest("attenuation_range_from_brightness", [&] { return suite.attenuation_range_from_brightness(); });
    runTest("gather_entity_lights_skips_disabled_beam_cones", [&] { return suite.gather_entity_lights_skips_disabled_beam_cones(); });
    runTest("gather_entity_lights_places_beam_cone_at_beam_end", [&] { return suite.gather_entity_lights_places_beam_cone_at_beam_end(); });
    runTest("environment_sun_and_ambient", [&] { return suite.environment_sun_and_ambient(); });
    runTest("is_emissive_material_name_families", [&] { return suite.is_emissive_material_name_families(); });
    runTest("probe_emissive_material_damps_near_white", [&] { return suite.probe_emissive_material_damps_near_white(); });
    runTest("finalize_emissive_surfaces_clusters_and_caps", [&] { return suite.finalize_emissive_surfaces_clusters_and_caps(); });
    runTest("finalize_emissive_surfaces_damps_nearby_entity_lights", [&] { return suite.finalize_emissive_surfaces_damps_nearby_entity_lights(); });
    runTest("emissive_light_for_scales_with_area", [&] { return suite.emissive_light_for_scales_with_area(); });
    runTest("enforce_lighting_floor_never_returns_black", [&] { return suite.enforce_lighting_floor_never_returns_black(); });
    runTest("audit_lighting_counters_are_exhaustive", [&] { return suite.audit_lighting_counters_are_exhaustive(); });
    runTest("options_gate_each_stage", [&] { return suite.options_gate_each_stage(); });

    ZHLN::Println("--------------------------------------------------");
    ZHLN::Println("Summary for BSPLightingTestSuite: {} Passed, {} Failed", passed, failed);
    ZHLN::Println("==================================================");

    return failed == 0 ? 0 : 1;
}
