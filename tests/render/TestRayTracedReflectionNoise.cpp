// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestRayTracedReflectionNoise.cpp
//
// GPU-side verification that the ray-traced reflection (RTR) path behaves the
// way its blue-noise GGX VNDF sampling promises, mirroring the shadow suite
// (TestRayTracedNoiseStability.cpp) for the other ray query in the frame.
//
// reflection.slang:RaytraceRTR draws a microfacet normal from the GGX visible
// normal distribution, seeded by the .zw pair of the same blue noise tile the
// shadow path reads (.xy), and traces one ray per pixel. Where that ray lands
// -- the object, the face, or a miss into the sky -- changes frame to frame,
// so a rough mirror shows a stochastic residual with exactly the same
// structural obligations as the shadow dither:
//
//   1. LIVENESS    - RTR must reach the image, its jitter must advance with
//                    the frame index, and surfaces rougher than the 0.4
//                    cutoff must be untouched by the switch (the cutoff is a
//                    correctness contract, not a fade).
//   2. PERIODICITY - the residual must not correlate with itself at short
//                    lag; a lattice dither would smear under the roughness
//                   -aware filter downstream.
//   3. ISOTROPY    - no preferred direction in the residual.
//   4. CONVERGENCE - under temporal accumulation the reflection residual must
//                    actually fall.
//   5. NO DEBRIS   - changed pixels cluster; isolated ones are ray debris.
//
// There is deliberately NO Bernoulli magnitude scenario here, unlike the
// shadow suite. A 1 SPP shadow is a two-valued draw (sun or no sun) so its
// temporal variance must equal p(1-p)d^2; a VNDF reflection ray is not
// two-valued -- the reflected radiance is continuous in the draw direction --
// so no closed-form variance exists to compare against. The roughness-cutoff
// assertion in scenario 1 is the reflection-specific quantity check instead.
//
// Scene: a dark room of metal under a full-strength sun. Metals have no
// diffuse, the floor's sun highlight is geometrically out of view, and
// nothing occludes the box, so the ONLY thing the enableRTR switch can
// change is the reflection term. A sun-lit box hangs over a glossy metal
// plate; its mirror image is the measurement region. The surrounding floor
// is rough metal (0.8, past the cutoff) and must stay byte-still when the
// switch flips.

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
#include <memory>
#include <numeric>
#include <string>
#include <vector>

enum class ReflectionNoiseError : uint8_t {
    EngineInitFailed[[= ZHLN::Reflect::Description("Failed to initialize the headless Engine for the RTR noise test.")]] = 1,
    CaptureFailed[[= ZHLN::Reflect::Description("A frame could not be captured or the PPM could not be read back.")]],
    SceneNotLit[[= ZHLN::Reflect::Description(
        "The off frame is black: the box is not lit or not in view, so there is nothing the reflection could show. Check the sun intensity/direction, the camera framing and the region luma stats printed by scenario 1 before suspecting the reflection path."
    )]],
    SsrWorksButRtrRaysDead[[= ZHLN::Reflect::Description(
        "The SSR probe (same reflection pass, same roughness branch, same plate, depth-buffer hit test) changes the mirror but the RTR switch changes nothing: the reflection pass, branch and output are alive, so the dead layer is the TLAS ray query of RaytraceRTR -- check the tlas address bound to the reflection pass heap and the BLAS/instance flags."
    )]],
    ReflectionBranchOrOutputDead[[= ZHLN::Reflect::Description(
        "The scene is lit but neither the SSR probe nor the RTR switch changes a pixel: the dead layer is below the hit test -- the roughness<=0.4 branch, the reflVariant selection, the push constants, or the Res_HdrSceneColor output not reaching the capture."
    )]],
    ReflectionPathInactive[[= ZHLN::Reflect::Description(
        "Enabling enableRTR changed no pixels on the glossy plate against a fully static scene, so RaytraceRTR is not executing: check pc.enableRTR_dynamic, the TLAS, the reflVariant pipeline selection, or that the plate roughness is at or under the 0.4 cutoff."
    )]],
    JitterTemporallyFrozen[[= ZHLN::Reflect::Description(
        "The RTR path executes but renders the identical image at two different frame indices, so the blue noise temporal scroll is not reaching SampleGGX_VNDF: check FrameIndexFromCamPosW(frame.camPos.w) and the .zw channel pair in blue_noise.slang."
    )]],
    RoughnessCutoffViolated[[= ZHLN::Reflect::Description(
        "Pixels outside the glossy plate changed when enableRTR flipped; surfaces rougher than the 0.4 cutoff must be untouched by the reflection switch (reflection.slang gates the RTR branch on roughness <= 0.4). Either the cutoff moved or the switch is leaking into another term."
    )]],
    ReflectionRegionTooSmall[[= ZHLN::Reflect::Description(
        "The per-frame reflection variation covers too small a region for the structural metrics; widen the VNDF lobe (raise the plate roughness toward the 0.4 cutoff) or enlarge the reflected object."
    )]],
    DitherIsPeriodic[[= ZHLN::Reflect::Description("The RTR residual repeats at a short spatial lag; the VNDF draw carries a lattice a spatial filter cannot integrate away.")]],
    DitherIsAnisotropic[[= ZHLN::Reflect::Description("The RTR residual has a preferred direction (structured lattice, not blue noise).")]],
    ResidualDidNotConverge[[= ZHLN::Reflect::Description("Under temporal accumulation the reflection residual oscillated instead of falling.")]],
    RayDebrisDetected[[= ZHLN::Reflect::Description("The reflection residual is dominated by isolated single-pixel outliers (ray debris / fireflies).")]],
    DenoiserDidNotReduceNoise[[= ZHLN::Reflect::Description(
        "The A-Trous HDR wavelet did not cut the on/on residual over the reflection band by the required margin: the pass may be skipped (denoiserPasses/rtCtx/RT-enable gate), its edge-stops may be rejecting every tap, or the filtered result may not reach Res_HdrSceneColor before the capture."
    )]],
};

namespace {

using ZHLN::Test::Frame::BBox;
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

/// Same convention as the shadow suite: luma units (0-255) above which a pixel
/// counts as changed.
constexpr double kChangeThreshold = 2.0;

/// Same floors as the shadow suite; see the region-size sweep comment over
/// there. The metrics need at least this many pixels of band to average over.
constexpr int kMinRegionWidth  = 64;
constexpr int kMinRegionHeight = 24;

/// Plate roughness. Must sit at or under the shader's 0.4 RTR cutoff or the
/// branch never runs; must sit far enough above 0 that the VNDF lobe is wide
/// enough for the hit/miss flip band at the mirror silhouette to clear
/// kMinRegionHeight. 0.35 gives alpha = 0.1225 and a roughnessFade of 0.3.
constexpr float kPlateRoughness = 0.35f;

/// Rows [0, kCutoffProbeRows) are sky, the (static) box and the rough far
/// floor beyond the plate's far edge (z>~90 at this camera): nothing the RTR
/// switch may touch. Used to assert the roughness cutoff.
constexpr int kCutoffProbeRows = 96;

} // namespace

struct RayTracedReflectionNoiseTestSuite {
    RayTracedReflectionNoiseTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~RayTracedReflectionNoiseTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    static auto CreateTestEngine() -> std::unique_ptr<ZHLN::Engine> {
        ZHLN::DefaultPreset::SetDisabled(true);
        const ZHLN::EngineConfig cfg {
            .physics = {.maxBodies = 256, .maxBodyPairs = 512, .maxContactConstraints = 512, .tempAllocatorSize = 8 * 1024 * 1024},
            .render  = {
                 .appName        = "Headless RTR Noise",
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

    /// Same switch the shadow suite drives: GraphicsSettingsSync.cpp turns
    /// PostProcessSettingsComponent::enableRTR into rayTracing.enableReflections,
    /// which RenderGraphBuilder ANDs with a non-null TLAS to form pc.enableRTR
    /// and to select the RTR pipeline variants for lighting and reflection.
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

    /// A dark room of metal. The plate is the only surface at or under the 0.4
    /// roughness cutoff, so it is the only surface the RTR switch may change;
    /// the rough metal floor doubles as the cutoff probe. The box is what the
    /// plate mirrors -- sun-lit faces against a near-black IBL, so hit/miss
    /// flips of the VNDF ray are high contrast.
    ///
    /// The sun runs at full intensity, and the on/off delta still isolates the
    /// reflection term:
    ///   - metals have no diffuse, so the RT shadow dither can never modulate
    ///     the plate or the floor;
    ///   - the sun specular highlight on the floor is geometrically out of
    ///     view (the reflected view direction carries -z while the sun
    ///     direction carries +z, so reflect(-V,N) can never align with L);
    ///   - nothing occludes the box, so its sun-lit color is identical in the
    ///     RT and NoRT lighting variants.
    /// An earlier revision set the sun to zero intensity "to be safe" and
    /// measured a perfect zero: with sky and sun both black, every surface --
    /// and the reflection of every surface -- rendered black, so the RTR term
    /// had nothing to add. The switch worked; the scene was black-on-black.
    static bool BuildReflectionScene(ZHLN::Engine& engine) {
        auto& reg = engine.GetRegistry();

        auto floorMat = ZHLN::CreativeWorksFactory::CreateMaterial(
            engine.GetRenderContext(),
            ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.8f, .baseColor = {0.05f, 0.05f, 0.06f, 1.0f}}
        );
        auto plateMat = ZHLN::CreativeWorksFactory::CreateMaterial(
            engine.GetRenderContext(),
            ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = kPlateRoughness, .baseColor = {0.5f, 0.5f, 0.5f, 1.0f}}
        );
        auto boxMat = ZHLN::CreativeWorksFactory::CreateMaterial(
            engine.GetRenderContext(),
            ZHLN::CreativeWorksFactory::MaterialDesc {
                .metallic  = 0.0f,
                .roughness = 0.5f,
                .baseColor = {0.2f, 0.2f, 0.2f, 1.0f},
                .emissive  = {4.0f, 4.0f, 4.0f, 1.0f}
            }
        );
        if (!floorMat || !plateMat || !boxMat) {
            return false;
        }

        ZHLN::CreativeWorksFactory::CreatePlane(
            engine, 120.0f, {0.05f, 0.05f, 0.06f, 1.0f},
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *floorMat}
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
        ZHLN::CreativeWorksFactory::CreatePlane(
            engine, 60.0f, {0.5f, 0.5f, 0.5f, 1.0f},
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.02, 8.0), .createPhysics = false, .materialOverride = *plateMat}
        );
        ZHLN::CreativeWorksFactory::CreateBox(
            engine, JPH::Vec3(2.0f, 2.0f, 2.0f),
            ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 6.0, 6.0), .createPhysics = false, .materialOverride = *boxMat}
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

    static RgbImage SettleAndCapture(ZHLN::Engine& engine, const std::string& name) {
        TickFrames(engine, 2);
        return Capture(engine, name);
    }

    /// Changed fraction restricted to the top probe rows (sky / box / rough
    /// far floor): the part of the frame the RTR switch must not touch.
    static double ChangedFractionInTopRows(const std::vector<double>& diff) {
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
        /// The 2x2 liveness diagnosis from the shadow suite, plus the cutoff
        /// contract: the glossy plate must change when the switch flips, its
        /// jitter must advance between two on-frames, and the rough top of the
        /// frame must stay still.
        std::expected<void, ZHLN::Error> rtr_is_live_and_rough_surfaces_stay_still() {
            auto engine = RayTracedReflectionNoiseTestSuite::CreateTestEngine();
            if (!ZHLN::Test::AssertTrue(engine != nullptr)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; RTR checks are not applicable.");
                return {};
            }
            if (!RayTracedReflectionNoiseTestSuite::BuildReflectionScene(*engine)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            RayTracedReflectionNoiseTestSuite::SetAA(*engine, ZHLN::AAMode::None);
            RayTracedReflectionNoiseTestSuite::SetDenoiser(*engine, 0);

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
            if (!ZHLN::Test::ExpectTrue(a.Valid() && b.Valid() && c.Valid() && ssr.Valid())) {
                return std::unexpected(ReflectionNoiseError::CaptureFailed);
            }

            const auto onVsOff  = ZHLN::Test::Noise::MeasureResidual(a.rgb.data(), b.rgb.data(), kWidth, kHeight, kChangeThreshold);
            const auto onVsOn   = ZHLN::Test::Noise::MeasureResidual(a.rgb.data(), c.rgb.data(), kWidth, kHeight, kChangeThreshold);
            const auto ssrVsOff = ZHLN::Test::Noise::MeasureResidual(ssr.rgb.data(), b.rgb.data(), kWidth, kHeight, kChangeThreshold);
            const double topChanged = ChangedFractionInTopRows(LumaDifference(a, b));
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

            if (!ZHLN::Test::ExpectTrue(onVsOff.changedFraction > 0.0)) {
                if (maxAll < 4.0) {
                    return std::unexpected(ReflectionNoiseError::SceneNotLit);
                }
                if (ssrVsOff.changedFraction > 0.0) {
                    return std::unexpected(ReflectionNoiseError::SsrWorksButRtrRaysDead);
                }
                return std::unexpected(ReflectionNoiseError::ReflectionBranchOrOutputDead);
            }
            if (!ZHLN::Test::ExpectTrue(onVsOn.changedFraction > 0.0)) {
                return std::unexpected(ReflectionNoiseError::JitterTemporallyFrozen);
            }
            // A few stray pixels are tolerated (variant switches can round
            // differently); a changed rough floor is not.
            if (!ZHLN::Test::ExpectTrue(topChanged < 0.002)) {
                return std::unexpected(ReflectionNoiseError::RoughnessCutoffViolated);
            }

            ZHLN::Println("    [PASS] RTR reaches the mirror, its jitter moves, and rough surfaces ignore the switch.");
            return {};
        }

        /// Blue-noise structure of the reflection residual, over the bounding
        /// box of the pixels that actually vary between two on-frames.
        std::expected<void, ZHLN::Error> rtr_residual_is_aperiodic_and_isotropic() {
            auto engine = RayTracedReflectionNoiseTestSuite::CreateTestEngine();
            if (!ZHLN::Test::AssertTrue(engine != nullptr)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; dither structure checks are not applicable.");
                return {};
            }
            if (!RayTracedReflectionNoiseTestSuite::BuildReflectionScene(*engine)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            RayTracedReflectionNoiseTestSuite::SetRTR(*engine, 0, 1);
            RayTracedReflectionNoiseTestSuite::SetAA(*engine, ZHLN::AAMode::None);
            RayTracedReflectionNoiseTestSuite::SetDenoiser(*engine, 0);

            RgbImage prev = RayTracedReflectionNoiseTestSuite::SettleAndCapture(*engine, "rt_refl_structure_a.ppm");
            RayTracedReflectionNoiseTestSuite::TickFrames(*engine, 1);
            RgbImage cur = RayTracedReflectionNoiseTestSuite::Capture(*engine, "rt_refl_structure_b.ppm");
            if (!ZHLN::Test::ExpectTrue(prev.Valid() && cur.Valid())) {
                return std::unexpected(ReflectionNoiseError::CaptureFailed);
            }

            const auto res = ZHLN::Test::Noise::MeasureResidual(prev.rgb.data(), cur.rgb.data(), kWidth, kHeight, kChangeThreshold);
            if (!ZHLN::Test::ExpectTrue(res.valid)) {
                return std::unexpected(ReflectionNoiseError::CaptureFailed);
            }

            const std::vector<double> diff = LumaDifference(prev, cur);
            const BBox                band = BBoxOfChangedPixels(diff.data(), kWidth, kHeight, kChangeThreshold, 4);

            ZHLN::Println(
                "    [INFO] full frame: meanAbs={:.3f} rms={:.3f} changed={:.5f} isolated={:.4f} maxAbs={:.1f}", res.meanAbs, res.rms,
                res.changedFraction, res.isolatedFraction, res.maxAbs
            );
            ZHLN::Println(
                "    [INFO] reflection bbox = [{},{}) x [{},{})  ({}x{}), region rms={:.3f}", band.x0, band.x1, band.y0, band.y1, band.Width(),
                band.Height(), RmsInRegion(diff.data(), kWidth, band)
            );

            if (!ZHLN::Test::ExpectTrue(!band.Empty() && band.Width() >= kMinRegionWidth && band.Height() >= kMinRegionHeight)) {
                return std::unexpected(ReflectionNoiseError::ReflectionRegionTooSmall);
            }

            const std::vector<double> region = Crop(diff.data(), kWidth, band);
            const auto                dir    = ZHLN::Test::Noise::MeasureDirectionalEnergy(region.data(), band.Width(), band.Height());
            const double              lob    = ZHLN::Test::Noise::AutocorrelationSideLobe(region.data(), band.Width(), band.Height(), 4);

            ZHLN::Println(
                "    [INFO] directional e0={:.4f} e45={:.4f} e90={:.4f} e135={:.4f} -> anisotropy={:.4f}", dir.e0, dir.e45, dir.e90, dir.e135,
                dir.Anisotropy()
            );
            ZHLN::Println("    [INFO] positive autocorrelation side lobe (lag<=4) = {:.4f}", lob);

            // Same measured thresholds as the shadow suite: the CPU sweep of
            // the shipped tile vs the IGN lattice puts blue noise at
            // anisotropy ~1.04-1.20 / lobe ~0.04-0.13 and the lattice at
            // ~2.9-3.3 / ~0.8-0.94 over regions this size; 1.60 and 0.30 sit
            // between the regimes.
            if (!ZHLN::Test::ExpectTrue(dir.Anisotropy() < 1.60)) {
                return std::unexpected(ReflectionNoiseError::DitherIsAnisotropic);
            }
            if (!ZHLN::Test::ExpectTrue(lob < 0.30)) {
                return std::unexpected(ReflectionNoiseError::DitherIsPeriodic);
            }

            ZHLN::Println("    [PASS] RTR residual is isotropic and aperiodic (blue noise, not a lattice).");
            return {};
        }

        /// The reflection noise must integrate away: the running mean over n
        /// captures must approach the all-capture mean at the Monte Carlo
        /// rate. Accumulation happens here on the CPU with AA off, because
        /// the engine's TAA feedback gives consecutive-frame RMS a floor of
        /// feedbackWeight * sigma -- it plateaus once the history is full,
        /// which is exactly what sank the first version of this scenario on
        /// real hardware (transient 2.03 -> 1.06, then a ~1.7 plateau).
        std::expected<void, ZHLN::Error> rtr_residual_converges_with_temporal_accumulation() {
            auto engine = RayTracedReflectionNoiseTestSuite::CreateTestEngine();
            if (!ZHLN::Test::AssertTrue(engine != nullptr)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; convergence check is not applicable.");
                return {};
            }
            if (!RayTracedReflectionNoiseTestSuite::BuildReflectionScene(*engine)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            RayTracedReflectionNoiseTestSuite::SetRTR(*engine, 0, 1);
            RayTracedReflectionNoiseTestSuite::SetAA(*engine, ZHLN::AAMode::None);
            RayTracedReflectionNoiseTestSuite::SetDenoiser(*engine, 0);
            RayTracedReflectionNoiseTestSuite::TickFrames(*engine, 2);

            RgbImage first = RayTracedReflectionNoiseTestSuite::Capture(*engine, "rt_refl_conv_prev.ppm");
            RayTracedReflectionNoiseTestSuite::TickFrames(*engine, 1);
            RgbImage second = RayTracedReflectionNoiseTestSuite::Capture(*engine, "rt_refl_conv_cur.ppm");
            if (!ZHLN::Test::ExpectTrue(first.Valid() && second.Valid())) {
                return std::unexpected(ReflectionNoiseError::CaptureFailed);
            }
            const std::vector<double> firstDiff = LumaDifference(first, second);
            const BBox                band      = BBoxOfChangedPixels(firstDiff.data(), kWidth, kHeight, kChangeThreshold, 4);
            if (!ZHLN::Test::ExpectTrue(!band.Empty() && band.Width() >= kMinRegionWidth && band.Height() >= kMinRegionHeight)) {
                return std::unexpected(ReflectionNoiseError::ReflectionRegionTooSmall);
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
                RayTracedReflectionNoiseTestSuite::TickFrames(*engine, 1);
                RgbImage cur = RayTracedReflectionNoiseTestSuite::Capture(*engine, "rt_refl_conv_cur.ppm");
                if (!ZHLN::Test::ExpectTrue(cur.Valid())) {
                    return std::unexpected(ReflectionNoiseError::CaptureFailed);
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
            const bool converged = series.front() > 0.5 && slope < 0.0 && fittedDrop > 0.35 * mean;
            if (!ZHLN::Test::ExpectTrue(converged)) {
                return std::unexpected(ReflectionNoiseError::ResidualDidNotConverge);
            }

            ZHLN::Println("    [PASS] Reflection noise integrates away under temporal accumulation.");
            return {};
        }

        /// Changed pixels must cluster; isolated single-pixel changes are ray
        /// debris, not VNDF jitter.
        std::expected<void, ZHLN::Error> rtr_residual_has_no_isolated_ray_debris() {
            auto engine = RayTracedReflectionNoiseTestSuite::CreateTestEngine();
            if (!ZHLN::Test::AssertTrue(engine != nullptr)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; debris check is not applicable.");
                return {};
            }
            if (!RayTracedReflectionNoiseTestSuite::BuildReflectionScene(*engine)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            RayTracedReflectionNoiseTestSuite::SetRTR(*engine, 0, 1);
            RayTracedReflectionNoiseTestSuite::SetAA(*engine, ZHLN::AAMode::None);
            RayTracedReflectionNoiseTestSuite::SetDenoiser(*engine, 0);

            RgbImage prev = RayTracedReflectionNoiseTestSuite::SettleAndCapture(*engine, "rt_refl_debris_a.ppm");
            RayTracedReflectionNoiseTestSuite::TickFrames(*engine, 1);
            RgbImage cur = RayTracedReflectionNoiseTestSuite::Capture(*engine, "rt_refl_debris_b.ppm");
            if (!ZHLN::Test::ExpectTrue(prev.Valid() && cur.Valid())) {
                return std::unexpected(ReflectionNoiseError::CaptureFailed);
            }

            const auto res = ZHLN::Test::Noise::MeasureResidual(prev.rgb.data(), cur.rgb.data(), kWidth, kHeight, kChangeThreshold);
            ZHLN::Println(
                "    [INFO] changed={:.5f} isolated-of-changed={:.4f} maxAbs={:.1f}", res.changedFraction, res.isolatedFraction, res.maxAbs
            );

            if (!ZHLN::Test::ExpectTrue(res.changedFraction > 1e-5)) {
                return std::unexpected(ReflectionNoiseError::JitterTemporallyFrozen);
            }
            if (!ZHLN::Test::ExpectTrue(res.isolatedFraction < 0.6)) {
                return std::unexpected(ReflectionNoiseError::RayDebrisDetected);
            }

            ZHLN::Println("    [PASS] Reflection changes cluster; no isolated ray debris.");
            return {};
        }

        /// The A-Trous HDR denoiser must remove variance, not just relocate
        /// it: same scene and the same on/on residual estimator as the raw
        /// scenarios, measured once with the wavelet bypassed and once with
        /// three iterations (steps 1/2/4). A symmetric kernel integrating
        /// blue noise must shrink the per-frame difference by a wide margin;
        /// a frozen or bypassed denoiser leaves it untouched.
        std::expected<void, ZHLN::Error> hdr_denoiser_reduces_reflection_noise() {
            // TEMPORARY: mirrors kDenoiseDispatchEnabled in
            // RenderGraphBuilder.cpp -- the denoiser dispatch is disabled
            // while the black-frame bisect runs; re-enable both together.
            static const bool kDenoiseDispatchEnabled = false;
            if (!kDenoiseDispatchEnabled) {
                ZHLN::Println("    [SKIP] Denoiser dispatch is disabled pending the black-frame bisect.");
                return {};
            }
            auto engine = RayTracedReflectionNoiseTestSuite::CreateTestEngine();
            if (!ZHLN::Test::AssertTrue(engine != nullptr)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().RayTracingSupported()) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; denoiser check is not applicable.");
                return {};
            }
            if (!RayTracedReflectionNoiseTestSuite::BuildReflectionScene(*engine)) {
                return std::unexpected(ReflectionNoiseError::EngineInitFailed);
            }
            RayTracedReflectionNoiseTestSuite::SetRTR(*engine, 0, 1);
            RayTracedReflectionNoiseTestSuite::SetAA(*engine, ZHLN::AAMode::None);

            // Raw pair: denoiser bypassed.
            RayTracedReflectionNoiseTestSuite::SetDenoiser(*engine, 0);
            RgbImage rawA = RayTracedReflectionNoiseTestSuite::SettleAndCapture(*engine, "rt_denoise_raw_a.ppm");
            RayTracedReflectionNoiseTestSuite::TickFrames(*engine, 1);
            RgbImage rawB = RayTracedReflectionNoiseTestSuite::Capture(*engine, "rt_denoise_raw_b.ppm");
            if (!ZHLN::Test::ExpectTrue(rawA.Valid() && rawB.Valid())) {
                return std::unexpected(ReflectionNoiseError::CaptureFailed);
            }
            const std::vector<double> rawDiff = LumaDifference(rawA, rawB);
            const BBox                band    = BBoxOfChangedPixels(rawDiff.data(), kWidth, kHeight, kChangeThreshold, 4);
            if (!ZHLN::Test::ExpectTrue(!band.Empty() && band.Width() >= kMinRegionWidth && band.Height() >= kMinRegionHeight)) {
                return std::unexpected(ReflectionNoiseError::ReflectionRegionTooSmall);
            }
            const double rawRms = RmsInRegion(rawDiff.data(), kWidth, band);
            ZHLN::Println(
                "    [INFO] denoiser OFF: on/on residual rms over [{},{}) x [{},{}) = {:.4f}", band.x0, band.x1, band.y0, band.y1, rawRms
            );

            // Denoised pair: three wavelet iterations.
            RayTracedReflectionNoiseTestSuite::SetDenoiser(*engine, 3);
            RgbImage denA = RayTracedReflectionNoiseTestSuite::SettleAndCapture(*engine, "rt_denoise_on_a.ppm");
            RayTracedReflectionNoiseTestSuite::TickFrames(*engine, 1);
            RgbImage denB = RayTracedReflectionNoiseTestSuite::Capture(*engine, "rt_denoise_on_b.ppm");
            if (!ZHLN::Test::ExpectTrue(denA.Valid() && denB.Valid())) {
                return std::unexpected(ReflectionNoiseError::CaptureFailed);
            }
            const std::vector<double> denDiff = LumaDifference(denA, denB);
            const double              denRms  = RmsInRegion(denDiff.data(), kWidth, band);
            const auto                meanLumaOf = [](const RgbImage& img) {
                const std::vector<double> pl = LumaPlane(img);
                return std::accumulate(pl.begin(), pl.end(), 0.0) / static_cast<double>(pl.size());
            };
            const double rawMeanLuma = meanLumaOf(rawA);
            const double denMeanLuma = meanLumaOf(denA);
            ZHLN::Println("    [INFO] denoiser ON (3 passes): on/on residual rms over the same band = {:.4f}", denRms);
            ZHLN::Println("    [INFO] frame mean luma: raw={:.3f} denoised={:.3f}", rawMeanLuma, denMeanLuma);

            // A wide margin on purpose: spatially integrating blue noise
            // should cut the residual far below 70%; anything close to 1.0
            // means the pass never ran or its output never reached the
            // capture. The raw floor rejects a degenerate scene where both
            // residuals are ~0 and the ratio gate passes vacuously.
            if (!ZHLN::Test::ExpectTrue(rawRms > 0.5)) {
                return std::unexpected(ReflectionNoiseError::JitterTemporallyFrozen);
            }
            // A spatial filter preserves the mean; if the denoised frame lost
            // most of its luma the pass blackened the image, and a ~0 residual
            // would otherwise sail through the variance gate below.
            if (!ZHLN::Test::ExpectTrue(denMeanLuma > 0.25 * rawMeanLuma)) {
                return std::unexpected(ReflectionNoiseError::DenoiserDidNotReduceNoise);
            }
            if (!ZHLN::Test::ExpectTrue(denRms < 0.7 * rawRms)) {
                return std::unexpected(ReflectionNoiseError::DenoiserDidNotReduceNoise);
            }

            ZHLN::Println("    [PASS] A-Trous wavelet cuts the on/on residual on the reflection band.");
            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<RayTracedReflectionNoiseTestSuite>();
}
