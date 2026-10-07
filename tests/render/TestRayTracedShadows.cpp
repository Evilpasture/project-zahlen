// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestRayTracedShadows.cpp
//
// Ray-traced sun shadow occlusion and its frame-to-frame stability.
//
// Split out of TestLightingRayTraced.cpp; the shared error enum, frame I/O
// and engine fixture live in LightingRTCommon.hpp.

#include "LightingRTCommon.hpp"

// ============================================================================
// Test Suite
// ============================================================================

struct RayTracedShadowsTestSuite {
    RayTracedShadowsTestSuite() {
        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~RayTracedShadowsTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    struct Tests {
        // ====================================================================
        // 4. Ray-Traced Shadow Occlusion & Stability
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> raytraced_shadow_occlusion_and_stability() {
            auto engine      = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            if (!engine->GetRenderContext().GetInfo().rayTracingSupported) {
                ZHLN::Println("    [SKIP] No raytracing support on this device; nothing to verify for RT shadows.");
                return {};
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 1.5f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 1;
                    });
                }

                auto floorMatRes = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.55f, 0.55f, 0.55f, 1.0f}});
                if (!ZHLN::Test::ExpectTrue(floorMatRes.has_value())) {
                    return std::unexpected(LightingRTTestError::MaterialCreationFailed);
                }
                ZHLN::PrefabFactory::CreatePlane(
                    *engine, 400.0f, {0.55f, 0.55f, 0.55f, 1.0f},
                    ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *floorMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(60.0f, 45.0f, 0.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({0.0f, 90.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 240.0f,
                        .direction = JPH::Vec3(0.8f, 0.6f, 0.0f).Normalized()
                    }
                );

                const auto camComp = engine->GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
                if (!ZHLN::Test::ExpectTrue(camComp.has_value())) {
                    return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
                }
                ZHLN::Camera& cam = camComp->camera;
                cam.position = JPH::Vec3(-10.0f, 6.0f, -8.0f);
                cam.yaw      = 0.0f;
                cam.pitch    = -20.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;
            // Set when the render could not be captured at all, which is not a
            // lighting failure and must not be reported as one.
            bool captureFailed = false;

            const auto stable = RunStableScene(
                *engine, 8, "raytraced_shadow_occlusion_and_stability",
                [&](ZHLN::Engine& eng) -> bool {
                    captureFailed = false;
                    auto& reg = eng.GetRegistry();

                    const ZHLN::Entity occluder = ZHLN::PrefabFactory::CreateBox(
                        eng, JPH::Vec3(0.5f, 3.0f, 4.0f),
                        ZHLN::PrefabFactory::SpawnParams {
                            .position = JPH::RVec3(0.0, 3.0, -8.0), .createPhysics = false, .color = {0.7f, 0.7f, 0.7f, 1.0f}
                        }
                    );

                    TickFrames(eng, 2);

                    const RgbImage shadowA       = Capture(eng, "headless_lighting_rt_shadow_a.ppm");
                    const RgbImage shadowARepeat = Capture(eng, "headless_lighting_rt_shadow_a_repeat.ppm");
                    TickFrames(eng, 1);
                    const RgbImage shadowB = Capture(eng, "headless_lighting_rt_shadow_b.ppm");

                    if (!(ZHLN::Test::ExpectTrue(shadowA.Valid()) && ZHLN::Test::ExpectTrue(shadowARepeat.Valid()) &&
                          ZHLN::Test::ExpectTrue(shadowB.Valid()))) {
                        return false;
                    }

                    ZHLN::DespawnEntity(eng, occluder);
                    eng.ProcessPendingDestroy();
                    ZHLN::Test::ExpectFalse(reg.IsAlive(occluder));

                    TickFrames(eng, 2);
                    const RgbImage shadowClear = Capture(eng, "headless_lighting_rt_shadow_clear.ppm");
                    if (!ZHLN::Test::ExpectTrue(shadowClear.Valid())) {
                        captureFailed = true;
                        return false;
                    }

                    constexpr double   kFloorRowFraction = 0.72;
                    const FrameMetrics mA                = MeasureImage(shadowA, kFloorRowFraction);
                    const FrameMetrics mB                = MeasureImage(shadowB, kFloorRowFraction);
                    const FrameMetrics mClear            = MeasureImage(shadowClear, kFloorRowFraction);

                    const FrameDiff repeatDiff   = CompareFrames(shadowA, shadowARepeat);
                    const FrameDiff temporalDiff = CompareFrames(shadowA, shadowB);
                    const FrameDiff signalDiff   = CompareFrames(shadowA, shadowClear);

                    ZHLN::Println(
                        "    [INFO] RT shadow: A mean={:.1f} B mean={:.1f} clear mean={:.1f} | temporal frac32={:.4f} signal frac32={:.4f} repeat frac32={:.4f}",
                        mA.meanLuma, mB.meanLuma, mClear.meanLuma, temporalDiff.frac32, signalDiff.frac32, repeatDiff.frac32
                    );

                    // Occlusion is the A/B mean delta (the shadow drops the
                    // floor band ~95 luma), not a dark-pixel count: luma < 24
                    // reads 0 on both sides of a working shadow once the scene
                    // is this bright. Stability is dither-aware: the RT shadow
                    // is 1 SPP stochastic by design (see the noise suite), so
                    // consecutive frames MUST differ in the penumbra -- what
                    // must hold is that the dither sits an order of magnitude
                    // below the occlusion signal, and that settled means agree.
                    const bool shadowExist       = ZHLN::Test::ExpectGt(mClear.meanLuma, mA.meanLuma * 1.25 + 1.0);
                    const bool notBlackout       = ZHLN::Test::ExpectGt(mA.meanLuma, 5.0);
                    const bool ditherBelowSignal = ZHLN::Test::ExpectLt(temporalDiff.frac32, 0.1 * signalDiff.frac32);
                    const bool meansAgree        = ZHLN::Test::ExpectLt(std::abs(mB.meanLuma - mA.meanLuma), 2.0);
                    const bool repeatClean       = ZHLN::Test::ExpectTrue(repeatDiff.frac32 == 0.0);

                    return shadowExist && notBlackout && ditherBelowSignal && meansAgree && repeatClean;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                if (captureFailed) {
                    return std::unexpected(LightingRTTestError::RenderOutputBlank);
                }
                return std::unexpected(LightingRTTestError::RayTracedShadowFailed);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            return {};
        }
    };
};

// Exported for the GPU_Lighting group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunRayTracedShadowsSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<RayTracedShadowsTestSuite>();
}
