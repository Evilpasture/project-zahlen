// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// tests/render/TestReflections.cpp
//
// Reflections: ray-traced reflection coverage and artifacts, multi-emissive
// sources on a mirrored surface, and dense multi-light interaction with
// emissive materials.
//
// Split out of TestLightingRayTraced.cpp; the shared error enum, frame I/O
// and engine fixture live in LightingRTCommon.hpp.

#include "LightingRTCommon.hpp"

// ============================================================================
// Test Suite
// ============================================================================

struct ReflectionsTestSuite {
    ReflectionsTestSuite() {
        ZHLN::Fiber::InitMainThread();
        ZHLN::TaskSystem::Init(2, 32, ZHLN::kMinimumFiberStackSize);
    }

    ~ReflectionsTestSuite() {
        ZHLN::TaskSystem::Shutdown();
    }

    struct Tests {
        // ====================================================================
        // 5. Ray-Traced Reflection Coverage & Artifacts
        // ====================================================================
        std::expected<void, ZHLN::Error> raytraced_reflection_coverage_and_artifacts() {
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
                        pp.ambientExposure = 6.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                    });
                }

                auto mirrorMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.03f, .baseColor = {0.85f, 0.85f, 0.88f, 1.0f}}
                );
                auto checkMirror = ZHLN::Test::AssertTrue(mirrorMatRes.has_value());
                if (!checkMirror) {
                    return checkMirror;
                }
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.85f, 0.85f, 0.88f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *mirrorMatRes}
                );

                auto emissiveMatRes = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.55f, .baseColor = {1.0f, 0.06f, 0.04f, 1.0f}, .emissive = {1.0f, 0.0f, 0.0f, 1.0f}
                        }
                );
                auto checkEmissive = ZHLN::Test::AssertTrue(emissiveMatRes.has_value());
                if (!checkEmissive) {
                    return checkEmissive;
                }
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 3.0, 0.0), .createPhysics = false, .materialOverride = *emissiveMatRes}
                );

                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 50.0f, 40.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({40.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 140.0f,
                        .direction = JPH::Vec3(0.0f, 0.75f, 0.66f).Normalized()
                    }
                );

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 5.0f, 14.0f);
                cam.yaw      = -90.0f;
                cam.pitch    = -22.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;
            // Set when the render could not be captured at all, which is not a
            // lighting failure and must not be reported as one.
            bool captureFailed = false;
            // Which kind of failure the scene actually saw. "No reflection at
            // all" means RTR/SSR silently fell back to IBL, which is a
            // different bug from a reflection that is present but blown out or
            // speckled -- and needs different things looked at.
            bool reflectionMissing = false;

            const auto stable = RunStableScene(
                *engine, 10, "raytraced_reflection_coverage_and_artifacts",
                [&](ZHLN::Engine& eng) -> bool {
                    captureFailed = false;
                    reflectionMissing = false;
                    std::vector<double> reflectionSeries;
                    std::vector<double> saturationSeries;
                    std::vector<double> isolatedSeries;
                    for (uint32_t f = 0; f < 4; ++f) {
                        TickFrames(eng, 1);
                        const RgbImage frame      = Capture(eng, "headless_lighting_rt_reflect_f" + std::to_string(f) + ".ppm");
                        auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                        if (!checkFrame) {
                            captureFailed = true;
                            return false;
                        }

                        const FrameMetrics m = MeasureImage(frame, 0.5);
                        reflectionSeries.push_back(static_cast<double>(m.red));
                        saturationSeries.push_back(static_cast<double>(m.saturated));
                        isolatedSeries.push_back(m.red > 0 ? static_cast<double>(m.redIsolated) / static_cast<double>(m.red) : 0.0);
                    }

                    const double   meanReflection  = Mean(reflectionSeries);
                    const double   reflectionCV    = CoefficientOfVariation(reflectionSeries);
                    const double   saturationCV    = CoefficientOfVariation(saturationSeries);
                    const double   meanSaturation  = Mean(saturationSeries);
                    const double   isolatedRatio   = Mean(isolatedSeries);
                    const uint32_t lowerHalfPixels = static_cast<uint32_t>(640 * 480 / 2);

                    const bool reflectionPresent = ZHLN::Test::ExpectTrue(meanReflection > 24.0);
                    const bool reflectionStable  = ZHLN::Test::ExpectTrue(reflectionCV < 0.15);
                    const bool noBlowout         = ZHLN::Test::ExpectTrue(meanSaturation < 0.04 * static_cast<double>(lowerHalfPixels));
                    const bool noRayDebris       = ZHLN::Test::ExpectTrue(isolatedRatio < 0.35);
                    const bool saturationStable  = ZHLN::Test::ExpectTrue(saturationCV < 0.25 || meanSaturation < 100.0);

                    // reflectionPresent is the coverage gate: when it fails the
                    // polished surface shows nothing at all, which is the
                    // RTR/SSR -> IBL fallback, not an artifact in a reflection
                    // that does exist.
                    reflectionMissing = !reflectionPresent;

                    return reflectionPresent && reflectionStable && noBlowout && noRayDebris && saturationStable;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                if (captureFailed) {
                    return std::unexpected(LightingRTTestError::RenderOutputBlank);
                }
                return std::unexpected(reflectionMissing ? LightingRTTestError::ReflectionMissing
                                                         : LightingRTTestError::ReflectionArtifacts);
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
        // 7. Multi-Emissive Sources & Surface Reflection Interaction
        // ====================================================================
        std::expected<void, ZHLN::Error> multi_emissive_sources_and_surface_reflection_interaction() {
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
                        pp.ambientExposure = 6.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                // Sun illumination to support scene depth and HDR tone mapping
                const ZHLN::Entity sunEnt = reg.Create();
                reg.Add(
                    sunEnt,
                    ZHLN::Components::TransformComponent {
                        .position = JPH::Vec3(0.0f, 50.0f, 40.0f), .rotation = ZHLN::Math::EulerDegreesToQuat({40.0f, 0.0f, 0.0f})
                    },
                    ZHLN::Components::LightComponent {
                        .type      = ZHLN::LightType::Sun,
                        .color     = JPH::Vec3(1.0f, 1.0f, 1.0f),
                        .intensity = 100.0f,
                        .direction = JPH::Vec3(0.0f, 0.75f, 0.66f).Normalized()
                    }
                );

                // Polished metallic mirror floor
                auto mirrorMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.02f, .baseColor = {0.9f, 0.9f, 0.95f, 1.0f}}
                );
                auto checkMirror = ZHLN::Test::AssertTrue(mirrorMat.has_value());
                if (!checkMirror) {
                    return checkMirror;
                }

                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 120.0f, {0.9f, 0.9f, 0.95f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = false, .materialOverride = *mirrorMat}
                );

                // 4 Distinct High-Luminance Emissive Geometric Emitters at Y = 3.0, Z = 0.0:
                //   Emitter 1: X = -4.5 (Pure Red)
                //   Emitter 2: X = -1.5 (Pure Green)
                //   Emitter 3: X = +1.5 (Pure Blue)
                //   Emitter 4: X = +4.5 (Golden Yellow)
                auto matEmissiveRed = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.5f, .baseColor = {1.0f, 0.05f, 0.05f, 1.0f}, .emissive = {6.0f, 0.0f, 0.0f, 1.0f}
                        }
                );
                auto matEmissiveGrn = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.5f, .baseColor = {0.05f, 1.0f, 0.05f, 1.0f}, .emissive = {0.0f, 6.0f, 0.0f, 1.0f}
                        }
                );
                auto matEmissiveBlu = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.5f, .baseColor = {0.05f, 0.05f, 1.0f, 1.0f}, .emissive = {0.0f, 0.0f, 6.0f, 1.0f}
                        }
                );
                auto matEmissiveYel = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.5f, .baseColor = {1.0f, 0.9f, 0.05f, 1.0f}, .emissive = {5.0f, 4.5f, 0.0f, 1.0f}
                        }
                );

                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(-4.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveRed
                    }
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {
                        .position = JPH::RVec3(-1.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveGrn
                    }
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(1.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveBlu}
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(4.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveYel}
                );

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 5.0f, 14.0f);
                cam.yaw      = -90.0f;
                cam.pitch    = -22.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;
            // Set when the render could not be captured at all, which is not a
            // lighting failure and must not be reported as one.
            bool captureFailed = false;

            const auto stable = RunStableScene(
                *engine, 8, "multi_emissive_sources_and_surface_reflection_interaction",
                [&](ZHLN::Engine& eng) -> bool {
                    captureFailed = false;
                    const RgbImage frame      = Capture(eng, "headless_lighting_multi_emissive.ppm");
                    auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                    if (!checkFrame) {
                        captureFailed = true;
                        return false;
                    }

                    // Slicing lower reflection half into 4 horizontal column bands centered on planar reflection centroids:
                    //   Strip 1 (Red Refl Centroid   ~0.30): X in [0.20, 0.36], Y in [0.52, 0.95]
                    //   Strip 2 (Green Refl Centroid ~0.43): X in [0.38, 0.48], Y in [0.52, 0.95]
                    //   Strip 3 (Blue Refl Centroid  ~0.57): X in [0.52, 0.62], Y in [0.52, 0.95]
                    //   Strip 4 (Yellow Refl Centroid~0.70): X in [0.64, 0.80], Y in [0.52, 0.95]
                    const auto reflStripRed = MeasureSubRegion(frame, {.x0 = 0.20, .y0 = 0.52, .x1 = 0.36, .y1 = 0.95});
                    const auto reflStripGrn = MeasureSubRegion(frame, {.x0 = 0.38, .y0 = 0.52, .x1 = 0.48, .y1 = 0.95});
                    const auto reflStripBlu = MeasureSubRegion(frame, {.x0 = 0.52, .y0 = 0.52, .x1 = 0.62, .y1 = 0.95});
                    const auto reflStripYel = MeasureSubRegion(frame, {.x0 = 0.64, .y0 = 0.52, .x1 = 0.80, .y1 = 0.95});

                    ZHLN::Println("    [INFO] Multi-Emissive Planar Mirror Reflection Slices:");
                    ZHLN::Println(
                        "      Strip 1 (Refl Red):    MeanRGB=({:.1f},{:.1f},{:.1f}), DominantRed={}", reflStripRed.meanR, reflStripRed.meanG,
                        reflStripRed.meanB, reflStripRed.dominantRed
                    );
                    ZHLN::Println(
                        "      Strip 2 (Refl Green):  MeanRGB=({:.1f},{:.1f},{:.1f}), DominantGreen={}", reflStripGrn.meanR, reflStripGrn.meanG,
                        reflStripGrn.meanB, reflStripGrn.dominantGrn
                    );
                    ZHLN::Println(
                        "      Strip 3 (Refl Blue):   MeanRGB=({:.1f},{:.1f},{:.1f}), DominantBlue={}", reflStripBlu.meanR, reflStripBlu.meanG,
                        reflStripBlu.meanB, reflStripBlu.dominantBlu
                    );
                    ZHLN::Println(
                        "      Strip 4 (Refl Yellow): MeanRGB=({:.1f},{:.1f},{:.1f}), YellowMixPixels={}", reflStripYel.meanR, reflStripYel.meanG,
                        reflStripYel.meanB, reflStripYel.yellowMix
                    );

                    // 1. Spatial mirror correspondence. Each gate is a ratio: channel
                    // against channel inside the strip, or bright pixels as a share of
                    // the strip. Absolute means are exposure outputs -- the same correct
                    // reflection measures 5.9 or 59.0 depending on ambientExposure -- and
                    // the strips are not even the same width, so a shared absolute floor
                    // grades geometry rather than the reflection.

                    const bool reflRedOk = ZHLN_CHECK(
                        reflStripRed.dominantRed * 200 > reflStripRed.pixels && reflStripRed.meanR > 1.3 * reflStripRed.meanG &&
                            reflStripRed.meanR > 1.3 * reflStripRed.meanB,
                        "strip 1 mirrors the red emitter",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantRed={}/{} px (need >0.5% of the strip)", reflStripRed.meanR, reflStripRed.meanG,
                        reflStripRed.meanB, reflStripRed.dominantRed, reflStripRed.pixels
                    );
                    const bool reflGrnOk = ZHLN_CHECK(
                        reflStripGrn.dominantGrn * 200 > reflStripGrn.pixels && reflStripGrn.meanG > 1.3 * reflStripGrn.meanR &&
                            reflStripGrn.meanG > 1.3 * reflStripGrn.meanB,
                        "strip 2 mirrors the green emitter",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantGreen={}/{} px (need >0.5% of the strip)", reflStripGrn.meanR, reflStripGrn.meanG,
                        reflStripGrn.meanB, reflStripGrn.dominantGrn, reflStripGrn.pixels
                    );
                    const bool reflBluOk = ZHLN_CHECK(
                        reflStripBlu.dominantBlu * 200 > reflStripBlu.pixels && reflStripBlu.meanB > 1.3 * reflStripBlu.meanR &&
                            reflStripBlu.meanB > 1.3 * reflStripBlu.meanG,
                        "strip 3 mirrors the blue emitter",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), dominantBlue={}/{} px (need >0.5% of the strip)", reflStripBlu.meanR, reflStripBlu.meanG,
                        reflStripBlu.meanB, reflStripBlu.dominantBlu, reflStripBlu.pixels
                    );
                    // Yellow has no single dominant channel to lean on, so its signature is
                    // the R+G mix count plus R and G clearing B by the same 1.3x the pure
                    // strips use and staying within 0.6x of each other (yellow, not amber).
                    const bool reflYelOk = ZHLN_CHECK(
                        reflStripYel.yellowMix * 200 > reflStripYel.pixels && reflStripYel.meanR > 1.3 * reflStripYel.meanB &&
                            reflStripYel.meanG > 1.3 * reflStripYel.meanB && reflStripYel.meanR > 0.6 * reflStripYel.meanG &&
                            reflStripYel.meanG > 0.6 * reflStripYel.meanR,
                        "strip 4 mirrors the yellow emitter",
                        "meanRGB=({:.1f},{:.1f},{:.1f}), yellowMix={}/{} px (need >0.5% of the strip)", reflStripYel.meanR, reflStripYel.meanG,
                        reflStripYel.meanB, reflStripYel.yellowMix, reflStripYel.pixels
                    );

                    // 2. Validate Upper Direct Emission visibility. The peak-luma guard
                    // stays absolute: it only asserts the emitters are directly visible
                    // somewhere in the upper frame, not that the scene is bright.
                    const auto     upperDirect      = MeasureSubRegion(frame, {.x0 = 0.0, .y0 = 0.05, .x1 = 1.0, .y1 = 0.45});
                    const uint32_t directChroma     = upperDirect.dominantRed + upperDirect.dominantGrn + upperDirect.dominantBlu + upperDirect.yellowMix;
                    const bool     directVisible    = ZHLN_CHECK(
                        upperDirect.maxLuma > 60.0 && directChroma * 1000 > upperDirect.pixels, "emitters are directly visible in the upper frame",
                        "maxLuma={:.1f} (need >60), chromaticPixels={}/{} px (need >0.1%)", upperDirect.maxLuma, directChroma, upperDirect.pixels
                    );

                    return reflRedOk && reflGrnOk && reflBluOk && reflYelOk && directVisible;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                if (captureFailed) {
                    return std::unexpected(LightingRTTestError::RenderOutputBlank);
                }
                return std::unexpected(LightingRTTestError::MultiEmissiveReflectionFailed);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            ZHLN::Println("    [PASS] Multi-emissive objects correctly mapped to their respective mirror reflections.");
            return {};
        }

        // ====================================================================
        // 8. Dense Multi-Light & Emissive Materials Cross-Interaction
        // ====================================================================
        std::expected<void, ZHLN::Error> dense_multi_light_emissive_materials_cross_interaction() {
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
                        pp.ambientExposure = 6.0f;
                        pp.enableSSR       = 1;
                        pp.enableRTR       = 0;
                        pp.giMode          = 0;
                    });
                }

                auto floorMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.5f, .roughness = 0.25f, .baseColor = {0.6f, 0.6f, 0.65f, 1.0f}}
                );
                ZHLN::CreativeWorksFactory::CreatePlane(
                    *engine, 80.0f, {0.6f, 0.6f, 0.65f, 1.0f},
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = false, .materialOverride = *floorMat}
                );

                auto monolithMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {
                            .metallic = 0.0f, .roughness = 0.2f, .baseColor = {0.0f, 1.0f, 1.0f, 1.0f}, .emissive = {0.0f, 20.0f, 20.0f, 1.0f}
                        }
                );
                ZHLN::CreativeWorksFactory::CreateBox(
                    *engine, JPH::Vec3(0.8f, 2.5f, 0.8f),
                    ZHLN::CreativeWorksFactory::SpawnParams {.position = JPH::RVec3(0.0, 2.5, 0.0), .createPhysics = false, .materialOverride = *monolithMat}
                );

                auto goldMat = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 1.0f, .roughness = 0.15f, .baseColor = {1.0f, 0.76f, 0.14f, 1.0f}}
                );
                auto roughPlastic = ZHLN::CreativeWorksFactory::CreateMaterial(
                    rc, ZHLN::CreativeWorksFactory::MaterialDesc {.metallic = 0.0f, .roughness = 0.8f, .baseColor = {0.8f, 0.2f, 0.2f, 1.0f}}
                );

                for (int x = -2; x <= 2; ++x) {
                    for (int z = -2; z <= 2; ++z) {
                        if (x == 0 && z == 0)
                            continue;
                        float px = static_cast<float>(x) * 3.5f;
                        float pz = static_cast<float>(z) * 3.5f;
                        ZHLN::CreativeWorksFactory::CreateBox(
                            *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                            ZHLN::CreativeWorksFactory::SpawnParams {
                                .position = JPH::RVec3(px, 0.5, pz), .createPhysics = false, .materialOverride = ((x + z) % 2 == 0) ? *goldMat : *roughPlastic
                            }
                        );
                    }
                }

                constexpr size_t kLightCount = 32;
                for (size_t i = 0; i < kLightCount; ++i) {
                    float angle  = (static_cast<float>(i) / static_cast<float>(kLightCount)) * 6.283185f;
                    float radius = 5.0f + (static_cast<float>(i % 3) * 2.0f);
                    float lx     = std::sin(angle) * radius;
                    float lz     = std::cos(angle) * radius;
                    float ly     = 1.0f + static_cast<float>(i % 4) * 0.8f;

                    JPH::Vec3 lightCol(std::sin(angle) * 0.5f + 0.5f, std::cos(angle * 0.5f) * 0.5f + 0.5f, std::sin(angle * 1.5f + 1.0f) * 0.5f + 0.5f);

                    const ZHLN::Entity lt = reg.Create();
                    reg.Add(
                        lt, ZHLN::Components::TransformComponent {.position = JPH::Vec3(lx, ly, lz)},
                        ZHLN::Components::LightComponent {
                            .type      = ZHLN::LightType::Point,
                            .color     = lightCol,
                            .intensity = 220.0f,
                            .range     = 12.0f,
                        }
                    );
                }

                auto& cam    = engine->GetCamera();
                cam.position = JPH::Vec3(0.0f, 8.0f, -18.0f);
                cam.yaw      = 90.0f;
                cam.pitch    = -22.0f;
                cam.fov      = 60.0f;
            }

            uint32_t validationRaised = 0;
            // Set when the render could not be captured at all, which is not a
            // lighting failure and must not be reported as one.
            bool captureFailed = false;

            const auto stable = RunStableScene(
                *engine, 10, "dense_multi_light_emissive_materials_cross_interaction",
                [&](ZHLN::Engine& eng) -> bool {
                    captureFailed = false;
                    const RgbImage frame      = Capture(eng, "headless_lighting_dense_interaction.ppm");
                    auto           checkFrame = ZHLN::Test::AssertTrue(frame.Valid());
                    if (!checkFrame) {
                        captureFailed = true;
                        return false;
                    }

                    const FrameMetrics m = MeasureImage(frame);

                    ZHLN::Println("    [INFO] Dense 32-Light + Emissive Monolith Scene Metrics:");
                    ZHLN::Println("      Lit Pixels: {}, Dark Pixels: {}, Saturated Pixels: {}, Mean Luma: {:.2f}", m.lit, m.dark, m.saturated, m.meanLuma);
                    ZHLN::Println(
                        "      Cyan (Monolith Reflection) Pixels: {}, Red Pixels: {}, Green Pixels: {}, Blue Pixels: {}", m.cyan, m.red, m.green, m.blue
                    );

                    const bool wellLit          = ZHLN::Test::ExpectTrue(m.lit > (m.total * 0.35));
                    const bool limitedBlowout   = ZHLN::Test::ExpectTrue(m.saturated < (m.total * 0.05));
                    const bool cyanObserved     = ZHLN::Test::ExpectTrue(m.cyan > 200u);
                    const bool multiColorActive = ZHLN::Test::ExpectTrue(m.red > 200u && m.green > 200u && m.blue > 200u);

                    return wellLit && limitedBlowout && cyanObserved && multiColorActive;
                },
                &validationRaised
            );

            if (stable == StableRunResult::AssertionsFailed) {
                if (captureFailed) {
                    return std::unexpected(LightingRTTestError::RenderOutputBlank);
                }
                return std::unexpected(LightingRTTestError::DenseCrossInteractionFailed);
            }
            if (stable != StableRunResult::Ok) {
                return std::unexpected(LightingRTTestError::DeviceLostDuringTest);
            }

            ZHLN::Test::ExpectEq(validationRaised, 0u);
            if (validationRaised != 0) {
                return std::unexpected(LightingRTTestError::ValidationErrorsRaised);
            }

            ZHLN::Println("    [PASS] 32 Dynamic point lights and emissive monolith cross-interaction verified.");
            return {};
        }
    };
};

// Exported for the GPU_Lighting group binary, which aggregates every suite in
// this domain through Runner::RunDeferred.
auto RunReflectionsSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<ReflectionsTestSuite>();
}
