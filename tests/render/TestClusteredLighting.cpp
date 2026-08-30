// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestClusteredLighting.cpp
//
// Point-light clustering: static frame stability, cluster culling sweep,
// static reference with no history, and multi-light clustered accumulation
// with chromatic interaction.
//
// Split out of TestLightingRayTraced.cpp; the shared error enum, frame I/O
// and engine fixture live in LightingRTCommon.hpp.

#include "LightingRTCommon.hpp"

// ============================================================================
// Test Suite
// ============================================================================

struct ClusteredLightingTestSuite {
    ClusteredLightingTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~ClusteredLightingTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        // ====================================================================
        // 1. Lit Scene Static Frame Stability
        // ====================================================================
        std::expected<void, ZHLN::Error> lit_scene_static_frame_stability() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 10.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                    });
                }

                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.55f, 0.55f, 0.58f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false}
                );

                auto grayMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.65f, .baseColor = {0.8f, 0.8f, 0.8f, 1.0f}}
                );
                auto redMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.7f, .baseColor = {0.9f, 0.1f, 0.1f, 1.0f}}
                );
                auto blueMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.1f, 0.2f, 0.9f, 1.0f}}
                );

                auto checkMaterials = ZHLN::Test::AssertTrue(grayMatRes && redMatRes && blueMatRes);
                if (!checkMaterials) {
                    return checkMaterials;
                }

                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.8f, 0.8f, 0.8f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(-2.2, 1.0, 0.0), .createPhysics = false, .materialOverride = *grayMatRes}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 1.0, -2.0), .createPhysics = false, .materialOverride = *redMatRes}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(2.2, 1.0, 1.0), .createPhysics = false, .materialOverride = *blueMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 40.0f, 30.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({45.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 220.0f,
                        .direction = JPH::Vec3(0.0f, 0.6f, 0.8f).Normalized()
                    }
                );

                auto addPointLight = [&](const JPH::Vec3& pos, const JPH::Vec3& color, float intensity) {
                    const ZHLN::Entity e = reg.Create();
                    reg.Add(
                        e, ZHLN::Components::TransformComponent {.position = pos},
                        ZHLN::Components::LightComponent {.type = ZHLN::LightType::Point, .color = color, .intensity = intensity, .range = 30.0f}
                    );
                };
                addPointLight(JPH::Vec3(-4.0f, 3.0f, 2.0f), JPH::Vec3(1.0f, 0.55f, 0.3f), 800.0f);
                addPointLight(JPH::Vec3(4.0f, 3.0f, -3.0f), JPH::Vec3(0.3f, 0.5f, 1.0f), 800.0f);

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 2.5f, 8.0f);
                cam.yaw      = -90.0f;
                cam.pitch    = -12.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;
            // Set when the render could not be captured at all, which is not a
            // lighting failure and must not be reported as one.
            bool captureFailed = false;

            const auto stable = RunStableScene(
                *engine, 14, "lit_scene_static_frame_stability",
                [&](ZHLN::Engine& eng) -> bool {
                    captureFailed = false;
                    const RgbImage  repeatA    = Capture(eng, "headless_lighting_rt_static_r0.ppm");
                    const RgbImage  repeatB    = Capture(eng, "headless_lighting_rt_static_r1.ppm");
                    const FrameDiff repeatDiff = CompareFrames(repeatA, repeatB);

                    std::vector<double>    litSeries;
                    std::vector<double>    lumaSeries;
                    std::vector<double>    redSeries;
                    std::vector<double>    satSeries;
                    std::vector<FrameDiff> temporalDiffs;

                    RgbImage prev;
                    for (uint32_t f = 0; f < 4; ++f) {
                        TickFrames(eng, 1);
                        const RgbImage frame = Capture(eng, "headless_lighting_rt_static_f" + std::to_string(f) + ".ppm");

                        auto checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                        if (!checkFrame) {
                            captureFailed = true;
                            return false;
                        }

                        const FrameMetrics m = MeasureImage(frame);
                        litSeries.push_back(static_cast<double>(m.lit));
                        lumaSeries.push_back(m.meanLuma);
                        redSeries.push_back(static_cast<double>(m.red));
                        satSeries.push_back(static_cast<double>(m.saturated));

                        if (prev.Valid()) {
                            temporalDiffs.push_back(CompareFrames(prev, frame));
                        }
                        prev = frame;
                    }

                    ZHLN::Test::ExpectFalse(eng.GetVisibleEntities().empty());
                    ZHLN::Test::ExpectTrue(ZHLN::CullingStats::TotalTriangles > 0);

                    const bool frameProduced   = ZHLN::Test::ExpectTrue(Mean(lumaSeries) > 1.0);
                    const bool geometryVisible = ZHLN::Test::ExpectTrue(Mean(litSeries) > 500.0);
                    if (!frameProduced || !geometryVisible) {
                        captureFailed = !frameProduced;
                        return false;
                    }

                    const double litCV   = CoefficientOfVariation(litSeries);
                    const double lumaCV  = CoefficientOfVariation(lumaSeries);
                    const double redCV   = CoefficientOfVariation(redSeries);
                    const double satCV   = CoefficientOfVariation(satSeries);
                    const double litMean = Mean(litSeries);

                    double worstFrac32 = 0.0;
                    double maxJump     = 0.0;
                    for (size_t i = 0; i < temporalDiffs.size(); ++i) {
                        worstFrac32 = std::max(worstFrac32, temporalDiffs[i].frac32);
                        if (i + 1 < litSeries.size() && litMean > 1.0) {
                            const double jump = std::abs(litSeries[i + 1] - litSeries[i]) / litMean;
                            maxJump           = std::max(maxJump, jump);
                        }
                    }

                    const double meanSaturated = Mean(satSeries);
                    const bool   stableLit     = ZHLN::Test::ExpectTrue(litCV < 0.03);
                    const bool   stableLuma    = ZHLN::Test::ExpectTrue(lumaCV < 0.01);
                    const bool   stableRed     = ZHLN::Test::ExpectTrue(redCV < 0.05);
                    const bool   stableSat     = ZHLN::Test::ExpectTrue(satCV < 0.25 || meanSaturated < 100.0);
                    const bool   noPixelPop    = ZHLN::Test::ExpectTrue(worstFrac32 < 0.01);
                    const bool   noJump        = ZHLN::Test::ExpectTrue(maxJump < 0.08);
                    const bool   noBlowout     = ZHLN::Test::ExpectTrue(meanSaturated < 0.02 * static_cast<double>(640 * 480));

                    return stableLit && stableLuma && stableRed && stableSat && noPixelPop && noJump && noBlowout;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                if (captureFailed) {
                    return std::unexpected(LightingRTTestError::RenderOutputBlank);
                }
                return std::unexpected(LightingRTTestError::TemporalFlickerDetected);
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

        // ====================================================================
        // 2. Point Light Cluster Culling Sweep
        // ====================================================================
        std::expected<void, ZHLN::Error> point_light_cluster_culling_sweep() {
            auto engine      = CreateTestEngine(320, 240);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            ZHLN::Entity redLight = ZHLN::Entity::Null();
            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 2.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                auto diffuseMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.75f, 0.75f, 0.75f, 1.0f}}
                );
                auto checkDiffuse = ZHLN::Test::AssertTrue(diffuseMatRes.has_value());
                if (!checkDiffuse) {
                    return checkDiffuse;
                }

                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.5f, 0.5f, 0.52f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *diffuseMatRes}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.7, 5.0), .createPhysics = false, .materialOverride = *diffuseMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 30.0f, 20.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({30.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 40.0f,
                        .direction = JPH::Vec3(0.0f, 0.5f, 0.85f).Normalized()
                    }
                );

                redLight = reg.Create();
                reg.Add(
                    redLight, ZHLN::Components::TransformComponent {.position = JPH::Vec3(-6.4f, 2.4f, 5.5f)},
                    ZHLN::Components::LightComponent {
                        .type = ZHLN::LightType::Point, .color = JPH::Vec3(1.0f, 0.06f, 0.03f), .intensity = 1600.0f, .range = 40.0f
                    }
                );

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 2.6f, -4.0f);
                cam.yaw      = 90.0f;
                cam.pitch    = -8.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;
            // Set when the render could not be captured at all, which is not a
            // lighting failure and must not be reported as one.
            bool captureFailed = false;
            // Which kind of failure the scene actually saw. A light losing its
            // contribution is a cluster-culling bug; ordinary frame-to-frame
            // change is a stability bug. Collapsing both into the single bool
            // RunStableScene returns is what made this case report every
            // culling pop as flicker.
            bool lightCullingPop = false;

            const auto stable = RunStableScene(
                *engine, 8, "point_light_cluster_culling_sweep",
                [&](ZHLN::Engine& eng) -> bool {
                    captureFailed = false;
                    lightCullingPop = false;
                    auto& reg = eng.GetRegistry();
                    ZHLN::Test::ExpectTrue(reg.IsAlive(redLight));

                    std::vector<double>   redCounts;
                    std::vector<uint32_t> redPeaks;
                    uint32_t              isolatedDips = 0;

                    constexpr int   kSteps = 21;
                    constexpr float kStepX = 0.64f;
                    for (int step = 0; step < kSteps; ++step) {
                        const float x = -6.4f + static_cast<float>(step) * kStepX;
                        const float z = 5.5f - 0.078125f * x;

                        reg.Patch<ZHLN::Components::TransformComponent>(redLight, [&](auto& t) { t.position = JPH::Vec3(x, 2.4f, z); });

                        TickFrames(eng, 2);

                        const RgbImage frame      = Capture(eng, "headless_lighting_rt_cull_" + std::to_string(step) + ".ppm");
                        auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                        if (!checkFrame) {
                            captureFailed = true;
                            return false;
                        }

                        const FrameMetrics m = MeasureImage(frame);
                        redCounts.push_back(static_cast<double>(m.red));
                        redPeaks.push_back(m.redPeak);
                    }

                    for (size_t i = 1; i + 1 < redCounts.size(); ++i) {
                        const double left  = redCounts[i - 1];
                        const double right = redCounts[i + 1];
                        const double base  = std::min(left, right);
                        if (base > 64.0 && redCounts[i] < 0.5 * base) {
                            ++isolatedDips;
                        }
                    }

                    TickFrames(eng, 1);
                    constexpr uint32_t                  kStableFrames = 8;
                    std::array<RgbImage, kStableFrames> stableFrames {};
                    std::vector<double>                 stableCounts;
                    std::vector<double>                 stableLuma;
                    std::vector<double>                 stableLit;
                    std::vector<uint64_t>               stableFrameIdx;
                    for (uint32_t r = 0; r < kStableFrames; ++r) {
                        stableFrameIdx.push_back(eng.GetCurrentFrame());
                        stableFrames[r]  = Capture(eng, "headless_lighting_rt_cull_stable_" + std::to_string(r) + ".ppm");
                        auto checkStable = ZHLN::Test::AssertTrue(stableFrames[r].Valid());
                        if (!checkStable) {
                            captureFailed = true;
                            return false;
                        }
                        const FrameMetrics m = MeasureImage(stableFrames[r]);
                        stableCounts.push_back(static_cast<double>(m.red));
                        stableLuma.push_back(m.meanLuma);
                        stableLit.push_back(static_cast<double>(m.lit));

                        TickFrames(eng, 1);
                    }

                    double    worstPairFrac = 0.0;
                    size_t    worstPair     = 0;
                    FrameDiff worstDiffs[kStableFrames - 1] {};
                    for (uint32_t i = 0; i + 1 < kStableFrames; ++i) {
                        worstDiffs[i] = CompareFrames(stableFrames[i], stableFrames[i + 1]);
                        if (worstDiffs[i].frac32 > worstPairFrac) {
                            worstPairFrac = worstDiffs[i].frac32;
                            worstPair     = i;
                        }
                    }
                    const ChangedRegion region = DiffRegion(stableFrames[worstPair], stableFrames[worstPair + 1]);
                    WriteAmplifiedDiff("headless_lighting_rt_cull_parity_diff.ppm", stableFrames[worstPair], stableFrames[worstPair + 1]);
                    WriteRegionCrop("headless_lighting_rt_cull_region_a.ppm", stableFrames[worstPair], region);
                    WriteRegionCrop("headless_lighting_rt_cull_region_b.ppm", stableFrames[worstPair + 1], region);

                    const double   minRed  = *std::ranges::min_element(redCounts);
                    const uint32_t minPeak = *std::ranges::min_element(redPeaks);

                    const bool neverCulled      = ZHLN::Test::ExpectTrue(minRed > 16.0);
                    const bool brightEverywhere = ZHLN::Test::ExpectTrue(minPeak > 60u);
                    const bool noIsolatedCull   = ZHLN::Test::ExpectTrue(isolatedDips == 0u);
                    const bool noStaticStep     = ZHLN::Test::ExpectTrue(worstPairFrac < 0.005);

                    // The first three measure the light's own contribution and
                    // are what a cluster-culling pop breaks; noStaticStep is the
                    // frame-to-frame stability gate. Recording which group
                    // failed is what lets the caller name the right error.
                    lightCullingPop = !neverCulled || !brightEverywhere || !noIsolatedCull;

                    return neverCulled && brightEverywhere && noIsolatedCull && noStaticStep;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                if (captureFailed) {
                    return std::unexpected(LightingRTTestError::RenderOutputBlank);
                }
                return std::unexpected(lightCullingPop ? LightingRTTestError::LightCullingPopDetected
                                                       : LightingRTTestError::TemporalFlickerDetected);
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

        // ====================================================================
        // 3. Point Light Static Reference (No History)
        // ====================================================================
        std::expected<void, ZHLN::Error> point_light_static_reference_no_history() {
            auto engine      = CreateTestEngine(320, 240);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            const JPH::Vec3 lightPos(6.4f, 2.4f, 5.0f);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 2.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                auto diffuseMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.75f, 0.75f, 0.75f, 1.0f}}
                );
                auto checkDiffuse = ZHLN::Test::AssertTrue(diffuseMatRes.has_value());
                if (!checkDiffuse) {
                    return checkDiffuse;
                }
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.5f, 0.5f, 0.52f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *diffuseMatRes}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.7f, 0.7f, 0.7f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.7, 5.0), .createPhysics = false, .materialOverride = *diffuseMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 30.0f, 20.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({30.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 40.0f,
                        .direction = JPH::Vec3(0.0f, 0.5f, 0.85f).Normalized()
                    }
                );
                const ZHLN::Entity redLight = reg.Create();
                reg.Add(
                    redLight, ZHLN::Components::TransformComponent {.position = lightPos},
                    ZHLN::Components::LightComponent {
                        .type = ZHLN::LightType::Point, .color = JPH::Vec3(1.0f, 0.06f, 0.03f), .intensity = 1600.0f, .range = 40.0f
                    }
                );
                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 2.6f, -4.0f);
                cam.yaw      = 90.0f;
                cam.pitch    = -8.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;
            // Set when the render could not be captured at all, which is not a
            // lighting failure and must not be reported as one.
            bool captureFailed = false;

            const auto stable = RunStableScene(
                *engine, 8, "point_light_static_reference_no_history",
                [&](ZHLN::Engine& eng) -> bool {
                    captureFailed = false;
                    constexpr uint32_t                  kStableFrames = 8;
                    std::array<RgbImage, kStableFrames> frames {};
                    std::vector<double>                 counts;
                    for (uint32_t r = 0; r < kStableFrames; ++r) {
                        frames[r]       = Capture(eng, "headless_lighting_rt_ref_stable_" + std::to_string(r) + ".ppm");
                        auto checkFrame = ZHLN::Test::AssertTrue(frames[r].Valid());
                        if (!checkFrame) {
                            captureFailed = true;
                            return false;
                        }
                        counts.push_back(static_cast<double>(MeasureImage(frames[r]).red));

                        TickFrames(eng, 1);
                    }

                    double worstFrac = 0.0;
                    size_t worstPair = 0;
                    for (uint32_t i = 0; i + 1 < kStableFrames; ++i) {
                        const double f = CompareFrames(frames[i], frames[i + 1]).frac32;
                        if (f > worstFrac) {
                            worstFrac = f;
                            worstPair = i;
                        }
                    }

                    const ChangedRegion region = DiffRegion(frames[worstPair], frames[worstPair + 1]);
                    WriteAmplifiedDiff("headless_lighting_rt_ref_parity_diff.ppm", frames[worstPair], frames[worstPair + 1]);
                    WriteRegionCrop("headless_lighting_rt_ref_region_a.ppm", frames[worstPair], region);
                    WriteRegionCrop("headless_lighting_rt_ref_region_b.ppm", frames[worstPair + 1], region);

                    const bool noStaticStep = ZHLN::Test::ExpectTrue(worstFrac < 0.005);
                    const bool lightVisible = ZHLN::Test::ExpectTrue(Mean(counts) > 16.0);

                    return noStaticStep && lightVisible;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                if (captureFailed) {
                    return std::unexpected(LightingRTTestError::RenderOutputBlank);
                }
                return std::unexpected(LightingRTTestError::TemporalFlickerDetected);
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

        // ====================================================================
        // 6. Multi-Light Clustered Accumulation & Chromatic Interaction
        // ====================================================================
        std::expected<void, ZHLN::Error> multi_light_cluster_accumulation_and_chromatic_interaction() {
            auto engine      = CreateTestEngine(640, 480);
            auto checkEngine = ZHLN::Test::AssertTrue(engine != nullptr);
            if (!checkEngine) {
                return std::unexpected(LightingRTTestError::EngineInitFailed);
            }

            DisableTAA(*engine);

            {
                auto& reg = engine->GetRegistry();
                auto& rc  = engine->GetRenderContext();

                const auto settingsEnts = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                if (!settingsEnts.empty()) {
                    reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settingsEnts[0], [](auto& pp) {
                        pp.fullBright      = 0;
                        pp.ambientExposure = 1.0f;
                        pp.enableSSR       = 0;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                // Explicit 0-intensity Sun to suppress the default 180-nit white sun injection
                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 40.0f, 30.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({45.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 0.0f,
                        .direction = JPH::Vec3(0.0f, 0.6f, 0.8f).Normalized()
                    }
                );

                // Neutral diffuse gray floor
                auto neutralMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.85f, .baseColor = {0.8f, 0.8f, 0.8f, 1.0f}}
                );
                auto checkNeutral = ZHLN::Test::AssertTrue(neutralMat.has_value());
                if (!checkNeutral) {
                    return checkNeutral;
                }

                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 80.0f, {0.8f, 0.8f, 0.8f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = false, .materialOverride = *neutralMat}
                );

                // Central pedestal at quadrant boundary to test additive color mixing
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(1.0f, 0.6f, 1.0f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.6, 0.0), .createPhysics = false, .materialOverride = *neutralMat}
                );

                // 64 Point Lights partitioned into 4 chromatic quadrants (perspective-aligned for yaw=+90 look along +Z):
                //   World X > 0, Z > 0 -> Screen Top-Left:     Pure Red   (1.0, 0.02, 0.02)
                //   World X <= 0, Z > 0 -> Screen Top-Right:   Pure Green (0.02, 1.0, 0.02)
                //   World X > 0, Z < 0 -> Screen Bottom-Left:  Pure Blue  (0.02, 0.02, 1.0)
                //   World X <= 0, Z < 0 -> Screen Bottom-Right: Amber     (1.0, 0.85, 0.15)
                constexpr int kGridDim = 8;
                for (int gx = 0; gx < kGridDim; ++gx) {
                    for (int gz = 0; gz < kGridDim; ++gz) {
                        float posX = -14.0f + static_cast<float>(gx) * 4.0f;
                        float posZ = -14.0f + static_cast<float>(gz) * 4.0f;
                        float posY = 2.0f;

                        JPH::Vec3 lightColor(1.0f, 1.0f, 1.0f);
                        if (posX > 0.0f && posZ >= 0.0f) {
                            lightColor = JPH::Vec3(1.0f, 0.02f, 0.02f); // Red -> Screen Top-Left
                        } else if (posX <= 0.0f && posZ >= 0.0f) {
                            lightColor = JPH::Vec3(0.02f, 1.0f, 0.02f); // Green -> Screen Top-Right
                        } else if (posX > 0.0f && posZ < 0.0f) {
                            lightColor = JPH::Vec3(0.02f, 0.02f, 1.0f); // Blue -> Screen Bottom-Left
                        } else {
                            lightColor = JPH::Vec3(1.0f, 0.85f, 0.15f); // Amber -> Screen Bottom-Right
                        }

                        const ZHLN::Entity lt = reg.Create();
                        reg.Add(
                            lt, ZHLN::Components::TransformComponent {.position = JPH::Vec3(posX, posY, posZ)},
                            ZHLN::Components::LightComponent {
                                .type      = ZHLN::LightType::Point,
                                .color     = lightColor,
                                .intensity = 250.0f,
                                .range     = 14.0f,
                            }
                        );
                    }
                }

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 22.0f, -24.0f);
                cam.yaw      = 90.0f;
                cam.pitch    = -42.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;
            // Set when the render could not be captured at all, which is not a
            // lighting failure and must not be reported as one.
            bool captureFailed = false;

            const auto stable = RunStableScene(
                *engine, 8, "multi_light_cluster_accumulation_and_chromatic_interaction",
                [&](ZHLN::Engine& eng) -> bool {
                    captureFailed = false;
                    const RgbImage frame      = Capture(eng, "headless_lighting_multi_cluster.ppm");
                    auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                    if (!checkFrame) {
                        captureFailed = true;
                        return false;
                    }

                    // Normalized Quadrant Sampling:
                    const auto quadTL    = MeasureSubRegion(frame, {.x0 = 0.05, .y0 = 0.05, .x1 = 0.40, .y1 = 0.45});
                    const auto quadTR    = MeasureSubRegion(frame, {.x0 = 0.60, .y0 = 0.05, .x1 = 0.95, .y1 = 0.45});
                    const auto quadBL    = MeasureSubRegion(frame, {.x0 = 0.05, .y0 = 0.55, .x1 = 0.40, .y1 = 0.95});
                    const auto centerMix = MeasureSubRegion(frame, {.x0 = 0.45, .y0 = 0.35, .x1 = 0.55, .y1 = 0.65});

                    ZHLN::Println("    [INFO] Multi-light 64-Light Clustered Grid:");
                    ZHLN::Println(
                        "      Top-Left Quad (Red):    MeanRGB=({:.1f},{:.1f},{:.1f}), DominantRed={}/{}", quadTL.meanR, quadTL.meanG, quadTL.meanB,
                        quadTL.dominantRed, quadTL.pixels
                    );
                    ZHLN::Println(
                        "      Top-Right Quad (Green): MeanRGB=({:.1f},{:.1f},{:.1f}), DominantGreen={}/{}", quadTR.meanR, quadTR.meanG, quadTR.meanB,
                        quadTR.dominantGrn, quadTR.pixels
                    );
                    ZHLN::Println(
                        "      Bottom-Left Quad (Blue):MeanRGB=({:.1f},{:.1f},{:.1f}), DominantBlue={}/{}", quadBL.meanR, quadBL.meanG, quadBL.meanB,
                        quadBL.dominantBlu, quadBL.pixels
                    );
                    ZHLN::Println(
                        "      Center Mixing (R+G->Y): MeanRGB=({:.1f},{:.1f},{:.1f}), YellowMixPixels={}/{}", centerMix.meanR, centerMix.meanG,
                        centerMix.meanB, centerMix.yellowMix, centerMix.pixels
                    );

                    // Every gate below is a ratio -- channel against channel, or
                    // saturated pixels as a share of their region -- so the scene can be
                    // re-exposed without the assertions moving. Absolute means are
                    // exposure/tone-map outputs, not lighting behaviour.

                    // 1. Quadrant Chromatic Purity
                    const bool redDominant = ZHLN_CHECK(
                        quadTL.meanR > 1.3 * quadTL.meanG && quadTL.meanR > 1.3 * quadTL.meanB && quadTL.dominantRed * 100 > quadTL.pixels,
                        "top-left quadrant is red-dominant",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantRed={}/{} px (need >1% of the quad)", quadTL.meanR, quadTL.meanG, quadTL.meanB,
                        quadTL.dominantRed, quadTL.pixels
                    );
                    const bool greenDominant = ZHLN_CHECK(
                        quadTR.meanG > 1.3 * quadTR.meanR && quadTR.meanG > 1.3 * quadTR.meanB && quadTR.dominantGrn * 100 > quadTR.pixels,
                        "top-right quadrant is green-dominant",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantGreen={}/{} px (need >1% of the quad)", quadTR.meanR, quadTR.meanG, quadTR.meanB,
                        quadTR.dominantGrn, quadTR.pixels
                    );
                    const bool blueDominant = ZHLN_CHECK(
                        quadBL.meanB > 1.3 * quadBL.meanR && quadBL.meanB > 1.3 * quadBL.meanG && quadBL.dominantBlu * 100 > quadBL.pixels,
                        "bottom-left quadrant is blue-dominant",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantBlue={}/{} px (need >1% of the quad)", quadBL.meanR, quadBL.meanG, quadBL.meanB,
                        quadBL.dominantBlu, quadBL.pixels
                    );

                    // 2. Additive Color Superposition at boundary (Red + Green -> Yellow).
                    // The pedestal sits under all four quadrants, so it must out-shine
                    // each pure quadrant in its own channel -- lights accumulating rather
                    // than the nearest one winning.
                    const bool additiveMixing = ZHLN_CHECK(
                        centerMix.meanR > 1.3 * centerMix.meanB && centerMix.meanG > 1.3 * centerMix.meanB && centerMix.meanR > 0.6 * centerMix.meanG &&
                            centerMix.meanG > 0.6 * centerMix.meanR && centerMix.meanR > 0.5 * quadTL.meanR && centerMix.meanG > 0.5 * quadTR.meanG &&
                            centerMix.yellowMix * 100 > centerMix.pixels,
                        "quadrant boundary mixes red + green into yellow",
                        "centerMeanRGB=({:.1f},{:.1f},{:.1f}) vs redQuad.meanR={:.1f} greenQuad.meanG={:.1f}, yellowMix={}/{} px (need >1%)", centerMix.meanR,
                        centerMix.meanG, centerMix.meanB, quadTL.meanR, quadTR.meanG, centerMix.yellowMix, centerMix.pixels
                    );

                    // 3. Coverage & headroom. "lit" counts Luma > 40, which a pure blue
                    // pixel can never reach (0.0722 * 255 = 18.4), so in a scene that is
                    // a quarter blue by construction that metric grades the palette
                    // instead of the lighting. Count saturated chroma instead: it is
                    // hue-aware and, as a share of the sampled area, exposure-relative.
                    const uint32_t chromaticPixels = quadTL.dominantRed + quadTR.dominantGrn + quadBL.dominantBlu + centerMix.yellowMix;
                    const uint32_t sampledPixels   = quadTL.pixels + quadTR.pixels + quadBL.pixels + centerMix.pixels;
                    const bool     lightCovered    = ZHLN_CHECK(
                        chromaticPixels * 10 > sampledPixels, "clustered lights cover a meaningful share of the frame",
                        "chromaticPixels={}/{} sampled px (need >10%)", chromaticPixels, sampledPixels
                    );

                    const FrameMetrics fullFrame         = MeasureImage(frame);
                    const bool         noBlackout        = ZHLN_CHECK(
                        fullFrame.meanLuma > 1.0, "frame is not blacked out", "meanLuma={:.2f} over {} px", fullFrame.meanLuma, fullFrame.total
                    );
                    const bool         noExtremeOverflow = ZHLN_CHECK(
                        fullFrame.saturated * 20 < fullFrame.total, "frame is not blown out", "saturated={}/{} px (need <5%)", fullFrame.saturated,
                        fullFrame.total
                    );

                    return redDominant && greenDominant && blueDominant && additiveMixing && lightCovered && noBlackout && noExtremeOverflow;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                if (captureFailed) {
                    return std::unexpected(LightingRTTestError::RenderOutputBlank);
                }
                return std::unexpected(LightingRTTestError::MultiLightClusteringFailed);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            ZHLN::Println("    [PASS] 64 Clustered lights correctly accumulated with clean chromatic superposition.");
            return {};
        }
    };
};

// Exported for the GPU_Lighting group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunClusteredLightingSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<ClusteredLightingTestSuite>();
}
