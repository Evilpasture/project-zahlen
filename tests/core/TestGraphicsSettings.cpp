// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Pure-logic coverage of the canonical GraphicsSettings model: quality-tier
// presets, tier detection, and configuration-equality semantics used by
// RenderContext::ApplySettings delta detection. No GPU / engine required.

#include "TestsFramework.hpp"
#include <Zahlen/GraphicsSettings.hpp>

using namespace ZHLN;

struct GraphicsSettingsSuite {
    GraphicsSettingsSuite() {
        // Suites mirror the framework layout used by TestECS: stateless setup,
        // nested Tests struct, expected<void, Error> test methods.
    }
    enum class GraphicsSettingsTestError : uint8_t {
        PresetDetectionFailed ZHLN_ANNOTATION(ZHLN::Description<"QualityLevel::DetectPreset() did not report the tier the settings were configured for."> {}) =
            1,
        PresetSignatureMismatch ZHLN_ANNOTATION(ZHLN::Description<"ApplyPreset() left a tier's signature field at the wrong value."> {}),
        ConfigEqualityFailed    ZHLN_ANNOTATION(ZHLN::Description<"Two GraphicsSettings values that should be identical compared unequal."> {}),
        EnumToStringFailed      ZHLN_ANNOTATION(ZHLN::Description<"GraphicsSettings enum <-> string conversion did not round-trip."> {}),
    };

    struct Tests {
        // --- 1. Defaults form exactly the Medium tier ------------------------
        std::expected<void, ZHLN::Error> defaults_are_medium_tier() {
            GraphicsSettings gfx {};
            if (!ZHLN::Test::ExpectEq(gfx.DetectPreset(), QualityLevel::Medium)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }
            if (!ZHLN::Test::ExpectEq(gfx.qualityPreset, QualityLevel::Medium)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }
            return {};
        }

        // --- 2. Presets pin their signature fields ---------------------------
        std::expected<void, ZHLN::Error> presets_pin_signature_fields() {
            GraphicsSettings low {};
            low.ApplyPreset(QualityLevel::Low);
            if (!ZHLN::Test::ExpectTrue(low.antiAliasing.mode == AAMode::FXAA && low.shadows.resolution == 1024 && low.post.giSamples == 4)) {
                return std::unexpected(GraphicsSettingsTestError::PresetSignatureMismatch);
            }
            if (!ZHLN::Test::ExpectTrue(low.post.enableSSR == 0 && low.post.enableRTR == 0 && low.rayTracing.denoiserPasses == 0)) {
                return std::unexpected(GraphicsSettingsTestError::PresetSignatureMismatch);
            }

            GraphicsSettings ultra {};
            ultra.ApplyPreset(QualityLevel::Ultra);
            if (!ZHLN::Test::ExpectTrue(
                    ultra.shadows.resolution == 4096 && ultra.post.giSamples == 16 && ultra.post.enableRTR == 1 && ultra.post.enableSSR == 1
                )) {
                return std::unexpected(GraphicsSettingsTestError::PresetSignatureMismatch);
            }
            // RT sample budget: the extension point for the upcoming RT shadow
            // mask / A-Trous denoiser / VNDF reflection passes.
            if (!ZHLN::Test::ExpectTrue(ultra.rayTracing.shadowSamples == 2 && ultra.rayTracing.reflectionSamples == 2 && ultra.rayTracing.maxBounces == 2)) {
                return std::unexpected(GraphicsSettingsTestError::PresetSignatureMismatch);
            }
            return {};
        }

        // --- 3. Round-trip: ApplyPreset -> DetectPreset ------------------------
        std::expected<void, ZHLN::Error> preset_round_trip_detection() {
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
        std::expected<void, ZHLN::Error> tier_detection_sensitivity() {
            GraphicsSettings gfx {};
            gfx.ApplyPreset(QualityLevel::High);

            // Non-signature knob: still High.
            gfx.shadows.sunSize             = 0.02f;
            gfx.post.vignetteIntensity      = 1.4f;
            gfx.environment.ambientExposure = 12.0f;
            gfx.antiAliasing.fxaaSubpix     = 0.5f;
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

        // --- 5. Custom preset is a no-op ---------------------------------------
        std::expected<void, ZHLN::Error> custom_preset_is_noop() {
            GraphicsSettings gfx {};
            gfx.ApplyPreset(QualityLevel::Ultra);
            gfx.ApplyPreset(QualityLevel::Custom);
            if (!ZHLN::Test::ExpectEq(gfx.DetectPreset(), QualityLevel::Ultra)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }
            return {};
        }

        // --- 6. ConfigEquals ignores jitter, catches configuration ------------
        std::expected<void, ZHLN::Error> config_equality_semantics() {
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

        // --- 7. Tier labels come from the reflection machinery -----------------
        // GraphicsSettings.hpp declares no hand-rolled ToString; the generic
        // ZHLN::ToString (Reflect::EnumToMessage -> identifier fallback) names
        // the tiers.
        std::expected<void, ZHLN::Error> quality_level_labels() {
            if (!ZHLN::Test::ExpectTrue(ToString(QualityLevel::Low) == "Low" && ToString(QualityLevel::Medium) == "Medium")) {
                return std::unexpected(GraphicsSettingsTestError::EnumToStringFailed);
            }
            if (!ZHLN::Test::ExpectTrue(
                    ToString(QualityLevel::High) == "High" && ToString(QualityLevel::Ultra) == "Ultra" && ToString(QualityLevel::Custom) == "Custom"
                )) {
                return std::unexpected(GraphicsSettingsTestError::EnumToStringFailed);
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
