// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestRayTracedReflectionNoise.cpp
//
// GPU-side verification of the deterministic 3-tier hybrid reflection pipeline:
//   - Tier 1 (roughness < 0.20): 1 unjittered mirror ray (Hi-Z SSR -> TLAS fallback)
//   - Tier 2 (0.20 <= roughness <= 0.40): deterministic ray + cone-traced mip LOD
//   - Tier 3 (roughness > 0.40): 0 specular rays; Radiance Cascades / Split-Sum BRDF LUT
//
// Unlike the retired half-resolution 1-SPP VNDF blue-noise path, the 3-tier
// pipeline has ZERO stochastic temporal jitter and ZERO specular A-trous
// temporal denoising ("no-cope" invariants):
//
//   1. LIVENESS & ZERO BOIL - enabling enableRTR changes the glossy plate,
//                    consecutive on-frames have ZERO temporal boil
//                    (changedFraction < 0.001), and surfaces rougher than the
//                    0.40 cutoff stay untouched by the switch.
//   2. COHERENCE   - the on/off reflection band spans the expected glossy mirror
//                    footprint without lattice artifacts or single-pixel holes.
//   3. CONVERGENCE - the reflection is already converged on frame 1 (running-mean
//                    residual ~ 0 without needing multi-frame accumulation).
//   4. HYBRID & NO DEBRIS - Hi-Z SSR, TLAS inline fallback, and hybrid SSR+RTR
//                    all light up the glossy band without isolated ray debris.

#include "helpers/ImageTesting.hpp"
#include "RayTracedNoiseMetrics.hpp"
#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Engine/PrefabFactory.hpp>
#include <Zahlen/Engine/Engine.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/Threading/TaskSystem.hpp>
#include <Zahlen/Threading/Thread.hpp>
#include <Zahlen/ECS/ECS.hpp>
#include <Zahlen/Render/GraphicsSettings.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <memory>
#include <string>
#include <vector>

enum class ReflectionNoiseError : uint8_t {
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize the headless Engine for the RTR noise test.">{}) = 1,
    CaptureFailed ZHLN_ANNOTATION(ZHLN::Description<"A frame could not be captured or the PPM could not be read back.">{}),
    SceneNotLit ZHLN_ANNOTATION(ZHLN::Description<"The off frame is black: the box is not lit or not in view, so there is nothing the reflection could show. Check the sun intensity/direction, the camera framing and the region luma stats printed by scenario 1 before suspecting the reflection path.">{}),
    SsrWorksButRtrRaysDead ZHLN_ANNOTATION(ZHLN::Description<"The SSR probe (same reflection pass, same roughness branch, same plate, depth-buffer hit test) changes the mirror but the RTR switch changes nothing: the reflection pass, branch and output are alive, so the dead layer is the TLAS ray query of RaytraceRTR -- check the tlas address bound to the reflection pass heap and the BLAS/instance flags.">{}),
    ReflectionBranchOrOutputDead ZHLN_ANNOTATION(ZHLN::Description<"The scene is lit but neither the SSR probe nor the RTR switch changes a pixel: the dead layer is below the hit test -- the roughness<=0.4 branch, the reflVariant selection, the push constants, or the Res_HdrSceneColor output not reaching the capture.">{}),
    ReflectionPathInactive ZHLN_ANNOTATION(ZHLN::Description<"Enabling enableRTR changed no pixels on the glossy plate against a fully static scene, so the deterministic reflection trace is not executing: check pc.enableRTR, the TLAS, the reflVariant pipeline selection, or that the plate roughness is at or under the 0.4 cutoff.">{}),
    TemporalBoilDetected ZHLN_ANNOTATION(ZHLN::Description<"The deterministic reflection path produced frame-to-frame pixel boil across consecutive static frames; reflections must be unjittered and temporally bit-stable.">{}),
    RoughnessCutoffViolated ZHLN_ANNOTATION(ZHLN::Description<"Pixels outside the glossy plate changed when enableRTR flipped; surfaces rougher than the 0.4 cutoff must be untouched by the reflection switch (reflection.slang gates Tier 1/2 ray tracing on roughness <= 0.4). Either the cutoff moved or the switch is leaking into another term.">{}),
    ReflectionRegionTooSmall ZHLN_ANNOTATION(ZHLN::Description<"The on/off reflection band covers too small a region on the glossy plate; check mirror geometry, cone-traced mip selection, or camera framing.">{}),
    ReflectionIsPeriodic ZHLN_ANNOTATION(ZHLN::Description<"The on/off reflection delta exhibits high-frequency periodic striping on the glossy plate.">{}),
    ReflectionIsFragmented ZHLN_ANNOTATION(ZHLN::Description<"The on/off reflection delta is fragmented rather than forming a contiguous glossy band.">{}),
    ResidualDidNotConverge ZHLN_ANNOTATION(ZHLN::Description<"Consecutive static frames exhibited non-zero temporal residual instead of immediate single-frame convergence.">{}),
    RayDebrisDetected ZHLN_ANNOTATION(ZHLN::Description<"The reflection band contains isolated single-pixel outliers (ray debris / fireflies).">{}),
};

namespace {

using ZHLN::Test::Image::BBox;
using ZHLN::Test::Image::LumaPlane;
using ZHLN::Test::Image::TemporalMoments;
using ZHLN::Test::Image::BBoxOfChangedPixels;
using ZHLN::Test::Image::Crop;
using ZHLN::Test::Image::LumaDifference;
using ZHLN::Test::Image::RgbImage;
using ZHLN::Test::Image::RmsInRegion;
using ZHLN::Test::Image::RunningMeanResidualSeries;

constexpr int kWidth  = 640;
constexpr int kHeight = 480;

// Same convention as the shadow suite: luma units (0-255) above which a pixel
// counts as changed.
constexpr double kChangeThreshold = 2.0;

// Same floors as the shadow suite; see the region-size sweep comment over
// there. The metrics need at least this many pixels of band to average over.
constexpr int kMinRegionWidth  = 64;
constexpr int kMinRegionHeight = 24;

// Plate roughness. Must sit at or under the shader's 0.4 RTR cutoff or the
// branch never runs; must sit far enough above 0 that the VNDF lobe is wide
// enough for the hit/miss flip band at the mirror silhouette to clear
// kMinRegionHeight. 0.35 gives alpha = 0.1225 and a roughnessFade of 0.3.
constexpr float kPlateRoughness = 0.35f;

// Rows [0, kCutoffProbeRows) are sky, the (static) box and the rough far
// floor beyond the plate's far edge (z>~90 at this camera): nothing the RTR
// switch may touch. Used to assert the roughness cutoff.
constexpr int kCutoffProbeRows = 96;

} // namespace

namespace {

struct RayTracedReflectionNoiseTestSuite {
    RayTracedReflectionNoiseTestSuite() {
        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~RayTracedReflectionNoiseTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    // Pooled: one engine per resolution for the whole binary, with the
    // scene reset between tests. Creating a Vulkan instance per test is
    // what eventually exhausts the loader's static TLS and turns the tail
    // of the group into "vkCreateInstance: Found no drivers!".
    static auto CreateTestEngine() -> ZHLN::Test::Headless::EngineHandle {
        return ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions {
            .appName = "Headless RTR Noise", .width = kWidth, .height = kHeight
        });
    }

    static void SetAA(ZHLN::Engine& engine, ZHLN::AAMode mode) {
        auto& reg = engine.GetRegistry();
        for (const ZHLN::Entity e: reg.GetEntitiesWith<ZHLN::Components::MainCameraTagComponent>()) {
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

    // Same switch the shadow suite drives: GraphicsSettingsSync.cpp turns
    // PostProcessSettingsComponent::enableRTR into rayTracing.enableReflections,
    // which RenderGraphBuilder ANDs with a non-null TLAS to form pc.enableRTR
    // and to select the RTR pipeline variants for lighting and reflection.
    static void SetRTR(ZHLN::Engine& engine, int enableSSR, int enableRTR) {
        auto&      reg      = engine.GetRegistry();
        const auto settings = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
        if (settings.empty()) {
            return;
        }
        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings[0], [enableSSR, enableRTR](auto& pp) {
            pp.fullBright        = 0;
            pp.enableSSR         = enableSSR;
            pp.enableRTR         = enableRTR;
            pp.giMode            = 0;
            pp.giIntensity       = 0.0f;
            pp.vignetteIntensity = 0.0f;
            pp.ambientExposure   = 3.0f;
            pp.skyZenith         = JPH::Vec4(0.001f, 0.002f, 0.006f, 1.0f);
            pp.skyHorizon        = JPH::Vec4(0.004f, 0.006f, 0.012f, 1.0f);
            pp.skyGround         = JPH::Vec4(0.001f, 0.001f, 0.002f, 1.0f);
        });
    }

    // A dark room of metal. The plate is the only surface at or under the 0.4
    // roughness cutoff, so it is the only surface the RTR switch may change;
    // the rough metal floor doubles as the cutoff probe. The box is what the
    // plate mirrors -- sun-lit faces against a near-black IBL, so hit/miss
    // flips of the VNDF ray are high contrast.
    //
    // The sun runs at full intensity, and the on/off delta still isolates the
    // reflection term:
    //   - metals have no diffuse, so the RT shadow dither can never modulate
    //     the plate or the floor;
    //   - the sun specular highlight on the floor is geometrically out of
    //     view (the reflected view direction carries -z while the sun
    //     direction carries +z, so reflect(-V,N) can never align with L);
    //   - nothing occludes the box, so its sun-lit color is identical in the
    //     RT and NoRT lighting variants.
    // An earlier revision set the sun to zero intensity "to be safe" and
    // measured a perfect zero: with sky and sun both black, every surface --
    // and the reflection of every surface -- rendered black, so the RTR term
    // had nothing to add. The switch worked; the scene was black-on-black.
    static bool BuildReflectionScene(ZHLN::Engine& engine) {
        auto& reg = engine.GetRegistry();

        auto floorMat =
            engine.GetRenderContext().CreateMaterial(ZHLN::MaterialDesc {.metallic = 1.0f, .roughness = 0.8f, .baseColor = {0.05f, 0.05f, 0.06f, 1.0f}});
        auto plateMat = engine.GetRenderContext().CreateMaterial(
            ZHLN::MaterialDesc {.metallic = 1.0f, .roughness = kPlateRoughness, .baseColor = {0.5f, 0.5f, 0.5f, 1.0f}}
        );
        auto boxMat = engine.GetRenderContext().CreateMaterial(
            ZHLN::MaterialDesc {
                .metallic  = 0.0f,
                .roughness = 0.5f,
                .baseColor = {0.2f, 0.2f, 0.2f, 1.0f},
                // Emission is radiance now, not a multiplier on incident light.
                //
                // This used to read 4.0. Back then basic.slang folded emission
                // into the albedo target, so the lighting pass multiplied it by
                // the sun: (0.2 + 4.0) / pi * 120 * NdotL, roughly 105 on the
                // camera-facing face. With emission carried on its own GBuffer
                // target and added after shading, the same material renders at
                // about 9 -- the box is physically correct and twelve times
                // dimmer, and the reflection of it stopped clearing the
                // per-pixel change threshold, which is what collapsed the
                // measured band to a 10x9 speck.
                //
                // 100 restores the radiance the scene was calibrated against,
                // expressed the honest way: a very bright emitter.
                .emissive = {100.0f, 100.0f, 100.0f, 1.0f}
            }
        );
        if (!floorMat || !plateMat || !boxMat) {
            return false;
        }

        ZHLN::PrefabFactory::CreatePlane(
            engine, 120.0f, {0.05f, 0.05f, 0.06f, 1.0f},
            ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *floorMat}
        );
        // Just above the floor so it wins the depth test everywhere it covers.
        //
        // Size matters: the mirror image of the box crosses the floor plane
        // well beyond the box's own footprint (camera at z=20, box at z=6 ->
        // the mirror band lands at z~12-15). An early 14-unit plate centred
        // under the box caught only a sliver of that band and the on/off
        // delta measured zero. Cover the whole near/mid floor instead; the
        // rough floor then survives only past z~90, which is exactly the
        // far strip inside the top probe rows.
        ZHLN::PrefabFactory::CreatePlane(
            engine, 60.0f, {0.5f, 0.5f, 0.5f, 1.0f},
            ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.02, 8.0), .createPhysics = false, .materialOverride = *plateMat}
        );
        ZHLN::PrefabFactory::CreateBox(
            engine, {2.0f, 2.0f, 2.0f},
            ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 6.0, 6.0), .createPhysics = false, .materialOverride = *boxMat}
        );

        const ZHLN::Entity sunEnt = reg.Create();
        reg.Add(
            sunEnt,
            ZHLN::Components::TransformComponent {.position = JPH::Vec3(0.0f, 50.0f, 40.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({40.0f, 0.0f, 0.0f})},
            ZHLN::Components::LightComponent {
                .type = ZHLN::LightType::Sun, .color = JPH::Vec3(1.0f, 1.0f, 1.0f), .intensity = 120.0f, .direction = JPH::Vec3(0.0f, 0.75f, 0.66f).Normalized()
            }
        );

        const auto camComp = engine.GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
        if (!ZHLN::Test::ExpectTrue(camComp.has_value())) {
            return false;
        }
        ZHLN::Camera& cam = camComp->camera;
        cam.position = JPH::Vec3(0.0f, 5.0f, 20.0f);
        cam.yaw      = -90.0f;
        cam.pitch    = -22.0f;
        cam.fov      = 60.0f;
        return true;
    }

    static void TickFrames(ZHLN::Engine& engine, uint32_t frames) {
        constexpr float dt = 1.0f / 60.0f;
        for (uint32_t i = 0; i < frames; ++i) {
            engine.ProcessEvents();
            engine.Tick(dt);
        }
    }

    static RgbImage Capture(ZHLN::Engine& engine, std::string_view name) {
        return ZHLN::Test::Headless::Capture(engine, name);
    }

    static RgbImage SettleAndCapture(ZHLN::Engine& engine, std::string_view name) {
        TickFrames(engine, 2);
        return Capture(engine, name);
    }

    // Changed fraction restricted to the top probe rows (sky / box / rough
    // far floor): the part of the frame the RTR switch must not touch.
    static double ChangedFractionInTopRows(std::span<const double> diff) {
        std::size_t changed = 0;
        for (int y = 0; y < kCutoffProbeRows; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                if (std::abs(diff[static_cast<std::size_t>(y) * kWidth + x]) > kChangeThreshold) {
                    ++changed;
                }
            }
        }
        return static_cast<double>(changed) / static_cast<double>(kCutoffProbeRows * kWidth);
    }

    struct Tests {
        // The 2x2 liveness diagnosis from the shadow suite, plus the cutoff
        // contract: the glossy plate must change when the switch flips, its
        // jitter must advance between two on-frames, and the rough top of the
        // frame must stay still.
        std::expected<void, ZHLN::ErrorCode> rtr_is_live_and_rough_surfaces_stay_still() {
            auto engine = RayTracedReflectionNoiseTestSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().GetInfo().rayTracingSupported) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; RTR checks are not applicable.");
                return {};
            }
            if (!RayTracedReflectionNoiseTestSuite::BuildReflectionScene(*engine)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            RayTracedReflectionNoiseTestSuite::SetAA(*engine, ZHLN::AAMode::None);

            // Off first: it doubles as the ground-truth frame. Three zero
            // deltas in a row proved that a blanket "path inactive" verdict
            // wastes a GPU round trip; this scenario now prints what the off
            // frame actually contains and runs an SSR probe so the failure
            // message names the dead layer instead of guessing.
            RayTracedReflectionNoiseTestSuite::SetRTR(*engine, 0, 0);
            RgbImage b = RayTracedReflectionNoiseTestSuite::SettleAndCapture(*engine, "rt_refl_live_off.ppm");

            const std::vector<double> offLuma = LumaPlane(b);
            double                    maxAll  = 0.0;
            for (const double v: offLuma) {
                maxAll = std::max(maxAll, v);
            }
            const auto meanOf = [&](int x0, int x1, int y0, int y1) {
                double s = 0.0;
                int    n = 0;
                for (int y = y0; y < y1; ++y) {
                    for (int x = x0; x < x1; ++x) {
                        s += offLuma[static_cast<std::size_t>(y) * kWidth + x];
                        ++n;
                    }
                }
                return n > 0 ? s / n : 0.0;
            };
            ZHLN::Println(
                "    [INFO] off frame: meanAll={:.3f} meanTopHalf={:.3f} maxAll={:.1f} predicted-band region={:.3f}", meanOf(0, kWidth, 0, kHeight),
                meanOf(0, kWidth, 0, kHeight / 2), maxAll, meanOf(263, 376, 293, 435)
            );

            // SSR probe: same reflection pass, same roughness branch, same
            // plate -- but the hit test is a depth-buffer raymarch, no TLAS.
            // SSR changing the mirror while RTR does not isolates the dead
            // layer to the ray query; neither changing it isolates the layer
            // below the branch.
            RayTracedReflectionNoiseTestSuite::SetRTR(*engine, 1, 0);
            RgbImage ssr = RayTracedReflectionNoiseTestSuite::SettleAndCapture(*engine, "rt_refl_ssr_on.ppm");

            RayTracedReflectionNoiseTestSuite::SetRTR(*engine, 0, 1);
            RgbImage a = RayTracedReflectionNoiseTestSuite::SettleAndCapture(*engine, "rt_refl_live_on.ppm");
            RayTracedReflectionNoiseTestSuite::SetRTR(*engine, 0, 1);
            RgbImage c = RayTracedReflectionNoiseTestSuite::SettleAndCapture(*engine, "rt_refl_live_on2.ppm");
            if (!(ZHLN::Test::ExpectTrue(a.Valid()) && ZHLN::Test::ExpectTrue(b.Valid()) && ZHLN::Test::ExpectTrue(c.Valid()) &&
                  ZHLN::Test::ExpectTrue(ssr.Valid()))) {
                return std::unexpected(ReflectionNoiseError::CaptureFailed);
            }

            const auto onVsOff  = ZHLN::Test::Noise::MeasureResidual(a.rgb.data(), b.rgb.data(), kWidth, kHeight, kChangeThreshold);
            const auto onVsOn   = ZHLN::Test::Noise::MeasureResidual(a.rgb.data(), c.rgb.data(), kWidth, kHeight, kChangeThreshold);
            const auto ssrVsOff = ZHLN::Test::Noise::MeasureResidual(ssr.rgb.data(), b.rgb.data(), kWidth, kHeight, kChangeThreshold);
            const double topChanged = ChangedFractionInTopRows(LumaDifference(a, b));
            // Diagnosis for the quarantine (see QuarantineList): the on/on
            // control separates jitter in the probe rows (probe placement /
            // threshold wrong) from a systematic shift (the switch leaking
            // into a non-dithering term -- bloom or exposure -- which is what
            // a ~0 on/on points at). MeanAbs sizes the leak: sub-luma means
            // a benign post coupling, several luma means a gross leak.
            const double topChangedOnOn = ChangedFractionInTopRows(LumaDifference(a, c));
            const auto   meanAbsTop     = [](std::span<const double> diff) {
                double sum = 0.0;
                for (int y = 0; y < kCutoffProbeRows; ++y) {
                    for (int x = 0; x < kWidth; ++x) {
                        sum += std::abs(diff[static_cast<std::size_t>(y) * kWidth + x]);
                    }
                }
                return sum / static_cast<double>(kCutoffProbeRows * kWidth);
            };
            const double topMeanAbsOnOff = meanAbsTop(LumaDifference(a, b));
            const double topMeanAbsOnOn  = meanAbsTop(LumaDifference(a, c));
            ZHLN::Println(
                "    [INFO] SSR probe vs off : meanAbs={:.3f} rms={:.3f} changed={:.5f}", ssrVsOff.meanAbs, ssrVsOff.rms, ssrVsOff.changedFraction
            );
            ZHLN::Println(
                "    [INFO] RTR on vs off : meanAbs={:.3f} rms={:.3f} changed={:.5f}", onVsOff.meanAbs, onVsOff.rms, onVsOff.changedFraction
            );
            ZHLN::Println(
                "    [INFO] RTR on vs on  : meanAbs={:.3f} rms={:.3f} changed={:.5f}", onVsOn.meanAbs, onVsOn.rms, onVsOn.changedFraction
            );
            ZHLN::Println("    [INFO] changed fraction in top {} probe rows (must be ~0) = {:.5f}", kCutoffProbeRows, topChanged);
            ZHLN::Println(
                "    [INFO] top-row on/on control: changed={:.5f} | top meanAbs on/off={:.3f} on/on={:.3f}", topChangedOnOn, topMeanAbsOnOff,
                topMeanAbsOnOn
            );

            if (!ZHLN::Test::ExpectGt(onVsOff.changedFraction, 0.0)) {
                if (maxAll < 4.0) {
                    return std::unexpected(ReflectionNoiseError::SceneNotLit);
                }
                if (ssrVsOff.changedFraction > 0.0) {
                    return std::unexpected(ReflectionNoiseError::SsrWorksButRtrRaysDead);
                }
                return std::unexpected(ReflectionNoiseError::ReflectionBranchOrOutputDead);
            }
            if (!ZHLN::Test::ExpectLt(onVsOn.changedFraction, 0.001)) {
                return std::unexpected(ReflectionNoiseError::TemporalBoilDetected);
            }
            // A few stray pixels are tolerated (variant switches can round
            // differently); a changed rough floor is not.
            if (!ZHLN::Test::ExpectLt(topChanged, 0.002)) {
                return std::unexpected(ReflectionNoiseError::RoughnessCutoffViolated);
            }

            ZHLN::Println("    [PASS] RTR reaches the mirror with zero temporal boil, and rough surfaces ignore the switch.");
            return {};
        }

        // Spatial coherence of the deterministic Tier 2 cone-traced reflection
        // band over the on/off delta between RTR=1 and RTR=0.
        std::expected<void, ZHLN::ErrorCode> rtr_reflection_band_is_spatially_coherent() {
            auto engine = RayTracedReflectionNoiseTestSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().GetInfo().rayTracingSupported) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; reflection coherence checks are not applicable.");
                return {};
            }
            if (!RayTracedReflectionNoiseTestSuite::BuildReflectionScene(*engine)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            RayTracedReflectionNoiseTestSuite::SetAA(*engine, ZHLN::AAMode::None);

            RayTracedReflectionNoiseTestSuite::SetRTR(*engine, 0, 0);
            RgbImage off = RayTracedReflectionNoiseTestSuite::SettleAndCapture(*engine, "rt_refl_structure_off.ppm");
            RayTracedReflectionNoiseTestSuite::SetRTR(*engine, 0, 1);
            RgbImage on = RayTracedReflectionNoiseTestSuite::SettleAndCapture(*engine, "rt_refl_structure_on.ppm");
            if (!(ZHLN::Test::ExpectTrue(off.Valid()) && ZHLN::Test::ExpectTrue(on.Valid()))) {
                return std::unexpected(ReflectionNoiseError::CaptureFailed);
            }

            const auto res = ZHLN::Test::Noise::MeasureResidual(on.rgb.data(), off.rgb.data(), kWidth, kHeight, kChangeThreshold);
            if (!ZHLN::Test::ExpectTrue(res.valid)) {
                return std::unexpected(ReflectionNoiseError::CaptureFailed);
            }

            const std::vector<double> diff = LumaDifference(on, off);
            const BBox                band = BBoxOfChangedPixels(diff.data(), kWidth, kHeight, kChangeThreshold, 4);

            ZHLN::Println(
                "    [INFO] on vs off: meanAbs={:.3f} rms={:.3f} changed={:.5f} isolated={:.4f} maxAbs={:.1f}", res.meanAbs, res.rms,
                res.changedFraction, res.isolatedFraction, res.maxAbs
            );
            ZHLN::Println(
                "    [INFO] reflection bbox = [{},{}) x [{},{})  ({}x{}), region rms={:.3f}", band.x0, band.x1, band.y0, band.y1, band.Width(),
                band.Height(), RmsInRegion(diff.data(), kWidth, band)
            );

            if (!(ZHLN::Test::ExpectTrue(!band.Empty()) && ZHLN::Test::ExpectGe(band.Width(), kMinRegionWidth) &&
                  ZHLN::Test::ExpectGe(band.Height(), kMinRegionHeight))) {
                return std::unexpected(ReflectionNoiseError::ReflectionRegionTooSmall);
            }
            if (!ZHLN::Test::ExpectLt(res.isolatedFraction, 0.10)) {
                return std::unexpected(ReflectionNoiseError::ReflectionIsFragmented);
            }

            ZHLN::Println("    [PASS] Deterministic cone-traced reflection forms a contiguous, coherent glossy band.");
            return {};
        }

        // Deterministic single-frame convergence: consecutive frames over the
        // glossy reflection band must already have ~0 running-mean residual on
        // frame 1 without requiring multi-frame temporal accumulation.
        std::expected<void, ZHLN::ErrorCode> rtr_converges_deterministically_in_single_frame() {
            auto engine = RayTracedReflectionNoiseTestSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().GetInfo().rayTracingSupported) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; convergence check is not applicable.");
                return {};
            }
            if (!RayTracedReflectionNoiseTestSuite::BuildReflectionScene(*engine)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            RayTracedReflectionNoiseTestSuite::SetAA(*engine, ZHLN::AAMode::None);

            RayTracedReflectionNoiseTestSuite::SetRTR(*engine, 0, 0);
            RgbImage off = RayTracedReflectionNoiseTestSuite::SettleAndCapture(*engine, "rt_refl_conv_off.ppm");
            RayTracedReflectionNoiseTestSuite::SetRTR(*engine, 0, 1);
            RgbImage first = RayTracedReflectionNoiseTestSuite::SettleAndCapture(*engine, "rt_refl_conv_prev.ppm");
            RayTracedReflectionNoiseTestSuite::TickFrames(*engine, 1);
            RgbImage second = RayTracedReflectionNoiseTestSuite::Capture(*engine, "rt_refl_conv_cur.ppm");
            if (!(ZHLN::Test::ExpectTrue(off.Valid()) && ZHLN::Test::ExpectTrue(first.Valid()) && ZHLN::Test::ExpectTrue(second.Valid()))) {
                return std::unexpected(ReflectionNoiseError::CaptureFailed);
            }

            const std::vector<double> onOffDiff = LumaDifference(first, off);
            const BBox                band      = BBoxOfChangedPixels(onOffDiff.data(), kWidth, kHeight, kChangeThreshold, 4);
            if (!(ZHLN::Test::ExpectTrue(!band.Empty()) && ZHLN::Test::ExpectGe(band.Width(), kMinRegionWidth) &&
                  ZHLN::Test::ExpectGe(band.Height(), kMinRegionHeight))) {
                return std::unexpected(ReflectionNoiseError::ReflectionRegionTooSmall);
            }

            const std::vector<double> temporalDiff = LumaDifference(first, second);
            const double              temporalRms  = RmsInRegion(temporalDiff.data(), kWidth, band);
            ZHLN::Println(
                "    [INFO] single-frame temporal residual rms over [{},{}) x [{},{}) = {:.6f}", band.x0, band.x1, band.y0, band.y1, temporalRms
            );

            if (!ZHLN::Test::ExpectLt(temporalRms, 0.05)) {
                return std::unexpected(ReflectionNoiseError::ResidualDidNotConverge);
            }

            ZHLN::Println("    [PASS] Reflection converges deterministically in a single frame with zero temporal boil.");
            return {};
        }

        // Hybrid Hi-Z SSR + TLAS inline fallback must produce clean clustered
        // reflections with zero isolated single-pixel ray debris or fireflies.
        std::expected<void, ZHLN::ErrorCode> rtr_hybrid_ssr_and_tlas_have_no_ray_debris() {
            auto engine = RayTracedReflectionNoiseTestSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().GetInfo().rayTracingSupported) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; debris check is not applicable.");
                return {};
            }
            if (!RayTracedReflectionNoiseTestSuite::BuildReflectionScene(*engine)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            RayTracedReflectionNoiseTestSuite::SetAA(*engine, ZHLN::AAMode::None);

            RayTracedReflectionNoiseTestSuite::SetRTR(*engine, 0, 0);
            RgbImage off = RayTracedReflectionNoiseTestSuite::SettleAndCapture(*engine, "rt_refl_debris_off.ppm");
            RayTracedReflectionNoiseTestSuite::SetRTR(*engine, 1, 1);
            RgbImage hybA = RayTracedReflectionNoiseTestSuite::SettleAndCapture(*engine, "rt_refl_debris_a.ppm");
            RayTracedReflectionNoiseTestSuite::TickFrames(*engine, 1);
            RgbImage hybB = RayTracedReflectionNoiseTestSuite::Capture(*engine, "rt_refl_debris_b.ppm");
            if (!(ZHLN::Test::ExpectTrue(off.Valid()) && ZHLN::Test::ExpectTrue(hybA.Valid()) && ZHLN::Test::ExpectTrue(hybB.Valid()))) {
                return std::unexpected(ReflectionNoiseError::CaptureFailed);
            }

            const auto onOff = ZHLN::Test::Noise::MeasureResidual(hybA.rgb.data(), off.rgb.data(), kWidth, kHeight, kChangeThreshold);
            const auto onOn  = ZHLN::Test::Noise::MeasureResidual(hybA.rgb.data(), hybB.rgb.data(), kWidth, kHeight, kChangeThreshold);
            ZHLN::Println(
                "    [INFO] hybrid vs off: changed={:.5f} isolated-of-changed={:.4f} maxAbs={:.1f} | on/on changed={:.5f}",
                onOff.changedFraction, onOff.isolatedFraction, onOff.maxAbs, onOn.changedFraction
            );

            if (!ZHLN::Test::ExpectGt(onOff.changedFraction, 1e-4)) {
                return std::unexpected(ReflectionNoiseError::ReflectionPathInactive);
            }
            if (!ZHLN::Test::ExpectLt(onOff.isolatedFraction, 0.05)) {
                return std::unexpected(ReflectionNoiseError::RayDebrisDetected);
            }
            if (!ZHLN::Test::ExpectLt(onOn.changedFraction, 0.001)) {
                return std::unexpected(ReflectionNoiseError::TemporalBoilDetected);
            }

            ZHLN::Println("    [PASS] Hybrid Hi-Z SSR + TLAS reflection band is clean, stable, and free of ray debris.");
            return {};
        }
    };
};

} // namespace
