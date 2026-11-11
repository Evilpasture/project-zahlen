// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Math/Vec2.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <array>
#include <cstdint>

namespace ZHLN {

enum class QualityLevel : uint8_t { Low = 0, Medium, High, Ultra, Custom };

// NOLINTNEXTLINE(performance-enum-size)
enum class AAMode : uint32_t { None = 0, FXAA, SMAA };

struct AAState {
    AAMode mode = AAMode::SMAA;

    float fxaaSubpix           = 0.75f;
    float fxaaEdgeThreshold    = 0.166f;
    float fxaaEdgeThresholdMin = 0.0833f;
};

struct GISettings {
    int   mode              = 0;
    float aoRadius          = 0.5f;
    float aoBias            = 0.05f;
    float aoPower           = 1.8f;
    float giIntensity       = 1.0f;
    int   giSamples         = 8;
    float vignetteIntensity = 0.0f;
    float vignettePower     = 1.5f;
    float glowIntensity     = 0.0f;
    int   enableSSR         = 0;

    float                exposure      = 1.0f;
    float                bloomStrength = 0.0f;
    float                contrast      = 1.0f;
    float                saturation    = 1.0f;
    int                  tonemapper    = 3;
    ZHLN::FVec3         colorFilter   = {1.0f, 1.0f, 1.0f};

    // Runtime RCAS dial forwarded to BlitPushConstants.rcasSharpness. 0.0 turns
    // sharpening off; positive values are AMD's exp2(-stops) sharpness, 0.2
    // stops being the default. Deliberately absent from the quality signature
    // in <Zahlen/QualityPresets.hpp>: this is a subjective look control, not a
    // quality tier, so retuning it never changes preset detection.
    float                rcasSharpness = 0.8705505632961241f; // exp2(-0.2)

    auto operator==(const GISettings&) const noexcept -> bool = default;
};

struct ShadowSettings {
    float    width      = 200.0f;
    uint32_t resolution = 2048;
    float    sunSize    = 0.05f;

    auto operator==(const ShadowSettings&) const noexcept -> bool = default;
};

struct RayTracingConfig {
    bool     enableReflections = false;
    bool     enableShadows     = false;
    bool     enableDiffuseGI   = false;
    uint32_t reflectionSamples = 1;
    uint32_t shadowSamples     = 1;
    uint32_t denoiserPasses    = 2;
    uint32_t maxBounces        = 1;
    float    roughnessCutoff   = 0.4f;
    bool     alphaTestingInBVH = true;

    auto operator==(const RayTracingConfig&) const noexcept -> bool = default;
};

struct EnvironmentSettings {
    float ambientExposure = 1.0f;
    int   fullBright      = 0;
    int   useLocalProbe   = 0;

    ZHLN::FVec3 probeMin      = {-22.0f, 0.0f, -22.0f};
    ZHLN::FVec3 probeMax      = {22.0f, 12.0f, 22.0f};
    ZHLN::FVec3 probePos      = {0.0f, 4.0f, 0.0f};

    ZHLN::FVec4 skyZenith     = {0.003f, 0.008f, 0.020f, 1.0f};
    ZHLN::FVec4 skyHorizon    = {0.015f, 0.035f, 0.080f, 1.0f};
    ZHLN::FVec4 skyGround     = {0.001f, 0.001f, 0.003f, 1.0f};

    auto operator==(const EnvironmentSettings&) const noexcept -> bool = default;
};

// Dumb data. How these values are *chosen* -- presets, tier detection,
// performance governing -- is application policy and lives in
// <Zahlen/QualityPresets.hpp> in the app layer, not here.
struct GraphicsSettings {
    QualityLevel        qualityPreset = QualityLevel::Custom;
    GISettings          post;
    AAState             antiAliasing;
    ShadowSettings      shadows;
    RayTracingConfig    rayTracing;
    EnvironmentSettings environment;

    [[nodiscard]] constexpr auto ConfigEquals(const GraphicsSettings& other) const noexcept -> bool {
        const bool aaMatches = antiAliasing.mode == other.antiAliasing.mode &&
                               antiAliasing.fxaaSubpix == other.antiAliasing.fxaaSubpix &&
                               antiAliasing.fxaaEdgeThreshold == other.antiAliasing.fxaaEdgeThreshold &&
                               antiAliasing.fxaaEdgeThresholdMin == other.antiAliasing.fxaaEdgeThresholdMin;
        return aaMatches && post == other.post && shadows == other.shadows && rayTracing == other.rayTracing && environment == other.environment;
    }
};

static_assert([] -> bool {
    const GraphicsSettings settings {};
    return settings.antiAliasing.mode == AAMode::SMAA && settings.post.mode == 0 && settings.post.giIntensity == 1.0f
           && settings.post.enableSSR == 0 && settings.post.exposure == 1.0f
           && settings.post.rcasSharpness > 0.0f
           && settings.post.bloomStrength == 0.0f && settings.post.glowIntensity == 0.0f && settings.post.vignetteIntensity == 0.0f
           && settings.post.tonemapper == 3 && settings.shadows.resolution == 2048
           && !settings.rayTracing.enableShadows && !settings.rayTracing.enableReflections && !settings.rayTracing.enableDiffuseGI
           && settings.rayTracing.denoiserPasses == 2 && settings.environment.ambientExposure == 1.0f;
}());

}
