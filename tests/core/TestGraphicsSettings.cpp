// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Pure-logic coverage of the canonical GraphicsSettings model: quality-tier
// presets, tier detection, and configuration-equality semantics used by
// RenderContext::ApplySettings delta detection. No GPU / engine required.
//
// What this file does not do is assert tuned constant values. A preset exists
// to be retuned, and a test that restates the table only reports that someone
// edited two files. What is asserted is the shape a retune has to keep: tiers
// are ordered by cost, detection reads the signature and nothing else, presets
// never touch blit colour style, and ConfigEquals ignores per-frame jitter.

#include "TestsFramework.hpp"
#include <Zahlen/GraphicsSettings.hpp>
#include <array>
#include <cstdint>
#include <string_view>

using namespace ZHLN;

namespace {

// The tiers that form a cost ladder, cheapest first. Custom is not on it: a
// Custom configuration is what the ladder fails to describe, not a rung.
constexpr std::array kCostLadder {QualityLevel::Low, QualityLevel::Medium, QualityLevel::High, QualityLevel::Ultra};

// A resource a higher tier may not buy less of.
struct TierBudget {
    std::string_view label;
    uint64_t         value;
};

// The resources a preset scales, read out of one settings value as plain
// numbers: flags become 0/1 so the same "may not shrink" rule covers them.
[[nodiscard]] auto Budgets(const GraphicsSettings& settings) -> std::array<TierBudget, 9> {
    return {{
        {"shadow map resolution", settings.shadows.resolution},
        {"gi samples", static_cast<uint64_t>(settings.post.giSamples)},
        {"screen-space reflections", static_cast<uint64_t>(settings.post.enableSSR)},
        {"ray-traced reflections", static_cast<uint64_t>(settings.post.enableRTR)},
        {"ray-traced shadows", static_cast<uint64_t>(settings.rayTracing.enableShadows)},
        {"rt shadow samples", settings.rayTracing.shadowSamples},
        {"rt reflection samples", settings.rayTracing.reflectionSamples},
        {"rt denoiser passes", settings.rayTracing.denoiserPasses},
        {"rt max bounces", settings.rayTracing.maxBounces},
    }};
}

// The blit's colour controls: the part of GISettings a preset must never write.
// GISettings also holds cost knobs (giSamples, enableSSR, enableRTR), which is
// why "style" is these six fields rather than the whole post block.
struct BlitStyle {
    float                exposure;
    float                bloomStrength;
    float                contrast;
    float                saturation;
    int                  tonemapper;
    std::array<float, 3> colorFilter;

    auto operator==(const BlitStyle&) const noexcept -> bool = default;
};

[[nodiscard]] auto StyleOf(const GraphicsSettings& settings) -> BlitStyle {
    return {settings.post.exposure,      settings.post.bloomStrength, settings.post.contrast,
            settings.post.saturation,    settings.post.tonemapper,     settings.post.colorFilter};
}

// A mode other than the one in hand. The default model is already SMAA, so a
// test that writes SMAA while checking a mode change writes the value it means
// to move away from -- which is how a mode-change assertion stops asserting.
[[nodiscard]] auto AADifferingFrom(AAMode current) -> AAMode {
    for (const AAMode candidate: {AAMode::None, AAMode::FXAA, AAMode::MLAA, AAMode::TAA, AAMode::SMAA}) {
        if (candidate != current) {
            return candidate;
        }
    }
    return current;
}

} // namespace

struct GraphicsSettingsSuite {
    GraphicsSettingsSuite() {
        // Suites mirror the framework layout used by TestECS: stateless setup,
        // nested Tests struct, expected<void, ErrorCode> test methods.
    }
    enum class GraphicsSettingsTestError : uint8_t {
        PresetDetectionFailed ZHLN_ANNOTATION(ZHLN::Description<"QualityLevel::DetectPreset() did not report the tier the settings were configured for."> {}) =
            1,
        TierBudgetRegression ZHLN_ANNOTATION(ZHLN::Description<"A higher tier allocated less of a resource than a lower tier, or the top tier allocated no more than the bottom."> {}),
        PresetSignatureDrift ZHLN_ANNOTATION(ZHLN::Description<"Applying Custom, or applying a tier twice, moved a signature field."> {}),
        ConfigEqualityFailed ZHLN_ANNOTATION(ZHLN::Description<"Two GraphicsSettings values that should be identical compared unequal."> {}),
        TierLabelFailed      ZHLN_ANNOTATION(ZHLN::Description<"Reflection did not name a QualityLevel tier."> {}),
        StyleControlsFailed  ZHLN_ANNOTATION(ZHLN::Description<"Blit colour-style controls were not retained as non-signature graphics settings."> {}),
    };

    struct Tests {
        // --- 1. Defaults are a configuration, not a tier ---------------------
        // The default model is not the Medium preset (Medium enables
        // screen-space reflections, the defaults do not), and both the
        // detected and the declared tier say so.
        std::expected<void, ZHLN::ErrorCode> defaults_are_not_a_named_tier() {
            const GraphicsSettings defaults {};
            if (!ZHLN::Test::ExpectEq(defaults.DetectPreset(), QualityLevel::Custom)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }
            if (!ZHLN::Test::ExpectEq(defaults.qualityPreset, QualityLevel::Custom)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }

            // Two independently default-constructed models are the same
            // configuration: defaults are deterministic, not environment- or
            // call-order-dependent.
            if (!ZHLN::Test::ExpectTrue(defaults.ConfigEquals(GraphicsSettings {}))) {
                return std::unexpected(GraphicsSettingsTestError::ConfigEqualityFailed);
            }
            return {};
        }

        // --- 2. Higher tiers allocate at least as much as lower tiers ---------
        std::expected<void, ZHLN::ErrorCode> tier_cost_is_ordered() {
            std::array<GraphicsSettings, kCostLadder.size()> rung {};
            for (size_t i = 0; i < kCostLadder.size(); ++i) {
                rung[i].ApplyPreset(kCostLadder[i]);
            }

            // Adjacent rungs: never cheaper. Equality is allowed -- a tier may
            // spend its budget somewhere else rather than on more of the same.
            for (size_t i = 1; i < rung.size(); ++i) {
                const auto below = Budgets(rung[i - 1]);
                const auto above = Budgets(rung[i]);
                for (size_t b = 0; b < below.size(); ++b) {
                    if (ZHLN::Test::ExpectGe(above[b].value, below[b].value)) {
                        continue;
                    }
                    ZHLN::Println("    [FAIL] {} to {}: {}", std::format("{}", kCostLadder[i - 1]), std::format("{}", kCostLadder[i]), above[b].label);
                    return std::unexpected(GraphicsSettingsTestError::TierBudgetRegression);
                }
            }

            // Bottom to top: strictly more, or the ladder is four names for one
            // preset.
            const auto cheapest = Budgets(rung.front());
            const auto dearest  = Budgets(rung.back());
            for (size_t b = 0; b < cheapest.size(); ++b) {
                if (ZHLN::Test::ExpectGt(dearest[b].value, cheapest[b].value)) {
                    continue;
                }
                ZHLN::Println("    [FAIL] {} does not buy more {} than {}", std::format("{}", kCostLadder.back()), dearest[b].label, std::format("{}", kCostLadder.front()));
                return std::unexpected(GraphicsSettingsTestError::TierBudgetRegression);
            }

            // Every rung resolves anti-aliasing: the cheapest way to keep a tier
            // inside its budget is still to not hand the raw image to the blit.
            for (const GraphicsSettings& settings: rung) {
                if (!ZHLN::Test::ExpectNe(settings.antiAliasing.mode, AAMode::None)) {
                    return std::unexpected(GraphicsSettingsTestError::TierBudgetRegression);
                }
            }
            return {};
        }

        // --- 3. Round-trip: ApplyPreset -> DetectPreset ------------------------
        std::expected<void, ZHLN::ErrorCode> preset_round_trip_detection() {
            for (const QualityLevel tier: kCostLadder) {
                GraphicsSettings gfx {};
                gfx.ApplyPreset(tier);
                if (!ZHLN::Test::ExpectEq(gfx.DetectPreset(), tier)) {
                    return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
                }
                if (!ZHLN::Test::ExpectEq(gfx.qualityPreset, tier)) {
                    return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
                }

                // Applying the same tier twice is the same tier. This is what
                // lets a caller re-assert its tier after writing style settings
                // (test 5) without losing them.
                const auto firstSignature = gfx.Signature();
                gfx.ApplyPreset(tier);
                if (!ZHLN::Test::ExpectTrue(gfx.Signature() == firstSignature)) {
                    return std::unexpected(GraphicsSettingsTestError::PresetSignatureDrift);
                }
            }
            return {};
        }

        // --- 4. Detection reads the signature, and only the signature --------
        std::expected<void, ZHLN::ErrorCode> tier_detection_reads_only_the_signature() {
            GraphicsSettings       gfx {};
            gfx.ApplyPreset(QualityLevel::High);
            const GraphicsSettings tuned = gfx;

            // Look knobs and non-signature cost knobs: the tier does not move.
            // The new values are derived from whatever the preset wrote -- what
            // matters is that each knob changed, not what it changed to.
            gfx.shadows.sunSize *= 2.0f;
            gfx.post.vignetteIntensity += 1.0f;
            gfx.post.glowIntensity += 1.0f;
            gfx.post.exposure *= 2.0f;
            gfx.environment.ambientExposure *= 2.0f;
            gfx.antiAliasing.fxaaSubpix *= 0.5f;
            gfx.antiAliasing.mlaaMaxSearchSteps += 1;
            gfx.rayTracing.roughnessCutoff *= 0.5f;
            gfx.rayTracing.alphaTestingInBVH = !gfx.rayTracing.alphaTestingInBVH;
            if (!ZHLN::Test::ExpectEq(gfx.DetectPreset(), QualityLevel::High)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }

            // A signature field moving drops the configuration to Custom...
            gfx.post.giSamples += 1;
            if (!ZHLN::Test::ExpectEq(gfx.DetectPreset(), QualityLevel::Custom)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }

            // ...and restoring that one field puts the tier back: detection is a
            // pure function of the signature, so nothing the look knobs did can
            // leak into the answer.
            gfx = tuned;
            if (!ZHLN::Test::ExpectEq(gfx.DetectPreset(), QualityLevel::High)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }
            return {};
        }

        // --- 5. Blit colour style is configuration, never a quality tier -----
        std::expected<void, ZHLN::ErrorCode> blit_style_is_non_signature_configuration() {
            GraphicsSettings baseline {};
            GraphicsSettings styled {};
            styled.ApplyPreset(QualityLevel::High);

            // Arbitrary payload, chosen to differ from the model's defaults: a
            // payload that repeats a default proves nothing about what survives
            // a preset change. The assertions below compare against this
            // snapshot, so retuning a default cannot break them.
            styled.post.exposure      = 0.02f;
            styled.post.bloomStrength = 0.8f;
            styled.post.contrast      = 1.1f;
            styled.post.saturation    = 0.85f;
            styled.post.tonemapper    = 1;
            styled.post.colorFilter   = {1.0f, 0.9f, 0.8f};
            const BlitStyle style = StyleOf(styled);

            // Presets choose cost, not colour: every tier leaves every style
            // control where the caller put it.
            for (const QualityLevel tier: kCostLadder) {
                styled.ApplyPreset(tier);
                if (!ZHLN::Test::ExpectTrue(StyleOf(styled) == style)) {
                    ZHLN::Println("    [FAIL] {} overwrote blit colour style", std::format("{}", tier));
                    return std::unexpected(GraphicsSettingsTestError::StyleControlsFailed);
                }
            }
            if (!ZHLN::Test::ExpectEq(styled.DetectPreset(), QualityLevel::Ultra)) {
                return std::unexpected(GraphicsSettingsTestError::StyleControlsFailed);
            }

            // The same style written over a different configuration is a
            // different configuration: ApplySettings' delta detection has to see
            // it even though no tier changed.
            styled = baseline;
            styled.post.exposure = 0.02f;
            if (!ZHLN::Test::ExpectFalse(baseline.ConfigEquals(styled))) {
                return std::unexpected(GraphicsSettingsTestError::StyleControlsFailed);
            }
            return {};
        }

        // --- 6. Custom is the absence of a preset, not a tier ------------------
        std::expected<void, ZHLN::ErrorCode> custom_preset_is_noop() {
            GraphicsSettings gfx {};
            gfx.ApplyPreset(QualityLevel::Ultra);
            const GraphicsSettings before = gfx;

            gfx.ApplyPreset(QualityLevel::Custom);
            // Custom names what the ladder cannot describe; applying it describes
            // nothing, so neither the signature nor the declared tier moves.
            if (!ZHLN::Test::ExpectTrue(gfx.Signature() == before.Signature())) {
                return std::unexpected(GraphicsSettingsTestError::PresetSignatureDrift);
            }
            if (!ZHLN::Test::ExpectEq(gfx.DetectPreset(), QualityLevel::Ultra)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }
            if (!ZHLN::Test::ExpectEq(gfx.qualityPreset, QualityLevel::Ultra)) {
                return std::unexpected(GraphicsSettingsTestError::PresetDetectionFailed);
            }
            return {};
        }

        // --- 7. ConfigEquals ignores jitter, catches configuration ------------
        std::expected<void, ZHLN::ErrorCode> config_equality_semantics() {
            GraphicsSettings a {};
            GraphicsSettings b {};

            // Per-frame jitter state must never count as a config change. These
            // are samples the AA pass writes every frame, not tuning values.
            b.antiAliasing.jitterX     = 0.25f;
            b.antiAliasing.jitterY     = -0.5f;
            b.antiAliasing.prevJitterX = 0.125f;
            b.antiAliasing.prevJitterY = -0.25f;
            b.antiAliasing.frameIndex  = 128;
            if (!ZHLN::Test::ExpectTrue(a.ConfigEquals(b))) {
                return std::unexpected(GraphicsSettingsTestError::ConfigEqualityFailed);
            }

            // A real knob change must be detected. Derived from the current value
            // so the check cannot rot when the default is retuned.
            b.shadows.resolution = a.shadows.resolution + 1;
            if (!ZHLN::Test::ExpectFalse(a.ConfigEquals(b))) {
                return std::unexpected(GraphicsSettingsTestError::ConfigEqualityFailed);
            }

            // AA mode is configuration; AA jitter is not. Move the mode to a
            // different one -- the model's default is already SMAA, so writing
            // SMAA here would be a no-op that passes for the wrong reason.
            b                   = a;
            b.antiAliasing.mode = AADifferingFrom(a.antiAliasing.mode);
            if (!ZHLN::Test::ExpectFalse(a.ConfigEquals(b))) {
                return std::unexpected(GraphicsSettingsTestError::ConfigEqualityFailed);
            }
            b.antiAliasing.mode = a.antiAliasing.mode;
            if (!ZHLN::Test::ExpectTrue(a.ConfigEquals(b))) {
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
