// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Pure-logic coverage of the canonical GraphicsSettings model: quality-tier
// presets, tier detection, and configuration-equality semantics used by
// RenderContext::ApplySettings delta detection. No GPU / engine required.

#include "TestsFramework.hpp"
#include <Zahlen/GraphicsSettings.hpp>
#include <array>

using namespace ZHLN;

struct GraphicsSettingsSuite {
    GraphicsSettingsSuite() {
        // Suites mirror the framework layout used by TestECS: stateless setup,
        // nested Tests struct, expected<void, ErrorCode> test methods.
    }
    enum class GraphicsSettingsTestError : uint8_t {
        PresetDetectionFailed ZHLN_ANNOTATION(ZHLN::Description<"QualityLevel::DetectPreset() did not report the tier the settings were configured for."> {}) =
            1,
        PresetSignatureMismatch ZHLN_ANNOTATION(ZHLN::Description<"ApplyPreset() left a tier's signature field at the wrong value."> {}),
        ConfigEqualityFailed    ZHLN_ANNOTATION(ZHLN::Description<"Two GraphicsSettings values that should be identical compared unequal."> {}),
        TierLabelFailed         ZHLN_ANNOTATION(ZHLN::Description<"Reflection did not name a QualityLevel tier."> {}),
        StyleControlsFailed     ZHLN_ANNOTATION(ZHLN::Description<"Blit colour-style controls were not retained as non-signature graphics settings."> {}),
    };

    struct Tests {
        // --- 1. Defaults form exactly the Medium tier ------------------------
        std::expected<void, ZHLN::ErrorCode> defaults_are_medium_tier() {
            GraphicsSettings gfx {};
            if (!ZHLN::Test::ExpectEq(gfx.DetectPreset(), QualityLevel::Medium)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }
            if (!ZHLN::Test::ExpectEq(gfx.qualityPreset, QualityLevel::Medium)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }
            if (!(ZHLN::Test::ExpectEq(gfx.post.exposure, 0.015f) && ZHLN::Test::ExpectEq(gfx.post.bloomStrength, 0.5f) &&
                  ZHLN::Test::ExpectEq(gfx.post.contrast, 1.0f) && ZHLN::Test::ExpectEq(gfx.post.saturation, 1.0f) &&
                  ZHLN::Test::ExpectEq(gfx.post.tonemapper, 1) && ZHLN::Test::ExpectEq(gfx.post.colorFilter, std::array {1.0f, 1.0f, 1.0f}))) {
                return std::unexpected(GraphicsSettingsTestError::StyleControlsFailed);
            }
            return {};
        }

        // --- 2. Presets pin their signature fields ---------------------------
        std::expected<void, ZHLN::ErrorCode> presets_pin_signature_fields() {
            GraphicsSettings low {};
            low.ApplyPreset(QualityLevel::Low);
            if (!(ZHLN::Test::ExpectEq(low.antiAliasing.mode, AAMode::FXAA) && ZHLN::Test::ExpectEq(low.shadows.resolution, 1024) &&
                  ZHLN::Test::ExpectEq(low.post.giSamples, 4))) {
                return std::unexpected(GraphicsSettingsTestError::PresetSignatureMismatch);
            }
            if (!(ZHLN::Test::ExpectEq(low.post.enableSSR, 0) && ZHLN::Test::ExpectEq(low.post.enableRTR, 0) &&
                  ZHLN::Test::ExpectEq(low.rayTracing.denoiserPasses, 0))) {
                return std::unexpected(GraphicsSettingsTestError::PresetSignatureMismatch);
            }

            GraphicsSettings ultra {};
            ultra.ApplyPreset(QualityLevel::Ultra);
            if (!(ZHLN::Test::ExpectEq(ultra.shadows.resolution, 4096) && ZHLN::Test::ExpectEq(ultra.post.giSamples, 16) &&
                  ZHLN::Test::ExpectEq(ultra.post.enableRTR, 1) && ZHLN::Test::ExpectEq(ultra.post.enableSSR, 1))) {
                return std::unexpected(GraphicsSettingsTestError::PresetSignatureMismatch);
            }
            // RT sample budget: the extension point for the upcoming RT shadow
            // mask / A-Trous denoiser / VNDF reflection passes.
            if (!(ZHLN::Test::ExpectEq(ultra.rayTracing.shadowSamples, 2) && ZHLN::Test::ExpectEq(ultra.rayTracing.reflectionSamples, 2) &&
                  ZHLN::Test::ExpectEq(ultra.rayTracing.maxBounces, 2))) {
                return std::unexpected(GraphicsSettingsTestError::PresetSignatureMismatch);
            }
            return {};
        }

        // --- 3. Round-trip: ApplyPreset -> DetectPreset ------------------------
        std::expected<void, ZHLN::ErrorCode> preset_round_trip_detection() {
            for (const QualityLevel tier: {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High, QualityLevel::Ultra}) {
                GraphicsSettings gfx {};
                gfx.ApplyPreset(tier);
                if (!ZHLN::Test::ExpectEq(gfx.DetectPreset(), tier)) {
                    return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
                }
                if (!ZHLN::Test::ExpectEq(gfx.qualityPreset, tier)) {
                    return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
                }
            }
            return {};
        }

        // --- 4. Signature tweaks drop to Custom; other tweaks keep the tier ---
        std::expected<void, ZHLN::ErrorCode> tier_detection_sensitivity() {
            GraphicsSettings gfx {};
            gfx.ApplyPreset(QualityLevel::High);

            // Non-signature knob: still High.
            gfx.shadows.sunSize             = 0.02f;
            gfx.post.vignetteIntensity      = 1.4f;
            gfx.environment.ambientExposure = 12.0f;
            gfx.antiAliasing.fxaaSubpix     = 0.5f;
            gfx.post.exposure               = 0.03f;
            gfx.post.tonemapper             = 3;
            if (!ZHLN::Test::ExpectEq(gfx.DetectPreset(), QualityLevel::High)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }

            // Signature knob: Custom.
            gfx.post.giSamples = 12;
            if (!ZHLN::Test::ExpectEq(gfx.DetectPreset(), QualityLevel::Custom)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }
            return {};
        }

        // --- 5. Blit colour style is configuration, never a quality tier ----
        std::expected<void, ZHLN::ErrorCode> blit_style_is_non_signature_configuration() {
            GraphicsSettings baseline {};
            GraphicsSettings styled {};
            styled.ApplyPreset(QualityLevel::High);
            styled.post.exposure      = 0.02f;
            styled.post.bloomStrength = 0.8f;
            styled.post.contrast      = 1.1f;
            styled.post.saturation    = 0.85f;
            styled.post.tonemapper    = 3;
            styled.post.colorFilter   = {1.0f, 0.9f, 0.8f};

            // Presets choose cost, not colour style; all six values survive.
            styled.ApplyPreset(QualityLevel::High);
            if (!(ZHLN::Test::ExpectEq(styled.post.exposure, 0.02f) && ZHLN::Test::ExpectEq(styled.post.bloomStrength, 0.8f) &&
                  ZHLN::Test::ExpectEq(styled.post.contrast, 1.1f) && ZHLN::Test::ExpectEq(styled.post.saturation, 0.85f) &&
                  ZHLN::Test::ExpectEq(styled.post.tonemapper, 3) && ZHLN::Test::ExpectEq(styled.post.colorFilter, std::array {1.0f, 0.9f, 0.8f}))) {
                return std::unexpected(GraphicsSettingsTestError::StyleControlsFailed);
            }
            if (!ZHLN::Test::ExpectEq(styled.DetectPreset(), QualityLevel::High)) {
                return std::unexpected(GraphicsSettingsTestError::StyleControlsFailed);
            }

            styled = baseline;
            styled.post.exposure = 0.02f;
            if (!ZHLN::Test::ExpectFalse(baseline.ConfigEquals(styled))) {
                return std::unexpected(GraphicsSettingsTestError::StyleControlsFailed);
            }
            return {};
        }

        // --- 6. Custom preset is a no-op ---------------------------------------
        std::expected<void, ZHLN::ErrorCode> custom_preset_is_noop() {
            GraphicsSettings gfx {};
            gfx.ApplyPreset(QualityLevel::Ultra);
            gfx.ApplyPreset(QualityLevel::Custom);
            if (!ZHLN::Test::ExpectEq(gfx.DetectPreset(), QualityLevel::Ultra)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }
            return {};
        }

        // --- 7. ConfigEquals ignores jitter, catches configuration ------------
        std::expected<void, ZHLN::ErrorCode> config_equality_semantics() {
            GraphicsSettings a {};
            GraphicsSettings b {};

            // Per-frame jitter state must never count as a config change.
            b.antiAliasing.jitterX     = 0.25f;
            b.antiAliasing.jitterY     = -0.5f;
            b.antiAliasing.prevJitterX = 0.125f;
            b.antiAliasing.frameIndex  = 128;
            if (!ZHLN::Test::ExpectTrue(a.ConfigEquals(b))) {
                return std::unexpected(GraphicsSettingsTestError::ConfigEqualityFailed);
            }

            // A real knob change must be detected.
            b.shadows.resolution = 4096;
            if (!ZHLN::Test::ExpectFalse(a.ConfigEquals(b))) {
                return std::unexpected(GraphicsSettingsTestError::ConfigEqualityFailed);
            }

            // AA mode is configuration; AA jitter is not.
            b                   = a;
            b.antiAliasing.mode = AAMode::SMAA;
            if (!ZHLN::Test::ExpectFalse(a.ConfigEquals(b))) {
                return std::unexpected(GraphicsSettingsTestError::ConfigEqualityFailed);
            }
            return {};
        }

        // --- 8. Tier labels come from the reflection machinery -----------------
        // GraphicsSettings.hpp declares no hand-rolled name helper: the
        // reflection tables name the tiers, and QualityLevel carries no
        // Description annotation, so `{}` prints the identifier. This is the
        // log line RenderResources.cpp writes when the tier changes, spelled
        // the way a caller spells it.
        std::expected<void, ZHLN::ErrorCode> quality_level_labels() {
            if (!(ZHLN::Test::ExpectEq(std::format("{}", QualityLevel::Low), "Low") &&
                  ZHLN::Test::ExpectEq(std::format("{}", QualityLevel::Medium), "Medium"))) {
                return std::unexpected(GraphicsSettingsTestError::TierLabelFailed);
            }
            if (!(ZHLN::Test::ExpectEq(std::format("{}", QualityLevel::High), "High") &&
                  ZHLN::Test::ExpectEq(std::format("{}", QualityLevel::Ultra), "Ultra") &&
                  ZHLN::Test::ExpectEq(std::format("{}", QualityLevel::Custom), "Custom"))) {
                return std::unexpected(GraphicsSettingsTestError::TierLabelFailed);
            }
            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunGraphicsSettingsSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<GraphicsSettingsSuite>();
}
