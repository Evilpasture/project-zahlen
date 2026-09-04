// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestRayTracedNoiseStability.cpp
//
// GPU-side verification that the ray-traced dither behaves like blue noise.
//
// The ray-traced sun shadow used to dither with Interleaved Gradient Noise. It
// now samples a blue noise tile and a Vogel disk over the sun
// (pbr_helpers.slang:CalculateShadowRayTraced). Old and new both produce a
// stochastic 1-bit visibility, so a test measuring only residual *magnitude*
// passes either way -- IGN is not louder than blue noise, it is structured.
// What changed is the structure:
//
//   1. LIVENESS    - the RT shadow must reach the image, and its dither must
//                    actually change frame to frame.
//   2. PERIODICITY - the residual must not correlate positively with itself at
//                    a short lag. An IGN lattice does, at its own period, which
//                    is why a wavelet denoiser or TAA smears it into streaks.
//   3. ISOTROPY    - the residual must not prefer a direction. IGN shares phase
//                    along its lattice axes, so its diagonal gradient energy
//                    drops and spatial filters drag the residual along them.
//   4. CONVERGENCE - under temporal accumulation the penumbra residual must
//                    actually fall, not merely fail to grow.
//   5. NO DEBRIS   - changed pixels must cluster; isolated ones are fireflies.
//
// Scenario 1 is a 2x2 diagnosis rather than a single assertion. A blank
// "nothing changed" result has two completely different causes -- the RT path
// is not executing at all, or it executes but its dither is temporally frozen
// -- and the fixes are in different places. Capturing the same static scene
// with the RT switch on, off, and on again separates them:
//
//                     on-vs-off differ   on-vs-on differ
//   yes / yes          RT live, dither varies        (expected)
//   yes / no           RT live, dither frozen        shader-side bug
//   no  / -            RT path not executing         settings/variant bug
//
// The analysis lives in tests/RayTracedNoiseMetrics.hpp, validated separately
// on the CPU (tests/TestRayTracedNoiseMetrics.cpp) against a real IGN lattice,
// the shipped blue noise tile and white noise, so the thresholds below cannot
// be satisfied by an arbitrary noise pattern.
//
// Measurement region: every structural metric is taken over the bounding box of
// the pixels that actually changed. Zeros are direction- and
// correlation-neutral, so feeding a mostly-static frame to the metrics reads
// "perfect" regardless of what the dither does.

#include "NoiseFrameCapture.hpp"
#include "RayTracedNoiseMetrics.hpp"
#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/CreativeWorksFactory.hpp>
#include <Zahlen/DefaultPreset.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/Types.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

enum class NoiseStabilityError : uint8_t {
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize the headless Engine for the RT noise stability test.">{}) = 1,
    CaptureFailed ZHLN_ANNOTATION(ZHLN::Description<"A frame could not be captured or the PPM could not be read back.">{}),
    RayTracedShadowPathInactive ZHLN_ANNOTATION(ZHLN::Description<"Enabling enableRTR did not change a single pixel against a fully static scene, so the ray-traced shadow path is not executing: either pc.enableRTR reaches the shader as 0, the TLAS is null, or the DISABLE_RTR lighting variant was selected.">{}),
    DitherTemporallyFrozen ZHLN_ANNOTATION(ZHLN::Description<"The ray-traced shadow path executes but renders the identical image at two different frame indices, so the blue noise temporal scroll is not advancing: check FrameIndexFromCamPosW(pc.camPos.w) and the R2 UV offset in blue_noise.slang.">{}),
    PenumbraTooSmallToMeasure ZHLN_ANNOTATION(ZHLN::Description<"The per-frame variation covers too small a region for the structural metrics to be trustworthy; widen the penumbra (frame.sunSize, or raise the occluder) rather than gating on a measurement over a handful of pixels.">{}),
    ShadowTooWeakToTest ZHLN_ANNOTATION(ZHLN::Description<"The shadow never develops from lit to shadowed: the fitted coverage does not span the 0..1 range. Usually the sun disk subtends a larger angle than the occluder, so no umbra forms and the penumbra degenerates into sparse speckle; reduce frame.sunSize or enlarge the occluder.">{}),
    NoiseMagnitudeMismatch ZHLN_ANNOTATION(ZHLN::Description<"The per-pixel temporal variance does not match what a one-sample-per-pixel stochastic shadow must produce: near zero means the dither is not modulating visibility, above 1 means extra variance beyond Bernoulli (instability or fireflies), near 1/N means the shader is averaging N samples.">{}),
    DitherIsPeriodic ZHLN_ANNOTATION(ZHLN::Description<"The ray-traced dither repeats at a short spatial lag; the residual carries a lattice a spatial filter cannot integrate away.">{}),
    DitherIsAnisotropic ZHLN_ANNOTATION(ZHLN::Description<"The ray-traced dither residual has a preferred direction (structured lattice, not blue noise).">{}),
    ResidualDidNotConverge ZHLN_ANNOTATION(ZHLN::Description<"Under temporal accumulation the penumbra residual oscillated instead of falling.">{}),
    RayDebrisDetected ZHLN_ANNOTATION(ZHLN::Description<"The residual is dominated by isolated single-pixel outliers (ray debris / fireflies).">{}),
};

namespace {

using ZHLN::Test::Frame::BBox;
using ZHLN::Test::Frame::BernoulliFit;
using ZHLN::Test::Frame::FitBernoulliNoise;
using ZHLN::Test::Frame::LumaPlane;
using ZHLN::Test::Frame::TemporalMoments;
using ZHLN::Test::Frame::BBoxOfChangedPixels;
using ZHLN::Test::Frame::Crop;
using ZHLN::Test::Frame::LoadPPM;
using ZHLN::Test::Frame::LumaDifference;
using ZHLN::Test::Frame::RgbImage;
using ZHLN::Test::Frame::RmsInRegion;
using ZHLN::Test::Frame::RunningMeanResidualSeries;

constexpr int kWidth  = 640;
constexpr int kHeight = 480;

/// Luma difference threshold, in 0-255 units, above which a pixel counts as
/// having changed between two frames.
constexpr double kChangeThreshold = 2.0;

/// Smallest penumbra bounding box the structural metrics are trustworthy on.
///
/// Deliberately NOT square. A shadow edge is a band, and demanding a square
/// region rejected a perfectly good 104x41 measurement -- the width was fine
/// and only the height fell short of a threshold that existed for no reason
/// other than being one number applied to both axes.
///
/// The floor comes from sweeping 700 crop offsets of the shipped tile at each
/// candidate size, thresholded to 1 bit the way the renderer's visibility is:
///
///   region      px    blue aniso max   blue lobe max   IGN aniso min   IGN lobe min
///    48x16     768        1.2637           0.1732          3.2604         0.8008
///    64x24    1536        1.1964           0.1269          3.3033         0.8379
///   104x41    4264        1.1246           0.0945          3.3050         0.8696
///   128x48    6144        1.1079           0.0870          3.3048         0.8778
///
/// 64x24 keeps the gates below at 1.34x / 2.36x clear of the worst noise crop
/// while the lattice stays 2.07x / 2.79x beyond them. AutocorrelationSideLobe
/// additionally needs >= 2*maxLag+3 = 11 per side, which 24 satisfies.
constexpr int kMinRegionWidth  = 64;
constexpr int kMinRegionHeight = 24;

/// Sun disk half-angle in radians. Two constraints fight over it:
///
///  * small -- the sun disk must subtend a SMALLER angle than the occluder,
///    or no umbra forms and the penumbra degenerates into sparse speckle.
///    At a previous 0.25 the disk (0.25 rad) out-sized the cube (~0.13 rad)
///    and the captured frame was exactly that speckle, with coverage stuck
///    near the lit end.
///  * large -- the penumbra width is sunSize * h / L.y and must stay above the
///    kMinRegionHeight floor.
///
/// 0.15 with the enlarged occluder below threads both: penumbra ~2.4 units and
/// an umbra that lets the coverage span reach the shadowed end. The engine's
/// slider clamp (0.05) is ignored here because nothing else enforces it and
/// frame.sunSize has exactly one consumer, the RT shadow.
constexpr float kSunSize = 0.15f;

} // namespace

struct RayTracedNoiseStabilityTestSuite {
    RayTracedNoiseStabilityTestSuite() {
        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~RayTracedNoiseStabilityTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    /// Pooled: one engine per resolution for the whole binary, with the
    /// scene reset between tests. Creating a Vulkan instance per test is
    /// what eventually exhausts the loader's static TLS and turns the tail
    /// of the group into "vkCreateInstance: Found no drivers!".
    static auto CreateTestEngine() -> ZHLN::Test::Headless::EngineHandle {
        return ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions {
            .appName = "Headless RT Noise Stability", .width = kWidth, .height = kHeight
        });
    }

    /// Pins the requested AA mode and zeroes sub-pixel jitter. Jitter must be
    /// off for the structural scenarios: camera jitter injects its own residual
    /// that has nothing to do with the shadow dither. `AAState::frameIndex` is
    /// deliberately left alone.
    static void SetAA(ZHLN::Engine& engine, ZHLN::AAMode mode) {
        auto& reg = engine.GetRegistry();
        for (const ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>()) {
            reg.Remove<ZHLN::Components::FreeCamTagComponent>(e);
            reg.Patch<ZHLN::Components::AASettingsComponent>(e, [mode](auto& aa) {
                aa.state.mode        = mode;
                aa.state.jitterX     = 0.0f;
                aa.state.jitterY     = 0.0f;
                aa.state.prevJitterX = 0.0f;
                aa.state.prevJitterY = 0.0f;
            });
        }
        engine.GetRenderContext().SetAAState(ZHLN::AAState {.mode = mode});
    }

    /// `PostProcessSettingsComponent::enableRTR` is what
    /// GraphicsSettingsSync.cpp:104 turns into rayTracing.enableReflections,
    /// which RenderGraphBuilder.cpp:1003 then ANDs with a non-null TLAS to form
    /// pc.enableRTR and to pick the lighting pipeline variant. SSR is off and
    /// every material is rough (0.7-0.85, past the 0.4 RTR cutoff), so the only
    /// thing the switch can change is the shadow.
    static void SetRayTracedShadows(ZHLN::Engine& engine, int enableRTR) {
        auto&      reg      = engine.GetRegistry();
        const auto settings = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
        if (settings.empty()) {
            return;
        }
        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings[0], [enableRTR](auto& pp) {
            pp.fullBright        = 0;
            pp.enableSSR         = 0;
            pp.enableRTR         = enableRTR;
            pp.giMode            = 0;
            pp.giIntensity       = 0.0f;
            pp.vignetteIntensity = 0.0f;
            pp.ambientExposure   = 3.0f;
            pp.skyZenith         = JPH::Vec4(0.001f, 0.002f, 0.006f, 1.0f);
            pp.skyHorizon        = JPH::Vec4(0.004f, 0.006f, 0.012f, 1.0f);
            pp.skyGround         = JPH::Vec4(0.001f, 0.001f, 0.002f, 1.0f);
        });
        // Locate the shadow component by type the way the engine's own readers
        // do, rather than assuming it shares the settings entity.
        const auto shadowEnts = reg.GetEntitiesWith<ZHLN::Components::ShadowSettingsComponent>();
        if (!shadowEnts.empty()) {
            reg.Patch<ZHLN::Components::ShadowSettingsComponent>(shadowEnts[0], [](auto& sh) { sh.sunSize = kSunSize; });
        }
    }

    /// The A-Trous HDR denoiser (RenderGraphBuilder MakeHdrDenoisePass) runs
    /// whenever rayTracing.denoiserPasses > 0 and any RT path is on. These
    /// suites measure RAW 1 SPP statistics -- the wavelet output is spatially
    /// correlated, so every raw scenario pins the denoiser off through the
    /// RayTracingSettingsComponent the settings sync reads.
    static bool SetDenoiser(ZHLN::Engine& engine, uint32_t passes) {
        auto&      reg  = engine.GetRegistry();
        const auto ents = reg.GetEntitiesWith<ZHLN::Components::RayTracingSettingsComponent>();
        if (ents.empty()) {
            const ZHLN::Entity e = reg.Create();
            if (e == ZHLN::Entity::Null()) {
                return false;
            }
            reg.Add(e, ZHLN::Components::RayTracingSettingsComponent {.config = ZHLN::RayTracingConfig {.denoiserPasses = passes}});
            return true;
        }
        return reg.Patch<ZHLN::Components::RayTracingSettingsComponent>(ents[0], [passes](auto& c) { c.config.denoiserPasses = passes; });
    }

    /// Ground + a raised occluder + a raking sun, so a broad penumbra lands on
    /// the floor inside the frame. Penumbra width tracks sunSize * height / L.y,
    /// so the occluder sits high.
    static bool BuildShadowScene(ZHLN::Engine& engine) {
        auto& reg = engine.GetRegistry();

        auto floorMat = ZHLN::CreativeWorksFactory::CreateMaterial(
            engine.GetRenderContext(),
            ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.8f, 0.8f, 0.82f, 1.0f}}
        );
        auto boxMat = ZHLN::CreativeWorksFactory::CreateMaterial(
            engine.GetRenderContext(),
            ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.7f, .baseColor = {0.25f, 0.25f, 0.28f, 1.0f}}
        );
        if (!floorMat || !boxMat) {
            return false;
        }

        ZHLN::CreativeWorksFactory::CreatePlane(
            engine, 120.0f, {0.8f, 0.8f, 0.82f, 1.0f},
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *floorMat}
        );
        // Large and raised so the cube's angular size exceeds the sun disk
        // (an umbra forms) while the penumbra still clears the region floor.
        ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(3.0f, 3.0f, 3.0f),
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 15.0, 0.0), .createPhysics = false, .materialOverride = *boxMat}
        );

        const ZHLN::Entity sunEnt = reg.Create();
        reg.Add(
            sunEnt,
            ZHLN::Components::TransformComponent {.position = JPH::Vec3(0.0f, 50.0f, 40.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({40.0f, 0.0f, 0.0f})},
            ZHLN::Components::LightComponent {
                .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 1.0f, 1.0f), .intensity = 120.0f, .direction = JPH::Vec3(0.0f, 0.75f, 0.66f).Normalized()
            }
        );

        auto& cam    = engine.GetCamera();
        cam.position = JPH::Vec3(0.0f, 10.0f, 34.0f);
        cam.yaw      = -90.0f;
        cam.pitch    = -17.0f;
        cam.fov      = 60.0f;
        return true;
    }

    static void TickFrames(ZHLN::Engine& engine, uint32_t frames) {
        constexpr float dt = 1.0f / 60.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            engine.ProcessEvents();
            engine.Tick(dt, ZHLN::GameplayDriver::Cpp);
        }
    }

    static RgbImage Capture(ZHLN::Engine& engine, const std::string& name) {
        const auto res = engine.GetRenderContext().CaptureScreenshotPPM(name);
        if (!res) {
            return {};
        }
        return LoadPPM(name);
    }

    /// Settles the scene, then grabs one frame. Two ticks after a settings
    /// change so the delta-detected GraphicsSettings apply and the history
    /// buffers have caught up before anything is measured.
    static RgbImage SettleAndCapture(ZHLN::Engine& engine, const std::string& name) {
        TickFrames(engine, 2);
        return Capture(engine, name);
    }

    struct Tests {
        /// The 2x2 diagnosis: is the RT shadow in the image at all, and does its
        /// dither move between frames? Every later scenario depends on both, so
        /// this one names the failure instead of reporting a blank residual.
        std::expected<void, ZHLN::Error> rt_shadow_is_live_and_its_dither_moves() {
            auto engine = RayTracedNoiseStabilityTestSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; dither checks are not applicable.");
                return {};
            }
            if (!RayTracedNoiseStabilityTestSuite::BuildShadowScene(*engine)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            RayTracedNoiseStabilityTestSuite::SetAA(*engine, ZHLN::AAMode::None);
            RayTracedNoiseStabilityTestSuite::SetDenoiser(*engine, 0);

            // A: RT on.  B: RT off.  C: RT on again, at a later frame index.
            RayTracedNoiseStabilityTestSuite::SetRayTracedShadows(*engine, 1);
            RgbImage a = RayTracedNoiseStabilityTestSuite::SettleAndCapture(*engine, "rt_noise_live_on.ppm");
            RayTracedNoiseStabilityTestSuite::SetRayTracedShadows(*engine, 0);
            RgbImage b = RayTracedNoiseStabilityTestSuite::SettleAndCapture(*engine, "rt_noise_live_off.ppm");
            RayTracedNoiseStabilityTestSuite::SetRayTracedShadows(*engine, 1);
            RgbImage c = RayTracedNoiseStabilityTestSuite::SettleAndCapture(*engine, "rt_noise_live_on2.ppm");
            if (!ZHLN::Test::ExpectTrue(a.Valid() && b.Valid() && c.Valid())) {
                return std::unexpected(NoiseStabilityError::CaptureFailed);
            }

            const auto onVsOff = ZHLN::Test::Noise::MeasureResidual(a.rgb.data(), b.rgb.data(), kWidth, kHeight, kChangeThreshold);
            const auto onVsOn  = ZHLN::Test::Noise::MeasureResidual(a.rgb.data(), c.rgb.data(), kWidth, kHeight, kChangeThreshold);
            ZHLN::Println(
                "    [INFO] RT on vs off : meanAbs={:.3f} rms={:.3f} changed={:.5f}", onVsOff.meanAbs, onVsOff.rms, onVsOff.changedFraction
            );
            ZHLN::Println(
                "    [INFO] RT on vs on  : meanAbs={:.3f} rms={:.3f} changed={:.5f}", onVsOn.meanAbs, onVsOn.rms, onVsOn.changedFraction
            );

            if (!ZHLN::Test::ExpectTrue(onVsOff.changedFraction > 0.0)) {
                return std::unexpected(NoiseStabilityError::RayTracedShadowPathInactive);
            }
            if (!ZHLN::Test::ExpectTrue(onVsOn.changedFraction > 0.0)) {
                return std::unexpected(NoiseStabilityError::DitherTemporallyFrozen);
            }

            ZHLN::Println("    [PASS] RT shadow reaches the image and its dither advances with the frame index.");
            return {};
        }

        /// The residual must be aperiodic and isotropic -- the direct
        /// blue-noise-vs-lattice test on rendered output. AA is off so the
        /// temporal filter does not blur the structure under test.
        std::expected<void, ZHLN::Error> rt_dither_residual_is_aperiodic_and_isotropic() {
            auto engine = RayTracedNoiseStabilityTestSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; dither structure checks are not applicable.");
                return {};
            }
            if (!RayTracedNoiseStabilityTestSuite::BuildShadowScene(*engine)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            RayTracedNoiseStabilityTestSuite::SetRayTracedShadows(*engine, 1);
            RayTracedNoiseStabilityTestSuite::SetAA(*engine, ZHLN::AAMode::None);
            RayTracedNoiseStabilityTestSuite::SetDenoiser(*engine, 0);

            RgbImage prev = RayTracedNoiseStabilityTestSuite::SettleAndCapture(*engine, "rt_noise_structure_a.ppm");
            RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 1);
            RgbImage cur = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_noise_structure_b.ppm");
            if (!ZHLN::Test::ExpectTrue(prev.Valid() && cur.Valid())) {
                return std::unexpected(NoiseStabilityError::CaptureFailed);
            }

            const auto res = ZHLN::Test::Noise::MeasureResidual(prev.rgb.data(), cur.rgb.data(), kWidth, kHeight, kChangeThreshold);
            if (!ZHLN::Test::ExpectTrue(res.valid)) {
                return std::unexpected(NoiseStabilityError::CaptureFailed);
            }

            const std::vector<double> diff = LumaDifference(prev, cur);
            const BBox                band = BBoxOfChangedPixels(diff.data(), kWidth, kHeight, kChangeThreshold, 4);

            ZHLN::Println(
                "    [INFO] full frame: meanAbs={:.3f} rms={:.3f} changed={:.5f} isolated={:.4f} maxAbs={:.1f}", res.meanAbs, res.rms,
                res.changedFraction, res.isolatedFraction, res.maxAbs
            );
            ZHLN::Println(
                "    [INFO] penumbra bbox = [{},{}) x [{},{})  ({}x{}), region rms={:.3f}", band.x0, band.x1, band.y0, band.y1, band.Width(),
                band.Height(), RmsInRegion(diff.data(), kWidth, band)
            );

            // A zero residual scores a perfect anisotropy of 1.0 and a side
            // lobe of 0.0, so it would satisfy both gates below. Scenario 1
            // already distinguishes "path inactive" from "dither frozen"; here
            // the only remaining explanation for a usable-but-tiny band is that
            // the penumbra is narrower than the metrics can average over.
            if (!ZHLN::Test::ExpectTrue(!band.Empty() && band.Width() >= kMinRegionWidth && band.Height() >= kMinRegionHeight)) {
                return std::unexpected(NoiseStabilityError::PenumbraTooSmallToMeasure);
            }

            const std::vector<double> region = Crop(diff.data(), kWidth, band);
            const auto                dir    = ZHLN::Test::Noise::MeasureDirectionalEnergy(region.data(), band.Width(), band.Height());
            const double              lob = ZHLN::Test::Noise::AutocorrelationSideLobe(region.data(), band.Width(), band.Height(), 4);

            ZHLN::Println(
                "    [INFO] directional e0={:.4f} e45={:.4f} e90={:.4f} e135={:.4f} -> anisotropy={:.4f}", dir.e0, dir.e45, dir.e90, dir.e135,
                dir.Anisotropy()
            );
            ZHLN::Println("    [INFO] positive autocorrelation side lobe (lag<=4) = {:.4f}", lob);

            // Thresholds are measured, not guessed. Running this exact pipeline
            // over a synthetic 406x189 penumbra built from the shipped tile and
            // the engine's own GetShadowDither gives:
            //
            //                          anisotropy   positive side lobe
            //   IGN lattice dither       2.8658          0.9410
            //   blue noise dither        1.0382          0.0385
            //
            // 1.60 and 0.30 sit between the two regimes.
            if (!ZHLN::Test::ExpectTrue(dir.Anisotropy() < 1.60)) {
                return std::unexpected(NoiseStabilityError::DitherIsAnisotropic);
            }
            if (!ZHLN::Test::ExpectTrue(lob < 0.30)) {
                return std::unexpected(NoiseStabilityError::DitherIsPeriodic);
            }

            ZHLN::Println("    [PASS] RT dither residual is isotropic and aperiodic (blue noise, not a lattice).");
            return {};
        }

        /// Is there the right AMOUNT of noise? The structural scenario says what
        /// shape it has; this says whether the magnitude is what a 1 SPP
        /// stochastic shadow must produce -- not too little (dither not actually
        /// modulating visibility), not too much (instability on top).
        ///
        /// A one-sample shadow makes each penumbra pixel a Bernoulli draw, so
        /// across frames it takes exactly two values and its temporal variance
        /// must equal p*(1-p)*d^2. That is exact whatever the tone curve does,
        /// because the curve is applied before the draw and cannot create a
        /// third value -- verified: the estimator returns 1.0000 on synthetic
        /// 1 SPP shadows both linear and tone-mapped.
        std::expected<void, ZHLN::Error> rt_dither_noise_magnitude_matches_one_sample() {
            auto engine = RayTracedNoiseStabilityTestSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; noise magnitude check is not applicable.");
                return {};
            }
            if (!RayTracedNoiseStabilityTestSuite::BuildShadowScene(*engine)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            RayTracedNoiseStabilityTestSuite::SetRayTracedShadows(*engine, 1);
            RayTracedNoiseStabilityTestSuite::SetAA(*engine, ZHLN::AAMode::None);
            RayTracedNoiseStabilityTestSuite::SetDenoiser(*engine, 0);
            RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 2);

            // Enough frames that the per-pixel mean and variance are stable but
            // the run stays well inside the 60 s CTest timeout.
            constexpr uint32_t kFrames         = 24;
            constexpr double   kClusterTol     = 1.5;

            TemporalMoments moments;
            moments.Reset(kWidth, kHeight);
            RgbImage first;
            BBox     band;
            for (uint32_t f = 0; f < kFrames; ++f) {
                RgbImage img = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_noise_magnitude.ppm");
                if (!ZHLN::Test::ExpectTrue(img.Valid())) {
                    return std::unexpected(NoiseStabilityError::CaptureFailed);
                }
                const std::vector<double> plane = LumaPlane(img);
                if (f == 0) {
                    first = img;
                } else if (f == 1) {
                    const std::vector<double> d = LumaDifference(first, img);
                    band = BBoxOfChangedPixels(d.data(), kWidth, kHeight, kChangeThreshold, 4);
                }
                moments.AddLuma(plane.data(), kClusterTol);
                if (f + 1 < kFrames) {
                    RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 1);
                }
            }
            if (!ZHLN::Test::ExpectTrue(!band.Empty() && band.Width() >= kMinRegionWidth && band.Height() >= kMinRegionHeight)) {
                return std::unexpected(NoiseStabilityError::PenumbraTooSmallToMeasure);
            }

            const BernoulliFit fit = FitBernoulliNoise(moments, band, kClusterTol);
            ZHLN::Println(
                "    [INFO] band [{},{}) x [{},{})  pixels used={} of {}", band.x0, band.x1, band.y0, band.y1, fit.pixelsUsed,
                fit.pixelsInRegion
            );
            ZHLN::Println(
                "    [INFO] shadowed level A={:.2f}  flip amplitude d={:.2f}  range-growth rate={:.4f} (two-valued implies ~{:.4f})",
                fit.shadowedLevel, fit.amplitude, fit.offClusterFrac, 1.0 / static_cast<double>(kFrames)
            );
            ZHLN::Println(
                "    [INFO] measured variance sum={:.1f}  expected p(1-p)d^2 sum={:.1f}  -> ratio={:.4f}", fit.measuredVarSum,
                fit.expectedVarSum, fit.ratio
            );
            ZHLN::Println("    [INFO] fitted coverage spans [{:.3f}, {:.3f}]", fit.coverageMin, fit.coverageMax);

            if (!ZHLN::Test::ExpectTrue(fit.valid)) {
                return std::unexpected(NoiseStabilityError::NoiseMagnitudeMismatch);
            }
            // The shadow must actually develop: coverage reaching the lit end
            // and the shadowed end. If the sun disk out-sizes the occluder the
            // span collapses toward the lit end and the structural/magnitude
            // gates would be measuring speckle rather than a shadow.
            if (!ZHLN::Test::ExpectTrue(fit.coverageMin < 0.30 && fit.coverageMax > 0.70)) {
                return std::unexpected(NoiseStabilityError::ShadowTooWeakToTest);
            }
            // 1 SPP is not an assumption, it is what the shader does:
            // lighting.slang calls CalculateShadowRayTraced without a `samples`
            // argument, so the 1u default applies. The tolerance is the
            // finite-sample error of a 24-frame variance estimate, not slack.
            if (!ZHLN::Test::ExpectTrue(fit.ratio > 0.85 && fit.ratio < 1.15)) {
                return std::unexpected(NoiseStabilityError::NoiseMagnitudeMismatch);
            }
            // A two-valued pixel needs exactly one range-growth event over the
            // run; materially more means the pixel is taking more than two
            // levels, i.e. something is filtering or adding noise on top.
            if (!ZHLN::Test::ExpectTrue(fit.offClusterFrac < 0.12)) {
                return std::unexpected(NoiseStabilityError::NoiseMagnitudeMismatch);
            }

            ZHLN::Println("    [PASS] Noise magnitude is exactly what a 1 SPP stochastic shadow must produce.");
            return {};
        }

        /// The penumbra noise must integrate away: the running mean over n
        /// captures must approach the all-capture mean at the Monte Carlo
        /// rate. Accumulation happens here on the CPU with AA off, because
        /// the engine's TAA feedback gives consecutive-frame RMS a floor of
        /// feedbackWeight * sigma -- the metric this scenario used to run
        /// only ever passed inside the brief post-reset transient, and once
        /// the Kawase bloom chain was fixed to composite real glow the
        /// transient deformed (a mid-window spike) and the fitted slope
        /// collapsed. The reflection suite made the same migration.
        std::expected<void, ZHLN::Error> rt_residual_converges_with_temporal_accumulation() {
            auto engine = RayTracedNoiseStabilityTestSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; convergence check is not applicable.");
                return {};
            }
            if (!RayTracedNoiseStabilityTestSuite::BuildShadowScene(*engine)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            RayTracedNoiseStabilityTestSuite::SetRayTracedShadows(*engine, 1);
            RayTracedNoiseStabilityTestSuite::SetAA(*engine, ZHLN::AAMode::None);
            RayTracedNoiseStabilityTestSuite::SetDenoiser(*engine, 0);
            RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 2);

            RgbImage first = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_noise_conv_prev.ppm");
            if (!ZHLN::Test::ExpectTrue(first.Valid())) {
                return std::unexpected(NoiseStabilityError::CaptureFailed);
            }

            // Fix the measurement window on the first pair and reuse it: the
            // shadow is static, so the window does not move, and holding it
            // fixed keeps the series comparable.
            RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 1);
            RgbImage second = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_noise_conv_cur.ppm");
            if (!ZHLN::Test::ExpectTrue(second.Valid())) {
                return std::unexpected(NoiseStabilityError::CaptureFailed);
            }
            const std::vector<double> firstDiff = LumaDifference(first, second);
            const BBox                band      = BBoxOfChangedPixels(firstDiff.data(), kWidth, kHeight, kChangeThreshold, 4);
            if (!ZHLN::Test::ExpectTrue(!band.Empty() && band.Width() >= kMinRegionWidth && band.Height() >= kMinRegionHeight)) {
                return std::unexpected(NoiseStabilityError::PenumbraTooSmallToMeasure);
            }
            ZHLN::Println("    [INFO] measuring convergence over [{},{}) x [{},{})", band.x0, band.x1, band.y0, band.y1);

            constexpr int kFrames = 24;
            std::vector<double>                              acc(static_cast<std::size_t>(kWidth) * kHeight, 0.0);
            std::vector<std::pair<int, std::vector<double>>> snapshots;
            int                                              total = 0;
            const auto addFrame = [&](const RgbImage& img) {
                const std::vector<double> pl = LumaPlane(img);
                for (std::size_t i = 0; i < acc.size(); ++i) {
                    acc[i] += pl[i];
                }
                ++total;
                if (total % 4 == 0) {
                    std::vector<double> mean = acc;
                    for (double& v: mean) {
                        v /= static_cast<double>(total);
                    }
                    snapshots.emplace_back(total, std::move(mean));
                }
            };
            addFrame(first);
            addFrame(second);
            while (total < kFrames) {
                RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 1);
                RgbImage cur = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_noise_conv_cur.ppm");
                if (!ZHLN::Test::ExpectTrue(cur.Valid())) {
                    return std::unexpected(NoiseStabilityError::CaptureFailed);
                }
                addFrame(cur);
            }
            std::vector<double> finalMean = acc;
            for (double& v: finalMean) {
                v /= static_cast<double>(total);
            }

            const std::vector<double> series = RunningMeanResidualSeries(snapshots, finalMean, kWidth, band);
            const double              slope  = ZHLN::Test::Noise::LinearSlope(series);
            const double              mean   = std::accumulate(series.begin(), series.end(), 0.0) / static_cast<double>(series.size());
            const double              fittedDrop = -slope * static_cast<double>(series.size() - 1);
            for (std::size_t i = 0; i < series.size(); ++i) {
                ZHLN::Println("    [INFO] running-mean residual after {} frames: {:.4f}", snapshots[i].first, series[i]);
            }
            ZHLN::Println(
                "    [INFO] mean={:.4f} slope={:.6f} predicted drop over window={:.4f} ({:.1f}% of mean)", mean, slope, fittedDrop,
                mean > 0.0 ? 100.0 * fittedDrop / mean : 0.0
            );

            // The first snapshot (4 of 24 frames) sits near
            // sigma*sqrt(1/4 - 1/24) ~= 0.46*sigma and the last at 0, so
            // genuine integrable noise drops ~100% of the window mean; the
            // >0.5 floor rejects a frozen dither whose series is ~0
            // everywhere and would satisfy the fitted-drop gate vacuously.
            // Same gate as the reflection suite, which measured a 212% drop
            // of the window mean on real hardware.
            const bool converged = series.front() > 0.5 && slope < 0.0 && fittedDrop > 0.35 * mean;
            if (!ZHLN::Test::ExpectTrue(converged)) {
                return std::unexpected(NoiseStabilityError::ResidualDidNotConverge);
            }

            ZHLN::Println("    [PASS] Penumbra residual falls under temporal accumulation.");
            return {};
        }

        /// Changed pixels must cluster. Isolated single-pixel changes are ray
        /// debris or fireflies, not stochastic shadow noise.
        std::expected<void, ZHLN::Error> rt_residual_has_no_isolated_ray_debris() {
            auto engine = RayTracedNoiseStabilityTestSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; debris check is not applicable.");
                return {};
            }
            if (!RayTracedNoiseStabilityTestSuite::BuildShadowScene(*engine)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            RayTracedNoiseStabilityTestSuite::SetRayTracedShadows(*engine, 1);
            RayTracedNoiseStabilityTestSuite::SetAA(*engine, ZHLN::AAMode::None);
            RayTracedNoiseStabilityTestSuite::SetDenoiser(*engine, 0);

            RgbImage prev = RayTracedNoiseStabilityTestSuite::SettleAndCapture(*engine, "rt_noise_debris_a.ppm");
            RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 1);
            RgbImage cur = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_noise_debris_b.ppm");
            if (!ZHLN::Test::ExpectTrue(prev.Valid() && cur.Valid())) {
                return std::unexpected(NoiseStabilityError::CaptureFailed);
            }

            const auto res = ZHLN::Test::Noise::MeasureResidual(prev.rgb.data(), cur.rgb.data(), kWidth, kHeight, kChangeThreshold);
            ZHLN::Println(
                "    [INFO] changed={:.5f} isolated-of-changed={:.4f} maxAbs={:.1f}", res.changedFraction, res.isolatedFraction, res.maxAbs
            );

            // Not a discriminator -- the synthetic blue noise penumbra scores
            // 0.163 and the IGN lattice 0.038, so a lattice is *more*
            // clustered. What this catches is the other failure mode: isolated
            // ray debris and fireflies. Scenario 1 already proved the dither
            // moves, so a zero here would contradict it; treat that as a
            // capture problem rather than passing on nothing.
            if (!ZHLN::Test::ExpectTrue(res.changedFraction > 1e-5)) {
                return std::unexpected(NoiseStabilityError::DitherTemporallyFrozen);
            }
            if (!ZHLN::Test::ExpectTrue(res.isolatedFraction < 0.6)) {
                return std::unexpected(NoiseStabilityError::RayDebrisDetected);
            }

            ZHLN::Println("    [PASS] Changed pixels cluster; no isolated ray debris.");
            return {};
        }
    };
};

// Exported for the GPU_Lighting group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunRayTracedNoiseStabilitySuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<RayTracedNoiseStabilityTestSuite>();
}

