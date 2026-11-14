// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Quality presets are APPLICATION POLICY. The engine is deliberately agnostic
// about how settings were chosen -- a config file, a launcher dropdown, a
// benchmark or a hardcoded default all look identical to it. It offers one
// sink, GraphicsDevice::ApplySettings, and nothing more.
//
// This header lives in the app layer so core and the renderer cannot depend
// on it. If your game wants a Low/Medium/High/Ultra dropdown, it belongs here.

#include <Zahlen/Render/GraphicsSettings.hpp>

namespace ZHLN {

struct QualitySignature {
    AAMode   antiAliasMode       = AAMode::SMAA;
    uint32_t shadowResolution    = 2048;
    uint32_t giSamples           = 8;
    int      enableSSR           = 0;
    bool     rtReflections       = false;
    bool     rtDiffuseGI         = false;
    uint32_t rtShadowSamples     = 1;
    uint32_t rtReflectionSamples = 1;
    uint32_t rtDenoiserPasses    = 2;
    uint32_t rtMaxBounces        = 1;

    auto operator==(const QualitySignature&) const noexcept -> bool = default;
};

[[nodiscard]] constexpr auto SignatureOf(const GraphicsSettings& s) noexcept -> QualitySignature {
    return QualitySignature {
        .antiAliasMode       = s.antiAliasing.mode,
        .shadowResolution    = s.shadows.resolution,
        .giSamples           = static_cast<uint32_t>(s.post.giSamples),
        .enableSSR           = s.post.enableSSR,
        .rtReflections       = s.rayTracing.enableReflections,
        .rtDiffuseGI         = s.rayTracing.enableDiffuseGI,
        .rtShadowSamples     = s.rayTracing.shadowSamples,
        .rtReflectionSamples = s.rayTracing.reflectionSamples,
        .rtDenoiserPasses    = s.rayTracing.denoiserPasses,
        .rtMaxBounces        = s.rayTracing.maxBounces,
    };
}

constexpr void ApplyPreset(GraphicsSettings& s, QualityLevel preset) noexcept {
    switch (preset) {
        case QualityLevel::Low:
            s.antiAliasing.mode            = AAMode::FXAA;
            s.shadows.resolution           = 1024;
            s.post.giSamples               = 4;
            s.post.enableSSR               = 0;
            s.rayTracing.enableReflections = false;
            s.rayTracing.shadowSamples     = 1;
            s.rayTracing.reflectionSamples = 1;
            s.rayTracing.denoiserPasses    = 0;
            s.rayTracing.maxBounces        = 1;
            s.rayTracing.enableShadows     = false;
            s.rayTracing.enableDiffuseGI   = false;
            break;
        case QualityLevel::Medium:
            s.antiAliasing.mode            = AAMode::SMAA;
            s.shadows.resolution           = 2048;
            s.post.giSamples               = 8;
            s.post.enableSSR               = 1;
            s.rayTracing.enableReflections = false;
            s.rayTracing.shadowSamples     = 1;
            s.rayTracing.reflectionSamples = 1;
            s.rayTracing.denoiserPasses    = 1;
            s.rayTracing.maxBounces        = 1;
            s.rayTracing.enableShadows     = true;
            s.rayTracing.enableDiffuseGI   = false;
            break;
        case QualityLevel::High:
            s.antiAliasing.mode            = AAMode::SMAA;
            s.shadows.resolution           = 2048;
            s.post.giSamples               = 8;
            s.post.enableSSR               = 1;
            s.rayTracing.enableReflections = true;
            s.rayTracing.shadowSamples     = 1;
            s.rayTracing.reflectionSamples = 1;
            s.rayTracing.denoiserPasses    = 2;
            s.rayTracing.maxBounces        = 1;
            s.rayTracing.enableShadows     = true;
            s.rayTracing.enableDiffuseGI   = true;
            break;
        case QualityLevel::Ultra:
            s.antiAliasing.mode            = AAMode::SMAA;
            s.shadows.resolution           = 4096;
            s.post.giSamples               = 16;
            s.post.enableSSR               = 1;
            s.rayTracing.enableReflections = true;
            s.rayTracing.shadowSamples     = 2;
            s.rayTracing.reflectionSamples = 2;
            s.rayTracing.denoiserPasses    = 3;
            s.rayTracing.maxBounces        = 2;
            s.rayTracing.enableShadows     = true;
            s.rayTracing.enableDiffuseGI   = true;
            s.rayTracing.alphaTestingInBVH = true;
            break;
        case QualityLevel::Custom:
            return;
    }
    s.qualityPreset = preset;
}

[[nodiscard]] constexpr auto DetectPreset(const GraphicsSettings& s) noexcept -> QualityLevel {
    const QualitySignature current = SignatureOf(s);
    for (const QualityLevel tier: {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High, QualityLevel::Ultra}) {
        GraphicsSettings probe {};
        ApplyPreset(probe, tier);
        if (SignatureOf(probe) == current) {
            return tier;
        }
    }
    return QualityLevel::Custom;
}

static_assert(DetectPreset(GraphicsSettings {}) == QualityLevel::Custom);
static_assert([] -> bool {
    GraphicsSettings s {};
    ApplyPreset(s, QualityLevel::Ultra);
    return s.shadows.resolution == 4096 && s.rayTracing.enableReflections && s.post.giSamples == 16
           && s.rayTracing.denoiserPasses == 3 && s.rayTracing.shadowSamples == 2 && s.rayTracing.enableDiffuseGI
           && DetectPreset(s) == QualityLevel::Ultra;
}());

} // namespace ZHLN
