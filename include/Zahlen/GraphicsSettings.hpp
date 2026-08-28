// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
// ============================================================================
// GraphicsSettings — the single canonical graphics configuration model.
//
// Data flow (one direction, one writer per hop):
//
//   UI (ImGui) / Lua scripts / quality presets
//        │ write
//        ▼
//   ECS components (editing surface: PostProcessSettingsComponent,
//   ShadowSettingsComponent, AASettingsComponent — script & inspector bound)
//        │ CollectGraphicsSettings() — once per frame in RenderSystem
//        ▼
//   GraphicsSettings (this struct — the canonical model)
//        │ RenderContext::ApplySettings() — delta-detected
//        ▼
//   RenderContext state (FrameUniforms / ScenePassPushConstants assembly,
//   pipeline-variant selection, reactive GPU target resizes)
//
// The renderer never queries the ECS components directly and the engine never
// calls the loose per-field setters (SetGISettings/SetAAState/
// SetShadowResolution) — those remain only as legacy bridges for tools and
// tests. RayTracingConfig is the extension point for the upcoming RT shadow
// mask pass, A-Trous wavelet denoiser and VNDF glossy reflections: their
// knobs (sample counts, denoiser iterations, roughness cutoff) belong there
// and automatically reach UI, scripts and GPU pushes through this pipeline.
//
// This header is deliberately dependency-free (no Jolt, no Vulkan) so it can
// be included by tools and tests on its own.
// ============================================================================

#include <array>
#include <cstdint>

namespace ZHLN {

// --- Quality tiers ---------------------------------------------------------
// A preset pins the fields of GraphicsSettings::QualitySignature; every other
// field (vignette, sky, probes, exposure, per-AA-knobs) stays user-tuned and
// does not affect the detected tier.
//
// Names for UI/logging come from the reflection machinery like every other
// engine enum: ZHLN::ToString / Reflect::EnumNames (identifier fallback), no
// hand-rolled helpers.
enum class QualityLevel : uint8_t { Low = 0, Medium, High, Ultra, Custom };

// NOLINTNEXTLINE(performance-enum-size)
enum class AAMode : uint32_t { None = 0, FXAA, MLAA, TAA, SMAA };

/// Anti-aliasing configuration. Mixes designer-facing knobs (mode, feedback,
/// thresholds) with per-frame jitter state the camera system advances; the
/// whole struct is carried inside GraphicsSettings so the renderer reads it
/// from exactly one place.
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

/// Post-process / GI / AO knobs (legacy "GI settings" bag). `enableSSR` and
/// `enableRTR` are the screen-space / ray-traced reflection toggles the
/// lighting + reflection pipelines specialise on; they also feed the raw GPU
/// ABI words (FrameUniforms::enableRTR, ScenePassPushConstants::enableSSR/RTR).
struct GISettings {
    int   mode              = 1;
    float aoRadius          = 0.5f;
    float aoBias            = 0.05f;
    float aoPower           = 1.8f;
    float giIntensity       = 1.2f;
    int   giSamples         = 8;
    float vignetteIntensity = 1.1f;
    float vignettePower     = 1.5f;
    int   enableSSR         = 1;
    int   enableRTR         = 0;

    auto operator==(const GISettings&) const noexcept -> bool = default;
};

/// Directional shadow configuration. `resolution` is delta-detected by
/// RenderContext::ApplySettings and reactively resizes the GPU cascade targets
/// (shadowMap + shadowMapPrev) — no caller needs to trigger the resize.
struct ShadowSettings {
    float    width              = 200.0f;
    uint32_t resolution         = 2048;
    uint32_t maxPunctualShadows = 1;
    float    sunSize            = 0.05f;

    auto operator==(const ShadowSettings&) const noexcept -> bool = default;
};

/// Ray-tracing configuration — the extension point for the upcoming passes:
///   - RT shadow mask pass    (enableShadows, shadowSamples)
///   - A-Trous denoiser       (denoiserPasses: 0 = off, 1 = spatial,
///                             2 = spatio-temporal)
///   - VNDF glossy reflections (reflectionSamples, roughnessCutoff, maxBounces)
/// `enableReflections` mirrors `GISettings::enableRTR` (the ABI-level toggle
/// the current reflection pipelines read); CollectGraphicsSettings is the
/// single writer keeping the pair in sync.
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

/// Environment / sky / probe values that feed FrameUniforms every frame.
/// Stored as plain arrays (not JPH vectors) to keep this header standalone;
/// the collector converts from the ECS component's Jolt types.
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
    QualityLevel        qualityPreset = QualityLevel::Medium; // informational; DetectPreset() is authoritative
    GISettings          post;
    AAState             antiAliasing;
    ShadowSettings      shadows;
    RayTracingConfig    rayTracing;
    EnvironmentSettings environment;

    /// The fields a quality preset pins. DetectPreset() compares a settings
    /// object's signature against each preset's signature; any other field is
    /// user-tuned and never disqualifies a tier.
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

    /// Quality-relevant projection of the settings (what presets control).
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

    /// Writes the preset's pinned fields. QualityLevel::Custom is a no-op.
    /// Fields not part of QualitySignature are left untouched.
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
                // No RT shadow mask and no denoiser: the sun shadow comes
                // entirely from the cascade maps.
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
                // Cutout geometry casts shaped shadows: BLAS geometries for
                // masked materials are built without VK_GEOMETRY_OPAQUE_BIT_KHR
                // so the mask can be evaluated per candidate hit.
                rayTracing.alphaTestingInBVH = true;
                break;
            case QualityLevel::Custom:
                return;
        }
        qualityPreset = preset;
    }

    /// Returns the tier whose signature matches, or Custom when the pinned
    /// fields were tweaked by hand. Note: RTR-heavy tiers remain valid
    /// presets on devices without acceleration structures — the renderer
    /// gates those paths on device capability at execution time.
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

    /// Configuration equality for delta detection. The AA jitter state
    /// (jitterX/Y, prevJitterX/Y, frameIndex) legitimately changes every
    /// frame and therefore never counts as a configuration change. The
    /// renderer's reactive paths key off specific fields (shadow resolution,
    /// quality tier); this predicate covers whole-model comparisons for
    /// tools and tests.
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

// The engine defaults (and therefore a freshly-created default scene) form
// exactly the Medium tier; applying a preset pins its signature fields.
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
    s.shadows.sunSize        = 0.02f; // non-signature tweaks keep the tier
    s.post.vignetteIntensity = 1.4f;
    return s.DetectPreset() == QualityLevel::High;
}());

} // namespace ZHLN
