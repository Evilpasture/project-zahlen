// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>

namespace ZHLN {

enum class QualityLevel : uint8_t { Low = 0, Medium, High, Ultra, Custom };

// NOLINTNEXTLINE(performance-enum-size)
enum class AAMode : uint32_t { None = 0, FXAA, MLAA, TAA, SMAA };

struct AAState {
    AAMode mode = AAMode::TAA;

    float    taaFeedback = 0.95f;
    float    jitterX     = 0.0f;
    float    jitterY     = 0.0f;
    float    prevJitterX = 0.0f;
    float    prevJitterY = 0.0f;
    uint32_t frameIndex  = 0;

    float    fxaaSubpix           = 0.75f;
    float    fxaaEdgeThreshold    = 0.166f;
    float    fxaaEdgeThresholdMin = 0.0833f;
    float    mlaaThreshold        = 0.1f;
    uint32_t mlaaMaxSearchSteps   = 16;
};

struct GISettings {
    int   mode              = 1;
    float aoRadius          = 0.5f;
    float aoBias            = 0.05f;
    float aoPower           = 1.8f;
    float giIntensity       = 1.2f;
    int   giSamples         = 8;
    float vignetteIntensity = 1.1f;
    float vignettePower     = 1.5f;
    float glowIntensity     = 0.15f;
    int   enableSSR         = 1;
    int   enableRTR         = 0;

    float                exposure      = 0.015f;
    float                bloomStrength = 0.5f;
    float                contrast      = 1.0f;
    float                saturation    = 1.0f;
    int                  tonemapper    = 1;
    std::array<float, 3> colorFilter   = {1.0f, 1.0f, 1.0f};

    auto operator==(const GISettings&) const noexcept -> bool = default;
};

struct ShadowSettings {
    float    width              = 200.0f;
    uint32_t resolution         = 2048;
    uint32_t maxPunctualShadows = 1;
    float    sunSize            = 0.05f;

    auto operator==(const ShadowSettings&) const noexcept -> bool = default;
};

struct RayTracingConfig {
    bool     enableReflections = false;
    bool     enableShadows     = false;
    uint32_t reflectionSamples = 1;
    uint32_t shadowSamples     = 1;
    uint32_t denoiserPasses    = 1;
    uint32_t maxBounces        = 1;
    float    roughnessCutoff   = 0.4f;
    bool     alphaTestingInBVH = true;

    auto operator==(const RayTracingConfig&) const noexcept -> bool = default;
};

struct EnvironmentSettings {
    float ambientExposure = 25.0f;
    int   fullBright      = 0;
    int   useLocalProbe   = 0;

    std::array<float, 3> probeMin = {-22.0f, 0.0f, -22.0f};
    std::array<float, 3> probeMax = {22.0f, 12.0f, 22.0f};
    std::array<float, 3> probePos = {0.0f, 4.0f, 0.0f};

    std::array<float, 4> skyZenith  = {0.003f, 0.008f, 0.020f, 1.0f};
    std::array<float, 4> skyHorizon = {0.015f, 0.035f, 0.080f, 1.0f};
    std::array<float, 4> skyGround  = {0.001f, 0.001f, 0.003f, 1.0f};

    auto operator==(const EnvironmentSettings&) const noexcept -> bool = default;
};

struct GraphicsSettings {
    QualityLevel        qualityPreset = QualityLevel::Medium;
    GISettings          post;
    AAState             antiAliasing;
    ShadowSettings      shadows;
    RayTracingConfig    rayTracing;
    EnvironmentSettings environment;

    struct QualitySignature {
        AAMode   antiAliasMode       = AAMode::TAA;
        float    taaFeedback         = 0.95f;
        uint32_t shadowResolution    = 2048;
        uint32_t giSamples           = 8;
        int      enableSSR           = 1;
        int      enableRTR           = 0;
        uint32_t rtShadowSamples     = 1;
        uint32_t rtReflectionSamples = 1;
        uint32_t rtDenoiserPasses    = 2;
        uint32_t rtMaxBounces        = 1;

        auto operator==(const QualitySignature&) const noexcept -> bool = default;
    };

    [[nodiscard]] constexpr auto Signature() const noexcept -> QualitySignature {
        return QualitySignature {
            .antiAliasMode       = antiAliasing.mode,
            .taaFeedback         = antiAliasing.taaFeedback,
            .shadowResolution    = shadows.resolution,
            .giSamples           = static_cast<uint32_t>(post.giSamples),
            .enableSSR           = post.enableSSR,
            .enableRTR           = post.enableRTR,
            .rtShadowSamples     = rayTracing.shadowSamples,
            .rtReflectionSamples = rayTracing.reflectionSamples,
            .rtDenoiserPasses    = rayTracing.denoiserPasses,
            .rtMaxBounces        = rayTracing.maxBounces,
        };
    }

    constexpr void ApplyPreset(QualityLevel preset) noexcept {
        switch (preset) {
            case QualityLevel::Low:
                antiAliasing.mode            = AAMode::FXAA;
                shadows.resolution           = 1024;
                post.giSamples               = 4;
                post.enableSSR               = 0;
                post.enableRTR               = 0;
                rayTracing.shadowSamples     = 1;
                rayTracing.reflectionSamples = 1;
                rayTracing.denoiserPasses    = 0;
                rayTracing.maxBounces        = 1;
                rayTracing.enableShadows     = false;
                break;
            case QualityLevel::Medium:
                antiAliasing.mode            = AAMode::TAA;
                antiAliasing.taaFeedback     = 0.95f;
                shadows.resolution           = 2048;
                post.giSamples               = 8;
                post.enableSSR               = 1;
                post.enableRTR               = 0;
                rayTracing.shadowSamples     = 1;
                rayTracing.reflectionSamples = 1;
                rayTracing.denoiserPasses    = 1;
                rayTracing.maxBounces        = 1;
                rayTracing.enableShadows     = true;
                break;
            case QualityLevel::High:
                antiAliasing.mode            = AAMode::TAA;
                antiAliasing.taaFeedback     = 0.95f;
                shadows.resolution           = 2048;
                post.giSamples               = 8;
                post.enableSSR               = 1;
                post.enableRTR               = 1;
                rayTracing.shadowSamples     = 1;
                rayTracing.reflectionSamples = 1;
                rayTracing.denoiserPasses    = 2;
                rayTracing.maxBounces        = 1;
                rayTracing.enableShadows     = true;
                break;
            case QualityLevel::Ultra:
                antiAliasing.mode            = AAMode::TAA;
                antiAliasing.taaFeedback     = 0.95f;
                shadows.resolution           = 4096;
                post.giSamples               = 16;
                post.enableSSR               = 1;
                post.enableRTR               = 1;
                rayTracing.shadowSamples     = 2;
                rayTracing.reflectionSamples = 2;
                rayTracing.denoiserPasses    = 3;
                rayTracing.maxBounces        = 2;
                rayTracing.enableShadows     = true;
                rayTracing.alphaTestingInBVH = true;
                break;
            case QualityLevel::Custom:
                return;
        }
        qualityPreset = preset;
    }

    [[nodiscard]] constexpr auto DetectPreset() const noexcept -> QualityLevel {
        const QualitySignature current = Signature();
        for (const QualityLevel tier: {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High, QualityLevel::Ultra}) {
            GraphicsSettings probe {};
            probe.ApplyPreset(tier);
            if (probe.Signature() == current) {
                return tier;
            }
        }
        return QualityLevel::Custom;
    }

    [[nodiscard]] constexpr auto ConfigEquals(const GraphicsSettings& other) const noexcept -> bool {
        const bool aaMatches = antiAliasing.mode == other.antiAliasing.mode && antiAliasing.taaFeedback == other.antiAliasing.taaFeedback &&
                               antiAliasing.fxaaSubpix == other.antiAliasing.fxaaSubpix &&
                               antiAliasing.fxaaEdgeThreshold == other.antiAliasing.fxaaEdgeThreshold &&
                               antiAliasing.fxaaEdgeThresholdMin == other.antiAliasing.fxaaEdgeThresholdMin &&
                               antiAliasing.mlaaThreshold == other.antiAliasing.mlaaThreshold &&
                               antiAliasing.mlaaMaxSearchSteps == other.antiAliasing.mlaaMaxSearchSteps;
        return aaMatches && post == other.post && shadows == other.shadows && rayTracing == other.rayTracing && environment == other.environment;
    }
};

static_assert(GraphicsSettings {}.DetectPreset() == QualityLevel::Medium);
static_assert([] -> bool {
    GraphicsSettings s {};
    s.ApplyPreset(QualityLevel::Ultra);
    return s.shadows.resolution == 4096 && s.post.enableRTR == 1 && s.post.giSamples == 16 && s.rayTracing.denoiserPasses == 3 &&
           s.rayTracing.shadowSamples == 2 && s.DetectPreset() == QualityLevel::Ultra;
}());
static_assert([] -> bool {
    GraphicsSettings s {};
    s.ApplyPreset(QualityLevel::High);
    s.shadows.sunSize        = 0.02f;
    s.post.vignetteIntensity = 1.4f;
    s.post.tonemapper        = 3;
    return s.DetectPreset() == QualityLevel::High;
}());

}
