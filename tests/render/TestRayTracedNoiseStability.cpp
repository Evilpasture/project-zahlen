// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestRayTracedNoiseStability.cpp
//
// GPU-side verification of deterministic hybrid sun shadows:
//   Shadow = CascadePCSS * RtContactShadow (0 -> 3.0m inline RayQuery)
//
// Broad scene shadows and distant soft penumbrae come from 4-cascade PCSS, while
// hardware ray tracing fires 1 deterministic unjittered contact ray along exact
// L_sun (kRayMaskWorld, TMax = 3.0m, full occlusion over [0, 1.2m] with smooth
// handoff over [1.2m, 3.0m]) to seal CSM Peter-Panning and ground thin casters
// with zero blue-noise boil and zero temporal history ghosting:
//
//   1. CONTACT LIVENESS & ZERO BOIL - toggling enableRTR seals the CSM contact
//      gap behind a low grounded curb, while two consecutive RT-enabled frames
//      produce zero temporal boil.
//   2. 3.0m RANGE HANDOFF           - a raised floating caster (y = 15m, ray
//      distance ~16m >> 3.0m) still casts a broad CascadePCSS shadow onto the
//      floor, while toggling enableRTR leaves pixels > 3.0m untouched.
//   3. SPATIAL COHERENCE & ZERO LAG - the contact-shadow delta clusters cleanly
//      without isolated ray debris, and moving the caster updates immediately on
//      the first frame without temporal history ghost trails.

#include "RayTracedNoiseMetrics.hpp"
#include "TestsFramework.hpp"
#include "helpers/HeadlessEngineFixture.hpp"
#include "helpers/ImageTesting.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Components.hpp>
#include <Zahlen/Engine.hpp>
#include <Zahlen/GraphicsSettings.hpp>
#include <Zahlen/Log.hpp>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/PrefabFactory.hpp>
#include <Zahlen/Render/GpuEnums.hpp>
#include <Zahlen/Render/Render.hpp>
#include <Zahlen/ecs/ECS.hpp>
#include <cstdint>
#include <expected>
#include <string_view>

enum class NoiseStabilityError : uint8_t {
    EngineInitFailed ZHLN_ANNOTATION(ZHLN::Description<"Failed to initialize the headless Engine for the RT contact shadow stability test.">{}) = 1,
    CaptureFailed ZHLN_ANNOTATION(ZHLN::Description<"A frame could not be captured or the PPM could not be read back.">{}),
    RayTracedShadowPathInactive ZHLN_ANNOTATION(ZHLN::Description<"Enabling enableRTR did not change a single pixel behind the grounded contact curb, so TraceSunContactShadow is not executing: check pc.enableRTR, the TLAS binding on Shaders::Lighting, or the lighting pipeline variant.">{}),
    TemporalBoilDetected ZHLN_ANNOTATION(ZHLN::Description<"Two consecutive static frames with enableRTR=1 differed: deterministic hybrid contact shadows must be 100% boil-free.">{}),
    CascadeShadowMissing ZHLN_ANNOTATION(ZHLN::Description<"Despawning the raised caster did not change the floor, so broad CascadePCSS sun shadows did not render.">{}),
    ContactRangeCutoffViolated ZHLN_ANNOTATION(ZHLN::Description<"Toggling enableRTR altered the shadow of a caster 16m away along L_sun; TraceSunContactShadow must stay bounded to its 3.0m contact range.">{}),
    RayDebrisDetected ZHLN_ANNOTATION(ZHLN::Description<"The contact-shadow on/off delta is dominated by isolated single-pixel outliers (ray debris / acne).">{}),
    TemporalGhostLagDetected ZHLN_ANNOTATION(ZHLN::Description<"The frame immediately following a caster move differed from the settled frame; hybrid sun shadows must have zero temporal history lag.">{}),
};

namespace {

using ZHLN::Test::Image::RgbImage;

constexpr int    kWidth           = 640;
constexpr int    kHeight          = 480;
constexpr double kChangeThreshold = 2.0;

} // namespace

struct RayTracedNoiseStabilityTestSuite {
    RayTracedNoiseStabilityTestSuite() {
        ZHLN::Test::Headless::BeginSession();
    }

    ~RayTracedNoiseStabilityTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    static auto CreateTestEngine() -> ZHLN::Test::Headless::EngineHandle {
        return ZHLN::Test::Headless::AcquireEngine(ZHLN::Test::Headless::EngineOptions {
            .appName = "Headless RT Contact Shadow Stability", .width = kWidth, .height = kHeight
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
    }

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

    // Rough floor + raking sun from +Z so contact seams project directly toward
    // the camera on the floor. Every material stays rough (>= 0.70 > 0.40) and
    // SSR is off, isolating pc.enableRTR to TraceSunContactShadow in lighting.slang.
    static auto BuildContactBaseScene(ZHLN::Engine& engine, ZHLN::Material& outBoxMat) -> bool {
        auto& reg = engine.GetRegistry();

        auto floorMat =
            engine.GetRenderContext().CreateMaterial(ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.8f, 0.8f, 0.82f, 1.0f}});
        auto boxMat =
            engine.GetRenderContext().CreateMaterial(ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.75f, .baseColor = {0.25f, 0.25f, 0.28f, 1.0f}});
        if (!floorMat || !boxMat) {
            return false;
        }
        outBoxMat = *boxMat;

        ZHLN::PrefabFactory::CreatePlane(
            engine, 120.0f, {0.8f, 0.8f, 0.82f, 1.0f},
            ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *floorMat}
        );

        const ZHLN::Entity sunEnt = reg.Create();
        reg.Add(
            sunEnt,
            ZHLN::Components::TransformComponent {.position = JPH::Vec3(0.0f, 50.0f, 40.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({40.0f, 0.0f, 0.0f})},
            ZHLN::Components::LightComponent {
                .type      = ZHLN::LightType::Sun,
                .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                .intensity = 120.0f,
                .direction = JPH::Vec3(0.0f, 0.45f, 0.89f).Normalized()
            }
        );

        const auto camComp = reg.GetSingleton<ZHLN::Components::CameraComponent>();
        if (!ZHLN::Test::ExpectTrue(camComp.has_value())) {
            return false;
        }
        ZHLN::Camera& cam = camComp->camera;
        cam.position = JPH::Vec3(0.0f, 4.5f, 22.0f);
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

    static RgbImage Capture(ZHLN::Engine& engine, std::string_view name) {
        return ZHLN::Test::Headless::Capture(engine, name);
    }

    static RgbImage SettleAndCapture(ZHLN::Engine& engine, std::string_view name) {
        TickFrames(engine, 2);
        return Capture(engine, name);
    }

    struct Tests {
        // 1. Contact-shadow liveness (sealing CSM Peter-Panning behind a low
        //    grounded curb) and zero frame-to-frame temporal boil.
        std::expected<void, ZHLN::ErrorCode> rt_contact_shadow_is_live_and_boil_free() {
            auto engine = RayTracedNoiseStabilityTestSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().GetInfo().rayTracingSupported) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; RT contact shadow check is not applicable.");
                return {};
            }

            ZHLN::Material boxMat {};
            if (!RayTracedNoiseStabilityTestSuite::BuildContactBaseScene(*engine, boxMat)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }

            // Low grounded curb (12 cm tall) + thin suspended contact bar (1.6 cm
            // thick at y = 0.18m, t ~ 0.4m < 1.2m along L_sun) right inside the
            // CSM normal/depth-bias Peter-Panning zone.
            ZHLN::PrefabFactory::CreateBox(
                *engine, JPH::Vec3(4.0f, 0.06f, 0.25f),
                ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.06, 15.0), .createPhysics = false, .materialOverride = boxMat}
            );
            ZHLN::PrefabFactory::CreateBox(
                *engine, JPH::Vec3(4.0f, 0.008f, 0.012f),
                ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.18, 16.5), .createPhysics = false, .materialOverride = boxMat}
            );

            RayTracedNoiseStabilityTestSuite::SetAA(*engine, ZHLN::AAMode::None);
            RayTracedNoiseStabilityTestSuite::SetDenoiser(*engine, 0);

            RayTracedNoiseStabilityTestSuite::SetRayTracedShadows(*engine, 0);
            RgbImage off = RayTracedNoiseStabilityTestSuite::SettleAndCapture(*engine, "rt_contact_live_off.ppm");

            RayTracedNoiseStabilityTestSuite::SetRayTracedShadows(*engine, 1);
            RgbImage on1 = RayTracedNoiseStabilityTestSuite::SettleAndCapture(*engine, "rt_contact_live_on1.ppm");
            RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 1);
            RgbImage on2 = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_contact_live_on2.ppm");

            if (!(ZHLN::Test::ExpectTrue(off.Valid()) && ZHLN::Test::ExpectTrue(on1.Valid()) && ZHLN::Test::ExpectTrue(on2.Valid()))) {
                return std::unexpected(NoiseStabilityError::CaptureFailed);
            }

            const auto onVsOff = ZHLN::Test::Noise::MeasureResidual(on1.rgb.data(), off.rgb.data(), kWidth, kHeight, kChangeThreshold);
            const auto onVsOn  = ZHLN::Test::Noise::MeasureResidual(on1.rgb.data(), on2.rgb.data(), kWidth, kHeight, kChangeThreshold);

            ZHLN::Println("    [INFO] RT contact on vs off : meanAbs={:.3f} rms={:.3f} changed={:.5f}", onVsOff.meanAbs, onVsOff.rms, onVsOff.changedFraction);
            ZHLN::Println("    [INFO] RT contact on vs on  : meanAbs={:.3f} rms={:.3f} changed={:.5f}", onVsOn.meanAbs, onVsOn.rms, onVsOn.changedFraction);

            if (!ZHLN::Test::ExpectGt(onVsOff.changedFraction, 0.0)) {
                return std::unexpected(NoiseStabilityError::RayTracedShadowPathInactive);
            }
            if (!ZHLN::Test::ExpectLt(onVsOn.changedFraction, 0.001)) {
                return std::unexpected(NoiseStabilityError::TemporalBoilDetected);
            }

            ZHLN::Println("    [PASS] RT contact shadow seals near-field contact gaps with zero temporal boil.");
            return {};
        }

        // 2. Casters well beyond the 3.0m contact range are owned exclusively by
        //    CascadePCSS (broad soft shadow exists, enableRTR leaves >3m untouched).
        std::expected<void, ZHLN::ErrorCode> rt_contact_shadow_respects_3m_range_cutoff() {
            auto engine = RayTracedNoiseStabilityTestSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().GetInfo().rayTracingSupported) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; 3.0m cutoff check is not applicable.");
                return {};
            }

            ZHLN::Material boxMat {};
            if (!RayTracedNoiseStabilityTestSuite::BuildContactBaseScene(*engine, boxMat)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }

            // Raised caster at y = 12.0m (bottom at y = 9.0m -> ray distance from
            // floor along L_sun is ~20m >> 3.0m), projecting onto z ~ -6..+4 in view.
            auto& reg = engine->GetRegistry();
            if (const auto camComp = reg.GetSingleton<ZHLN::Components::CameraComponent>(); camComp.has_value()) {
                camComp->camera.position = JPH::Vec3(0.0f, 10.0f, 30.0f);
                camComp->camera.pitch    = -18.0f;
            }
            const ZHLN::Entity highCaster = ZHLN::PrefabFactory::CreateBox(
                *engine, JPH::Vec3(3.0f, 3.0f, 3.0f),
                ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 12.0, 22.0), .createPhysics = false, .materialOverride = boxMat}
            );

            RayTracedNoiseStabilityTestSuite::SetAA(*engine, ZHLN::AAMode::None);
            RayTracedNoiseStabilityTestSuite::SetDenoiser(*engine, 0);

            RayTracedNoiseStabilityTestSuite::SetRayTracedShadows(*engine, 0);
            RgbImage off = RayTracedNoiseStabilityTestSuite::SettleAndCapture(*engine, "rt_contact_far_off.ppm");

            RayTracedNoiseStabilityTestSuite::SetRayTracedShadows(*engine, 1);
            RgbImage on = RayTracedNoiseStabilityTestSuite::SettleAndCapture(*engine, "rt_contact_far_on.ppm");

            ZHLN::DespawnEntity(*engine, highCaster);
            engine->ProcessPendingDestroy();
            RgbImage clear = RayTracedNoiseStabilityTestSuite::SettleAndCapture(*engine, "rt_contact_far_clear.ppm");

            if (!(ZHLN::Test::ExpectTrue(off.Valid()) && ZHLN::Test::ExpectTrue(on.Valid()) && ZHLN::Test::ExpectTrue(clear.Valid()))) {
                return std::unexpected(NoiseStabilityError::CaptureFailed);
            }

            const auto onVsClear = ZHLN::Test::Noise::MeasureResidual(on.rgb.data(), clear.rgb.data(), kWidth, kHeight, kChangeThreshold);
            const auto onVsOff   = ZHLN::Test::Noise::MeasureResidual(on.rgb.data(), off.rgb.data(), kWidth, kHeight, kChangeThreshold);

            ZHLN::Println("    [INFO] Far caster (>3m) on vs clear : changed={:.5f} rms={:.3f}", onVsClear.changedFraction, onVsClear.rms);
            ZHLN::Println("    [INFO] Far caster (>3m) on vs off   : changed={:.5f} rms={:.3f}", onVsOff.changedFraction, onVsOff.rms);

            if (!ZHLN::Test::ExpectGt(onVsClear.changedFraction, 0.005)) {
                return std::unexpected(NoiseStabilityError::CascadeShadowMissing);
            }
            if (!ZHLN::Test::ExpectLt(onVsOff.changedFraction, 0.002)) {
                return std::unexpected(NoiseStabilityError::ContactRangeCutoffViolated);
            }

            ZHLN::Println("    [PASS] Broad shadows >3.0m come from CascadePCSS and respect the 3.0m RT contact cutoff.");
            return {};
        }

        // 3. Spatial clustering (no isolated ray debris) and instant single-frame
        //    response on moving casters (zero temporal shadow history lag).
        std::expected<void, ZHLN::ErrorCode> rt_contact_shadow_has_no_debris_and_zero_motion_lag() {
            auto engine = RayTracedNoiseStabilityTestSuite::CreateTestEngine();
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }
            if (!engine->GetRenderContext().GetInfo().rayTracingSupported) {
                ZHLN::Println("    [SKIP] Device has no ray tracing support; debris/motion check is not applicable.");
                return {};
            }

            ZHLN::Material boxMat {};
            if (!RayTracedNoiseStabilityTestSuite::BuildContactBaseScene(*engine, boxMat)) {
                return std::unexpected(NoiseStabilityError::EngineInitFailed);
            }

            const ZHLN::Entity curb = ZHLN::PrefabFactory::CreateBox(
                *engine, JPH::Vec3(2.2f, 0.06f, 0.25f),
                ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(-2.0, 0.06, 15.0), .createPhysics = false, .materialOverride = boxMat}
            );

            RayTracedNoiseStabilityTestSuite::SetAA(*engine, ZHLN::AAMode::None);
            RayTracedNoiseStabilityTestSuite::SetDenoiser(*engine, 0);

            RayTracedNoiseStabilityTestSuite::SetRayTracedShadows(*engine, 0);
            RgbImage off = RayTracedNoiseStabilityTestSuite::SettleAndCapture(*engine, "rt_contact_debris_off.ppm");

            RayTracedNoiseStabilityTestSuite::SetRayTracedShadows(*engine, 1);
            RgbImage on = RayTracedNoiseStabilityTestSuite::SettleAndCapture(*engine, "rt_contact_debris_on.ppm");

            if (!(ZHLN::Test::ExpectTrue(off.Valid()) && ZHLN::Test::ExpectTrue(on.Valid()))) {
                return std::unexpected(NoiseStabilityError::CaptureFailed);
            }

            const auto delta = ZHLN::Test::Noise::MeasureResidual(on.rgb.data(), off.rgb.data(), kWidth, kHeight, kChangeThreshold);
            ZHLN::Println(
                "    [INFO] Contact delta: changed={:.5f} isolated-of-changed={:.4f} maxAbs={:.1f}", delta.changedFraction, delta.isolatedFraction,
                delta.maxAbs
            );

            if (!ZHLN::Test::ExpectGt(delta.changedFraction, 1e-5)) {
                return std::unexpected(NoiseStabilityError::RayTracedShadowPathInactive);
            }
            if (!ZHLN::Test::ExpectLt(delta.isolatedFraction, 0.6)) {
                return std::unexpected(NoiseStabilityError::RayDebrisDetected);
            }

            // Teleport the contact curb sideways (+4.0m) and capture the VERY NEXT
            // rendered frame vs a settled frame 3 ticks later: without temporal
            // shadow history, frame 1 at the new position must already be settled.
            engine->GetRegistry().Patch<ZHLN::Components::TransformComponent>(curb, [](auto& tr) { tr.position = JPH::Vec3(2.0f, 0.06f, 15.0f); });
            RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 1);
            RgbImage movedImmediate = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_contact_moved_immediate.ppm");
            RayTracedNoiseStabilityTestSuite::TickFrames(*engine, 3);
            RgbImage movedSettled = RayTracedNoiseStabilityTestSuite::Capture(*engine, "rt_contact_moved_settled.ppm");

            if (!(ZHLN::Test::ExpectTrue(movedImmediate.Valid()) && ZHLN::Test::ExpectTrue(movedSettled.Valid()))) {
                return std::unexpected(NoiseStabilityError::CaptureFailed);
            }

            const auto lag = ZHLN::Test::Noise::MeasureResidual(movedImmediate.rgb.data(), movedSettled.rgb.data(), kWidth, kHeight, kChangeThreshold);
            ZHLN::Println("    [INFO] Dynamic caster immediate vs settled: changed={:.5f} rms={:.3f}", lag.changedFraction, lag.rms);

            if (!ZHLN::Test::ExpectLt(lag.changedFraction, 0.001)) {
                return std::unexpected(NoiseStabilityError::TemporalGhostLagDetected);
            }

            ZHLN::Println("    [PASS] Contact shadow clusters cleanly and tracks caster motion on frame 1 with zero ghost lag.");
            return {};
        }
    };
};

// Exported for the GPU_Lighting group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunRayTracedNoiseStabilitySuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<RayTracedNoiseStabilityTestSuite>();
}
