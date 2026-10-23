// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// plugins/BSP/BSPLighting.cpp
//
// See BSPLighting.hpp for the rule list. Everything here is deterministic and
// engine-free: given the same BSPMap and ImportOptions it produces the same
// lights, which is what makes the diagnostics trustworthy.

#include "BSPLighting.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <span>
#include <unordered_map>
#include <utility>

namespace ZHLN::BSP {

// Internal helpers. Everything the header declares is defined below at
// namespace scope; only the file-local utilities live in the anonymous
// namespace.
namespace {

constexpr float kPi = 3.14159265358979323846f;

// Source bspflags.h. Only the bits the lighting pass reads.

// Spawnflag bits, per class. The light family reuses bit 0 for opposite
// meanings, so each class gets its own spelling.
constexpr int32_t kSfLightStartDisabled      = 1; // light / light_spot
constexpr int32_t kSfSpotlightStartOn        = 1; // point_spotlight
constexpr int32_t kSfSpotlightNoDynamicLight = 2; // point_spotlight
constexpr int32_t kSfProjectedTextureEnabled = 1; // env_projectedtexture

// point_spotlight's beam defaults, from its FGD: a mapper who left the keys
// out still gets Source's own numbers.
constexpr float kSpotlightDefaultWidthInches  = 50.0f;
constexpr float kSpotlightDefaultLengthInches = 500.0f;

// env_projectedtexture's default cone, in degrees.
constexpr float kProjectedTextureDefaultFovDegrees = 90.0f;

// Ambient floor the guarantee enforces, in engine sky-radiance terms. The
// engine's own default skyZenith is (0.003, 0.008, 0.020); anything at or below
// that is what an unlit room looks like, so the floor sits just above it.

// The synthesised fill light, in engine intensity units, and its throw.

[[nodiscard]] auto ToLower(std::string_view text) -> std::string {
    std::string out;
    out.reserve(text.size());
    for (const char c: text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

[[nodiscard]] auto ContainsIgnoreCase(std::string_view text, std::string_view needle) noexcept -> bool {
    if (needle.empty() || text.size() < needle.size()) {
        return false;
    }
    const std::string haystack = ToLower(text);
    const std::string target   = ToLower(needle);
    return haystack.find(target) != std::string::npos;
}

[[nodiscard]] auto KeyFloat(const BSPEntity& entity, std::string_view key, float fallback) noexcept -> float {
    return entity.FindFloat(key).value_or(fallback);
}

// Source parses "0" and "" the same way for floats, so a present-but-zero
// override has to stay distinguishable from an absent one.
[[nodiscard]] auto KeyFloatPresent(const BSPEntity& entity, std::string_view key, float& out) noexcept -> bool {
    if (const auto value = entity.FindFloat(key)) {
        out = *value;
        return true;
    }
    return false;
}

// The luminance the renderer itself uses for light-importance sorting, so the
// emissive ranking and the engine agree on what "bright" means.
[[nodiscard]] auto Rec709Luma(std::span<const float, 3> rgb) noexcept -> float {
    return rgb[0] * 0.2126f + rgb[1] * 0.7152f + rgb[2] * 0.0722f;
}

// Reads up to four whitespace-separated floats out of a keyvalue. Returns how
// many were read, which is what tells a 3-component legacy `_light`
// ("119 49 47") from a proper 4-component one.
[[nodiscard]] auto ParseFloats(std::string_view text, std::span<float, 4> out) noexcept -> size_t {
    size_t      count = 0;
    const char* cur   = text.data();
    const char* end   = text.data() + text.size();
    while (cur < end && count < out.size()) {
        while (cur < end && (*cur == ' ' || *cur == '\t')) {
            ++cur;
        }
        if (cur >= end) {
            break;
        }
        auto [ptr, ec] = std::from_chars(cur, end, out[count]);
        if (ec != std::errc {}) {
            break;
        }
        cur = ptr;
        ++count;
    }
    return count;
}

// Source's angle triple -> unit direction, in Source's Z-up space with pitch
// positive down.
[[nodiscard]] auto AngleDirection(std::array<float, 3> angles) noexcept -> JPH::Vec3 {
    const float pitch = angles[0] * (kPi / 180.0f);
    const float yaw   = angles[1] * (kPi / 180.0f);
    const float cp    = std::cos(pitch);
    return JPH::Vec3(cp * std::cos(yaw), cp * std::sin(yaw), -std::sin(pitch));
}

// The pitch/yaw pair, honouring `pitch` as an override for `angles`' X lane
// (Hammer writes both for light_spot and point_spotlight).
[[nodiscard]] auto EntityAngles(const BSPEntity& entity) noexcept -> std::array<float, 3> {
    auto angles = entity.FindVector("angles").value_or(std::array<float, 3> {0.0f, 0.0f, 0.0f});
    if (const auto pitch = entity.FindFloat("pitch")) {
        angles[0] = *pitch;
    }
    return angles;
}

// ---------------------------------------------------------------------------
// Light entity classification
// ---------------------------------------------------------------------------

// Classnames this pass reads, and the kind each one is. The list is
// deliberately wider than "the three the old code knew": a compiled map
// carries several light families, and a mapper only has to use one of them for
// the old code to see nothing.
//
//   light                  point, `_light` colour + brightness
//   light_spot             spot, `_light`, `_cone` / `_inner_cone`
//   light_environment      sun + ambient, `_light` / `_ambient`
//   point_spotlight        beam sprite; light at the beam END when the
//                          "No Dynamic Light" flag is clear
//   light_dynamic          CS:GO's toggleable light; `_light` + `distance`
//   env_projectedtexture   projected-texture spot; `lightcolor` + `lightfov`
//
// Deliberately absent: `env_fog_controller` (volumetrics, not a light),
// `info_lighting` (an entity-side hint for VRAD with no runtime light of its
// own), and `light_spot`-shaped decoys some mods ship.
} // namespace

// Classnames this pass reads, and the kind each one is. The list is
// deliberately wider than "the three the old code knew": a compiled map
// carries several light families, and a mapper only has to use one of them for
// the old code to see nothing.
//
//   light                  point, `_light` colour + brightness
//   light_spot             spot, `_light`, `_cone` / `_inner_cone`
//   light_environment      sun + ambient, `_light` / `_ambient`
//   point_spotlight        beam sprite; light at the beam END when the
//                          "No Dynamic Light" flag is clear
//   light_dynamic          CS:GO's toggleable light; `_light` + `distance`
//   env_projectedtexture   projected-texture spot; `lightcolor` + `lightfov`
//
// `BeamCone` is the interesting one: Source's point_spotlight draws a beam
// sprite and *optionally* a light, and the light lives at the END of the beam,
// not at the origin.
// `rendercolor` + `renderamt` (+ `HDRColorScale`): the CS:GO / GMod spelling
// used by point_spotlight, env_projectedtexture and the *_dynamic family.
auto ParseRenderColorValue(const BSPEntity& entity) noexcept -> SourceLightValue {
    SourceLightValue out;
    // `rendercolor` is colour255; `lightcolor` is colour32. Both carry the RGB
    // in the first three lanes.
    const auto rgb = entity.FindVector("rendercolor").or_else([&] { return entity.FindVector("lightcolor"); });
    if (rgb) {
        out.rgba[0] = (*rgb)[0];
        out.rgba[1] = (*rgb)[1];
        out.rgba[2] = (*rgb)[2];
    }
    // `renderamt` is the brightness in the same 0-255 space as `_light`'s fourth
    // component; `lightcolor`'s alpha lane is the same thing.
    const auto brightness = entity.FindFloat("renderamt").or_else([&] { return entity.FindFloat("lightcoloralpha"); });
    if (brightness) {
        out.rgba[3]       = *brightness;
        out.hasBrightness = true;
    }
    out.rgba[3] *= std::max(entity.FindFloat("HDRColorScale").value_or(1.0f), 0.0f);
    return out;
}
auto ClassifyLightEntity(std::string_view className) noexcept -> LightEntityKind {
    if (className == "light") {
        return LightEntityKind::Point;
    }
    if (className == "light_spot") {
        return LightEntityKind::Spot;
    }
    if (className == "light_environment") {
        return LightEntityKind::Sun;
    }
    if (className == "point_spotlight") {
        return LightEntityKind::BeamCone;
    }
    if (className == "env_projectedtexture") {
        // A projected texture: always a cone, `lightfov` is its angle.
        return LightEntityKind::Spot;
    }
    if (className == "light_dynamic") {
        // CS:GO's toggleable light. Hammer authors it as a spot, but its only
        // keyvalues are `_light` and `distance` -- there is no cone to read --
        // so it is a point light with a switch.
        return LightEntityKind::Point;
    }
    return LightEntityKind::NotALight;
}

// ---------------------------------------------------------------------------
// Attenuation
// ---------------------------------------------------------------------------

auto ParseSourceAttenuation(const BSPEntity& entity) noexcept -> SourceAttenuation {
    SourceAttenuation atten;
    atten.constant  = KeyFloat(entity, "_constant_attn", 0.0f);
    atten.linear    = KeyFloat(entity, "_linear_attn", 0.0f);
    atten.quadratic = KeyFloat(entity, "_quadratic_attn", 1.0f);

    float value = 0.0f;
    if (KeyFloatPresent(entity, "_fifty_percent_distance", value)) {
        atten.fiftyPercentDistance = value;
    }
    // `_distance` and the legacy `_falloff` are the zero-percent distance under
    // two other names; the newest spelling wins.
    if (KeyFloatPresent(entity, "_zero_percent_distance", value)) {
        atten.zeroPercentDistance = value;
    } else if (KeyFloatPresent(entity, "_distance", value)) {
        atten.zeroPercentDistance = value;
    } else if (KeyFloatPresent(entity, "_falloff", value)) {
        atten.zeroPercentDistance = value;
    }

    atten.hardFalloff = KeyFloat(entity, "_hardfalloff", 0.0f) > 0.5f;
    if (atten.quadratic <= 0.0f && atten.linear <= 0.0f && atten.constant <= 0.0f) {
        // Nothing usable: fall back to Source's default inverse-square window
        // rather than dividing by zero.
        atten.quadratic = 1.0f;
    }
    return atten;
}

auto AttenuationRangeMetres(const SourceAttenuation& atten, float engineIntensity, float unitScale) noexcept -> float {
    if (engineIntensity <= 0.0f) {
        return kMinRangeMetres;
    }

    // 1. An authored cutoff always wins: it is the mapper's own answer, and
    //    `_zero_percent_distance` / `_distance` are real inch measurements.
    if (atten.zeroPercentDistance > 0.0f) {
        return std::clamp(atten.zeroPercentDistance * unitScale, kMinRangeMetres, kMaxRangeMetres);
    }

    // 2. Hammer's "fifty percent distance" is the softer spelling of the same
    //    intent: the window sits at 0.5 there, so the cutoff lands sqrt(2)
    //    further out.
    if (atten.fiftyPercentDistance > 0.0f) {
        const float halfMetres = std::max(atten.fiftyPercentDistance, 1.0f) * unitScale;
        return std::clamp(halfMetres * 1.41421356f, kMinRangeMetres, kMaxRangeMetres);
    }

    // 3. Otherwise derive from the window the renderer actually evaluates:
    //    LightDesc's falloff is atten = intensity/(d^2 + 1) with d in metres,
    //    so the surviving fraction drops under kAttenuationCutoff at
    //    d = sqrt(intensity/cutoff - 1). Source's `_constant_attn` /
    //    `_linear_attn` / `_quadratic_attn` coefficients are deliberately not
    //    used for this -- see kAttenuationCutoff's comment for why. Only
    //    `_hardfalloff`, which is a shape statement rather than a scale, still
    //    applies.
    float metres = std::sqrt(std::max(engineIntensity / kAttenuationCutoff, 1.0f) - 1.0f);
    if (atten.hardFalloff) {
        metres *= 0.75f;
    }
    return std::clamp(metres, kMinRangeMetres, kMaxRangeMetres);
}

// ---------------------------------------------------------------------------
// Light values
// ---------------------------------------------------------------------------

auto ParseSourceLightValue(const BSPEntity& entity) noexcept -> SourceLightValue {
    SourceLightValue out;

    std::string_view text = entity.Find("_light");
    if (text.empty()) {
        text = entity.Find("light");
    }
    if (!text.empty()) {
        if (const size_t count = ParseFloats(text, out.rgba); count >= 3) {
            // A 3-component legacy value has no brightness lane; keep the
            // default rather than reading a zero into the intensity.
            out.hasBrightness = (count >= 4);
        }
    }

    // The HDR override. vrad writes "-1 -1 -1 1" when the map has none and a
    // real colour plus a `_lightscaleHDR` multiplier when it does.
    const std::string_view hdr = entity.Find("_lightHDR");
    if (!hdr.empty()) {
        std::array<float, 4> parsed {};
        if (const size_t count = ParseFloats(hdr, parsed); count >= 3 && parsed[0] >= 0.0f && parsed[1] >= 0.0f && parsed[2] >= 0.0f) {
            const float scale = std::max(entity.FindFloat("_lightscaleHDR").value_or(1.0f), 0.0f);
            out.rgba[0]       = parsed[0];
            out.rgba[1]       = parsed[1];
            out.rgba[2]       = parsed[2];
            if (count >= 4) {
                out.rgba[3] = parsed[3];
            }
            out.rgba[3] *= scale;
            out.hasBrightness = true;
        }
    }

    return out;
}

// ---------------------------------------------------------------------------
// Ambient
// ---------------------------------------------------------------------------

auto ReadEnvironmentAmbient(const BSPMap& map) noexcept -> AmbientTerm {
    for (const BSPEntity& entity: map.entities) {
        if (entity.Find("classname") != "light_environment") {
            continue;
        }
        AmbientTerm out;
        out.authored = true;

        std::string_view text = entity.Find("_ambient");
        if (text.empty()) {
            text = entity.Find("_ambientHDR");
        }
        std::array<float, 4> rgba {255.0f, 255.0f, 255.0f, 0.0f};
        if (!text.empty()) {
            (void) ParseFloats(text, rgba);
        }
        const float scale = std::max(entity.FindFloat("_AmbientScaleHDR").value_or(1.0f), 0.0f);
        out.color     = JPH::Vec3(std::clamp(rgba[0] / 255.0f, 0.0f, 1.0f), std::clamp(rgba[1] / 255.0f, 0.0f, 1.0f), std::clamp(rgba[2] / 255.0f, 0.0f, 1.0f));
        out.intensity = std::max(rgba[3], 0.0f) * scale;
        return out; // one light_environment per map; the first one wins
    }
    return {};
}

auto EnvironmentSunDirection(const BSPMap& map) noexcept -> JPH::Vec3 {
    for (const BSPEntity& entity: map.entities) {
        if (entity.Find("classname") != "light_environment") {
            continue;
        }
        return AngleDirection(EntityAngles(entity));
    }
    // No light_environment at all: fall back to a 45-degree elevation sun from
    // the +X/+Y quadrant rather than returning a zero vector, which the engine
    // would normalise into NaN and silently delete the directional term.
    return JPH::Vec3(0.5f, 0.5f, 1.0f).Normalized();
}

// ---------------------------------------------------------------------------
// Emissive materials
// ---------------------------------------------------------------------------

// Name families that mean "this surface is its own light source" in Source
// content. Each entry is (token, base strength). The strength is the authored
// self-illumination the family implies when the .vmt does not say otherwise; it
// sits below 1 for the diffuse-ish families so a poster does not read as a lamp.
struct EmissiveNameFamily {
    std::string_view token;
    float            strength;
};

inline constexpr EmissiveNameFamily kEmissiveNameFamilies[] = {
    // Valve's own lamp-panel textures. lights/white is the classic ceiling wash
    // and lights/white002 backs the UnlitGeneric WHITE007 panel.
    {"lights/", 0.85f},
    {"light_", 0.70f},
    // Screen / display families: MonitorScreen and the effects/combinedisplay
    // set are animated emissive surfaces.
    {"screen", 0.90f},
    {"display", 0.90f},
    {"monitor", 0.90f},
    // Signs: backlit panels -- brighter than a poster, not a lamp.
    {"sign", 0.80f},
    {"neon", 0.95f},
    {"glow", 0.85f},
    {"lamp", 0.90f},
    {"bulb", 0.95f},
    {"led", 0.85f},
    {"strip", 0.70f},
    {"emissive", 1.00f},
    {"selfillum", 1.00f},
    // The literal material names this map ships.
    {"white007", 1.00f},
};

auto IsEmissiveMaterialName(std::string_view materialName) noexcept -> bool {
    const std::string lowered = ToLower(materialName);
    for (const EmissiveNameFamily& family: kEmissiveNameFamilies) {
        if (lowered.find(family.token) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// `$selfillumtint` / `$color2` / `$color`, all of which Source spells as
// "[r g b]" 0-1 or "{r g b}" 0-255.
auto VmtTint(const VMTMaterial& vmt) noexcept -> JPH::Vec3 {
    for (const std::string_view key: {"$selfillumtint", "$color2", "$color"}) {
        const auto found = vmt.Find(key);
        if (!found || found->empty()) {
            continue;
        }
        std::array<float, 4> rgb {1.0f, 1.0f, 1.0f, 1.0f};
        if (ParseFloats(*found, rgb) == 3) {
            const float divisor = (found->front() == '{') ? 255.0f : 1.0f;
            return JPH::Vec3(std::clamp(rgb[0] / divisor, 0.0f, 4.0f), std::clamp(rgb[1] / divisor, 0.0f, 4.0f), std::clamp(rgb[2] / divisor, 0.0f, 4.0f));
        }
    }
    return JPH::Vec3::sReplicate(1.0f);
}

auto ProbeEmissiveMaterial(std::string_view materialName, float reflectivityLuma, const VMTMaterial* vmt, bool noLightFlag) noexcept -> EmissiveMaterialProbe {
    EmissiveMaterialProbe out;

    // 1. The name family is the gate; everything else only modulates it.
    const std::string lowered  = ToLower(materialName);
    float             strength = 0.0f;
    for (const EmissiveNameFamily& family: kEmissiveNameFamilies) {
        if (lowered.find(family.token) != std::string::npos) {
            strength = std::max(strength, family.strength);
        }
    }

    // 2. The .vmt, when it resolves, is authoritative about the shader class and
    //    about any authored self-illumination. vbsp's SURF_NOLIGHT texinfo flag
    //    is the same statement from the compiler's side: it is set on a surface
    //    the mapper told vbsp not to bake light onto, which is exactly "this
    //    surface is its own light". It is the only per-surface evidence in the
    //    whole .bsp, so it disables the reflectivity damping below.
    bool authorsSelfIllum = noLightFlag;
    if (vmt != nullptr) {
        strength = std::max(strength, ShaderEmissiveStrength(vmt->shader));

        // `$selfillum` is either a bool or a mask-texture strength; both mean
        // "this surface emits".
        if (const auto found = vmt->Find("$selfillum"); found && !found->empty() && *found != "0") {
            strength         = std::max(strength, 1.0f);
            authorsSelfIllum = true;
        }
        // `$emissiveBlend*` is the Source 2013 MP spelling of the same thing.
        if (const auto found = vmt->Find("$emissiveblendstrength"); found && !found->empty()) {
            if (const float parsed = std::strtof(std::string(*found).c_str(), nullptr); parsed > 0.0f) {
                strength         = std::max(strength, std::clamp(parsed, 0.0f, 1.0f));
                authorsSelfIllum = true;
            }
        }
        if (vmt->Find("$emissiveblend").has_value()) {
            strength         = std::max(strength, 0.9f);
            authorsSelfIllum = true;
        }
        // `$selfillummaskscale` multiplies the mask, so a mapper who set it has
        // authored emission even when `$selfillum` is a bare 1.
        if (const auto found = vmt->Find("$selfillummaskscale"); found && !found->empty()) {
            if (const float parsed = std::strtof(std::string(*found).c_str(), nullptr); parsed > 0.0f) {
                strength = std::max(strength, 0.8f);
            }
        }

        out.tint = VmtTint(*vmt);
    }

    if (strength <= 0.0f) {
        return out; // not an emissive family, or a family that authors nothing
    }

    // 3. The reflectivity gate. A material that reflects almost everything is a
    //    lit surface wearing an emissive name -- `lights/white` with texdata
    //    reflectivity (1,1,1) is the sky wash, not a lamp. A material that
    //    reflects almost nothing cannot be lit at all, so whatever brightness it
    //    shows is its own emission.
    // Only when nothing else has vouched for the emission: a surface that
    // authors it (a `$selfillum` key, or vbsp's SURF_NOLIGHT flag) keeps its
    // full family strength no matter how bright its albedo is, because the
    // albedo is what the emission is made of.
    if (!authorsSelfIllum) {
        if (reflectivityLuma > 0.75f) {
            strength *= 0.1f;
        } else if (reflectivityLuma > 0.55f) {
            strength *= 0.45f;
        }
    }

    out.isEmissive = strength > 0.01f;
    out.strength   = out.isEmissive ? std::clamp(strength, 0.0f, 1.0f) : 0.0f;
    if (Rec709Luma(out.tint) <= 0.0f) {
        out.tint = JPH::Vec3::sReplicate(1.0f);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Entity light harvesting
// ---------------------------------------------------------------------------

// Fills the shared position/direction/cone/range fields and applies the
// calibration. Returns false when the light should not be emitted at all.
auto BuildLight(const BSPEntity& entity, const ImportOptions& options, LightEntityKind kind, const SourceLightValue& value, bool useRenderColor)
    -> std::optional<BspPointLight> {
    BspPointLight light;
    light.type      = (kind == LightEntityKind::Sun) ? LightType::Sun : ((kind == LightEntityKind::Spot) ? LightType::Spot : LightType::Point);
    light.name      = std::string(entity.Find("targetname"));
    light.className = std::string(entity.Find("classname"));
    if (light.name.empty()) {
        light.name = light.className;
    }

    const auto originArr = entity.FindVector("origin").value_or(std::array<float, 3> {0.0f, 0.0f, 0.0f});
    light.position       = ConvertPosition(options, JPH::Vec3(originArr[0], originArr[1], originArr[2]));

    // Colour: Source stores 0-255 bytes; the engine wants linear 0-1.
    light.color = JPH::Vec3(
        std::clamp(value.rgba[0] / 255.0f, 0.0f, 1.0f), std::clamp(value.rgba[1] / 255.0f, 0.0f, 1.0f), std::clamp(value.rgba[2] / 255.0f, 0.0f, 1.0f)
    );

    // Brightness: the point/spot constant, the sun constant for the
    // directional term. A light with no brightness lane at all (legacy
    // 3-component `_light`) keeps Source's own default of 200 rather than
    // becoming invisible.
    const float brightness = value.hasBrightness ? value.rgba[3] : 200.0f;
    const float scale      = (light.type == LightType::Sun) ? kSourceSunBrightnessToEngine : kSourceBrightnessToEngine;
    light.intensity        = std::max(brightness, 0.0f) * scale;

    if (light.type == LightType::Point || light.type == LightType::Spot) {
        if (Rec709Luma(light.color) <= 0.0f || light.intensity <= 0.0f) {
            return std::nullopt; // black or switched off: nothing to emit
        }
    }

    light.direction = ConvertDirection(options, AngleDirection(EntityAngles(entity)));

    // Spawnflags: "start disabled" for the light family, "start on" for the
    // beam cones and the projected textures.
    light.spawnFlags = entity.FindInt("spawnflags").value_or(0);
    if (light.className == "light" || light.className == "light_spot" || light.className == "light_dynamic") {
        light.startsOn = (light.spawnFlags & kSfLightStartDisabled) == 0;
    } else {
        light.startsOn = (light.spawnFlags & (kSfSpotlightStartOn | kSfProjectedTextureEnabled)) != 0 || light.spawnFlags == 0;
    }
    // A mapper who wrote the alpha/brightness as zero meant "off" whatever the
    // flags say.
    if (value.hasBrightness && value.rgba[3] <= 0.0f) {
        light.startsOn = false;
    }
    if (!light.startsOn) {
        return std::nullopt;
    }

    light.castShadows = KeyFloat(entity, "_castentityshadow", 1.0f) > 0.5f;
    if (const auto shadows = entity.Find("disablereceiveshadows"); !shadows.empty() && shadows != "0") {
        light.castShadows = false;
    }

    // Cone angles, in degrees, for the spot family.
    if (light.type == LightType::Spot) {
        float outerDegrees = 45.0f;
        float innerDegrees = 0.0f;
        if (kind == LightEntityKind::BeamCone) {
            // point_spotlight's beam is a cone of `spotlightwidth` diameter at
            // `spotlightlength` distance, so the half angle is
            // atan(width / 2 / length).
            const float width  = KeyFloat(entity, "spotlightwidth", kSpotlightDefaultWidthInches);
            const float length = std::max(KeyFloat(entity, "spotlightlength", kSpotlightDefaultLengthInches), 1.0f);
            outerDegrees       = std::atan(std::max(width, 0.0f) * 0.5f / length) * (180.0f / kPi);
        } else if (light.className == "env_projectedtexture") {
            outerDegrees = KeyFloat(entity, "lightfov", KeyFloat(entity, "fov", kProjectedTextureDefaultFovDegrees));
        } else {
            outerDegrees = KeyFloat(entity, "_cone", 45.0f);
            innerDegrees = KeyFloat(entity, "_inner_cone", 0.0f);
            if (innerDegrees <= 0.0f) {
                innerDegrees = outerDegrees * (2.0f / 3.0f);
            }
        }
        if (innerDegrees <= 0.0f) {
            innerDegrees = outerDegrees * (2.0f / 3.0f);
        }
        innerDegrees           = std::clamp(innerDegrees, 0.0f, outerDegrees);
        light.outerConeRadians = std::clamp(outerDegrees, 1.0f, 179.0f) * (kPi / 180.0f);
        light.innerConeRadians = std::clamp(innerDegrees, 0.0f, outerDegrees) * (kPi / 180.0f);
    }

    // Range from Source's attenuation window.
    const SourceAttenuation atten = ParseSourceAttenuation(entity);
    light.range                   = AttenuationRangeMetres(atten, light.intensity, options.unitScale);
    if (light.className == "point_spotlight" || light.className == "env_projectedtexture") {
        // Source caps these at the beam length / the projected texture's far
        // plane, whichever the mapper authored.
        const float authored = (light.className == "point_spotlight") ? KeyFloat(entity, "spotlightlength", 0.0f) : KeyFloat(entity, "farz", 0.0f);
        if (authored > 0.0f) {
            light.range = std::min(light.range, authored * options.unitScale);
        }
    }
    // A point light's `radius` is the emitter's physical size; Source has no
    // equivalent, so leave it at 0 and let the packer use its point default.
    light.radius = 0.0f;
    (void) useRenderColor;
    return light;
}

auto GatherEntityLights(const BSPMap& map, const ImportOptions& options) -> std::vector<BspPointLight> {
    std::vector<BspPointLight> out;
    for (const BSPEntity& entity: map.entities) {
        const std::string_view className = entity.Find("classname");
        const LightEntityKind  kind      = ClassifyLightEntity(className);
        if (kind == LightEntityKind::NotALight) {
            continue;
        }

        // --- point_spotlight: the beam cone, and the light at its end --------
        if (kind == LightEntityKind::BeamCone) {
            // Source's CPointSpotlight::SpotlightThink sets the beam-end light's
            // scale to exactly 0 when the "No Dynamic Light" spawnflag is set,
            // and the wiki is explicit that in that state the entity "will not
            // actually illuminate other objects on its own and can be used with
            // an accompanying light_spot or env_projectedtexture instead".
            // gm_murder_drones ships all 743 of them with the flag set, and
            // VRAD's own LUMP_WORLDLIGHTS for that map contains exactly 688
            // point + 538 spot + 1 sun + 1 skylight records -- zero derived
            // from point_spotlight. Emitting one here would double a fixture
            // that already has its own light beside it.
            const int32_t spawnFlags = entity.FindInt("spawnflags").value_or(0);
            if ((spawnFlags & kSfSpotlightNoDynamicLight) != 0) {
                continue;
            }
            // Otherwise the light lands at the end of the beam, not at the
            // origin: Source parents a spotlight_end entity there with
            // m_Radius = spotlightlength.
            const SourceLightValue value = ParseRenderColorValue(entity);
            if (auto light = BuildLight(entity, options, kind, value, true)) {
                const JPH::Vec3 dir    = light->direction;
                const float     length = std::max(KeyFloat(entity, "spotlightlength", kSpotlightDefaultLengthInches), 1.0f);
                light->position        = light->position + dir * (length * options.unitScale);
                light->radius          = std::max(length * options.unitScale, 0.05f);
                out.push_back(std::move(*light));
            }
            continue;
        }

        // --- env_projectedtexture: lightcolor + lightfov ---------------------
        if (className == "env_projectedtexture") {
            const int32_t spawnFlags = entity.FindInt("spawnflags").value_or(0);
            if ((spawnFlags & kSfProjectedTextureEnabled) == 0) {
                continue; // off by default in Source
            }
            const SourceLightValue value = ParseRenderColorValue(entity);
            if (auto light = BuildLight(entity, options, kind, value, true)) {
                out.push_back(std::move(*light));
            }
            continue;
        }

        // --- light / light_spot / light_dynamic / light_environment ----------
        const SourceLightValue value = ParseSourceLightValue(entity);
        if (auto light = BuildLight(entity, options, kind, value, false)) {
            out.push_back(std::move(*light));
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Emissive surface assembly
// ---------------------------------------------------------------------------

auto FinalizeEmissiveSurfaces(std::span<const EmissiveSurface> surfaces, std::span<const BspPointLight> entityLights, float unitScale, uint32_t maxLights)
    -> std::vector<EmissiveSurface> {
    const float cellMetres      = std::max(kEmissiveClusterCellInches * unitScale, 0.1f);
    const float fixtureMetres   = std::max(kEmissiveFixtureRadiusInches * unitScale, 0.1f);
    const float fixtureMetresSq = fixtureMetres * fixtureMetres;
    const float mergeMetres     = std::max(kEmissiveFixtureMergeInches * unitScale, 0.1f);
    const float mergeMetresSq   = mergeMetres * mergeMetres;

    // 1. Cluster: same material, same grid cell, coplanar, and close enough to
    //    be the same fixture. A lamp housing built from many coplanar sheets
    //    becomes one area-light proxy; the next panel down the runway does not,
    //    because a proxy is only allowed to speak for the fixture it sits on.
    //    Merging further inflates the proxy's area -- and with it both its
    //    intensity and its range -- and drags it to the group's centroid, which
    //    is the difference between "a lamp lights the floor under it" and "a
    //    pool of light floats between two lamps".
    struct Bucket {
        std::string material;
        float       areaSquareInches = 0.0f;
        float       areaSquareMetres = 0.0f;
        JPH::Vec3   weighted {0.0f, 0.0f, 0.0f};
        JPH::Vec3   weightedNormal {0.0f, 0.0f, 0.0f};
        JPH::Vec3   color {0.0f, 0.0f, 0.0f};
        float       strength         = 0.0f;
        float       reflectivityLuma = -1.0f;
        bool        noLightFlag      = false;
        uint32_t    faces            = 0;
    };
    std::vector<Bucket>                                 buckets;
    std::unordered_map<std::string, std::vector<size_t>> cells;
    buckets.reserve(surfaces.size());
    cells.reserve(surfaces.size());
    for (const EmissiveSurface& surface: surfaces) {
        const int64_t        cx         = static_cast<int64_t>(std::floor(surface.position.GetX() / cellMetres));
        const int64_t        cy         = static_cast<int64_t>(std::floor(surface.position.GetY() / cellMetres));
        const int64_t        cz         = static_cast<int64_t>(std::floor(surface.position.GetZ() / cellMetres));
        const std::string    key        = surface.materialName + "@" + std::to_string(cx) + "_" + std::to_string(cy) + "_" + std::to_string(cz);
        std::vector<size_t>& candidates = cells[key];

        size_t target     = buckets.size();
        float  bestDistSq = mergeMetresSq;
        for (const size_t index: candidates) {
            const Bucket& existing = buckets[index];
            if (existing.areaSquareMetres <= 0.0f) {
                continue;
            }
            // Compare against the group's running centroid, not its first face:
            // a housing's sheets are consecutive and overlap each other, so the
            // centroid tracks the fixture as it grows.
            const JPH::Vec3 centroid = existing.weighted * (1.0f / existing.areaSquareMetres);
            const float     distSq   = (centroid - surface.position).LengthSq();
            if (distSq >= bestDistSq) {
                continue;
            }
            const JPH::Vec3 normal =
                (existing.weightedNormal.LengthSq() > 1e-8f) ? existing.weightedNormal.Normalized() : JPH::Vec3::sAxisY();
            if (normal.Dot(surface.normal) < kEmissiveFixtureCoplanarCos) {
                continue;
            }
            bestDistSq = distSq;
            target     = index;
        }
        if (target == buckets.size()) {
            buckets.emplace_back();
            buckets.back().material = surface.materialName;
            candidates.push_back(buckets.size() - 1u);
            target = buckets.size() - 1u;
        }

        Bucket& bucket = buckets[target];
        bucket.areaSquareInches += surface.areaSquareInches;
        bucket.areaSquareMetres += surface.areaSquareMetres;
        bucket.weighted         = bucket.weighted + surface.position * surface.areaSquareMetres;
        bucket.weightedNormal   = bucket.weightedNormal + surface.normal * surface.areaSquareMetres;
        bucket.color            = bucket.color + surface.color * surface.areaSquareMetres;
        bucket.strength         = std::max(bucket.strength, surface.strength);
        bucket.reflectivityLuma = surface.reflectivityLuma;
        bucket.noLightFlag      = bucket.noLightFlag || surface.noLightFlag;
        bucket.faces            += surface.faceCount;
    }

    std::vector<EmissiveSurface> out;
    out.reserve(buckets.size());
    for (const Bucket& bucket: buckets) {
        if (bucket.areaSquareMetres <= 0.0f || bucket.faces == 0) {
            continue;
        }
        const float     invArea = 1.0f / bucket.areaSquareMetres;
        EmissiveSurface surface;
        surface.materialName     = bucket.material;
        surface.areaSquareMetres = bucket.areaSquareMetres;
        surface.areaSquareInches = bucket.areaSquareInches;
        surface.position         = bucket.weighted * invArea;
        surface.normal           = (bucket.weightedNormal.LengthSq() > 1e-8f) ? bucket.weightedNormal.Normalized() : JPH::Vec3::sAxisY();
        surface.color            = bucket.color * invArea;
        surface.reflectivityLuma = bucket.reflectivityLuma;
        surface.noLightFlag      = bucket.noLightFlag;
        surface.faceCount        = bucket.faces;
        // The name-family strength and the reflectivity damping. This layer has
        // no .vmt, so this is the "no further evidence" answer; the importer
        // adapter re-probes with the material's own keys and overrides it.
        const EmissiveMaterialProbe probe = ProbeEmissiveMaterial(bucket.material, bucket.reflectivityLuma, nullptr, bucket.noLightFlag);
        surface.strength                  = std::max(bucket.strength, probe.strength);
        surface.color                     = probe.tint;
        out.push_back(std::move(surface));
    }

    // 2. Fixture damping: an emissive panel this close to an entity light is the
    //    housing of that light, and its emission is already accounted for.
    for (EmissiveSurface& surface: out) {
        float bestSq = fixtureMetresSq;
        for (const BspPointLight& light: entityLights) {
            if (light.type == LightType::Sun) {
                continue;
            }
            const float distSq = (light.position - surface.position).LengthSq();
            bestSq             = std::min(bestSq, distSq);
        }
        if (bestSq < fixtureMetresSq) {
            surface.fixtureDamping = kEmissiveFixtureDamping;
        }
    }

    // 3. Rank by strength * area (the panel's total emitted flux) and cap. The
    //    cap is what keeps a 400-sheet fixture from flooding the clustered
    //    lighting pass.
    std::sort(out.begin(), out.end(), [](const EmissiveSurface& a, const EmissiveSurface& b) {
        const float fluxA = a.strength * a.areaSquareMetres * a.fixtureDamping;
        const float fluxB = b.strength * b.areaSquareMetres * b.fixtureDamping;
        if (fluxA != fluxB) {
            return fluxA > fluxB;
        }
        return a.materialName < b.materialName;
    });
    const uint32_t cap = (maxLights > 0u) ? maxLights : kEmissiveMaxLights;
    if (out.size() > cap) {
        out.resize(cap);
    }
    return out;
}

auto EmissiveLightFor(const EmissiveSurface& surface, const ImportOptions& options) -> BspPointLight {
    BspPointLight light;
    light.name      = "Glow_" + surface.materialName;
    light.className = "info_emissive_proxy";
    light.type      = LightType::Point;

    // Push the proxy just off the surface so it is not buried inside the wall
    // it belongs to (a light exactly on a surface self-shadows and reads as a
    // dark blob under RTGI).
    const float offset = std::clamp(std::sqrt(surface.areaSquareMetres) * 0.25f, 0.02f, 0.5f);
    light.position     = surface.position + surface.normal * offset;

    light.color     = surface.color;
    light.direction = -surface.normal;

    // The equivalent-Source-brightness model: a white panel of area A reads as
    // a `light` of brightness (A * kEmissiveSourceBrightnessPerSquareMetre),
    // then goes through the same point/spot calibration the entity lights use.
    const float equivalentBrightness = Rec709Luma(surface.color) * surface.areaSquareMetres * kEmissiveSourceBrightnessPerSquareMetre * surface.strength *
                                       surface.fixtureDamping;
    light.intensity                  = std::max(equivalentBrightness * kSourceBrightnessToEngine, 0.0f);
    light.fromEmissive               = true;

    // Cap: see kEmissiveMaxLightIntensity.
    if (light.intensity > kEmissiveMaxLightIntensity) {
        light.intensity = kEmissiveMaxLightIntensity;
    }

    // Range from the surface's own footprint, not from its intensity: an area
    // light reaches a few times its characteristic radius, so a big dim wall
    // out-reaches a small bright panel. Deriving it from the intensity instead
    // would make a 25 m2 video wall with the intensity cap applied reach less
    // than a 32x32 lamp panel.
    const float characteristic = std::sqrt(std::max(surface.areaSquareMetres, 1e-4f) / kPi);
    light.range                = std::clamp(characteristic * kEmissiveRangeScale, kMinRangeMetres, kMaxRangeMetres);
    light.radius               = std::clamp(characteristic, 0.02f, 2.0f);
    light.castShadows          = false; // a proxy is not the authored occluder
    light.startsOn             = light.intensity > 0.0f;
    return light;
}

// ---------------------------------------------------------------------------
// Diagnostics / guarantee
// ---------------------------------------------------------------------------

auto AuditLighting(std::span<const BspPointLight> entityLights, std::span<const EmissiveSurface> emissive, const AmbientTerm& ambient, const BSPMap& map)
    -> LightingDiagnostics {
    LightingDiagnostics out;
    uint32_t            offered   = 0;
    uint32_t            beamCones = 0;
    for (const BSPEntity& entity: map.entities) {
        const LightEntityKind kind = ClassifyLightEntity(entity.Find("classname"));
        if (kind == LightEntityKind::NotALight) {
            continue;
        }
        ++offered;
        if (kind == LightEntityKind::BeamCone && (entity.FindInt("spawnflags").value_or(0) & kSfSpotlightNoDynamicLight) != 0) {
            ++beamCones;
        }
    }
    // Entity lights only: the emissive proxies are appended to the same vector
    // after this pass has already harvested the entities, so counting the whole
    // vector here would make "offered - dropped" go negative and wrap.
    uint32_t entityEmitted = 0;
    for (const BspPointLight& light: entityLights) {
        if (!light.fromEmissive) {
            ++entityEmitted;
        }
    }
    out.entityLightsOffered = offered;
    out.entityLightsEmitted = entityEmitted;
    out.droppedBeamCone     = beamCones;
    out.droppedDisabled     = (offered > beamCones + entityEmitted) ? (offered - beamCones - entityEmitted) : 0u;
    out.ambientAuthored     = ambient.authored ? 1u : 0u;
    out.emissiveSurfaces    = static_cast<uint32_t>(emissive.size());
    for (const EmissiveSurface& surface: emissive) {
        out.emissiveFaces += surface.faceCount;
        out.emissiveAreaSquareMetres += surface.areaSquareMetres;
        if (surface.fixtureDamping < 1.0f) {
            ++out.emissiveFixtureDamped;
        }
        out.totalEmissiveIntensity += EmissiveLightFor(surface, ImportOptions {}).intensity;
    }
    for (const BspPointLight& light: entityLights) {
        out.totalEntityIntensity += light.intensity;
    }
    return out;
}

auto EnforceLightingFloor(ImportedMapData& data, const BSPMap& map, const ImportOptions& options, LightingDiagnostics& diagnostics) -> bool {
    if (!options.enforceLightingFloor) {
        return false;
    }
    bool intervened = false;

    // 1. Ambient floor. The engine's ambient term is
    //    EvaluateSH(N, frame.sh) * ambientExposure, and frame.sh integrates the
    //    sky gradient -- so a black sky is a black scene no matter how many
    //    point/spot lights it carries. Lift a black or near-black ambient to a
    //    visible floor while keeping whatever hue the mapper did author, so a
    //    night map stays a night map and does not become a grey room.
    const float ambientIrradiance = Rec709Luma(data.ambient.color) * data.ambient.intensity / kAmbientBrightnessLadder;
    if (ambientIrradiance <= kAmbientFloorRadiance) {
        const float hueLuma    = Rec709Luma(data.ambient.color);
        const float hueScale   = (hueLuma > 1e-4f) ? (0.34f / hueLuma) : 0.0f;
        data.ambient.color     = (hueLuma > 1e-4f) ? (data.ambient.color * hueScale) : JPH::Vec3(0.30f, 0.34f, 0.42f);
        data.ambient.intensity = kAmbientFloorRadiance * kAmbientBrightnessLadder / 0.34f;
        data.ambient.authored  = true;
        intervened             = true;
    }

    // 2. Fill light. A map with no entity light, no sun and no emissive surface
    //    renders black under PBR and gives RTGI/DDGI nothing to trace. Drop a
    //    neutral point light at the map's centre so the pipeline has a floor.
    const bool hasLights   = !data.lights.empty();
    const bool hasEmissive = !data.emissive.empty();
    if (!hasLights && !hasEmissive) {
        BspPointLight fill;
        fill.name         = "map_fill_light";
        fill.className    = "info_fill_light";
        fill.fromEmissive = true;
        fill.type         = LightType::Point;
        fill.color        = JPH::Vec3(0.85f, 0.88f, 1.0f);
        fill.intensity    = kFillLightIntensity;
        fill.range        = kFillLightRangeMetres;
        fill.startsOn     = true;
        if (!map.models.empty()) {
            const DModel&   world = map.models.front();
            const JPH::Vec3 mins(world.mins[0], world.mins[1], world.mins[2]);
            const JPH::Vec3 maxs(world.maxs[0], world.maxs[1], world.maxs[2]);
            fill.position = ConvertPosition(options, (mins + maxs) * 0.5f + JPH::Vec3(0.0f, 0.0f, (maxs.GetZ() - mins.GetZ()) * 0.25f));
        }
        data.lights.push_back(std::move(fill));
        intervened = true;
    }

    // 3. Sun floor. A map whose only directional term is a very dim sun still
    //    needs the sky to carry something, or the RTGI/DDGI probes converge on
    //    black and the whole map reads as an interior.
    // 3. Sun floor. A map whose only directional term is a very dim sun still
    //    needs the sky to carry something, or the RTGI/DDGI probes converge on
    //    black and the whole map reads as an interior. Lift the sun itself: the
    //    renderer derives the environment sun's intensity from the ambient
    //    exposure, so a brightened sun carries the sky with it.
    auto sunIt = std::find_if(data.lights.begin(), data.lights.end(), [](const BspPointLight& light) { return light.type == LightType::Sun; });
    if (sunIt != data.lights.end() && sunIt->intensity < kSunFloorIntensity) {
        sunIt->intensity = kSunFloorIntensity;
        intervened       = true;
    }

    diagnostics               = AuditLighting(data.lights, data.emissive, data.ambient, map);
    diagnostics.floorEnforced = intervened;
    data.lighting             = diagnostics;
    return intervened;
}

} // namespace ZHLN::BSP
