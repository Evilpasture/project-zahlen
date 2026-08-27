// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestRayTracedNoiseStability.cpp
//
// GPU-side verification that the ray-traced dither behaves like blue noise.
//
// The ray-traced sun shadow used to dither with Interleaved Gradient Noise and
// trace one rotated Poisson point per pixel. It now samples a blue noise tile
// and a Vogel disk over the sun (pbr_helpers.slang:
// CalculateShadowRayTraced, gated in lighting.slang on `pc.enableRTR != 0`).
// Both old and new produce a stochastic 1-bit visibility, so a frame-comparison
// test that only measures *magnitude* passes either way -- IGN is not louder
// than blue noise, it is structured. What changed is the structure, and that is
// what this suite measures, on real captured PPMs:
//
//   1. PERIODICITY  - the frame-to-frame residual must not correlate positively
//                     with itself at a short lag. An IGN lattice does, at its
//                     own period, which is why an A-Trous wavelet or TAA smears
//                     it into streaks instead of averaging it away.
//   2. ISOTROPY     - the residual must not prefer a direction. IGN shares phase
//                     along its lattice axes, so its diagonal gradient energy
//                     drops and any spatial filter drags the residual along
//                     those axes.
//   3. CONVERGENCE  - with the temporal accumulator on, the residual in the
//                     penumbra must shrink across frames. Noise that is
//                     regenerated rather than converged holds its level.
//   4. NO DEBRIS    - changed pixels must be spatially clustered. Single-pixel
//                     outliers are ray debris / fireflies, not noise.
//
// The analysis lives in tests/RayTracedNoiseMetrics.hpp, which is validated
// separately on the CPU (tests/TestRayTracedNoiseMetrics.cpp) against a real
// IGN lattice, the shipped blue noise tile and white noise -- so the thresholds
// below cannot be satisfied by an arbitrary noise pattern, and their margins
// are measured rather than guessed.
//
// Measurement region: the penumbra is a few dozen pixels wide in an otherwise
// static 640x480 frame, so every structural metric is taken over the bounding
// box of the pixels that actually changed. Feeding the metrics the whole frame
// would dilute a narrow band with hundreds of thousands of exact zeros.
//
// Scenario 1 runs with AA disabled on purpose: the temporal filter would blur
// the very structure under test. Scenario 3 turns it back on to check the
// opposite property.

#include "NoiseFrameCapture.hpp"
#include "RayTracedNoiseMetrics.hpp"
#include "TestsFramework.hpp"
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
#include <fstream>
#include <memory>
#include <string>
#include <vector>

enum class NoiseStabilityError : uint8_t {
    EngineInitFailed[[= ZHLN::Reflect::Description("Failed to initialize the headless Engine for the RT noise stability test.")]] = 1,
    CaptureFailed[[= ZHLN::Reflect::Description("A frame could not be captured or the PPM could not be read back.")]],
    NoPenumbraFound[[= ZHLN::Reflect::Description(
        "No usable per-frame variation was found; the ray-traced dither is not reaching the measured pixels, so the structural gates would pass vacuously."
    )]],
    DitherIsPeriodic[[= ZHLN::Reflect::Description(
        "The ray-traced dither repeats at a short spatial lag; the residual carries a lattice a spatial filter cannot integrate away."
    )]],
    DitherIsAnisotropic[[= ZHLN::Reflect::Description("The ray-traced dither residual has a preferred direction (structured lattice, not blue noise).")]],
    ResidualDidNotConverge[[= ZHLN::Reflect::Description("With the temporal accumulator enabled the penumbra residual did not shrink across frames.")]],
    RayDebrisDetected[[= ZHLN::Reflect::Description("The residual is dominated by isolated single-pixel outliers (ray debris / fireflies).")]],
};

namespace {

using ZHLN::Test::Frame::BBox;
using ZHLN::Test::Frame::BBoxOfChangedPixels;
using ZHLN::Test::Frame::Crop;
using ZHLN::Test::Frame::LoadPPM;
using ZHLN::Test::Frame::LumaDifference;
using ZHLN::Test::Frame::RgbImage;
using ZHLN::Test::Frame::RmsInRegion;

constexpr int kWidth  = 640;
constexpr int kHeight = 480;

/// Luma difference threshold, in 0-255 units, above which a pixel counts as
/// having changed between two frames.
constexpr double kChangeThreshold = 2.0;

/// Smallest penumbra bounding box that carries enough samples for
/// AutocorrelationSideLobe (which needs >= 2*maxLag+3 = 11 per side) and for
/// the four-directional gradient energies to be statistically meaningful.
constexpr int kMinRegion = 48;

} // namespace

struct RayTracedNoiseStabilityTestSuite {
    RayTracedNoiseStabilityTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~RayTracedNoiseStabilityTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine() -> std::unique_ptr<ZHLN::Engine> {
        ZHLN::DefaultPreset::SetDisabled(true);
        const ZHLN::EngineConfig cfg {
            .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
            .render  = {
                 .appName        = "Headless RT Noise Stability",
                 .width          = kWidth,
                 .height         = kHeight,
                 .vsync          = false,
                 .fullscreen     = false,
                 .validationMode = ZHLN::ValidationMode::On,
                 .headless       = true
            }
        };
        auto engineRes = ZHLN::Engine::Create(cfg);
        if (!engineRes) {
            return nullptr;
        }
        auto engine = std::move(engineRes.value());
        engine->InitializeDefaultScene();
        return engine;
    }

    /// Pins the requested AA mode and zeroes sub-pixel jitter. Jitter must be
    /// off for the structural scenarios: camera jitter injects its own residual
    /// that has nothing to do with the shadow dither. `AAState::frameIndex` is
    /// deliberately left alone -- the blue noise temporal scroll is derived from
    /// `camPos.w`, which RenderSystem drives off GetCurrentFrame(), so the
    /// dither advances whether or not the TAA counter is reset.
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

    /// `enableRTR` is the switch lighting.slang actually reads to take the
    /// ray-traced sun shadow instead of the cascade map. SSR is off so no
    /// screen-space reflection contributes a residual of its own, and the sky /
    /// vignette are flattened so the only thing moving between frames is the
    /// shadow.
    static void EnableRayTracedShadows(ZHLN::Engine& engine) {
        auto&      reg      = engine.GetRegistry();
        const auto settings = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
        if (settings.empty()) {
            return;
        }
        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings[0], [](auto& pp) {
            pp.fullBright        = 0;
            pp.enableSSR         = 0;
            pp.enableRTR         = 1;
            pp.giMode            = 0;
            pp.giIntensity       = 0.0f;
            pp.vignetteIntensity = 0.0f;
            pp.ambientExposure   = 3.0f;
            pp.skyZenith         = JPH::Vec4(0.001f, 0.002f, 0.006f, 1.0f);
            pp.skyHorizon        = JPH::Vec4(0.004f, 0.006f, 0.012f, 1.0f);
            pp.skyGround         = JPH::Vec4(0.001f, 0.001f, 0.002f, 1.0f);
        });
        // ShadowSettingsComponent shares the settings entity with the post
        // process one (src/engine/Engine.cpp:688 adds GlobalSettingsTag,
        // PostProcessSettings and ShadowSettings together), but look it up
        // through its own archetype the way GraphicsSettingsSync does so a
        // future split does not turn this into a silent no-op.
        const auto shadowEnts = reg.GetEntitiesWith<ZHLN::Components::ShadowSettingsComponent>();
        if (!shadowEnts.empty()) {
            reg.Patch<ZHLN::Components::ShadowSettingsComponent>(shadowEnts[0], [](auto& sh) {
                // Widest softness the engine's own UI offers (src/main.cpp
                // clamps the slider to 0.05). A wide sun disk widens the
                // penumbra, which is what puts enough per-frame-varying pixels
                // on screen for the structural metrics to average over.
                sh.sunSize = 0.05f;
            });
        }
    }

    /// Ground + a raised occluder + a raking sun, so a broad penumbra lands on
    /// the floor inside the frame.
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
            engine, 60.0f, {0.8f, 0.8f, 0.82f, 1.0f},
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *floorMat}
        );
        // Raised so the penumbra (sunSize * height-above-floor) is wide enough
        // to span dozens of pixels.
        ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(1.5f, 1.5f, 1.5f),
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 5.0, 0.0), .createPhysics = false, .materialOverride = *boxMat}
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
        cam.position = JPH::Vec3(0.0f, 6.0f, 14.0f);
        cam.yaw      = -90.0f;
        cam.pitch    = -12.0f;
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

    struct Tests {
        /// The residual must be aperiodic and isotropic. This is the direct
        /// blue-noise-vs-lattice test on rendered output.
        std::expected<void, ZHLN::Error> rt_dither_residual_is_aperiodic_and_isotropic() {
            auto engine = RayTracedNoiseStabilityTestSuite::CreateTestEngine();
            if (!ZHLN::Test::AssertTrue(engine != nullptr)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; dither structure checks are not applicable.");
                return {};
            }

            EnableRayTracedShadows(*engine);
            if (!RayTracedNoiseStabilityTestSuite::BuildShadowScene(*engine)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            // AA off: the temporal filter would blur the structure under test.
            RayTracedNoiseStabilityTestSuite::SetAA(*engine, ZHLN::AAMode::None);
            RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 4);

            RgbImage prev = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_noise_structure_a.ppm");
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
            const BBox band = BBoxOfChangedPixels(diff.data(), kWidth, kHeight, kChangeThreshold, 4);

            ZHLN::Println(
                "    [INFO] full frame: meanAbs={:.3f} rms={:.3f} changed={:.5f} isolated={:.4f} maxAbs={:.1f}", res.meanAbs, res.rms,
                res.changedFraction, res.isolatedFraction, res.maxAbs
            );

            // Guard against a vacuous pass. A zero residual scores a perfect
            // anisotropy of 1.0 and a side lobe of 0.0, so a dither that never
            // reaches the screen would satisfy both gates below. One sample per
            // pixel over a per-frame-varying Vogel disk flips roughly half the
            // penumbra pixels every frame, so an empty or thread-like band
            // means the ray-traced shadow is not in the image.
            const bool regionUsable = !band.Empty() && band.Width() >= kMinRegion && band.Height() >= kMinRegion;
            ZHLN::Println(
                "    [INFO] penumbra bbox = [{},{}) x [{},{})  ({}x{}), region rms={:.3f}", band.x0, band.x1, band.y0, band.y1, band.Width(),
                band.Height(), RmsInRegion(diff.data(), kWidth, band)
            );
            if (!ZHLN::Test::ExpectTrue(regionUsable)) {
                return std::unexpected(NoiseStabilityError::NoPenumbraFound);
            }

            const std::vector<double> region = Crop(diff.data(), kWidth, band);
            const auto                dir    = ZHLN::Test::Noise::MeasureDirectionalEnergy(region.data(), band.Width(), band.Height());
            const double              lob = ZHLN::Test::Noise::AutocorrelationSideLobe(region.data(), band.Width(), band.Height(), 4);

            ZHLN::Println(
                "    [INFO] directional e0={:.4f} e45={:.4f} e90={:.4f} e135={:.4f} -> anisotropy={:.4f}", dir.e0, dir.e45, dir.e90, dir.e135,
                dir.Anisotropy()
            );
            ZHLN::Println("    [INFO] positive autocorrelation side lobe (lag<=4) = {:.4f}", lob);

            // Thresholds are measured, not guessed. Running this exact
            // pipeline (LumaDifference -> BBoxOfChangedPixels -> Crop ->
            // MeasureDirectionalEnergy / AutocorrelationSideLobe) over a
            // synthetic 406x189 penumbra built from the shipped tile and the
            // engine's own GetShadowDither gives:
            //
            //                          anisotropy   positive side lobe
            //   IGN lattice dither       2.8658          0.9410
            //   blue noise dither        1.0382          0.0385
            //
            // 1.60 and 0.30 sit between the two regimes: 1.5x above the blue
            // noise result and 1.8x / 3.1x below the lattice one. The CPU
            // suite (tests/TestRayTracedNoiseMetrics.cpp) independently
            // validates that the underlying metrics separate the two on the
            // real asset.
            if (!ZHLN::Test::ExpectTrue(dir.Anisotropy() < 1.60)) {
                return std::unexpected(NoiseStabilityError::DitherIsAnisotropic);
            }
            if (!ZHLN::Test::ExpectTrue(lob < 0.30)) {
                return std::unexpected(NoiseStabilityError::DitherIsPeriodic);
            }

            ZHLN::Println("    [PASS] RT dither residual is isotropic and aperiodic (blue noise, not a lattice).");
            return {};
        }

        /// With the temporal accumulator on, the penumbra residual must fall.
        /// This is what the blue noise temporal scroll is for: consecutive
        /// frames land on well-separated R2 points, so the history converges
        /// instead of revisiting the same offsets.
        std::expected<void, ZHLN::Error> rt_residual_converges_with_temporal_accumulation() {
            auto engine = RayTracedNoiseStabilityTestSuite::CreateTestEngine();
            if (!ZHLN::Test::AssertTrue(engine != nullptr)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; convergence check is not applicable.");
                return {};
            }

            EnableRayTracedShadows(*engine);
            if (!RayTracedNoiseStabilityTestSuite::BuildShadowScene(*engine)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            RayTracedNoiseStabilityTestSuite::SetAA(*engine, ZHLN::AAMode::TAA);
            RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 3);

            // Captures reuse two filenames rather than accumulating one PPM per
            // frame: only the current and previous frame are ever read, and
            // *.ppm is not gitignored, so a suite that leaves eight files
            // behind per run would dirty the working tree.
            RgbImage prev = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_noise_conv_prev.ppm");
            if (!ZHLN::Test::ExpectTrue(prev.Valid())) {
                return std::unexpected(NoiseStabilityError::CaptureFailed);
            }

            // Fix the measurement window on the first pair, then reuse it for
            // every later pair: the shadow is static, so the window does not
            // move, and holding it fixed keeps the series comparable.
            RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 1);
            RgbImage second = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_noise_conv_cur.ppm");
            if (!ZHLN::Test::ExpectTrue(second.Valid())) {
                return std::unexpected(NoiseStabilityError::CaptureFailed);
            }

            const std::vector<double> firstDiff = LumaDifference(prev, second);
            const BBox band = BBoxOfChangedPixels(firstDiff.data(), kWidth, kHeight, kChangeThreshold, 4);
            if (!ZHLN::Test::ExpectTrue(!band.Empty() && band.Width() >= kMinRegion && band.Height() >= kMinRegion)) {
                return std::unexpected(NoiseStabilityError::NoPenumbraFound);
            }
            ZHLN::Println("    [INFO] measuring convergence over [{},{}) x [{},{})", band.x0, band.x1, band.y0, band.y1);

            std::vector<double> rmsSeries;
            rmsSeries.push_back(RmsInRegion(firstDiff.data(), kWidth, band));
            prev = std::move(second);

            for (uint32_t f = 2; f < 8; ++f) {
                RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 1);
                RgbImage cur = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_noise_conv_cur.ppm");
                if (!ZHLN::Test::ExpectTrue(cur.Valid())) {
                    return std::unexpected(NoiseStabilityError::CaptureFailed);
                }
                const std::vector<double> d = LumaDifference(prev, cur);
                rmsSeries.push_back(RmsInRegion(d.data(), kWidth, band));
                prev = std::move(cur);
            }

            const double slope = ZHLN::Test::Noise::LinearSlope(rmsSeries);
            for (std::size_t i = 0; i < rmsSeries.size(); ++i) {
                ZHLN::Println("    [INFO] penumbra residual RMS frame {} -> {}: {:.4f}", i + 1, i + 2, rmsSeries[i]);
            }
            ZHLN::Println("    [INFO] least-squares slope = {:.6f} (negative = converging)", slope);

            // A settled accumulator flattens out rather than driving the
            // residual to zero, so the gate is "not growing" plus a real drop
            // from the first frame -- a scene that is still resolving must
            // actually improve.
            const bool notGrowing = rmsSeries.back() <= rmsSeries.front() * 1.05;
            const bool improved   = rmsSeries.back() < rmsSeries.front() * 0.95;
            if (!ZHLN::Test::ExpectTrue(notGrowing && improved)) {
                return std::unexpected(NoiseStabilityError::ResidualDidNotConverge);
            }

            ZHLN::Println("    [PASS] Penumbra residual falls under temporal accumulation.");
            return {};
        }

        /// Changed pixels must cluster. Isolated single-pixel changes are ray
        /// debris or fireflies, not stochastic shadow noise.
        std::expected<void, ZHLN::Error> rt_residual_has_no_isolated_ray_debris() {
            auto engine = RayTracedNoiseStabilityTestSuite::CreateTestEngine();
            if (!ZHLN::Test::AssertTrue(engine != nullptr)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; debris check is not applicable.");
                return {};
            }

            EnableRayTracedShadows(*engine);
            if (!RayTracedNoiseStabilityTestSuite::BuildShadowScene(*engine)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            RayTracedNoiseStabilityTestSuite::SetAA(*engine, ZHLN::AAMode::None);
            RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 4);

            RgbImage prev = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_noise_debris_a.ppm");
            RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 1);
            RgbImage cur = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_noise_debris_b.ppm");
            if (!ZHLN::Test::ExpectTrue(prev.Valid() && cur.Valid())) {
                return std::unexpected(NoiseStabilityError::CaptureFailed);
            }

            const auto res = ZHLN::Test::Noise::MeasureResidual(prev.rgb.data(), cur.rgb.data(), kWidth, kHeight, kChangeThreshold);
            ZHLN::Println(
                "    [INFO] changed={:.5f} isolated-of-changed={:.4f} maxAbs={:.1f}", res.changedFraction, res.isolatedFraction, res.maxAbs
            );

            // isolatedFraction is a ratio, so it is independent of where in
            // the frame the noise sits; no crop needed. It is meaningless on a
            // fully static pair, so skip rather than pass on nothing.
            //
            // This gate is deliberately loose and is not a discriminator: the
            // synthetic blue noise penumbra scores 0.163 and the IGN lattice
            // scores 0.038, i.e. a lattice is *more* clustered. What it catches
            // is the other failure mode -- single-pixel ray debris or
            // fireflies, which are isolated by definition.
            if (res.changedFraction < 1e-5) {
                ZHLN::Println("    [INFO] No changed pixels; debris classification skipped.");
                return {};
            }
            if (!ZHLN::Test::ExpectTrue(res.isolatedFraction < 0.6)) {
                return std::unexpected(NoiseStabilityError::RayDebrisDetected);
            }

            ZHLN::Println("    [PASS] Changed pixels cluster; no isolated ray debris.");
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<RayTracedNoiseStabilityTestSuite>();
}
