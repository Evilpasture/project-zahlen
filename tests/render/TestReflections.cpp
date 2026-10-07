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
        // Nested in the group binary's session: the task system and the pooled
        // engine outlive this suite (see HeadlessEngineFixture.hpp).
        ZHLN::Test::Headless::BeginSession();
    }

    ~ReflectionsTestSuite() {
        ZHLN::Test::Headless::EndSession();
    }

    struct Tests {
        // ====================================================================
        // 5. Ray-Traced Reflection Coverage & Artifacts
        // ====================================================================
        std::expected<void, ZHLN::ErrorCode> raytraced_reflection_coverage_and_artifacts() {
            auto engine      = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
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

                auto mirrorMatRes = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 1.0f, .roughness = 0.03f, .baseColor = {0.85f, 0.85f, 0.88f, 1.0f}});
                if (!ZHLN::Test::ExpectTrue(mirrorMatRes.has_value())) {
                    return std::unexpected(LightingRTTestError::MaterialCreationFailed);
                }
                ZHLN::PrefabFactory::CreatePlane(
                    *engine, 120.0f, {0.85f, 0.85f, 0.88f, 1.0f},
                    ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 0.0, 0.0), .createPhysics = false, .materialOverride = *mirrorMatRes}
                );

                auto emissiveMatRes = rc.CreateMaterial(
                    ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.55f, .baseColor = {1.0f, 0.06f, 0.04f, 1.0f}, .emissive = {1.0f, 0.0f, 0.0f, 1.0f}}
                );
                if (!ZHLN::Test::ExpectTrue(emissiveMatRes.has_value())) {
                    return std::unexpected(LightingRTTestError::MaterialCreationFailed);
                }
                ZHLN::PrefabFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 3.0, 0.0), .createPhysics = false, .materialOverride = *emissiveMatRes}
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

                const auto camComp = engine->GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
                if (!ZHLN::Test::ExpectTrue(camComp.has_value())) {
                    return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
                }
                ZHLN::Camera& cam = camComp->camera;
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

            // NOTE: this scene runs SSR-only (enableRTR = 0 above), so the A/B
            // flips SSR, not RTR -- the test name is historical. The old gate
            // counted red pixels in the lower half, but the scene's own sun
            // (140) plus ambient exposure wash the 1x emitter below the red
            // threshold even in direct view, so a perfect reflection still
            // read 0. Flipping the switch detects ANY reflection -- red,
            // white, or otherwise. (Artifact shape -- debris, speckle -- is
            // owned by the RayTracedReflectionNoise suite, which measures it
            // against the noise floor instead of absolute counts.)
            const auto stable = RunStableScene(
                *engine, 10, "raytraced_reflection_coverage_and_artifacts",
                [&](ZHLN::Engine& eng) -> bool {
                    captureFailed = false;
                    reflectionMissing = false;

                    // The emitter's mirror image lands mid-frame below the
                    // emitter (u ~ 0.5, v ~ 0.63); this crop holds it with
                    // wide margin while excluding most of the sky-mirror.
                    constexpr ZHLN::Test::Image::NormalizedRect kReflectionCrop {.x0 = 0.35, .y0 = 0.52, .x1 = 0.65, .y1 = 0.77};

                    TickFrames(eng, 1);
                    const RgbImage onA = Capture(eng, "headless_lighting_rt_reflect_f0.ppm");
                    TickFrames(eng, 1);
                    const RgbImage onB = Capture(eng, "headless_lighting_rt_reflect_f1.ppm");
                    if (!(ZHLN::Test::ExpectTrue(onA.Valid()) && ZHLN::Test::ExpectTrue(onB.Valid()))) {
                        captureFailed = true;
                        return false;
                    }

                    auto&      reg      = eng.GetRegistry();
                    const auto settings = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                    if (!settings.empty()) {
                        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings[0], [](auto& pp) { pp.enableSSR = 0; });
                    }
                    TickFrames(eng, 2);
                    const RgbImage off = Capture(eng, "headless_lighting_rt_reflect_ssr_off.ppm");
                    if (!settings.empty()) {
                        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings[0], [](auto& pp) { pp.enableSSR = 1; });
                    }
                    if (!ZHLN::Test::ExpectTrue(off.Valid())) {
                        captureFailed = true;
                        return false;
                    }

                    const RgbImage cropA         = ZHLN::Test::Image::CropImage(onA, kReflectionCrop);
                    const RgbImage cropB         = ZHLN::Test::Image::CropImage(onB, kReflectionCrop);
                    const RgbImage cropOff       = ZHLN::Test::Image::CropImage(off, kReflectionCrop);
                    const double   cropPixels    = static_cast<double>(cropA.width * cropA.height);
                    const double   noiseChanged  = CompareFrames(cropA, cropB).frac32 * cropPixels;
                    const double   signalChanged = CompareFrames(cropA, cropOff).frac32 * cropPixels;
                    ZHLN::Println(
                        "    [INFO] SSR reflection A/B in mirror crop: on/on changed={:.1f}px, on/off changed={:.1f}px ({} crop px)",
                        noiseChanged, signalChanged, cropA.width * cropA.height
                    );

                    // Coverage is changed pixels when the switch flips: the
                    // blob is ~200px, so 24 (the old coverage number) keeps
                    // its meaning while the noise term keeps bit-exact runs
                    // honest. A polished surface showing nothing at all is the
                    // SSR -> IBL fallback, not an artifact in a reflection
                    // that does exist.
                    const bool reflectionPresent = ZHLN::Test::ExpectGt(signalChanged, std::max(24.0, 5.0 * noiseChanged));
                    reflectionMissing = !reflectionPresent;
                    return reflectionPresent;
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
        std::expected<void, ZHLN::ErrorCode> multi_emissive_sources_and_surface_reflection_interaction() {
            auto engine      = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
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
                auto mirrorMat = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 1.0f, .roughness = 0.02f, .baseColor = {0.9f, 0.9f, 0.95f, 1.0f}});
                if (!ZHLN::Test::ExpectTrue(mirrorMat.has_value())) {
                    return std::unexpected(LightingRTTestError::MaterialCreationFailed);
                }

                ZHLN::PrefabFactory::CreatePlane(
                    *engine, 120.0f, {0.9f, 0.9f, 0.95f, 1.0f},
                    ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = false, .materialOverride = *mirrorMat}
                );

                // 4 Distinct High-Luminance Emissive Geometric Emitters at Y = 3.0, Z = 0.0:
                //   Emitter 1: X = -4.5 (Pure Red)
                //   Emitter 2: X = -1.5 (Pure Green)
                //   Emitter 3: X = +1.5 (Pure Blue)
                //   Emitter 4: X = +4.5 (Golden Yellow)
                auto matEmissiveRed = rc.CreateMaterial(
                    ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.5f, .baseColor = {1.0f, 0.05f, 0.05f, 1.0f}, .emissive = {6.0f, 0.0f, 0.0f, 1.0f}}
                );
                auto matEmissiveGrn = rc.CreateMaterial(
                    ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.5f, .baseColor = {0.05f, 1.0f, 0.05f, 1.0f}, .emissive = {0.0f, 6.0f, 0.0f, 1.0f}}
                );
                auto matEmissiveBlu = rc.CreateMaterial(
                    ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.5f, .baseColor = {0.05f, 0.05f, 1.0f, 1.0f}, .emissive = {0.0f, 0.0f, 6.0f, 1.0f}}
                );
                auto matEmissiveYel = rc.CreateMaterial(
                    ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.5f, .baseColor = {1.0f, 0.9f, 0.05f, 1.0f}, .emissive = {5.0f, 4.5f, 0.0f, 1.0f}}
                );

                ZHLN::PrefabFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::PrefabFactory::SpawnParams {
                        .position = JPH::RVec3(-4.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveRed
                    }
                );
                ZHLN::PrefabFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::PrefabFactory::SpawnParams {
                        .position = JPH::RVec3(-1.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveGrn
                    }
                );
                ZHLN::PrefabFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(1.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveBlu}
                );
                ZHLN::PrefabFactory::CreateBox(
                    *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                    ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(4.5, 3.0, 0.0), .createPhysics = false, .materialOverride = *matEmissiveYel}
                );

                const auto camComp = engine->GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
                if (!ZHLN::Test::ExpectTrue(camComp.has_value())) {
                    return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
                }
                ZHLN::Camera& cam = camComp->camera;
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
                    // Two SSR-on frames (noise baseline) plus one SSR-off
                    // frame: the A/B proves a reflection lands in each strip,
                    // the cross-strip ordering below proves each strip carries
                    // its own emitter's hue. The switch is restored before
                    // returning so RunStableScene retries see the same scene.
                    const RgbImage frame = Capture(eng, "headless_lighting_multi_emissive.ppm");
                    const RgbImage onB   = Capture(eng, "headless_lighting_multi_emissive_b.ppm");
                    if (!(ZHLN::Test::ExpectTrue(frame.Valid()) && ZHLN::Test::ExpectTrue(onB.Valid()))) {
                        captureFailed = true;
                        return false;
                    }

                    auto&      reg      = eng.GetRegistry();
                    const auto settings = reg.GetEntitiesWith<ZHLN::Components::GlobalSettingsTagComponent>();
                    if (!settings.empty()) {
                        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings[0], [](auto& pp) { pp.enableSSR = 0; });
                    }
                    TickFrames(eng, 2);
                    const RgbImage off = Capture(eng, "headless_lighting_multi_emissive_ssr_off.ppm");
                    if (!settings.empty()) {
                        reg.Patch<ZHLN::Components::PostProcessSettingsComponent>(settings[0], [](auto& pp) { pp.enableSSR = 1; });
                    }
                    if (!ZHLN::Test::ExpectTrue(off.Valid())) {
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

                    // Per-strip SSR A/B: changed pixels when the switch flips,
                    // against the on/on noise in the same strip. A presence
                    // floor of 2px only asks "a systematic change exists" --
                    // blob sizes vary wildly by emitter (red ~112px, green
                    // ~15px), so any shared absolute floor either misses green
                    // or grades blob size instead of the reflection.
                    const auto stripAB = [&](double x0, double x1) {
                        const ZHLN::Test::Image::NormalizedRect rect {.x0 = x0, .y0 = 0.52, .x1 = x1, .y1 = 0.95};
                        const RgbImage a  = ZHLN::Test::Image::CropImage(frame, rect);
                        const RgbImage b  = ZHLN::Test::Image::CropImage(onB, rect);
                        const RgbImage o  = ZHLN::Test::Image::CropImage(off, rect);
                        const double   px = static_cast<double>(a.width * a.height);
                        return std::array<double, 2> {CompareFrames(a, o).frac32 * px, CompareFrames(a, b).frac32 * px};
                    };
                    const auto [redSig, redNoise] = stripAB(0.20, 0.36);
                    const auto [grnSig, grnNoise] = stripAB(0.38, 0.48);
                    const auto [bluSig, bluNoise] = stripAB(0.52, 0.62);
                    const auto [yelSig, yelNoise] = stripAB(0.64, 0.80);
                    ZHLN::Println(
                        "      strip on/off changed px (noise): red={:.1f} ({:.1f}) green={:.1f} ({:.1f}) blue={:.1f} ({:.1f}) yellow={:.1f} ({:.1f})",
                        redSig, redNoise, grnSig, grnNoise, bluSig, bluNoise, yelSig, yelNoise
                    );
                    const bool redAB = ZHLN::Test::ExpectGt(redSig, std::max(2.0, 5.0 * redNoise));
                    const bool grnAB = ZHLN::Test::ExpectGt(grnSig, std::max(2.0, 5.0 * grnNoise));
                    const bool bluAB = ZHLN::Test::ExpectGt(bluSig, std::max(2.0, 5.0 * bluNoise));
                    const bool yelAB = ZHLN::Test::ExpectGt(yelSig, std::max(2.0, 5.0 * yelNoise));

                    // 1. Spatial mirror correspondence. The strips are ~100x
                    // the blob area, so strip MEANS can never show the
                    // reflection (a perfect 200px blob moves a 21kpx sky-blue
                    // mean by <1 luma); the old 1.3x mean ratios were
                    // unachievable by construction. Order dominant COUNTS
                    // across strips instead: the sky contributes zero to every
                    // detector's floors except blue's, so each strip must
                    // carry its own hue most.
                    ZHLN::Println(
                        "      dominant red/grn per strip: s1={}/{} s2={}/{} s3={}/{} s4={}/{}", reflStripRed.dominantRed, reflStripRed.dominantGrn,
                        reflStripGrn.dominantRed, reflStripGrn.dominantGrn, reflStripBlu.dominantRed, reflStripBlu.dominantGrn,
                        reflStripYel.dominantRed, reflStripYel.dominantGrn
                    );
                    ZHLN::Println(
                        "      dominant blu/yel per strip: s1={}/{} s2={}/{} s3={}/{} s4={}/{}", reflStripRed.dominantBlu, reflStripRed.yellowMix,
                        reflStripGrn.dominantBlu, reflStripGrn.yellowMix, reflStripBlu.dominantBlu, reflStripBlu.yellowMix,
                        reflStripYel.dominantBlu, reflStripYel.yellowMix
                    );
                    // strip 1 mirrors the red emitter
                    const bool reflRedOk = ZHLN::Test::ExpectGt(reflStripRed.dominantRed, reflStripGrn.dominantRed + 10u) &&
                                           ZHLN::Test::ExpectGt(reflStripRed.dominantRed, reflStripBlu.dominantRed + 10u) &&
                                           ZHLN::Test::ExpectGt(reflStripRed.dominantRed, reflStripYel.dominantRed + 10u);
                    // strip 2 mirrors the green emitter (smaller margin: the green blob is dimmer)
                    const bool reflGrnOk = ZHLN::Test::ExpectGt(reflStripGrn.dominantGrn, reflStripRed.dominantGrn + 5u) &&
                                           ZHLN::Test::ExpectGt(reflStripGrn.dominantGrn, reflStripBlu.dominantGrn + 5u) &&
                                           ZHLN::Test::ExpectGt(reflStripGrn.dominantGrn, reflStripYel.dominantGrn + 5u);
                    // strip 3 mirrors the blue emitter: blue-on-blue is
                    // unmeasurable by color (the sky passes any blue count),
                    // so the strip A/B above owns blue and there is no color
                    // ordering gate for it.
                    const bool reflBluOk = bluAB;
                    // Yellow has no single dominant channel to lean on, so its
                    // signature is the R+G mix count leading the other strips.
                    // strip 4 mirrors the yellow emitter
                    const bool reflYelOk = ZHLN::Test::ExpectGt(reflStripYel.yellowMix, reflStripRed.yellowMix + 10u) &&
                                           ZHLN::Test::ExpectGt(reflStripYel.yellowMix, reflStripGrn.yellowMix + 10u) &&
                                           ZHLN::Test::ExpectGt(reflStripYel.yellowMix, reflStripBlu.yellowMix + 10u);

                    // 2. Validate Upper Direct Emission visibility. The peak-luma guard
                    // stays absolute: it only asserts the emitters are directly visible
                    // somewhere in the upper frame, not that the scene is bright.
                    const auto     upperDirect      = MeasureSubRegion(frame, {.x0 = 0.0, .y0 = 0.05, .x1 = 1.0, .y1 = 0.45});
                    const uint32_t directChroma     = upperDirect.dominantRed + upperDirect.dominantGrn + upperDirect.dominantBlu + upperDirect.yellowMix;
                    // emitters are directly visible in the upper frame
                    const bool directVisible = ZHLN::Test::ExpectGt(upperDirect.maxLuma, 60.0) && ZHLN::Test::ExpectGt(directChroma * 1000, upperDirect.pixels);

                    return redAB && grnAB && yelAB && reflRedOk && reflGrnOk && reflBluOk && reflYelOk && directVisible;
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
        std::expected<void, ZHLN::ErrorCode> dense_multi_light_emissive_materials_cross_interaction() {
            auto engine      = CreateTestEngine(640, 480);
            if (!ZHLN::Test::ExpectTrue(engine != nullptr)) {
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

                auto floorMat = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 0.5f, .roughness = 0.25f, .baseColor = {0.6f, 0.6f, 0.65f, 1.0f}});
                ZHLN::PrefabFactory::CreatePlane(
                    *engine, 80.0f, {0.6f, 0.6f, 0.65f, 1.0f},
                    ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0, 0, 0), .createPhysics = false, .materialOverride = *floorMat}
                );

                auto monolithMat = rc.CreateMaterial(
                    ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.2f, .baseColor = {0.0f, 1.0f, 1.0f, 1.0f}, .emissive = {0.0f, 20.0f, 20.0f, 1.0f}}
                );
                ZHLN::PrefabFactory::CreateBox(
                    *engine, JPH::Vec3(0.8f, 2.5f, 0.8f),
                    ZHLN::PrefabFactory::SpawnParams {.position = JPH::RVec3(0.0, 2.5, 0.0), .createPhysics = false, .materialOverride = *monolithMat}
                );

                auto goldMat      = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 1.0f, .roughness = 0.15f, .baseColor = {1.0f, 0.76f, 0.14f, 1.0f}});
                auto roughPlastic = rc.CreateMaterial(ZHLN::MaterialDesc {.metallic = 0.0f, .roughness = 0.8f, .baseColor = {0.8f, 0.2f, 0.2f, 1.0f}});

                for (int x = -2; x <= 2; ++x) {
                    for (int z = -2; z <= 2; ++z) {
                        if (x == 0 && z == 0)
                            continue;
                        float px = static_cast<float>(x) * 3.5f;
                        float pz = static_cast<float>(z) * 3.5f;
                        ZHLN::PrefabFactory::CreateBox(
                            *engine, JPH::Vec3(0.5f, 0.5f, 0.5f),
                            ZHLN::PrefabFactory::SpawnParams {
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

                const auto camComp = engine->GetRegistry().GetSingleton<ZHLN::Components::CameraComponent>();
                if (!ZHLN::Test::ExpectTrue(camComp.has_value())) {
                    return std::unexpected(ZHLN::ErrorCode(ZHLN::Test::TestFrameworkError::AssertionFailed));
                }
                ZHLN::Camera& cam = camComp->camera;
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
                    if (!ZHLN::Test::ExpectTrue(frame.Valid())) {
                        captureFailed = true;
                        return false;
                    }

                    const FrameMetrics m = MeasureImage(frame);

                    ZHLN::Println("    [INFO] Dense 32-Light + Emissive Monolith Scene Metrics:");
                    ZHLN::Println("      Lit Pixels: {}, Dark Pixels: {}, Saturated Pixels: {}, Mean Luma: {:.2f}", m.lit, m.dark, m.saturated, m.meanLuma);
                    ZHLN::Println(
                        "      Cyan (Monolith Reflection) Pixels: {}, Red Pixels: {}, Green Pixels: {}, Blue Pixels: {}", m.cyan, m.red, m.green, m.blue
                    );

                    // Which primaries the 32-light palette can even produce: the
                    // palette formula authors 1 red-dominant light, 11
                    // blue-dominant, and ZERO green-dominant, so green > 200
                    // could never pass (mixed lights on neutral surfaces only
                    // dilute further). Derive the expectation from the palette
                    // instead of asserting all three primaries blindly.
                    // 32 mirrors kLightCount in the scene block: same palette, same order.
                    constexpr size_t kPaletteLights = 32;
                    size_t             paletteRed   = 0;
                    size_t             paletteGrn   = 0;
                    size_t             paletteBlu   = 0;
                    for (size_t i = 0; i < kPaletteLights; ++i) {
                        const float angle = (static_cast<float>(i) / static_cast<float>(kPaletteLights)) * 6.283185f;
                        const float r     = std::sin(angle) * 0.5f + 0.5f;
                        const float g     = std::cos(angle * 0.5f) * 0.5f + 0.5f;
                        const float b     = std::sin(angle * 1.5f + 1.0f) * 0.5f + 0.5f;
                        // MeasureImage's strict classifiers, run on the palette.
                        if (r >= 1.6f * g && r >= 1.6f * b) {
                            ++paletteRed;
                        }
                        if (g >= 1.6f * r && g >= 1.6f * b) {
                            ++paletteGrn;
                        }
                        if (b >= 1.6f * r && b >= 1.6f * g) {
                            ++paletteBlu;
                        }
                    }
                    ZHLN::Println("      palette-authored primaries: red={} green={} blue={}", paletteRed, paletteGrn, paletteBlu);

                    const bool wellLit = ZHLN::Test::ExpectGt(m.lit, (m.total * 0.35));
                    // 32x220 lights plus 20x emissive at exposure 6 renders
                    // ~19% near-white BY CONSTRUCTION; the old 5% cap graded
                    // the look, not the lighting. The bound is a white-out
                    // sanity bound with headroom, not a calibration.
                    const bool limitedBlowout = ZHLN::Test::ExpectLt(m.saturated, (m.total * 0.50));
                    const bool cyanObserved   = ZHLN::Test::ExpectGt(m.cyan, 200u);
                    // The only emissive geometry is cyan, not pure blue. Each
                    // primary is gated only when the palette authors it: red
                    // needs its >200 pool (one red-dominant light plus gold
                    // surfaces), blue a visible >24 patch (the coverage gate
                    // the single-reflection case uses), green nothing at all.
                    const bool multiColorActive = (paletteRed == 0 || ZHLN::Test::ExpectGt(m.red, 200u)) &&
                                                  (paletteGrn == 0 || ZHLN::Test::ExpectGt(m.green, 200u)) &&
                                                  (paletteBlu == 0 || ZHLN::Test::ExpectGt(m.blue, 24u));

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
