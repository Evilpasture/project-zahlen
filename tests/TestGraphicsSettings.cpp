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

    struct Tests {
        // --- 1. Defaults form exactly the Medium tier ------------------------
        std::expected<void, ZHLN::Error> defaults_are_medium_tier() {
            GraphicsSettings gfx {};
            auto             check = AssertEq(gfx.DetectPreset(), QualityLevel::Medium);
            if (!check) {
                return check;
            }
            return AssertEq(gfx.qualityPreset, QualityLevel::Medium);
        }

        // --- 2. Presets pin their signature fields ---------------------------
        std::expected<void, ZHLN::Error> presets_pin_signature_fields() {
            GraphicsSettings low {};
            low.ApplyPreset(QualityLevel::Low);
            auto check = AssertTrue(low.antiAliasing.mode == AAMode::FXAA && low.shadows.resolution == 1024 && low.post.giSamples == 4);
            if (!check) {
                return check;
            }
            check = AssertTrue(low.post.enableSSR == 0 && low.post.enableRTR == 0 && low.rayTracing.denoiserPasses == 0);
            if (!check) {
                return check;
            }

            GraphicsSettings ultra {};
            ultra.ApplyPreset(QualityLevel::Ultra);
            check = AssertTrue(
                ultra.shadows.resolution == 4096 && ultra.post.giSamples == 16 && ultra.post.enableRTR == 1 && ultra.post.enableSSR == 1
            );
            if (!check) {
                return check;
            }
            // RT sample budget: the extension point for the upcoming RT shadow
            // mask / A-Trous denoiser / VNDF reflection passes.
            return AssertTrue(ultra.rayTracing.shadowSamples == 2 && ultra.rayTracing.reflectionSamples == 2 && ultra.rayTracing.maxBounces == 2);
        }

        // --- 3. Round-trip: ApplyPreset -> DetectPreset ------------------------
        std::expected<void, ZHLN::Error> preset_round_trip_detection() {
            for (const QualityLevel tier : {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High, QualityLevel::Ultra}) {
                GraphicsSettings gfx {};
                gfx.ApplyPreset(tier);
                auto check = AssertEq(gfx.DetectPreset(), tier);
                if (!check) {
                    return check;
                }
                check = AssertEq(gfx.qualityPreset, tier);
                if (!check) {
                    return check;
                }
            }
            return {};
        }

        // --- 4. Signature tweaks drop to Custom; other tweaks keep the tier ---
        std::expected<void, ZHLN::Error> tier_detection_sensitivity() {
            GraphicsSettings gfx {};
            gfx.ApplyPreset(QualityLevel::High);

            // Non-signature knob: still High.
            gfx.shadows.sunSize            = 0.02f;
            gfx.post.vignetteIntensity     = 1.4f;
            gfx.environment.ambientExposure = 12.0f;
            gfx.antiAliasing.fxaaSubpix    = 0.5f;
            auto check                     = AssertEq(gfx.DetectPreset(), QualityLevel::High);
            if (!check) {
                return check;
            }

            // Signature knob: Custom.
            gfx.post.giSamples = 12;
            return AssertEq(gfx.DetectPreset(), QualityLevel::Custom);
        }

        // --- 5. Custom preset is a no-op ---------------------------------------
        std::expected<void, ZHLN::Error> custom_preset_is_noop() {
            GraphicsSettings gfx {};
            gfx.ApplyPreset(QualityLevel::Ultra);
            gfx.ApplyPreset(QualityLevel::Custom);
            return AssertEq(gfx.DetectPreset(), QualityLevel::Ultra);
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
            auto check                 = AssertTrue(a.ConfigEquals(b));
            if (!check) {
                return check;
            }

            // A real knob change must be detected.
            b.shadows.resolution = 4096;
            check                = AssertFalse(a.ConfigEquals(b));
            if (!check) {
                return check;
            }

            // AA mode is configuration; AA jitter is not.
            b                   = a;
            b.antiAliasing.mode = AAMode::SMAA;
            return AssertFalse(a.ConfigEquals(b));
        }

        // --- 7. Tier labels ------------------------------------------------------
        std::expected<void, ZHLN::Error> quality_level_labels() {
            auto check = AssertTrue(ToString(QualityLevel::Low) == "Low" && ToString(QualityLevel::Medium) == "Medium");
            if (!check) {
                return check;
            }
            return AssertTrue(ToString(QualityLevel::High) == "High" && ToString(QualityLevel::Ultra) == "Ultra" && ToString(QualityLevel::Custom) == "Custom");
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<GraphicsSettingsSuite>();
}
