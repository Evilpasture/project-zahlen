// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestAmbientOcclusion.cpp
//
// Ambient occlusion / GI mode comparison. Exercises every giMode against the
// same static contact scene and reports per-mode deltas versus the giMode 0
// baseline, so a run tells you numerically -- without eyeballing frames --
// which AO modes are actually doing something and how strongly.
//
//   giMode 0: SH ambient only (the baseline)
//   giMode 1: stochastic sample AO (inline in the lighting pass)
//   giMode 2: SSGI gather (replaces occlusion with gathered indirect light)
//   giMode 3: GTAO via the dedicated half-resolution GtaoAo pass
//   giMode 4: GTAO via the dedicated half-resolution GtaoAo pass (alternate)
//
// Methodology: every mode renders a window of consecutive frames that are
// averaged channel-by-channel before measurement. The AO slices are
// Weyl-jittered per frame, so a single capture carries full Monte-Carlo
// grain; averaging cancels the grain and leaves the deterministic occlusion
// signal. A giMode 0 repeat window measures the residual noise floor the
// assertions are scaled against.
//
// The shared error enum, frame I/O and engine fixture live in
// LightingRTCommon.hpp.

#include "LightingRTCommon.hpp"

namespace {

// ============================================================================
// Averaged-frame capture + luma-field metrics
// ============================================================================

/// Renders `frames` consecutive frames into `rawPath` (overwriting it each
/// time, so the artifact left behind is the final raw frame plus its PNG
/// mirror) and returns the per-channel average of the window.
///
/// The screenshot API reads the current frame back without advancing the
/// engine, so each capture is preceded by one tick -- the same rhythm the
/// other headless suites use when sampling consecutive frames.
[[nodiscard]] RgbImage CaptureAveraged(ZHLN::Engine& engine, uint32_t frames, const std::string& rawPath) {
    std::vector<double> acc;
    RgbImage            avg;
    for (uint32_t f = 0; f < frames; ++f) {
        TickFrames(engine, 1);
        const RgbImage frame = Capture(engine, rawPath);
        if (!frame.Valid()) {
            return {};
        }
        if (acc.empty()) {
            acc.assign(frame.rgb.size(), 0.0);
            avg.width  = frame.width;
            avg.height = frame.height;
        }
        for (size_t i = 0; i < frame.rgb.size(); ++i) {
            acc[i] += static_cast<double>(frame.rgb[i]);
        }
    }
    if (acc.empty()) {
        return {};
    }
    avg.rgb.resize(acc.size());
    const double inv = 1.0 / static_cast<double>(frames);
    for (size_t i = 0; i < acc.size(); ++i) {
        avg.rgb[i] = static_cast<uint8_t>(std::lround(acc[i] * inv));
    }
    return avg;
}

/// Per-pixel luminance (0..255) of one frame, row-major.
struct LumaField {
    int                 width  = 0;
    int                 height = 0;
    std::vector<double> v;

    [[nodiscard]] bool Valid() const noexcept { return width > 0 && height > 0 && !v.empty(); }
};

[[nodiscard]] LumaField MakeLumaField(const RgbImage& img) {
    LumaField out;
    if (!img.Valid()) {
        return out;
    }
    out.width  = img.width;
    out.height = img.height;
    const size_t n = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
    out.v.resize(n);
    for (size_t p = 0; p < n; ++p) {
        out.v[p] = Luma(img.rgb[p * 3 + 0], img.rgb[p * 3 + 1], img.rgb[p * 3 + 2]);
    }
    return out;
}

[[nodiscard]] double MeanLumaOf(const LumaField& f) {
    if (!f.Valid()) {
        return 0.0;
    }
    double sum = 0.0;
    for (const double l: f.v) {
        sum += l;
    }
    return sum / static_cast<double>(f.v.size());
}

/// Per-pixel delta statistics of `mode` against the mode-0 `base`. All luma
/// values are 0..255. `darkThreshold` is negative: pixels whose delta drops
/// below it count as "darkened by this mode"; symmetrically for brightening.
struct AoDeltaStats {
    double meanDelta    = 0.0;
    double minDelta     = 0.0;
    double maxDelta     = 0.0;
    double stdDelta     = 0.0;
    double meanAbsDelta = 0.0;
    double darkPct      = 0.0; ///< percent of pixels with delta < darkThreshold
    double brightPct    = 0.0; ///< percent of pixels with delta > -darkThreshold
};

[[nodiscard]] AoDeltaStats DeltaStats(const LumaField& base, const LumaField& mode, double darkThreshold = -3.0) {
    AoDeltaStats s;
    if (!base.Valid() || !mode.Valid() || base.v.size() != mode.v.size()) {
        return s;
    }
    const size_t n = base.v.size();

    double   sum = 0.0, sumAbs = 0.0, mn = 1e30, mx = -1e30;
    uint64_t dark = 0, bright = 0;
    for (size_t i = 0; i < n; ++i) {
        const double d = mode.v[i] - base.v[i];
        sum += d;
        sumAbs += std::abs(d);
        mn = std::min(mn, d);
        mx = std::max(mx, d);
        if (d < darkThreshold) {
            ++dark;
        } else if (d > -darkThreshold) {
            ++bright;
        }
    }
    s.meanDelta    = sum / static_cast<double>(n);
    s.meanAbsDelta = sumAbs / static_cast<double>(n);
    s.minDelta     = mn;
    s.maxDelta     = mx;
    s.darkPct      = 100.0 * static_cast<double>(dark) / static_cast<double>(n);
    s.brightPct    = 100.0 * static_cast<double>(bright) / static_cast<double>(n);

    double var = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double d = (mode.v[i] - base.v[i]) - s.meanDelta;
        var += d * d;
    }
    s.stdDelta = std::sqrt(var / static_cast<double>(n));
    return s;
}

/// One line of the per-mode report; fixed-width columns so it reads as a
/// table in plain console output.
void PrintReportRow(int mode, const char* name, double meanLuma, const AoDeltaStats& s) {
    char line[224];
    std::snprintf(
        line, sizeof(line),
        "    %-2d %-19s meanLuma=%8.2f meanDelta=%+8.3f minDelta=%+8.2f dark%%=%7.3f bright%%=%7.3f stdDelta=%7.3f",
        mode, name, meanLuma, s.meanDelta, s.minDelta, s.darkPct, s.brightPct, s.stdDelta
    );
    ZHLN::Println("{}", line);
}

// ============================================================================
// Scene + settings plumbing
// ============================================================================

/// Contact-occlusion scene: three boxes standing on a plane under one sun.
/// AO modulates only the SH ambient term, so its signature is only as big
/// as the ambient's share of the image: the sun is kept modest and
/// ambientExposure raised so ambient dominates the shading and contact
/// darkening is measurable. SSR/RTR stay off so the deltas measure AO
/// alone, and the sample budget is raised so the GTAO branch (steps =
/// giSamples/6) gets more than its minimum.
void BuildAoScene(ZHLN::Engine& engine) {
    auto& reg = engine.GetRegistry();
    auto& rc  = engine.GetRenderContext();

    const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
    if (!settingsEnts.empty()) {
        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
            pp.fullBright      = 0;
            pp.ambientExposure = 50.0f;
            pp.enableSSR       = 0;
            pp.enableRTR       = 0;
            pp.giMode          = 0;
            pp.giSamples       = 24;
            pp.aoRadius        = 1.0f;
            pp.aoBias          = 0.05f;
            pp.aoPower         = 1.8f;
            pp.giIntensity     = 1.2f;
        });
    }

    ZHLN::CreativeWorksFactory::CreatePlane(
        engine, 120.0f, {0.55f, 0.55f, 0.58f, 1.0f},
        ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false}
    );

    auto makeMat = [&](float gray) {
        return rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.75f, .baseColor = {gray, gray, gray, 1.0f}});
    };
    auto matA = makeMat(0.85f);
    auto matB = makeMat(0.70f);
    auto matC = makeMat(0.55f);
    if (!matA || !matB || !matC) {
        return; // the caller's capture checks surface this as a blank frame
    }

    ZHLN::CreativeWorksFactory::CreateBox(
        engine, JPH::Vec3(0.8f, 0.8f, 0.8f),
        ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(-2.2, 1.0, 0.0), .createPhysics = false, .materialOverride = *matA}
    );
    ZHLN::CreativeWorksFactory::CreateBox(
        engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
        ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 1.0, -2.0), .createPhysics = false, .materialOverride = *matB}
    );
    ZHLN::CreativeWorksFactory::CreateBox(
        engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
        ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(2.2, 1.0, 1.0), .createPhysics = false, .materialOverride = *matC}
    );

    const ZHLN::Entity sunEnt = reg.Create();
    reg.Add(
        sunEnt,
        ZHLN::Components::TransformComponent {.position = JPH::Vec3(0.0f, 40.0f, 30.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({45.0f, 0.0f, 0.0f})},
        ZHLN::Components::LightComponent {
            .type      = ZHLN::LightType::Sun,
            .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
            .intensity = 220.0f,
            .direction = JPH::Vec3(0.0f, 0.6f, 0.8f).Normalized()
        }
    );

    auto& cam    = engine.GetCamera();
    cam.position = JPH::Vec3(0.0f, 2.5f, 8.0f);
    cam.yaw      = -90.0f;
    cam.pitch    = -12.0f;
    cam.fov      = 60.0f;
}

void SetGiMode(ZHLN::Engine& engine, int mode) {
    auto&      reg  = engine.GetRegistry();
    const auto ents = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
    if (ents.empty()) {
        return;
    }
    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(ents[0], [mode](auto& pp) { pp.giMode = mode; });
}

void SetAoRadius(ZHLN::Engine& engine, float radius) {
    auto&      reg  = engine.GetRegistry();
    const auto ents = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
    if (ents.empty()) {
        return;
    }
    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(ents[0], [radius](auto& pp) { pp.aoRadius = radius; });
}

} // namespace

// ============================================================================
// Test Suite
// ============================================================================

struct AmbientOcclusionTestSuite {
    AmbientOcclusionTestSuite() {
        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~AmbientOcclusionTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    struct Tests {
        // ====================================================================
        // 1. Every AO/GI mode, quantified against the giMode 0 baseline
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> all_ao_modes_produce_their_signature() {
            auto engine = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);
            BuildAoScene(*engine);

            struct ModeRun {
                int         mode;
                const char* name;
                const char* file;
            };
            constexpr std::array<ModeRun, 5> runs {{
                {0, "off (SH only)", "headless_ao_mode0_off.ppm"},
                {1, "sample AO", "headless_ao_mode1_sample.ppm"},
                {2, "SSGI gather", "headless_ao_mode2_ssgi.ppm"},
                {3, "GTAO (mode 3)", "headless_ao_mode3_gtao.ppm"},
                {4, "GTAO (mode 4)", "headless_ao_mode4_gtao_alt.ppm"},
            }};
            constexpr uint32_t kAveragedFrames = 8;

            std::array<RgbImage, runs.size()>  averages {};
            std::array<LumaField, runs.size()> fields {};
            RgbImage                           repeatAverage {};
            LumaField                          repeatField {};

            uint32_t validationRaised = 0;
            bool     captureFailed    = false;

            const auto stable = RunStableScene(
                *engine, 8, "all_ao_modes_produce_their_signature",
                [&](ZHLN::Engine& eng) -> bool {
                    captureFailed = false;

                    for (size_t i = 0; i < runs.size(); ++i) {
                        SetGiMode(eng, runs[i].mode);
                        // Let the patched settings reach the renderer before the
                        // measurement window starts.
                        TickFrames(eng, 2);
                        averages[i] = CaptureAveraged(eng, kAveragedFrames, runs[i].file);
                        if (!averages[i].Valid()) {
                            captureFailed = true;
                            return false;
                        }
                        fields[i] = MakeLumaField(averages[i]);
                    }

                    // Noise floor: a second giMode 0 window must agree with the
                    // first; its delta stats bound how much "change" is just
                    // residual jitter leaking through the averaging.
                    SetGiMode(eng, 0);
                    TickFrames(eng, 2);
                    repeatAverage = CaptureAveraged(eng, kAveragedFrames, "headless_ao_mode0_repeat.ppm");
                    if (!repeatAverage.Valid()) {
                        captureFailed = true;
                        return false;
                    }
                    repeatField = MakeLumaField(repeatAverage);
                    return true;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                if (captureFailed) {
                    return std::unexpected(LightingRTTestError::FrameCaptureFailed);
                }
                return std::unexpected(LightingRTTestError::AoModeInactive);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            // ----------------------------------------------------------------
            // Report: per-mode deltas against the mode-0 baseline.
            // ----------------------------------------------------------------
            ZHLN::Println("    [INFO] AO mode comparison ({} frames averaged per mode, deltas vs giMode 0):", kAveragedFrames);

            const LumaField    baseline = fields[0];
            const AoDeltaStats self     = DeltaStats(baseline, baseline);
            PrintReportRow(runs[0].mode, runs[0].name, MeanLumaOf(baseline), self);

            // The darkened-pixel share counts pixels whose delta drops below
            // -2 luma: comfortably above any residual noise yet small enough
            // that the contact bands register in this sparse scene.
            std::array<AoDeltaStats, runs.size()> stats {};
            for (size_t i = 1; i < runs.size(); ++i) {
                stats[i] = DeltaStats(baseline, fields[i], -2.0);
                PrintReportRow(runs[i].mode, runs[i].name, MeanLumaOf(fields[i]), stats[i]);
                WriteAmplifiedDiff(
                    std::string("headless_ao_diff_mode") + std::to_string(runs[i].mode) + ".ppm", averages[0], averages[i]
                );
            }

            const AoDeltaStats noise = DeltaStats(baseline, repeatField, -2.0);
            PrintReportRow(0, "repeat (noise floor)", MeanLumaOf(repeatField), noise);

            // ----------------------------------------------------------------
            // Checks. Every Expect* failure is listed by the framework with
            // its own line, and the table above carries the numbers.
            // ----------------------------------------------------------------
            bool ok = true;

            // Frames are not blacked out in any mode.
            for (size_t i = 0; i < runs.size(); ++i) {
                ok &= ZHLN::Test::ExpectGt(MeanLumaOf(fields[i]), 1.0);
            }

            // The two mode-0 windows must agree: this is the noise floor the
            // AO checks below are scaled against.
            ok &= ZHLN::Test::ExpectLt(std::abs(noise.meanDelta), 0.5);
            ok &= ZHLN::Test::ExpectLt(noise.stdDelta, 2.0);

            // The AO modes (1, 3, 4) must each darken the frame, with
            // structure clearly above the (bit-exact zero) noise floor and
            // without blacking the frame out. The scene makes ambient the
            // dominant modulated term, but occlusion still concentrates in
            // the contact bands, so the frame-wide mean stays well below a
            // luma unit even when AO is fully active.
            for (const size_t i: {size_t {1}, size_t {3}, size_t {4}}) {
                const AoDeltaStats& s = stats[i];
                ok &= ZHLN::Test::ExpectLt(s.meanDelta, -0.10);
                ok &= ZHLN::Test::ExpectGt(s.meanDelta, -80.0);
                ok &= ZHLN::Test::ExpectLt(s.minDelta, -3.0);
                ok &= ZHLN::Test::ExpectGt(s.darkPct, std::max(0.02, 3.0 * noise.darkPct));
                ok &= ZHLN::Test::ExpectLt(s.darkPct, 80.0);
                ok &= ZHLN::Test::ExpectGt(s.stdDelta, std::max(0.1, 2.0 * noise.stdDelta));
            }

            // Both GTAO modes run through the same dedicated half-resolution
            // pass, so they should land in the same ballpark -- a large split
            // would mean one of them silently fell back to another code path.
            ok &= ZHLN::Test::ExpectLt(std::abs(stats[3].meanDelta - stats[4].meanDelta), 2.0);

            // SSGI replaces occlusion with gathered light: it can only add
            // light, so require a measurable net change just above the
            // (bit-exact zero) noise floor. The gather mostly misses
            // geometry in this sparse scene, so the bar stays low.
            ok &= ZHLN::Test::ExpectGt(stats[2].meanAbsDelta, std::max(0.01, 3.0 * noise.meanAbsDelta));

            if (!ok) {
                return std::unexpected(LightingRTTestError::AoModeInactive);
            }

            ZHLN::Println("    [PASS] All AO/GI modes produced their expected signature; artifacts written as headless_ao_*.ppm/.png.");
            return {};
        }

        // ====================================================================
        // 2. GTAO responds to the aoRadius setting (push-constant path)
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> gtao_radius_responds_to_settings() {
            auto engine = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);
            BuildAoScene(*engine);
            SetGiMode(*engine, 3);
            TickFrames(*engine, 8);

            RgbImage  smallRadius {};
            RgbImage  bigRadius {};
            uint32_t  validationRaised = 0;
            bool      captureFailed    = false;
            LumaField smallField {};
            LumaField bigField {};

            const auto stable = RunStableScene(
                *engine, 2, "gtao_radius_responds_to_settings",
                [&](ZHLN::Engine& eng) -> bool {
                    captureFailed = false;

                    SetAoRadius(eng, 0.3f);
                    TickFrames(eng, 2);
                    smallRadius = CaptureAveraged(eng, 8, "headless_ao_radius_small.ppm");
                    if (!smallRadius.Valid()) {
                        captureFailed = true;
                        return false;
                    }

                    SetAoRadius(eng, 2.5f);
                    TickFrames(eng, 2);
                    bigRadius = CaptureAveraged(eng, 8, "headless_ao_radius_big.ppm");
                    if (!bigRadius.Valid()) {
                        captureFailed = true;
                        return false;
                    }

                    smallField = MakeLumaField(smallRadius);
                    bigField   = MakeLumaField(bigRadius);
                    return true;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                if (captureFailed) {
                    return std::unexpected(LightingRTTestError::FrameCaptureFailed);
                }
                return std::unexpected(LightingRTTestError::AoModeInactive);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            const double meanSmall = MeanLumaOf(smallField);
            const double meanBig   = MeanLumaOf(bigField);
            ZHLN::Println("    [INFO] GTAO aoRadius response: meanLuma(r=0.3)={}, meanLuma(r=2.5)={}", meanSmall, meanBig);

            WriteAmplifiedDiff("headless_ao_radius_diff.ppm", smallRadius, bigRadius);

            // The radius must reach the pass: the two windows may not come
            // out identical. The sign of the response is not asserted -- the
            // horizon search samples slice positions as t^2, so a wider
            // radius spreads the few samples further out and can either pick
            // up more occluders or skip the near-field ones depending on the
            // geometry. A zero delta is the only outcome that proves the
            // setting never reached the push constants.
            const AoDeltaStats radiusStats = DeltaStats(smallField, bigField, -2.0);
            ZHLN::Println(
                "    [INFO] radius delta: mean={}, min={}, max={}, std={}", radiusStats.meanDelta, radiusStats.minDelta, radiusStats.maxDelta,
                radiusStats.stdDelta
            );

            const double peak = std::max(std::abs(radiusStats.minDelta), std::abs(radiusStats.maxDelta));
            bool ok           = ZHLN::Test::ExpectGt(std::abs(radiusStats.meanDelta), 0.01);
            ok &= ZHLN::Test::ExpectGt(peak, 0.5);
            // ...and neither setting may black the frame out.
            ok &= ZHLN::Test::ExpectGt(meanBig, 1.0);
            ok &= ZHLN::Test::ExpectGt(meanSmall, 1.0);

            if (!ok) {
                return std::unexpected(LightingRTTestError::AoRadiusUnresponsive);
            }

            ZHLN::Println("    [PASS] GTAO darkening responds to the aoRadius setting.");
            return {};
        }
    };
};

// Exported for the GPU_Lighting group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunAmbientOcclusionSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<AmbientOcclusionTestSuite>();
}
