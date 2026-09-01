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
        std::expected<void, ZHLN::Error> raytraced_shadow_occlusion_and_stability() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            if (!engine->GetRenderContext().RayTracingSupported()) {
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

                auto floorMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.55f, 0.55f, 0.55f, 1.0f}}
                );
                auto checkFloorMat = ZHLN::Test::AssertTrue(floorMatRes.has_value());
                if (!checkFloorMat) {
                    return checkFloorMat;
                }
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 400.0f, {0.55f, 0.55f, 0.55f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *floorMatRes}
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

                auto& cam    = engine->GetCamera();
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

                    const ZHLN::Entity occluder = ZHLN::CreativeWorksFactory::CreateBox(
                        eng, JPH::Vec3(0.5f, 3.0f, 4.0f),
                        ZHLN::CreativeWorksFactory::SpawnParams {
                            .position = JPH::RVec3(0.0, 3.0, -8.0), .createPhysics = false, .color = {0.7f, 0.7f, 0.7f, 1.0f}
                        }
                    );

                    TickFrames(eng, 2);

                    const RgbImage shadowA       = Capture(eng, "headless_lighting_rt_shadow_a.ppm");
                    const RgbImage shadowARepeat = Capture(eng, "headless_lighting_rt_shadow_a_repeat.ppm");
                    TickFrames(eng, 1);
                    const RgbImage shadowB = Capture(eng, "headless_lighting_rt_shadow_b.ppm");

                    auto checkFrame = ZHLN::Test::AssertTrue(shadowA.Valid() && shadowARepeat.Valid() && shadowB.Valid());
                    if (!checkFrame) {
                        captureFailed = true;
                        reg.Destroy(occluder);
                        return false;
                    }

                    reg.Destroy(occluder);
                    ZHLN::Test::ExpectFalse(reg.IsAlive(occluder));

                    TickFrames(eng, 2);
                    const RgbImage shadowClear = Capture(eng, "headless_lighting_rt_shadow_clear.ppm");
                    checkFrame                 = ZHLN::Test::AssertTrue(shadowClear.Valid());
                    if (!checkFrame) {
                        captureFailed = true;
                        return false;
                    }

                    constexpr double   kFloorRowFraction = 0.72;
                    const FrameMetrics mA                = MeasureImage(shadowA, kFloorRowFraction);
                    const FrameMetrics mA2               = MeasureImage(shadowARepeat, kFloorRowFraction);
                    const FrameMetrics mB                = MeasureImage(shadowB, kFloorRowFraction);
                    const FrameMetrics mClear            = MeasureImage(shadowClear, kFloorRowFraction);

                    const uint32_t darkA     = mA.dark;
                    const uint32_t darkA2    = mA2.dark;
                    const uint32_t darkB     = mB.dark;
                    const uint32_t darkClear = mClear.dark;
                    const uint32_t litA      = mA.lit;
                    const uint32_t litClear  = mClear.lit;

                    const FrameDiff repeatDiff   = CompareFrames(shadowA, shadowARepeat);
                    const FrameDiff temporalDiff = CompareFrames(shadowA, shadowB);

                    const double darkJump = (darkA > 0) ? static_cast<double>(std::abs(static_cast<int64_t>(darkB) - static_cast<int64_t>(darkA))) /
                                                              static_cast<double>(darkA) :
                                                          0.0;

                    const bool shadowExist     = ZHLN::Test::ExpectTrue(darkA > darkClear + 1500u);
                    const bool lightRestored   = ZHLN::Test::ExpectTrue(darkClear < darkA / 3u);
                    const bool meanBrightens   = ZHLN::Test::ExpectTrue(mClear.meanLuma > mA.meanLuma * 1.25 + 1.0);
                    const bool notBlackout     = ZHLN::Test::ExpectTrue(darkA < 0.85 * static_cast<double>(mA.total));
                    const bool shadowStable    = ZHLN::Test::ExpectTrue(darkJump < 0.15);
                    const bool noShadowFlicker = ZHLN::Test::ExpectTrue(temporalDiff.frac32 < 0.015);
                    const bool repeatClean     = ZHLN::Test::ExpectTrue(repeatDiff.frac32 == 0.0);

                    return shadowExist && lightRestored && meanBrightens && notBlackout && shadowStable && noShadowFlicker && repeatClean;
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
