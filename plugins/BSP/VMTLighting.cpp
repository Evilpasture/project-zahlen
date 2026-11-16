// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#include "VMTLighting.hpp"
#include "VTFDecoder.hpp"
#include "BSPLighting.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <initializer_list>
namespace ZHLN::BSP {
// Measured Rec.709 luma of a material's base texture, or -1 when it cannot be
// resolved or decoded. Sets how brightly an emissive-by-construction shader
// class glows from its own art; it is never evidence that a lit surface emits.
auto MeasureVmtTextureLuma(const SourceVFS& vfs, const VMTMaterial& vmt) -> float {
    if (vmt.baseTexture.empty()) {
        return -1.0f;
    }
    const auto texPath = vfs.ResolveTexture(vmt.baseTexture);
    if (!texPath.has_value()) {
        return -1.0f;
    }
    const auto texBytes = vfs.ReadFile(*texPath);
    if (!texBytes.has_value() || texBytes->empty()) {
        return -1.0f;
    }
    const auto decoded = DecodeVTF(*texBytes);
    if (!decoded.has_value() || decoded->rgba8.empty()) {
        return -1.0f;
    }
    const size_t pixelCount = decoded->rgba8.size() / 4;
    if (pixelCount == 0) {
        return -1.0f;
    }
    // Stride over the texture: a 2048x512 VTF is a megapixel, and the mean over
    // every 97th pixel is stable to three decimals while touching ~1% of it.
    double   sum   = 0.0;
    uint64_t count = 0;
    for (uint64_t i = 0; i < pixelCount; i += 97) {
        const size_t offset = static_cast<size_t>(i) * 4;
        const auto   r      = static_cast<double>(static_cast<uint8_t>(decoded->rgba8[offset + 0]));
        const auto   g      = static_cast<double>(static_cast<uint8_t>(decoded->rgba8[offset + 1]));
        const auto   b      = static_cast<double>(static_cast<uint8_t>(decoded->rgba8[offset + 2]));
        const auto   a      = static_cast<double>(static_cast<uint8_t>(decoded->rgba8[offset + 3])) / 255.0;
        sum += (r * 0.2126 + g * 0.7152 + b * 0.0722) / 255.0 * a;
        ++count;
    }
    return (count > 0) ? static_cast<float>(sum / static_cast<double>(count)) : -1.0f;
}

namespace {
constexpr float kUnlitDim = 0.55f;
constexpr float kSourceEmissionHdrScale = 4.0f;
// The engine's own luminance, so the emissive ranking here and the renderer's
// light-importance sort agree on what "bright" means.
// Below this, a surface is not emitting anything the renderer can see.
constexpr float kEmissiveStrengthEpsilon = 0.02f;

// Leading scalar of a KeyValues value, or nullopt when it is not a number.
// Source writes `$selfillum 1`, `$selfillum 0.35`, `$selfillum "255 255 255
// 255"` and the texture-path form; the first token is the one that matters.
[[nodiscard]] auto ParseFloatScalar(std::string_view text) noexcept -> std::optional<float> {
    const auto* begin = text.data();
    const auto* end   = text.data() + text.size();
    while (begin < end && (*begin == ' ' || *begin == '\t' || *begin == '[' || *begin == '"')) {
        ++begin;
    }
    float value          = 0.0f;
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc {}) {
        return std::nullopt;
    }
    return value;
}

// 0..1 for a material that declares self-illumination, or -1 when it does not.
// Reads every spelling Source uses: the plain `$selfillum <0|1|scalar>` mask
// switch, the mask texture form, and the `$selfillummaskscale` multiplier that
// a lot of ported content pairs with it.
[[nodiscard]] auto SelfIllumStrength(const VMTMaterial& vmt) noexcept -> float {
    static constexpr std::string_view kKeys[] = {"$selfillum", "$selfillummask", "$selfillummaskscale", "$emissiveblendstrength"};
    float                             found   = -1.0f;
    for (const std::string_view key: kKeys) {
        const auto value = vmt.Find(key);
        if (!value || value->empty()) {
            continue;
        }
        // A texture path is a mask, not a magnitude: the surface emits, at full
        // mask strength, wherever the mask is not black.
        if (value->find('/') != std::string_view::npos || value->ends_with(".vtf")) {
            found = std::max(found, 0.0f);
            continue;
        }
        if (const auto scalar = ParseFloatScalar(*value)) {
            found = std::max(found, *scalar);
        } else {
            found = std::max(found, 1.0f);
        }
    }
    return found;
}

// Source shader names are not case-sensitive. Keep these predicates local:
// the shared strength table and these rendering decisions must use the same
// case-insensitive semantics.
[[nodiscard]] auto ShaderNameEquals(std::string_view shader, std::string_view expected) noexcept -> bool {
    if (shader.size() != expected.size()) {
        return false;
    }
    return std::equal(shader.begin(), shader.end(), expected.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

[[nodiscard]] auto ShaderNameIn(std::string_view shader, std::initializer_list<std::string_view> names) noexcept -> bool {
    return std::ranges::any_of(names, [shader](std::string_view name) { return ShaderNameEquals(shader, name); });
}

} // namespace
auto ResolveVmtEmission(const SourceVFS& vfs, const VMTMaterial& vmt) -> VmtEmission {
    // Source maps carry their emission in three places, and a mapper only has
    // to use one of them for the surface to be dark without all of them:
    //
    //   1. `$selfillum` / `$selfillummask` / `$selfillummaskscale` /
    //      `$emissiveblendstrength` -- the authored case. `$selfillum` is
    //      either a bare 0/1 mask switch, a single scalar, or a full
    //      "r g b a" colour; all three spell the same intent, so take the
    //      scalar when there is one and fall back to a plain 1.0 mask.
    //      Self-illuminated LightmappedGeneric is not unlit (it still
    //      receives light), so the shader class scales the term rather than
    //      replacing it -- with a 0.35 floor, because a mapper who wrote
    //      `$selfillum` meant "this emits".
    //   2. The shader class itself: UnlitGeneric / Sprite / Cable / Beam /
    //      MonitorScreen are emissive by construction, whatever the keyvalues
    //      say, and they are the classes a map's lamp panels and screens use.
    //   3. The measured luminance of the base texture, for the emissive-by-
    //      construction classes of rule 2 that author neither keyvalue. Their
    //      albedo IS the emission, so its brightness sets the term -- a screen
    //      shows its own picture.
    //
    // An ordinary lit material (LightmappedGeneric and kin) with no $selfillum
    // does not emit, whatever its texture looks like. A white poster is lit
    // paint, not a lamp: reading bright albedo as intent to glow added white
    // emission on top of the lighting of every pale wall and sign in imported
    // maps, which is what bleached them.
    //
    // The term is emitted in the material's own colour, and `emissiveFactorScale`
    // stays at 1.0 on the prefabs so Source's authored linear values survive to
    // the renderer unchanged.
    const float shaderStrength    = ShaderEmissiveStrength(vmt.shader);
    const float selfIllumStrength = SelfIllumStrength(vmt);

    VmtEmission emission;
    if (selfIllumStrength >= 0.0f) {
        emission.strength = selfIllumStrength * std::max(shaderStrength, 0.35f);
    } else if (shaderStrength >= 1.0f) {
        // No authored emission, but the shader class is emissive by
        // construction (the 1.0 band of ShaderEmissiveStrength). Measured
        // lazily: decoding a .vtf is not free and the authored case above
        // never needs it.
        const float textureLuma = vmt.baseTexture.empty() ? -1.0f : MeasureVmtTextureLuma(vfs, vmt);
        if (textureLuma > 0.0f) {
            emission.strength = textureLuma * shaderStrength;
        }
    }

    // Keep the term at the material's own chromaticity and let its luminance
    // set the magnitude: a red neon sign emits red, not a grey scaled by red.
    emission.tint = {vmt.baseColor.x, vmt.baseColor.y, vmt.baseColor.z};
    if (Rec709Luma(emission.tint) < 1e-4f) {
        emission.tint = {1.0f, 1.0f, 1.0f}; // a black albedo is "no tint", not "black light"
    }
    return emission;
}

auto ApplyVmtLighting(const SourceVFS& vfs, const VMTMaterial& vmt, MaterialDesc& desc) -> void {
    // 0. Source materials are dielectrics: $basetexture is diffuse colour and
    // $envmap/$phong add a specular highlight on top, not metalness. MaterialDesc
    // defaults metallic to 1.0 and the glTF importer always writes the factor,
    // so this path is the one that must write it too: the opaque shaders shade
    // diffuse as albedo * (1 - metallic), and the default erased the diffuse
    // term of every imported surface -- flat grey metal instead of painted
    // walls. Roughness follows the material's specular vocabulary: $phong is
    // the glossiest, $envmap the varnished-studio look, and a plain
    // LightmappedGeneric is a matte wall.
    desc.metallic  = 0.0f;
    desc.roughness = vmt.Has("$phong") ? 0.45f : (vmt.Has("$envmap") ? 0.60f : 0.85f);

    // Source's $envmap is a reflected environment response, not a PBR metal
    // and not the KHR specular extension (which requires the forward path).
    // Preserve it through the G-buffer's otherwise-unused opaque alpha lane so
    // deferred props keep their ambient-cube lighting. The scalar projection is
    // exact for Source's common neutral tint, including the chair's 0.25 gray.
    if (vmt.Has("$envmap")) {
        desc.environmentReflectionEnabled          = true;
        desc.environmentReflectionAlphaMask = vmt.baseAlphaEnvMapMask;
        desc.environmentReflectionFactor = std::clamp((vmt.envMapTint.x + vmt.envMapTint.y + vmt.envMapTint.z) / 3.0f, 0.0f, 1.0f);
    }

    // 1. Unlit by construction. UnlitGeneric and its siblings have no lighting
    // path at all: the albedo goes straight to the framebuffer. MonitorScreen
    // and UnlitTwoTexture belong to the same family -- a screen shows its own
    // picture and is not lit like a wall -- and leaving them out of this list
    // shaded every display in an imported map with the world's lightmap and
    // then added their picture on top.
    const bool unlitByConstruction =
        ShaderNameIn(vmt.shader, {"UnlitGeneric", "UnlitTwoTexture", "MonitorScreen", "Sprite", "SpriteCard", "Cable", "Beam"});
    if (unlitByConstruction) {
        desc.unlit = true;
        // Source's plain UnlitGeneric panels would otherwise land at luma 1.0
        // -- exactly the bloom threshold -- and a missing texture would fall
        // back to the raw baseColor (usually pure white) and blow out wherever
        // direct light reaches. A texlight keeps this dim as well: the emission
        // below is the panel's brightness, and the albedo is the same picture.
        desc.baseColor.x *= kUnlitDim;
        desc.baseColor.y *= kUnlitDim;
        desc.baseColor.z *= kUnlitDim;
    }

    // 2. The emission term.
    const VmtEmission emission = ResolveVmtEmission(vfs, vmt);
    if (emission.strength <= kEmissiveStrengthEpsilon) {
        return;
    }

    // 3. A self-illuminated surface by construction is Source's texlight:
    // `lights/white` and friends are UnlitGeneric plus `$selfillum`, and Source
    // draws the panel at full brightness with bloom. This path used to return
    // here -- "their unlit path IS the emission" -- and that is where a map has
    // the most of it: the recessed strips along gm_murder_drones' walkways came
    // out as flat grey tiles beside the light pools they were casting, because
    // the albedo was dimmed off the bloom threshold and nothing stood in for
    // the emission. The term is the same one a lit $selfillum surface gets, so
    // a lamp reads the same whichever shader class the mapper reached for.
    const float luma  = std::max(Rec709Luma(emission.tint), kEmissiveStrengthEpsilon);
    const float scale = emission.strength * kSourceEmissionHdrScale / luma;
    desc.emissive     = {emission.tint.x * scale, emission.tint.y * scale, emission.tint.z * scale, 1.0f};

    // The additive term carries the surface's own art whenever the material
    // builder did not already say so: a screen glows with its picture, not
    // with a flat wash of its average colour.
    if (desc.emissiveMap == TextureID::Invalid && desc.albedoMap != TextureID::Invalid) {
        desc.emissiveMap = desc.albedoMap;
    }

    // ...and an unlit texlight whose art does not resolve is a missing asset,
    // not a lamp. Its factor would then apply on its own, with no texture to
    // modulate it, painting the surface as a solid HDR blob; the albedo was
    // already dimmed to the missing-texture grey, and that is all it should
    // read as.
    if (unlitByConstruction && !vmt.baseTexture.empty() && !vfs.ResolveTexture(vmt.baseTexture).has_value()) {
        desc.emissive = {0.0f, 0.0f, 0.0f, 1.0f};
    }
}

} // namespace ZHLN::BSP
